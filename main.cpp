#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <dbgeng.h>
#include <memory>
#include <string>
#include <windows.h>

#include "http_server.hpp"
#include "mcp_server.hpp"
#include "session_store.hpp"
#include "settings.hpp"
#include "system_prompt.hpp"
#include "windbg_client.hpp"

#include <libagents/agent.hpp>
#include <libagents/tool_builder.hpp>

// Version info
#define WINDBG_AGENT_VERSION_MAJOR 1
#define WINDBG_AGENT_VERSION_MINOR 0
#define WINDBG_AGENT_VERSION_PATCH 0

// Set to 1 to disable session management (for debugging MCP tool visibility issues)
#define WINDBG_AGENT_DISABLE_SESSIONS 0

namespace
{

// Format milliseconds as human-readable duration
static std::string FormatDuration(int ms)
{
    if (ms < 1000)
        return std::to_string(ms) + " ms";

    int total_seconds = ms / 1000;
    int hours = total_seconds / 3600;
    int minutes = (total_seconds % 3600) / 60;
    int seconds = total_seconds % 60;

    std::string result;

    if (hours > 0)
    {
        result += std::to_string(hours) + " hour" + (hours != 1 ? "s" : "");
        if (minutes > 0)
            result += " " + std::to_string(minutes) + " minute" + (minutes != 1 ? "s" : "");
    }
    else if (minutes > 0)
    {
        result += std::to_string(minutes) + " minute" + (minutes != 1 ? "s" : "");
        if (seconds > 0)
            result += " " + std::to_string(seconds) + " second" + (seconds != 1 ? "s" : "");
    }
    else
    {
        result = std::to_string(seconds) + " second" + (seconds != 1 ? "s" : "");
    }

    return result;
}

// Gather runtime context from the debugger session
static windbg_agent::RuntimeContext GatherRuntimeContext(windbg_agent::WinDbgClient& dbg_client)
{
    windbg_agent::RuntimeContext ctx;

    // Target info
    ctx.target_name = dbg_client.GetTargetName();
    ctx.target_arch = dbg_client.GetTargetArchitecture();
    ctx.debugger_type = dbg_client.GetDebuggerType();

    // Working directory
    char cwd[MAX_PATH] = {0};
    if (GetCurrentDirectoryA(MAX_PATH, cwd))
        ctx.cwd = cwd;

    // Timestamp (ISO 8601 local time)
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char time_buf[32];
    struct tm local_tm;
    localtime_s(&local_tm, &t);
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S", &local_tm);
    ctx.timestamp = time_buf;

    // Platform
    ctx.platform = "Windows";

    return ctx;
}

struct AgentSession
{
    std::unique_ptr<libagents::IAgent> agent;
    libagents::ProviderType provider = libagents::ProviderType::Copilot;
    std::string provider_name;
    std::string target;
    std::string session_id;
    std::string system_prompt;
    bool primed = false;
    bool initialized = false;
    bool host_ready = false;
    std::atomic<bool> aborted{false};
    windbg_agent::WinDbgClient* dbg = nullptr;
    libagents::HostContext host;
};

// Helper to get IDebugControl for output
static IDebugControl* GetControl(PDEBUG_CLIENT Client)
{
    IDebugControl* control = nullptr;
    Client->QueryInterface(__uuidof(IDebugControl), (void**)&control);
    return control;
}

static AgentSession& GetAgentSession()
{
    static AgentSession session;
    return session;
}

static void ResetAgentSession(AgentSession& session)
{
    if (session.agent)
    {
        session.agent->shutdown();
        session.agent.reset();
    }
    session.initialized = false;
    session.host_ready = false;
    session.provider_name.clear();
    session.session_id.clear();
    session.system_prompt.clear();
    session.target.clear();
    session.primed = false;
}

static libagents::Tool BuildDebuggerTool(AgentSession& session)
{
    return libagents::make_tool(
        "dbg_exec",
        "Execute a WinDbg/CDB debugger command and return its output. "
        "Use this to inspect the target process, memory, threads, exceptions, etc.",
        [&session](std::string command) -> std::string
        {
            if (session.aborted.load())
                return "(Aborted)";

            if (!session.dbg)
                return "Error: No debugger client available";

            return session.dbg->ExecuteCommand(command);
        },
        {"command"});
}

static void ConfigureHost(AgentSession& session)
{
    if (session.host_ready)
        return;

    session.host.should_abort = [&session]()
    {
        if (session.dbg && session.dbg->IsInterrupted())
            session.aborted = true;
        return session.aborted.load();
    };

    session.host.on_event = [&session](const libagents::Event& event)
    {
        if (!session.dbg)
            return;

        switch (event.type)
        {
        case libagents::EventType::ContentDelta:
            session.dbg->OutputThinking(event.content);
            break;
        case libagents::EventType::ContentComplete:
            session.dbg->Output("\n");
            session.dbg->OutputResponse(event.content.empty() ? "(No output)" : event.content);
            break;
        case libagents::EventType::Error:
            if (!event.error_message.empty())
                session.dbg->OutputError(event.error_message);
            else if (!event.content.empty())
                session.dbg->OutputError(event.content);
            else
                session.dbg->OutputError("Error");
            break;
        default:
            break;
        }
    };

    session.host_ready = true;
}

static bool EnsureAgent(AgentSession& session, windbg_agent::WinDbgClient& dbg_client,
                        const windbg_agent::Settings& settings, const std::string& target,
                        const windbg_agent::RuntimeContext& runtime_ctx, std::string* error,
                        bool* created)
{
    if (created)
        *created = false;

    session.dbg = &dbg_client;

    if (session.agent && session.provider != settings.default_provider)
        ResetAgentSession(session);

    if (!session.agent)
    {
        session.provider = settings.default_provider;
        session.provider_name = libagents::provider_type_name(session.provider);
        session.agent = libagents::create_agent(session.provider);
        if (!session.agent)
        {
            if (error)
                *error = "Failed to create agent";

            return false;
        }

        session.agent->register_tool(BuildDebuggerTool(session));

        session.system_prompt =
            windbg_agent::GetFullSystemPrompt(settings.custom_prompt, runtime_ctx);
        session.primed = false; // will prepend on first user query instead of system_prompt

        // Apply BYOK settings if enabled
        const auto* byok = settings.get_byok();
        if (byok && byok->is_usable())
            session.agent->set_byok(byok->to_config());

        // Apply response timeout setting
        if (settings.response_timeout_ms > 0)
            session.agent->set_response_timeout(
                std::chrono::milliseconds(settings.response_timeout_ms));

#if !WINDBG_AGENT_DISABLE_SESSIONS
        // Skip session resume when BYOK is enabled (not supported by BYOK providers)
        if (!(byok && byok->is_usable()))
        {
            session.session_id =
                windbg_agent::GetSessionStore().GetSessionId(target, session.provider_name);
            if (!session.session_id.empty())
                session.agent->set_session_id(session.session_id);
        }
#endif

        if (!session.agent->initialize())
        {
            if (error)
            {
                *error = "Failed to initialize: " + session.agent->provider_name();
                std::string last_error = session.agent->get_last_error();
                if (!last_error.empty())
                    *error += " - " + last_error;
            }

            ResetAgentSession(session);
            return false;
        }

        ConfigureHost(session);
        session.initialized = true;

        if (created)
            *created = true;
    }

    std::string updated_prompt =
        windbg_agent::GetFullSystemPrompt(settings.custom_prompt, runtime_ctx);
    if (updated_prompt != session.system_prompt)
    {
        session.system_prompt = updated_prompt;
        session.primed = false; // re-prime next turn with new prompt
    }

    if (session.target != target)
    {
        session.target = target;
#if !WINDBG_AGENT_DISABLE_SESSIONS
        // Skip session resume when BYOK is enabled (not supported by BYOK providers)
        const auto* byok_check = settings.get_byok();
        if (!(byok_check && byok_check->is_usable()))
        {
            std::string new_session_id =
                windbg_agent::GetSessionStore().GetSessionId(target, session.provider_name);
            if (new_session_id != session.session_id)
            {
                if (session.agent)
                {
                    session.agent->clear_session();
                    session.session_id = new_session_id;
                    if (!session.session_id.empty())
                        session.agent->set_session_id(session.session_id);
                }
            }
        }
#endif
        session.primed = false; // new target -> re-prime on next ask
    }

    session.aborted = false;
    return true;
}
} // namespace

// Extension entry point
extern "C" HRESULT CALLBACK DebugExtensionInitialize(PULONG Version, PULONG Flags)
{
    *Version = DEBUG_EXTENSION_VERSION(WINDBG_AGENT_VERSION_MAJOR, WINDBG_AGENT_VERSION_MINOR);
    *Flags = 0;
    return S_OK;
}

// Extension cleanup
extern "C" void CALLBACK DebugExtensionUninitialize()
{
    ResetAgentSession(GetAgentSession());
}

// Extension notification
extern "C" void CALLBACK DebugExtensionNotify(ULONG Notify, ULONG64 Argument)
{
    // Could handle session changes here if needed
}

// Implementation
HRESULT CALLBACK agent_impl(PDEBUG_CLIENT Client, PCSTR Args)
{
    IDebugControl* control = GetControl(Client);
    if (!control)
        return E_FAIL;

    // Parse subcommand
    std::string args_str = Args ? Args : "";

    // Trim leading whitespace
    size_t start = args_str.find_first_not_of(" \t");
    if (start != std::string::npos)
        args_str = args_str.substr(start);

    // Extract subcommand
    std::string subcmd;
    std::string rest;
    size_t space = args_str.find(' ');
    if (space != std::string::npos)
    {
        subcmd = args_str.substr(0, space);
        rest = args_str.substr(space + 1);
        // Trim leading whitespace from rest
        size_t rest_start = rest.find_first_not_of(" \t");
        if (rest_start != std::string::npos)
            rest = rest.substr(rest_start);
    }
    else
    {
        subcmd = args_str;
    }

    // Handle subcommands
    if (subcmd.empty() || subcmd == "help")
    {
        auto settings = windbg_agent::LoadSettings();
        const auto* byok = settings.get_byok();
        control->Output(
            DEBUG_OUTPUT_NORMAL,
            "WinDbg Agent - AI-powered debugger assistant\n"
            "\n"
            "Usage: !agent <command> [args]\n"
            "       !ai <question>          (shorthand for !agent ask)\n"
            "\n"
            "Commands:\n"
            "  help                  Show this help\n"
            "  version               Show version information\n"
            "  version prompt        Show injected system prompt\n"
            "  ask <question>        Ask the AI agent a question\n"
            "  clear                 Clear conversation history\n"
            "  provider              Show current provider\n"
            "  provider <name>       Switch provider (claude, copilot)\n"
            "  prompt                Show custom prompt\n"
            "  prompt <text>         Set custom prompt (additive)\n"
            "  prompt clear          Clear custom prompt\n"
            "  timeout               Show response timeout\n"
            "  timeout <ms>          Set response timeout (e.g., 120000 = 2 min)\n"
            "  http [bind_addr]      Start HTTP server for external tools (port auto-assigned)\n"
            "  mcp [bind_addr]       Start MCP server for MCP-compatible clients\n"
            "  byok                  Show BYOK (Bring Your Own Key) status\n"
            "  byok enable|disable   Enable or disable BYOK for current provider\n"
            "  byok key <value>      Set BYOK API key\n"
            "  byok endpoint <url>   Set BYOK API endpoint\n"
            "  byok type <type>      Set BYOK provider type (openai, anthropic, azure)\n"
            "  byok model <model>    Set BYOK model name\n"
            "\n"
            "Current provider: %s%s\n"
            "\n"
            "Examples:\n"
            "  !ai what is the call stack?           (quick query)\n"
            "  !ai and what about the registers?     (follow-up)\n"
            "  !agent provider claude                (switch to Claude)\n"
            "  !agent byok key sk-xxx                (set your API key)\n"
            "  !agent byok enable                    (use custom API key)\n",
            libagents::provider_type_name(settings.default_provider),
            (byok && byok->is_usable()) ? " (BYOK enabled)" : "");

        // Show current session context
        windbg_agent::WinDbgClient dbg_client(Client);
        auto ctx = GatherRuntimeContext(dbg_client);
        control->Output(DEBUG_OUTPUT_NORMAL, "Session context:\n");
        if (!ctx.target_name.empty())
            control->Output(DEBUG_OUTPUT_NORMAL, "  Target:       %s\n", ctx.target_name.c_str());
        if (!ctx.target_arch.empty())
            control->Output(DEBUG_OUTPUT_NORMAL, "  Architecture: %s\n", ctx.target_arch.c_str());
        if (!ctx.debugger_type.empty())
            control->Output(DEBUG_OUTPUT_NORMAL, "  Debugger:     %s\n", ctx.debugger_type.c_str());
        if (!ctx.cwd.empty())
            control->Output(DEBUG_OUTPUT_NORMAL, "  Working dir:  %s\n", ctx.cwd.c_str());
        if (!ctx.timestamp.empty())
            control->Output(DEBUG_OUTPUT_NORMAL, "  Timestamp:    %s\n", ctx.timestamp.c_str());
        control->Output(DEBUG_OUTPUT_NORMAL, "  Platform:     %s\n", ctx.platform.c_str());
    }
    else if (subcmd == "version")
    {
        auto settings = windbg_agent::LoadSettings();

        if (rest == "prompt")
        {
            // Show the system prompt
            control->Output(DEBUG_OUTPUT_NORMAL, "=== WinDbg Agent System Prompt ===\n\n");
            control->Output(DEBUG_OUTPUT_NORMAL, "%s\n", windbg_agent::kSystemPrompt);
            if (!settings.custom_prompt.empty())
            {
                control->Output(DEBUG_OUTPUT_NORMAL, "\n=== Custom Prompt (additive) ===\n\n");
                control->Output(DEBUG_OUTPUT_NORMAL, "%s\n", settings.custom_prompt.c_str());
            }
        }
        else
        {
            control->Output(DEBUG_OUTPUT_NORMAL, "WinDbg Agent v%d.%d.%d\n",
                            WINDBG_AGENT_VERSION_MAJOR, WINDBG_AGENT_VERSION_MINOR,
                            WINDBG_AGENT_VERSION_PATCH);
            control->Output(DEBUG_OUTPUT_NORMAL, "Current provider: %s\n",
                            libagents::provider_type_name(settings.default_provider));
            control->Output(DEBUG_OUTPUT_NORMAL, "\nUse '!agent version prompt' to see the injected system prompt.\n");
        }
    }
    else if (subcmd == "provider")
    {
        auto settings = windbg_agent::LoadSettings();

        if (rest.empty())
        {
            // Show current provider
            control->Output(DEBUG_OUTPUT_NORMAL, "Current provider: %s\n",
                            libagents::provider_type_name(settings.default_provider));
            control->Output(DEBUG_OUTPUT_NORMAL, "\nAvailable providers:\n");
            control->Output(DEBUG_OUTPUT_NORMAL, "  claude   - Claude Code (Anthropic)\n");
            control->Output(DEBUG_OUTPUT_NORMAL, "  copilot  - GitHub Copilot\n");
        }
        else
        {
            // Switch provider
            try
            {
                auto type = windbg_agent::ParseProviderType(rest);
                if (type != settings.default_provider)
                {
                    settings.default_provider = type;
                    windbg_agent::SaveSettings(settings);
                    ResetAgentSession(GetAgentSession());
                }
                control->Output(DEBUG_OUTPUT_NORMAL, "Provider set to: %s (saved to settings)\n",
                                libagents::provider_type_name(type));
            }
            catch (const std::exception& e)
            {
                control->Output(DEBUG_OUTPUT_ERROR, "Error: %s\n", e.what());
                control->Output(DEBUG_OUTPUT_NORMAL, "Available providers: claude, copilot\n");
            }
        }
    }
    else if (subcmd == "clear")
    {
        auto settings = windbg_agent::LoadSettings();
        windbg_agent::WinDbgClient dbg_client(Client);
        std::string target = dbg_client.GetTargetName();
        std::string provider_name = libagents::provider_type_name(settings.default_provider);

        auto& session = GetAgentSession();
        if (session.agent)
        {
            session.agent->clear_session();
            session.session_id.clear();
        }
        windbg_agent::GetSessionStore().ClearSession(target, provider_name);
        control->Output(DEBUG_OUTPUT_NORMAL,
                        "Conversation history cleared (new session for this target).\n");
    }
    else if (subcmd == "prompt")
    {
        auto settings = windbg_agent::LoadSettings();

        if (rest.empty())
        {
            if (settings.custom_prompt.empty())
            {
                control->Output(DEBUG_OUTPUT_NORMAL, "No custom prompt set.\n");
            }
            else
            {
                control->Output(DEBUG_OUTPUT_NORMAL, "Custom prompt:\n%s\n",
                                settings.custom_prompt.c_str());
            }
        }
        else if (rest == "clear")
        {
            settings.custom_prompt.clear();
            windbg_agent::SaveSettings(settings);
            auto& session = GetAgentSession();
            if (session.agent)
            {
                session.system_prompt = windbg_agent::GetFullSystemPrompt(settings.custom_prompt);
                session.primed = false; // re-prime next turn
            }
            control->Output(DEBUG_OUTPUT_NORMAL, "Custom prompt cleared.\n");
        }
        else
        {
            settings.custom_prompt = rest;
            windbg_agent::SaveSettings(settings);
            auto& session = GetAgentSession();
            if (session.agent)
            {
                session.system_prompt = windbg_agent::GetFullSystemPrompt(settings.custom_prompt);
                session.primed = false; // re-prime next turn
            }
            control->Output(DEBUG_OUTPUT_NORMAL, "Custom prompt set (saved to settings).\n");
        }
    }
    else if (subcmd == "timeout")
    {
        auto settings = windbg_agent::LoadSettings();

        if (rest.empty())
        {
            control->Output(DEBUG_OUTPUT_NORMAL, "Response timeout: %s\n",
                            FormatDuration(settings.response_timeout_ms).c_str());
        }
        else
        {
            try
            {
                int ms = std::stoi(rest);
                if (ms < 1000)
                {
                    control->Output(DEBUG_OUTPUT_ERROR,
                                    "Timeout must be at least 1000 ms (1 second).\n");
                }
                else
                {
                    settings.response_timeout_ms = ms;
                    windbg_agent::SaveSettings(settings);
                    auto& session = GetAgentSession();
                    if (session.agent)
                        session.agent->set_response_timeout(std::chrono::milliseconds(ms));
                    control->Output(DEBUG_OUTPUT_NORMAL, "Timeout set to %s.\n",
                                    FormatDuration(ms).c_str());
                }
            }
            catch (...)
            {
                control->Output(DEBUG_OUTPUT_ERROR, "Invalid timeout value. Use milliseconds.\n");
            }
        }
    }
    else if (subcmd == "byok")
    {
        auto settings = windbg_agent::LoadSettings();
        std::string provider_name = libagents::provider_type_name(settings.default_provider);

        // Parse BYOK subcommand
        std::string byok_subcmd;
        std::string byok_value;
        size_t byok_space = rest.find(' ');
        if (byok_space != std::string::npos)
        {
            byok_subcmd = rest.substr(0, byok_space);
            byok_value = rest.substr(byok_space + 1);
            // Trim leading whitespace
            size_t val_start = byok_value.find_first_not_of(" \t");
            if (val_start != std::string::npos)
                byok_value = byok_value.substr(val_start);
        }
        else
        {
            byok_subcmd = rest;
        }

        if (byok_subcmd.empty())
        {
            // Show BYOK status for current provider
            const auto* byok = settings.get_byok();
            control->Output(DEBUG_OUTPUT_NORMAL, "BYOK status for provider '%s':\n",
                            provider_name.c_str());
            if (byok)
            {
                control->Output(DEBUG_OUTPUT_NORMAL, "  Enabled:  %s\n",
                                byok->enabled ? "yes" : "no");
                control->Output(DEBUG_OUTPUT_NORMAL, "  API Key:  %s\n",
                                byok->api_key.empty() ? "(not set)" : "********");
                control->Output(DEBUG_OUTPUT_NORMAL, "  Endpoint: %s\n",
                                byok->base_url.empty() ? "(default)" : byok->base_url.c_str());
                control->Output(DEBUG_OUTPUT_NORMAL, "  Model:    %s\n",
                                byok->model.empty() ? "(default)" : byok->model.c_str());
                control->Output(DEBUG_OUTPUT_NORMAL, "  Type:     %s\n",
                                byok->provider_type.empty() ? "(default)"
                                                            : byok->provider_type.c_str());
                control->Output(DEBUG_OUTPUT_NORMAL, "  Usable:   %s\n",
                                byok->is_usable() ? "yes" : "no");
            }
            else
            {
                control->Output(DEBUG_OUTPUT_NORMAL, "  (not configured)\n");
            }
            control->Output(DEBUG_OUTPUT_NORMAL,
                            "\nUse '!agent byok <cmd>' where cmd is:\n"
                            "  enable|disable  - Enable or disable BYOK\n"
                            "  key <value>     - Set API key\n"
                            "  endpoint <url>  - Set API endpoint\n"
                            "  model <name>    - Set model name\n"
                            "  type <type>     - Set provider type (openai, anthropic, azure)\n");
        }
        else if (byok_subcmd == "enable")
        {
            auto& byok = settings.get_or_create_byok();
            byok.enabled = true;
            windbg_agent::SaveSettings(settings);
            ResetAgentSession(GetAgentSession());
            control->Output(DEBUG_OUTPUT_NORMAL, "BYOK enabled for provider '%s'.\n",
                            provider_name.c_str());
            if (byok.api_key.empty())
            {
                control->Output(
                    DEBUG_OUTPUT_WARNING,
                    "Warning: API key not set. Use '!agent byok key <value>' to set it.\n");
            }
        }
        else if (byok_subcmd == "disable")
        {
            auto& byok = settings.get_or_create_byok();
            byok.enabled = false;
            windbg_agent::SaveSettings(settings);
            ResetAgentSession(GetAgentSession());
            control->Output(DEBUG_OUTPUT_NORMAL, "BYOK disabled for provider '%s'.\n",
                            provider_name.c_str());
        }
        else if (byok_subcmd == "key")
        {
            if (byok_value.empty())
            {
                control->Output(DEBUG_OUTPUT_ERROR, "Error: API key value required.\n");
                control->Output(DEBUG_OUTPUT_NORMAL, "Usage: !agent byok key <value>\n");
            }
            else
            {
                auto& byok = settings.get_or_create_byok();
                byok.api_key = byok_value;
                windbg_agent::SaveSettings(settings);
                ResetAgentSession(GetAgentSession());
                control->Output(DEBUG_OUTPUT_NORMAL, "BYOK API key set for provider '%s'.\n",
                                provider_name.c_str());
            }
        }
        else if (byok_subcmd == "endpoint")
        {
            auto& byok = settings.get_or_create_byok();
            byok.base_url = byok_value; // Empty clears it
            windbg_agent::SaveSettings(settings);
            ResetAgentSession(GetAgentSession());
            if (byok_value.empty())
            {
                control->Output(DEBUG_OUTPUT_NORMAL, "BYOK endpoint cleared (using default).\n");
            }
            else
            {
                control->Output(DEBUG_OUTPUT_NORMAL, "BYOK endpoint set to: %s\n",
                                byok_value.c_str());
            }
        }
        else if (byok_subcmd == "model")
        {
            auto& byok = settings.get_or_create_byok();
            byok.model = byok_value; // Empty clears it
            windbg_agent::SaveSettings(settings);
            ResetAgentSession(GetAgentSession());
            if (byok_value.empty())
                control->Output(DEBUG_OUTPUT_NORMAL, "BYOK model cleared (using default).\n");
            else
                control->Output(DEBUG_OUTPUT_NORMAL, "BYOK model set to: %s\n", byok_value.c_str());
        }
        else if (byok_subcmd == "type")
        {
            auto& byok = settings.get_or_create_byok();
            byok.provider_type = byok_value; // Empty clears it
            windbg_agent::SaveSettings(settings);
            ResetAgentSession(GetAgentSession());
            if (byok_value.empty())
                control->Output(DEBUG_OUTPUT_NORMAL, "BYOK type cleared (using default).\n");
            else
                control->Output(DEBUG_OUTPUT_NORMAL, "BYOK type set to: %s\n", byok_value.c_str());
        }
        else
        {
            control->Output(DEBUG_OUTPUT_ERROR, "Unknown byok subcommand: %s\n",
                            byok_subcmd.c_str());
            control->Output(DEBUG_OUTPUT_NORMAL, "Use '!agent byok' to see available commands.\n");
        }
    }
    else if (subcmd == "http")
    {
        // Start HTTP server for external tool integration
        // Usage: !agent http [bind_addr]
        // bind_addr: "127.0.0.1" (default, localhost only) or "0.0.0.0" (all interfaces)
        windbg_agent::WinDbgClient dbg_client(Client);
        auto settings = windbg_agent::LoadSettings();
        auto& session = GetAgentSession();
        std::string target = dbg_client.GetTargetName();

        // Parse optional bind address
        std::string bind_addr = "127.0.0.1";
        if (!rest.empty())
        {
            bind_addr = rest;
            // Trim whitespace
            size_t start = bind_addr.find_first_not_of(" \t");
            size_t end = bind_addr.find_last_not_of(" \t");
            if (start != std::string::npos)
                bind_addr = bind_addr.substr(start, end - start + 1);
        }

        if (bind_addr != "127.0.0.1")
        {
            control->Output(DEBUG_OUTPUT_WARNING,
                "WARNING: Binding to non-loopback address '%s'. "
                "The server has no authentication.\n", bind_addr.c_str());
        }

        // Get target state
        std::string state = dbg_client.GetTargetState();
        ULONG pid = dbg_client.GetProcessId();

        // Create exec callback - executes debugger commands
        windbg_agent::ExecCallback exec_cb = [&dbg_client](const std::string& command) -> std::string
        {
            return dbg_client.ExecuteCommand(command);
        };

        // Create ask callback - routes through same AI path as !agent ask
        windbg_agent::AskCallback ask_cb = [Client, &settings, &session, &dbg_client,
                                              &target](const std::string& query) -> std::string
        {
            auto runtime_ctx = GatherRuntimeContext(dbg_client);
            std::string error;
            bool created = false;
            if (!EnsureAgent(session, dbg_client, settings, target, runtime_ctx, &error, &created))
            {
                return error.empty() ? "Failed to initialize agent" : error;
            }

            try
            {
                std::string message =
                    session.primed || session.system_prompt.empty()
                        ? query
                        : (session.system_prompt + "\n\n---\n\n" + query);

                std::string response = session.agent->query_hosted(message, session.host);
                session.primed = true;

#if !WINDBG_AGENT_DISABLE_SESSIONS
                const auto* byok_save = settings.get_byok();
                if (!(byok_save && byok_save->is_usable()))
                {
                    std::string new_session_id = session.agent->get_session_id();
                    std::string provider_name =
                        libagents::provider_type_name(settings.default_provider);
                    if (!new_session_id.empty() && new_session_id != session.session_id)
                    {
                        windbg_agent::GetSessionStore().SetSessionId(target, provider_name,
                                                                       new_session_id);
                        session.session_id = new_session_id;
                    }
                }
#endif
                return response;
            }
            catch (const std::exception& e)
            {
                return std::string("Error: ") + e.what();
            }
        };

        // Start the HTTP server (OS assigns port)
        static windbg_agent::HttpServer http_server;
        if (http_server.is_running())
        {
            control->Output(DEBUG_OUTPUT_ERROR,
                            "HTTP server already running. Stop it before starting a new one.\n");
            control->Release();
            return E_FAIL;
        }
        int actual_port = http_server.start(exec_cb, ask_cb, bind_addr);
        if (actual_port <= 0)
        {
            control->Output(DEBUG_OUTPUT_ERROR,
                            "Failed to start HTTP server.\n");
            control->Release();
            return E_FAIL;
        }
        std::string url = "http://" + http_server.bind_addr() + ":" + std::to_string(http_server.port());

        // Format and output HTTP server info
        std::string http_info =
            windbg_agent::format_http_info(target, pid, state, url);
        control->Output(DEBUG_OUTPUT_NORMAL, "%s\n", http_info.c_str());

        // Copy to clipboard
        if (windbg_agent::copy_to_clipboard(http_info))
        {
            control->Output(DEBUG_OUTPUT_NORMAL, "[Copied to clipboard]\n");
        }

        control->Output(DEBUG_OUTPUT_NORMAL, "Press Ctrl+C to stop HTTP server.\n");

        // Set up interrupt check - stop server when user presses Ctrl+C
        http_server.set_interrupt_check([&dbg_client]() {
            return dbg_client.IsInterrupted();
        });

        // Block until server stops (user presses Ctrl+C or sends /shutdown)
        http_server.wait();
        control->Output(DEBUG_OUTPUT_NORMAL, "HTTP server stopped.\n");
    }
    else if (subcmd == "mcp")
    {
        // Start MCP server for MCP-compatible clients
        // Usage: !agent mcp [bind_addr]
        // bind_addr: "127.0.0.1" (default, localhost only) or "0.0.0.0" (all interfaces)
        windbg_agent::WinDbgClient dbg_client(Client);
        auto settings = windbg_agent::LoadSettings();
        auto& session = GetAgentSession();
        std::string target = dbg_client.GetTargetName();

        // Parse optional bind address
        std::string bind_addr = "127.0.0.1";
        if (!rest.empty())
        {
            bind_addr = rest;
            size_t start = bind_addr.find_first_not_of(" \t");
            size_t end = bind_addr.find_last_not_of(" \t");
            if (start != std::string::npos)
                bind_addr = bind_addr.substr(start, end - start + 1);
        }

        if (bind_addr != "127.0.0.1")
        {
            control->Output(DEBUG_OUTPUT_WARNING,
                "WARNING: Binding to non-loopback address '%s'. "
                "The server has no authentication.\n", bind_addr.c_str());
        }

        // Port 0 lets the MCP server pick a free port
        int port = 0;
        std::string url;

        // Get target state
        std::string state = dbg_client.GetTargetState();
        ULONG pid = dbg_client.GetProcessId();

        // Create exec callback - executes debugger commands
        windbg_agent::ExecCallback exec_cb = [&dbg_client](const std::string& command) -> std::string
        {
            return dbg_client.ExecuteCommand(command);
        };

        // Create ask callback - routes through same AI path as !agent ask
        windbg_agent::AskCallback ask_cb = [Client, &settings, &session, &dbg_client,
                                              &target](const std::string& query) -> std::string
        {
            auto runtime_ctx = GatherRuntimeContext(dbg_client);
            std::string error;
            bool created = false;
            if (!EnsureAgent(session, dbg_client, settings, target, runtime_ctx, &error, &created))
            {
                return error.empty() ? "Failed to initialize agent" : error;
            }

            try
            {
                std::string message =
                    session.primed || session.system_prompt.empty()
                        ? query
                        : (session.system_prompt + "\n\n---\n\n" + query);

                std::string response = session.agent->query_hosted(message, session.host);
                session.primed = true;

#if !WINDBG_AGENT_DISABLE_SESSIONS
                const auto* byok_save = settings.get_byok();
                if (!(byok_save && byok_save->is_usable()))
                {
                    std::string new_session_id = session.agent->get_session_id();
                    std::string provider_name =
                        libagents::provider_type_name(settings.default_provider);
                    if (!new_session_id.empty() && new_session_id != session.session_id)
                    {
                        windbg_agent::GetSessionStore().SetSessionId(target, provider_name,
                                                                       new_session_id);
                        session.session_id = new_session_id;
                    }
                }
#endif
                return response;
            }
            catch (const std::exception& e)
            {
                return std::string("Error: ") + e.what();
            }
        };

        // Start the MCP server
        static windbg_agent::MCPServer mcp_server;
        if (mcp_server.is_running())
        {
            control->Output(DEBUG_OUTPUT_ERROR,
                            "MCP server already running. Stop it before starting a new one.\n");
            control->Release();
            return E_FAIL;
        }
        int actual_port = mcp_server.start(port, exec_cb, ask_cb, bind_addr);
        if (actual_port <= 0)
        {
            control->Output(DEBUG_OUTPUT_ERROR,
                            "Failed to start MCP server.\n");
            control->Release();
            return E_FAIL;
        }
        url = "http://" + bind_addr + ":" + std::to_string(actual_port);

        // Format and output MCP server info
        std::string mcp_info =
            windbg_agent::format_mcp_info(target, pid, state, url);
        control->Output(DEBUG_OUTPUT_NORMAL, "%s\n", mcp_info.c_str());

        // Copy to clipboard
        if (windbg_agent::copy_to_clipboard(mcp_info))
        {
            control->Output(DEBUG_OUTPUT_NORMAL, "[Copied to clipboard]\n");
        }

        control->Output(DEBUG_OUTPUT_NORMAL, "Press Ctrl+C to stop MCP server.\n");

        // Set up interrupt check - stop MCP server when user presses Ctrl+C
        mcp_server.set_interrupt_check([&dbg_client]() {
            return dbg_client.IsInterrupted();
        });

        // Block until server stops (user presses Ctrl+C)
        mcp_server.wait();
        control->Output(DEBUG_OUTPUT_NORMAL, "MCP server stopped.\n");
    }
    else if (subcmd == "ask")
    {
        if (rest.empty())
        {
            control->Output(DEBUG_OUTPUT_ERROR, "Error: No question provided\n");
            control->Output(DEBUG_OUTPUT_NORMAL, "Usage: !agent ask <question>\n");
        }
        else
        {
            windbg_agent::WinDbgClient dbg_client(Client);
            auto settings = windbg_agent::LoadSettings();
            auto& session = GetAgentSession();
            std::string target = dbg_client.GetTargetName();
            auto runtime_ctx = GatherRuntimeContext(dbg_client);

            std::string error;
            bool created = false;
            if (!EnsureAgent(session, dbg_client, settings, target, runtime_ctx, &error, &created))
            {
                dbg_client.OutputError(error.empty() ? "Failed to initialize agent" : error);
                control->Release();
                return E_FAIL;
            }

            std::string provider_name = libagents::provider_type_name(settings.default_provider);
            dbg_client.OutputThinking("[" + provider_name + "] Asking: " + rest);
            if (created)
                dbg_client.OutputThinking("Initializing " + provider_name + " provider...");

            try
            {
                std::string message =
                    session.primed || session.system_prompt.empty()
                        ? rest
                        : (session.system_prompt + "\n\n---\n\n" + rest);

                std::string response = session.agent->query_hosted(message, session.host);
                session.primed = true;
                if (response == "(Aborted)")
                    dbg_client.OutputWarning("Aborted.");

#if !WINDBG_AGENT_DISABLE_SESSIONS
                // Skip session persistence when BYOK is enabled (not supported by BYOK providers)
                const auto* byok_save = settings.get_byok();
                if (!(byok_save && byok_save->is_usable()))
                {
                    std::string new_session_id = session.agent->get_session_id();
                    if (!new_session_id.empty() && new_session_id != session.session_id)
                    {
                        windbg_agent::GetSessionStore().SetSessionId(target, provider_name,
                                                                       new_session_id);
                        session.session_id = new_session_id;
                    }
                }
#endif
            }
            catch (const std::exception& e)
            {
                dbg_client.OutputError(e.what());
            }
        }
    }
    else
    {
        control->Output(DEBUG_OUTPUT_ERROR, "Unknown subcommand: %s\n", subcmd.c_str());
        control->Output(DEBUG_OUTPUT_NORMAL, "Use '!agent help' for usage information.\n");
    }

    control->Release();
    return S_OK;
}

// !agent command - main entry point
extern "C" HRESULT CALLBACK agent(PDEBUG_CLIENT Client, PCSTR Args)
{
    return agent_impl(Client, Args);
}

// !ai command - shorthand for "!agent ask"
extern "C" HRESULT CALLBACK ai(PDEBUG_CLIENT Client, PCSTR Args)
{
    // Prepend "ask " to make it equivalent to "!agent ask <args>"
    std::string full_args = "ask ";
    if (Args && *Args)
        full_args += Args;
    return agent_impl(Client, full_args.c_str());
}
