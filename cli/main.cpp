#include <iostream>
#include <string>
#include <cstdlib>

#include <httplib.h>
#include <nlohmann/json.hpp>

void print_usage() {
    std::cerr << "Usage: windbg_copilot.exe [--url=URL] <command> [args]\n\n";
    std::cerr << "Commands:\n";
    std::cerr << "  exec <cmd>       Run debugger command, return raw output\n";
    std::cerr << "  ask <question>   AI-assisted query with reasoning\n";
    std::cerr << "  interactive      Start interactive chat session\n";
    std::cerr << "  status           Check server status\n";
    std::cerr << "  shutdown         Stop handoff server\n\n";
    std::cerr << "Environment:\n";
    std::cerr << "  WINDBG_COPILOT_URL   Default handoff URL (default: http://127.0.0.1:9999)\n";
}

std::string get_url(int argc, char* argv[]) {
    // Priority: --url=X flag > WINDBG_COPILOT_URL env > default
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.rfind("--url=", 0) == 0) {
            return arg.substr(6);
        }
    }
    if (const char* env = std::getenv("WINDBG_COPILOT_URL")) {
        return env;
    }
    return "http://127.0.0.1:9999";
}

class HandoffClient {
public:
    explicit HandoffClient(const std::string& url) : url_(url) {
        // Parse host and port from URL
        // Format: http://host:port
        std::string host_port = url;
        if (host_port.rfind("http://", 0) == 0) {
            host_port = host_port.substr(7);
        }
        client_ = std::make_unique<httplib::Client>(url);
        client_->set_read_timeout(120, 0);  // 120 seconds for AI queries
        client_->set_connection_timeout(5, 0);
    }

    std::string exec(const std::string& cmd) {
        nlohmann::json body = {{"command", cmd}};
        auto res = client_->Post("/exec", body.dump(), "application/json");

        if (!res) {
            throw std::runtime_error("Connection failed - is handoff server running?");
        }
        if (res->status != 200) {
            auto json = nlohmann::json::parse(res->body);
            throw std::runtime_error(json.value("error", "Request failed"));
        }

        auto json = nlohmann::json::parse(res->body);
        return json.value("output", "");
    }

    std::string ask(const std::string& query) {
        nlohmann::json body = {{"query", query}};
        auto res = client_->Post("/ask", body.dump(), "application/json");

        if (!res) {
            throw std::runtime_error("Connection failed - is handoff server running?");
        }
        if (res->status != 200) {
            auto json = nlohmann::json::parse(res->body);
            throw std::runtime_error(json.value("error", "Request failed"));
        }

        auto json = nlohmann::json::parse(res->body);
        return json.value("response", "");
    }

    std::string status() {
        auto res = client_->Get("/status");
        if (!res) {
            throw std::runtime_error("Connection failed - is handoff server running?");
        }
        return res->body;
    }

    void shutdown() {
        auto res = client_->Post("/shutdown", "", "application/json");
        if (!res) {
            throw std::runtime_error("Connection failed - is handoff server running?");
        }
    }

private:
    std::string url_;
    std::unique_ptr<httplib::Client> client_;
};

void run_interactive(HandoffClient& client) {
    std::cout << "Connected to handoff server. Type 'exit' to quit.\n\n";
    std::string input;

    while (true) {
        std::cout << "> ";
        std::cout.flush();

        if (!std::getline(std::cin, input)) {
            break;
        }
        if (input == "exit" || input == "quit") {
            break;
        }
        if (input.empty()) {
            continue;
        }

        try {
            std::cout << client.ask(input) << "\n\n";
        }
        catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string url = get_url(argc, argv);

    // Find command index (skip --url if present)
    int cmd_idx = 1;
    if (std::string(argv[1]).rfind("--url=", 0) == 0) {
        cmd_idx = 2;
    }

    if (cmd_idx >= argc) {
        print_usage();
        return 1;
    }

    std::string command = argv[cmd_idx];

    // Collect remaining args as the command/query
    std::string args;
    for (int i = cmd_idx + 1; i < argc; i++) {
        if (!args.empty()) args += " ";
        args += argv[i];
    }

    try {
        HandoffClient client(url);

        if (command == "exec") {
            if (args.empty()) {
                std::cerr << "Error: exec requires a command\n";
                return 1;
            }
            std::cout << client.exec(args);
            return 0;
        }
        else if (command == "ask") {
            if (args.empty()) {
                std::cerr << "Error: ask requires a question\n";
                return 1;
            }
            std::cout << client.ask(args) << "\n";
            return 0;
        }
        else if (command == "interactive") {
            run_interactive(client);
            return 0;
        }
        else if (command == "status") {
            std::cout << client.status() << "\n";
            return 0;
        }
        else if (command == "shutdown") {
            client.shutdown();
            std::cout << "Handoff server stopped.\n";
            return 0;
        }
        else {
            std::cerr << "Unknown command: " << command << "\n";
            print_usage();
            return 1;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        std::cerr << "URL: " << url << "\n";
        return 1;
    }

    return 0;
}
