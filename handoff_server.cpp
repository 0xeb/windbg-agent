#include "handoff_server.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <chrono>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")

namespace windbg_copilot {

class HandoffServer::Impl {
public:
    httplib::Server server;
};

HandoffServer::HandoffServer() = default;

HandoffServer::~HandoffServer() {
    stop();
}

QueueResult HandoffServer::queue_and_wait(PendingCommand::Type type, const std::string& input) {
    if (!running_.load()) {
        return {false, "Error: handoff server is not running"};
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
        return {false, "Error: handoff server stopped"};
    }

    return {true, cmd.result};
}

int HandoffServer::start(int port, ExecCallback exec_cb, AskCallback ask_cb) {
    if (running_.load()) {
        return port_;
    }

    exec_cb_ = exec_cb;
    ask_cb_ = ask_cb;

    impl_ = std::make_unique<Impl>();

    // bind_to_port returns bool, not the port number
    bool bound = impl_->server.bind_to_port("127.0.0.1", port);
    if (!bound) {
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

    port_ = port;
    running_.store(true);

    server_thread_ = std::thread([this]() {
        impl_->server.listen_after_bind();
        running_.store(false);
        queue_cv_.notify_all();
        complete_pending_commands("Error: handoff server stopped");
    });

    return port_;
}

void HandoffServer::set_interrupt_check(std::function<bool()> check) {
    interrupt_check_ = check;
}

void HandoffServer::wait() {
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

void HandoffServer::stop() {
    if (impl_) {
        impl_->server.stop();
    }
    running_.store(false);
    queue_cv_.notify_all();
    complete_pending_commands("Error: handoff server stopped");
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void HandoffServer::complete_pending_commands(const std::string& result) {
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

int find_free_port(int start_port) {
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return start_port;
    }

    int found_port = start_port;

    for (int port = start_port; port < start_port + 100; port++) {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) {
            continue;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(static_cast<u_short>(port));

        int result = bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        closesocket(sock);

        if (result == 0) {
            found_port = port;
            break;
        }
    }

    WSACleanup();
    return found_port;
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

std::string format_handoff_info(
    const std::string& target_name,
    unsigned long pid,
    const std::string& state,
    const std::string& url
) {
    std::ostringstream ss;
    ss << "DEBUGGER HANDOFF ACTIVE\n";
    ss << "Target: " << target_name << " (PID " << pid << ")\n";
    ss << "State: " << state << "\n";
    ss << "URL: " << url << "\n\n";

    ss << "WinDbg Copilot is an expert debugger assistant. You don't need to know\n";
    ss << "WinDbg commands - just describe what you want in plain English.\n\n";

    ss << "QUICK START:\n";
    ss << "  windbg_copilot.exe --url=" << url << " ask \"what caused this crash?\"\n";
    ss << "  windbg_copilot.exe --url=" << url << " ask \"show me the call stack\"\n";
    ss << "  windbg_copilot.exe --url=" << url << " ask \"what are the local variables?\"\n\n";

    ss << "The AI will execute the right debugger commands and explain the results.\n\n";

    ss << "RAW COMMANDS (if you know WinDbg syntax):\n";
    ss << "  windbg_copilot.exe --url=" << url << " exec \"kb\"\n";
    ss << "  windbg_copilot.exe --url=" << url << " exec \"!analyze -v\"\n\n";

    ss << "CAPABILITIES:\n";
    ss << "- Crash analysis, stack traces, memory inspection\n";
    ss << "- Expression evaluation, disassembly, type display\n";
    ss << "- Reverse engineering and decompilation\n";
    ss << "- Shellcode and suspicious memory detection\n";
    ss << "- Just ask - it knows WinDbg/CDB commands\n\n";

    ss << "OTHER: status, shutdown, interactive\n";
    return ss.str();
}

} // namespace windbg_copilot
