#include <iostream>
#include <string>
#include <cstdlib>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "../settings.hpp"

// TODO: Evolve windbg_agent.exe into a standalone headless debugger.
//
// Currently this is an HTTP client that talks to a running !agent http server
// inside WinDbg/CDB. The goal is to make it a self-contained tool that:
//
//   1. Host dbgeng directly — call DebugCreate() to get IDebugClient, attach
//      to processes (CreateProcess/AttachProcess) or open dumps (OpenDumpFile),
//      and run a debugger event loop. Essentially a programmable cdb.exe.
//
//   2. Serve HTTP/MCP — reuse the existing HttpServer and MCPServer to expose
//      /exec, /ask, /status, /shutdown endpoints. External AI agents (Claude
//      Code, Copilot, etc.) connect here to drive the debugger.
//
//   3. Keep the current HTTP client mode — when --url is given or no target is
//      specified, behave as today: forward commands to a remote server.
//
// This turns a single binary into both the debugger and its integration layer:
//   windbg_agent.exe -z crash.dmp --serve        # open dump + start server
//   windbg_agent.exe -p 1234 --serve              # attach to PID + serve
//   windbg_agent.exe --url=http://... exec "kb"   # client mode (current)
//
// Key pieces needed:
//   - Debugger init: DebugCreate(), IDebugClient, IDebugControl
//   - Target management: -z (dump), -p (attach), spawn process
//   - Event loop: WaitForEvent / DispatchCallbacks
//   - Console output: replace DML with plain-text for headless use
//   - HttpServer/MCPServer already work standalone (no WinDbg dependency)

void print_usage() {
    std::cerr << "Usage: windbg_agent.exe [--url=URL] <command> [args]\n\n";
    std::cerr << "Commands:\n";
    std::cerr << "  exec <cmd>       Run debugger command, return raw output\n";
    std::cerr << "  ask <question>   AI-assisted query with reasoning\n";
    std::cerr << "  interactive      Start interactive chat session\n";
    std::cerr << "  status           Check server status\n";
    std::cerr << "  shutdown         Stop HTTP server\n\n";
    std::cerr << "Config commands (no server required):\n";
    std::cerr << "  config show              Show all settings\n";
    std::cerr << "  config provider <name>   Set default provider (claude, copilot)\n";
    std::cerr << "  config byok              Show BYOK status for current provider\n";
    std::cerr << "  config byok key <val>    Set BYOK API key\n";
    std::cerr << "  config byok endpoint <url>  Set BYOK endpoint\n";
    std::cerr << "  config byok model <name>    Set BYOK model\n";
    std::cerr << "  config byok type <type>     Set BYOK type (openai, anthropic, azure)\n";
    std::cerr << "  config byok enable       Enable BYOK\n";
    std::cerr << "  config byok disable      Disable BYOK\n\n";
    std::cerr << "Environment:\n";
    std::cerr << "  WINDBG_AGENT_URL     HTTP server URL (default: http://127.0.0.1:9999)\n";
}

std::string get_url(int argc, char* argv[]) {
    // Priority: --url=X flag > WINDBG_AGENT_URL env > default
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.rfind("--url=", 0) == 0) {
            return arg.substr(6);
        }
    }
    if (const char* env = std::getenv("WINDBG_AGENT_URL")) {
        return env;
    }
    return "http://127.0.0.1:9999";
}

class HttpClient {
public:
    explicit HttpClient(const std::string& url) : url_(url) {
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
            throw std::runtime_error("Connection failed - is HTTP server running?");
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
            throw std::runtime_error("Connection failed - is HTTP server running?");
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
            throw std::runtime_error("Connection failed - is HTTP server running?");
        }
        return res->body;
    }

    void shutdown() {
        auto res = client_->Post("/shutdown", "", "application/json");
        if (!res) {
            throw std::runtime_error("Connection failed - is HTTP server running?");
        }
    }

private:
    std::string url_;
    std::unique_ptr<httplib::Client> client_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Config commands (no server required)
// ─────────────────────────────────────────────────────────────────────────────

int run_config(int argc, char* argv[], int cmd_idx) {
    using namespace windbg_agent;

    // Collect remaining args
    std::vector<std::string> args;
    for (int i = cmd_idx + 1; i < argc; i++) {
        args.push_back(argv[i]);
    }

    if (args.empty() || args[0] == "show") {
        // Show all settings
        auto settings = LoadSettings();
        std::cout << "Settings file: " << GetSettingsPath() << "\n\n";
        std::cout << "Provider: " << libagents::provider_type_name(settings.default_provider) << "\n";
        std::cout << "Response timeout: " << settings.response_timeout_ms << " ms\n";
        if (!settings.custom_prompt.empty()) {
            std::cout << "Custom prompt: " << settings.custom_prompt << "\n";
        }
        std::cout << "\nBYOK configurations:\n";
        if (settings.byok.empty()) {
            std::cout << "  (none configured)\n";
        } else {
            for (const auto& [provider, byok] : settings.byok) {
                std::cout << "  " << provider << ":\n";
                std::cout << "    Enabled:  " << (byok.enabled ? "yes" : "no") << "\n";
                std::cout << "    API Key:  " << (byok.api_key.empty() ? "(not set)" : "********") << "\n";
                std::cout << "    Endpoint: " << (byok.base_url.empty() ? "(default)" : byok.base_url) << "\n";
                std::cout << "    Model:    " << (byok.model.empty() ? "(default)" : byok.model) << "\n";
                std::cout << "    Type:     " << (byok.provider_type.empty() ? "(default)" : byok.provider_type) << "\n";
            }
        }
        return 0;
    }

    if (args[0] == "provider") {
        if (args.size() < 2) {
            auto settings = LoadSettings();
            std::cout << "Current provider: " << libagents::provider_type_name(settings.default_provider) << "\n";
            return 0;
        }
        auto settings = LoadSettings();
        settings.default_provider = ParseProviderType(args[1]);
        SaveSettings(settings);
        std::cout << "Provider set to: " << libagents::provider_type_name(settings.default_provider) << "\n";
        return 0;
    }

    if (args[0] == "byok") {
        auto settings = LoadSettings();
        std::string provider_name = libagents::provider_type_name(settings.default_provider);

        if (args.size() < 2) {
            // Show BYOK status for current provider
            const auto* byok = settings.get_byok();
            std::cout << "BYOK status for provider '" << provider_name << "':\n";
            if (byok) {
                std::cout << "  Enabled:  " << (byok->enabled ? "yes" : "no") << "\n";
                std::cout << "  API Key:  " << (byok->api_key.empty() ? "(not set)" : "********") << "\n";
                std::cout << "  Endpoint: " << (byok->base_url.empty() ? "(default)" : byok->base_url) << "\n";
                std::cout << "  Model:    " << (byok->model.empty() ? "(default)" : byok->model) << "\n";
                std::cout << "  Type:     " << (byok->provider_type.empty() ? "(default)" : byok->provider_type) << "\n";
                std::cout << "  Usable:   " << (byok->is_usable() ? "yes" : "no") << "\n";
            } else {
                std::cout << "  (not configured)\n";
            }
            return 0;
        }

        std::string subcmd = args[1];
        std::string value = args.size() > 2 ? args[2] : "";

        if (subcmd == "enable") {
            settings.get_or_create_byok().enabled = true;
            SaveSettings(settings);
            std::cout << "BYOK enabled for provider '" << provider_name << "'.\n";
        }
        else if (subcmd == "disable") {
            settings.get_or_create_byok().enabled = false;
            SaveSettings(settings);
            std::cout << "BYOK disabled for provider '" << provider_name << "'.\n";
        }
        else if (subcmd == "key") {
            if (value.empty()) {
                std::cerr << "Error: API key value required.\n";
                return 1;
            }
            settings.get_or_create_byok().api_key = value;
            SaveSettings(settings);
            std::cout << "BYOK API key set for provider '" << provider_name << "'.\n";
        }
        else if (subcmd == "endpoint") {
            settings.get_or_create_byok().base_url = value;
            SaveSettings(settings);
            if (value.empty())
                std::cout << "BYOK endpoint cleared (using default).\n";
            else
                std::cout << "BYOK endpoint set to: " << value << "\n";
        }
        else if (subcmd == "model") {
            settings.get_or_create_byok().model = value;
            SaveSettings(settings);
            if (value.empty())
                std::cout << "BYOK model cleared (using default).\n";
            else
                std::cout << "BYOK model set to: " << value << "\n";
        }
        else if (subcmd == "type") {
            settings.get_or_create_byok().provider_type = value;
            SaveSettings(settings);
            if (value.empty())
                std::cout << "BYOK type cleared (using default).\n";
            else
                std::cout << "BYOK type set to: " << value << "\n";
        }
        else {
            std::cerr << "Unknown byok subcommand: " << subcmd << "\n";
            return 1;
        }
        return 0;
    }

    std::cerr << "Unknown config subcommand: " << args[0] << "\n";
    return 1;
}

void run_interactive(HttpClient& client) {
    std::cout << "Connected to HTTP server. Type 'exit' to quit.\n\n";
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

    // Config commands don't need server connection
    if (command == "config") {
        return run_config(argc, argv, cmd_idx);
    }

    try {
        HttpClient client(url);

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
            std::cout << "HTTP server stopped.\n";
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
