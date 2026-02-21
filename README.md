# WinDbg Agent

AI-powered debugging assistant for WinDbg. Ask questions about your debug session and get intelligent answers with automatic debugger command execution.

Supports multiple AI providers:
- **GitHub Copilot** - via [copilot-sdk-cpp](https://github.com/0xeb/copilot-sdk-cpp)
- **Claude** (Anthropic) - via [claude-agent-sdk-cpp](https://github.com/0xeb/claude-agent-sdk-cpp)

## Building

### Prerequisites
- Windows 10/11
- Visual Studio 2022 with C++ workload (2019 also works — [see below](#alternative-no-visual-studio-2022))
- CMake 3.20+
- Windows SDK (for dbgeng.h)

### Build Steps

```bash
# Clone with submodules
git clone --recursive https://github.com/0xeb/windbg-agent.git
cd windbg-agent

# Build for x64 (most common)
cmake --preset x64
cmake --build --preset x64

# Build for x86 (32-bit targets)
cmake --preset x86
cmake --build --preset x86
```

Output:
- **x64**: `build-x64/Release/windbg_agent.dll`
- **x86**: `build-x86/Release/windbg_agent.dll`

### Alternative: No Visual Studio 2022

If you have a different Visual Studio version or just the Build Tools, skip the presets and specify the generator manually:

```bash
# Visual Studio 2019 — x64
cmake -B build-x64 -G "Visual Studio 16 2019" -A x64
cmake --build build-x64 --config Release

# Visual Studio 2019 — x86 (32-bit targets)
cmake -B build-x86 -G "Visual Studio 16 2019" -A Win32
cmake --build build-x86 --config Release

# Ninja (works with any MSVC version — run from Developer Command Prompt)
cmake -B build-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-x64
```

> **Ninja x86 note**: For 32-bit Ninja builds, open the **x86 Native Tools Command Prompt** (instead of x64) so `cl.exe` targets Win32.

## Usage

### Loading the Extension

1. Open WinDbg and load a dump file or attach to a process
2. Load the extension:

```
.load C:\path\to\windbg_agent.dll
```

> **Note:** The extension DLL architecture must match the target. Use the x86 build when debugging 32-bit processes, and the x64 build for 64-bit processes.

### Commands

| Command | Description |
|---------|-------------|
| `!agent help` | Show help |
| `!agent version` | Show version and current provider |
| `!agent provider` | Show current provider |
| `!agent provider <name>` | Switch provider (claude, copilot) |
| `!agent ask <question>` | Ask the AI a question |
| `!agent clear` | Clear conversation history |
| `!agent prompt` | Show current custom prompt |
| `!agent prompt <text>` | Set custom prompt (appended to system prompt) |
| `!agent prompt clear` | Clear custom prompt |
| `!agent http [bind_addr]` | Start HTTP server for external tools (port auto-assigned) |
| `!agent mcp [bind_addr]` | Start MCP server for MCP-compatible clients |
| `!agent version prompt` | Show injected system prompt |
| `!ai <question>` | Shorthand for `!agent ask` |

### Examples

```
# Ask about the current state
!ai what is the call stack?
!ai what is rax + rbx?

# Run commands directly - AI executes and explains
!ai db @rip L20
!ai !peb

# Decompilation
!ai decompile ntdll!RtlAllocateHeap

# Follow-up questions (uses conversation history)
!ai what about the registers?

# Ask for analysis
!ai explain this crash

# Switch to Claude provider
!agent provider claude

# Set a custom prompt for your debugging session
!agent prompt Focus on memory corruption and heap issues

# Clear history and start fresh
!agent clear
```

### HTTP Server (External Tool Integration)

Let external AI agents control the debugger via HTTP:

```bash
# In WinDbg - start the HTTP server
!agent http                  # localhost only (default)
!agent http 0.0.0.0          # all interfaces (no auth warning)

# From another terminal - use the CLI tool (use the URL printed by !agent http)
windbg_agent.exe --url=http://127.0.0.1:<port> ask "what caused this crash?"
windbg_agent.exe --url=http://127.0.0.1:<port> exec "kb"
windbg_agent.exe --url=http://127.0.0.1:<port> interactive
windbg_agent.exe --url=http://127.0.0.1:<port> status
windbg_agent.exe --url=http://127.0.0.1:<port> shutdown
```

Settings are saved in `%USERPROFILE%\.windbg_agent\settings.json`.

## Features

- **Direct command execution**: Pass debugger commands directly (`!ai db @rsp L10`) - AI runs and explains
- **Expression evaluation**: Uses `?`, `??`, `dx` for calculations instead of guessing
- **Decompilation**: Ask to decompile functions - AI uses `uf`, `dv`, `dt` to generate pseudocode
- **Automatic tool execution**: AI runs debugger commands to gather information
- **Conversation continuity**: Follow-up questions remember context
- **Session persistence**: Claude restores sessions across debugger restarts
- **Multiple providers**: Switch between Claude and Copilot
- **HTTP Server**: Let external AI agents (like Copilot or Claude Code) control the debugger

## Screenshots

### Debugging a Double-Free Bug

AI analyzes a heap corruption crash, identifies the root cause, and offers next steps:

![Crash analysis](assets/crash_analysis.jpg)

User asks for decompilation — AI generates readable pseudocode from assembly:

![Decompilation](assets/decompile.jpg)

### HTTP Server: External Tool Integration

Start `!agent http` to let external tools control WinDbg. The CLI executes commands remotely:

![Handoff with CLI exec](assets/handoff_exec.jpg)

Claude Code (Opus 4.5) controlling WinDbg via HTTP server — analyzing a double-free crash:

![Claude Code controlling WinDbg](assets/handoff_claude_code.jpg)

## Author

Elias Bachaalany ([@0xeb](https://github.com/0xeb))

Pair-programmed with Claude Code and Codex.

## License

MIT
