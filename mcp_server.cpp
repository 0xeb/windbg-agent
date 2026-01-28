#include "mcp_server.hpp"

#include <fastmcpp/mcp/handler.hpp>
#include <fastmcpp/server/sse_server.hpp>
#include <fastmcpp/tools/manager.hpp>
#include <fastmcpp/tools/tool.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <sstream>

namespace windbg_agent {

using Json = nlohmann::json;

class MCPServer::Impl {
public:
    fastmcpp::tools::ToolManager tool_manager;
    std::unique_ptr<fastmcpp::server::SseServerWrapper> server;
};

MCPServer::MCPServer() = default;

MCPServer::~MCPServer() {
    stop();
}

MCPQueueResult MCPServer::queue_and_wait(MCPPendingCommand::Type type, const std::string& input) {
    if (!running_.load()) {
        return {false, "Error: MCP server is not running"};
    }

    MCPPendingCommand cmd;
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
        return {false, "Error: MCP server stopped"};
    }

    return {true, cmd.result};
}

int MCPServer::start(int port, ExecCallback exec_cb, AskCallback ask_cb) {
    if (running_.load()) {
        return port_;
    }

    exec_cb_ = exec_cb;
    ask_cb_ = ask_cb;

    impl_ = std::make_unique<Impl>();

    // Register dbg_exec tool
    Json exec_input_schema = {
        {"type", "object"},
        {"properties", {
            {"command", {
                {"type", "string"},
                {"description", "WinDbg/CDB debugger command to execute (e.g., 'kb', '!analyze -v', 'dt')"}
            }}
        }},
        {"required", Json::array({"command"})}
    };

    Json exec_output_schema = {
        {"type", "object"},
        {"properties", {
            {"output", {{"type", "string"}}},
            {"success", {{"type", "boolean"}}}
        }}
    };

    fastmcpp::tools::Tool dbg_exec_tool{
        "dbg_exec",
        exec_input_schema,
        exec_output_schema,
        [this](const Json& args) -> Json {
            std::string command = args.value("command", "");
            if (command.empty()) {
                return Json{
                    {"content", Json::array({
                        Json{{"type", "text"}, {"text", "Error: missing command"}}
                    })},
                    {"isError", true}
                };
            }

            auto result = queue_and_wait(MCPPendingCommand::Type::Exec, command);

            // MCP tools/call expects content array format
            return Json{
                {"content", Json::array({
                    Json{{"type", "text"}, {"text", result.payload}}
                })},
                {"isError", !result.success}
            };
        }
    };
    dbg_exec_tool.set_description("Execute a WinDbg/CDB debugger command and return its output");
    impl_->tool_manager.register_tool(dbg_exec_tool);

    // Register dbg_ask tool
    Json ask_input_schema = {
        {"type", "object"},
        {"properties", {
            {"query", {
                {"type", "string"},
                {"description", "Question to ask the AI debugging assistant"}
            }}
        }},
        {"required", Json::array({"query"})}
    };

    Json ask_output_schema = {
        {"type", "object"},
        {"properties", {
            {"response", {{"type", "string"}}},
            {"success", {{"type", "boolean"}}}
        }}
    };

    fastmcpp::tools::Tool dbg_ask_tool{
        "dbg_ask",
        ask_input_schema,
        ask_output_schema,
        [this](const Json& args) -> Json {
            std::string query = args.value("query", "");
            if (query.empty()) {
                return Json{
                    {"content", Json::array({
                        Json{{"type", "text"}, {"text", "Error: missing query"}}
                    })},
                    {"isError", true}
                };
            }

            auto result = queue_and_wait(MCPPendingCommand::Type::Ask, query);

            return Json{
                {"content", Json::array({
                    Json{{"type", "text"}, {"text", result.payload}}
                })},
                {"isError", !result.success}
            };
        }
    };
    dbg_ask_tool.set_description("Ask the AI debugging assistant a question about the current debug session");
    impl_->tool_manager.register_tool(dbg_ask_tool);

    // Create MCP handler
    std::unordered_map<std::string, std::string> descriptions = {
        {"dbg_exec", "Execute a WinDbg/CDB debugger command and return its output"},
        {"dbg_ask", "Ask the AI debugging assistant a question about the current debug session"}
    };

    auto handler = fastmcpp::mcp::make_mcp_handler(
        "windbg-agent",
        "1.0.0",
        impl_->tool_manager,
        descriptions
    );

    // Create and start SSE server
    impl_->server = std::make_unique<fastmcpp::server::SseServerWrapper>(
        handler,
        "127.0.0.1",
        port,
        "/sse",
        "/messages"
    );

    if (!impl_->server->start()) {
        impl_.reset();
        return -1;
    }

    port_ = port;
    running_.store(true);

    return port_;
}

void MCPServer::set_interrupt_check(std::function<bool()> check) {
    interrupt_check_ = check;
}

void MCPServer::wait() {
    while (running_.load()) {
        if (interrupt_check_ && interrupt_check_()) {
            stop();
            break;
        }

        MCPPendingCommand* cmd = nullptr;

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
                if (cmd->type == MCPPendingCommand::Type::Exec && exec_cb_) {
                    cmd->result = exec_cb_(cmd->input);
                } else if (cmd->type == MCPPendingCommand::Type::Ask && ask_cb_) {
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
}

void MCPServer::stop() {
    running_.store(false);
    queue_cv_.notify_all();
    complete_pending_commands("Error: MCP server stopped");

    if (impl_ && impl_->server) {
        impl_->server->stop();
    }
}

void MCPServer::complete_pending_commands(const std::string& result) {
    std::queue<MCPPendingCommand*> pending;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::swap(pending, pending_commands_);
    }

    while (!pending.empty()) {
        MCPPendingCommand* cmd = pending.front();
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

std::string format_mcp_info(
    const std::string& target_name,
    unsigned long pid,
    const std::string& state,
    const std::string& url
) {
    std::ostringstream ss;
    ss << "MCP SERVER ACTIVE\n";
    ss << "Target: " << target_name << " (PID " << pid << ")\n";
    ss << "State: " << state << "\n";
    ss << "SSE Endpoint: " << url << "/sse\n";
    ss << "Message Endpoint: " << url << "/messages\n\n";

    ss << "AVAILABLE TOOLS:\n";
    ss << "  dbg_exec  - Execute a debugger command\n";
    ss << "  dbg_ask   - Ask the AI assistant a question\n\n";

    ss << "MCP CLIENT CONFIGURATION:\n";
    ss << "Add to your MCP client (e.g., Claude Desktop):\n";
    ss << "{\n";
    ss << "  \"mcpServers\": {\n";
    ss << "    \"windbg-agent\": {\n";
    ss << "      \"url\": \"" << url << "/sse\"\n";
    ss << "    }\n";
    ss << "  }\n";
    ss << "}\n\n";

    ss << "EXAMPLE CURL COMMANDS:\n";
    ss << "  # List available tools\n";
    ss << "  curl -X POST " << url << "/messages \\\n";
    ss << "    -H \"Content-Type: application/json\" \\\n";
    ss << "    -d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}'\n\n";

    ss << "  # Execute a debugger command\n";
    ss << "  curl -X POST " << url << "/messages \\\n";
    ss << "    -H \"Content-Type: application/json\" \\\n";
    ss << "    -d '{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"dbg_exec\",\"arguments\":{\"command\":\"kb\"}}}'\n";

    return ss.str();
}

} // namespace windbg_agent
