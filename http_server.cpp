#include "http_server.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <chrono>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")

namespace windbg_agent {

class HttpServer::Impl {
public:
    httplib::Server server;
};

HttpServer::HttpServer() = default;

HttpServer::~HttpServer() {
    stop();
}

QueueResult HttpServer::queue_and_wait(PendingCommand::Type type, const std::string& input) {
    if (!running_.load()) {
        return {false, "Error: HTTP server is not running"};
    }

    PendingCommand cmd;
    cmd.type = type;
    cmd.input = input;
    cmd.completed = false;

    std::mutex done_mutex;
    std::condition_variable done_cv;
    cmd.done_mutex = &done_mutex;
    cmd.done_cv = &done_cv;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        pending_commands_.push(&cmd);
    }
    queue_cv_.notify_one();

    {
        std::unique_lock<std::mutex> lock(done_mutex);
        done_cv.wait(lock, [&]() { return cmd.completed || !running_.load(); });
    }

    if (!cmd.completed) {
        return {false, "Error: HTTP server stopped"};
    }

    return {true, cmd.result};
}

int HttpServer::start(ExecCallback exec_cb, AskCallback ask_cb,
                      const std::string& bind_addr) {
    if (running_.load()) {
        return port_;
    }

    exec_cb_ = exec_cb;
    ask_cb_ = ask_cb;
    bind_addr_ = bind_addr;

    impl_ = std::make_unique<Impl>();

    // Let the OS assign a free port
    int assigned_port = impl_->server.bind_to_any_port(bind_addr.c_str());
    if (assigned_port < 0) {
        impl_.reset();
        return -1;
    }

    impl_->server.Post("/exec", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto json = nlohmann::json::parse(req.body);
            std::string command = json.value("command", "");

            if (command.empty()) {
                res.status = 400;
                res.set_content(R"({"error":"missing command","success":false})", "application/json");
                return;
            }

            auto result = queue_and_wait(PendingCommand::Type::Exec, command);
            nlohmann::json response = {{"output", result.payload}, {"success", result.success}};
            if (!result.success) {
                res.status = 503;
            }
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            nlohmann::json response = {{"error", e.what()}, {"success", false}};
            res.set_content(response.dump(), "application/json");
        }
    });

    impl_->server.Post("/ask", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto json = nlohmann::json::parse(req.body);
            std::string query = json.value("query", "");

            if (query.empty()) {
                res.status = 400;
                res.set_content(R"({"error":"missing query","success":false})", "application/json");
                return;
            }

            auto result = queue_and_wait(PendingCommand::Type::Ask, query);
            nlohmann::json response = {{"response", result.payload}, {"success", result.success}};
            if (!result.success) {
                res.status = 503;
            }
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            nlohmann::json response = {{"error", e.what()}, {"success", false}};
            res.set_content(response.dump(), "application/json");
        }
    });

    impl_->server.Get("/status", [](const httplib::Request&, httplib::Response& res) {
        nlohmann::json response = {{"status", "ready"}, {"success", true}};
        res.set_content(response.dump(), "application/json");
    });

    impl_->server.Post("/shutdown", [this](const httplib::Request&, httplib::Response& res) {
        nlohmann::json response = {{"status", "stopping"}, {"success", true}};
        res.set_content(response.dump(), "application/json");

        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stop();
        }).detach();
    });

    port_ = assigned_port;
    running_.store(true);

    server_thread_ = std::thread([this]() {
        impl_->server.listen_after_bind();
        running_.store(false);
        queue_cv_.notify_all();
        complete_pending_commands("Error: HTTP server stopped");
    });

    return port_;
}

void HttpServer::set_interrupt_check(std::function<bool()> check) {
    interrupt_check_ = check;
}

void HttpServer::wait() {
    while (running_.load()) {
        if (interrupt_check_ && interrupt_check_()) {
            stop();
            break;
        }

        PendingCommand* cmd = nullptr;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (queue_cv_.wait_for(lock, std::chrono::milliseconds(100),
                                   [this]() { return !pending_commands_.empty() || !running_.load(); })) {
                if (!pending_commands_.empty()) {
                    cmd = pending_commands_.front();
                    pending_commands_.pop();
                }
            }
        }

        if (cmd) {
            try {
                if (cmd->type == PendingCommand::Type::Exec && exec_cb_) {
                    cmd->result = exec_cb_(cmd->input);
                } else if (cmd->type == PendingCommand::Type::Ask && ask_cb_) {
                    cmd->result = ask_cb_(cmd->input);
                } else {
                    cmd->result = "Error: No handler for command type";
                }
            } catch (const std::exception& e) {
                cmd->result = std::string("Error: ") + e.what();
            }

            if (cmd->done_mutex && cmd->done_cv) {
                {
                    std::lock_guard<std::mutex> lock(*cmd->done_mutex);
                    cmd->completed = true;
                }
                cmd->done_cv->notify_one();
            }
        }
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void HttpServer::stop() {
    if (impl_) {
        impl_->server.stop();
    }
    running_.store(false);
    queue_cv_.notify_all();
    complete_pending_commands("Error: HTTP server stopped");
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void HttpServer::complete_pending_commands(const std::string& result) {
    std::queue<PendingCommand*> pending;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::swap(pending, pending_commands_);
    }

    while (!pending.empty()) {
        PendingCommand* cmd = pending.front();
        pending.pop();
        if (!cmd || !cmd->done_mutex || !cmd->done_cv) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(*cmd->done_mutex);
            if (!cmd->completed) {
                cmd->result = result;
                cmd->completed = true;
            }
        }
        cmd->done_cv->notify_one();
    }
}

bool copy_to_clipboard(const std::string& text) {
    if (!OpenClipboard(nullptr)) {
        return false;
    }

    EmptyClipboard();

    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (!hg) {
        CloseClipboard();
        return false;
    }

    memcpy(GlobalLock(hg), text.c_str(), text.size() + 1);
    GlobalUnlock(hg);

    SetClipboardData(CF_TEXT, hg);
    CloseClipboard();

    return true;
}

std::string format_http_info(
    const std::string& target_name,
    unsigned long pid,
    const std::string& state,
    const std::string& url
) {
    std::ostringstream ss;
    ss << "HTTP SERVER ACTIVE\n";
    ss << "Target: " << target_name << " (PID " << pid << ")\n";
    ss << "State: " << state << "\n";
    ss << "URL: " << url << "\n\n";

    ss << "HTTP API ENDPOINTS:\n";
    ss << "  POST " << url << "/exec   - Execute raw debugger command\n";
    ss << "  POST " << url << "/ask    - AI-assisted query (natural language)\n";
    ss << "  GET  " << url << "/status - Server status\n";
    ss << "  POST " << url << "/shutdown - Stop server\n\n";

    ss << "CURL EXAMPLES:\n";
    ss << "  # Execute debugger command (returns raw output)\n";
    ss << "  curl -X POST " << url << "/exec \\\n";
    ss << "    -H \"Content-Type: application/json\" \\\n";
    ss << "    -d '{\"command\": \"kb\"}'\n\n";

    ss << "  # AI query (natural language, returns explanation)\n";
    ss << "  curl -X POST " << url << "/ask \\\n";
    ss << "    -H \"Content-Type: application/json\" \\\n";
    ss << "    -d '{\"query\": \"what is the value of RAX?\"}'\n\n";

    ss << "  # More examples\n";
    ss << "  curl -X POST " << url << "/exec -H \"Content-Type: application/json\" -d '{\"command\": \"r rax\"}'\n";
    ss << "  curl -X POST " << url << "/exec -H \"Content-Type: application/json\" -d '{\"command\": \"!analyze -v\"}'\n";
    ss << "  curl -X POST " << url << "/ask -H \"Content-Type: application/json\" -d '{\"query\": \"explain this crash\"}'\n\n";

    ss << "PYTHON:\n";
    ss << "  import requests\n";
    ss << "  # Execute command\n";
    ss << "  r = requests.post('" << url << "/exec', json={'command': 'kb'})\n";
    ss << "  print(r.json()['output'])\n\n";

    ss << "  # AI query\n";
    ss << "  r = requests.post('" << url << "/ask', json={'query': 'what caused this crash?'})\n";
    ss << "  print(r.json()['response'])\n\n";

    ss << "RESPONSE FORMAT:\n";
    ss << "  /exec returns: {\"output\": \"...\", \"success\": true}\n";
    ss << "  /ask returns:  {\"response\": \"...\", \"success\": true}\n\n";

    ss << "CLI TOOL:\n";
    ss << "  windbg_agent.exe --url=" << url << " exec \"kb\"\n";
    ss << "  windbg_agent.exe --url=" << url << " ask \"what caused this crash?\"\n";
    ss << "  windbg_agent.exe --url=" << url << " interactive\n";

    return ss.str();
}

} // namespace windbg_agent
