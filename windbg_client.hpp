#pragma once

#include "dml_output.hpp"
#include <dbgeng.h>
#include <memory>
#include <string>
#include <windows.h>

namespace windbg_agent
{

// WinDbg/CDB debugger client using dbgeng interfaces and DML for colored output
class WinDbgClient
{
  public:
    // Construct with an IDebugClient (typically from extension callback)
    explicit WinDbgClient(IDebugClient* client);
    ~WinDbgClient();

    // Execute a debugger command and return its output
    std::string ExecuteCommand(const std::string& command);

    // Output methods for displaying messages to the user
    void Output(const std::string& message);
    void OutputError(const std::string& message);
    void OutputWarning(const std::string& message);

    // Styled output for agent interactions
    void OutputCommand(const std::string& command);
    void OutputCommandResult(const std::string& result);
    void OutputThinking(const std::string& message);
    void OutputResponse(const std::string& response);

    // Query capabilities
    bool SupportsColor() const;

    // Get target info (dump file path or process name)
    std::string GetTargetName() const;

    // Get target architecture (x86, x64, ARM64, etc.)
    std::string GetTargetArchitecture() const;

    // Get debugger type (WinDbg, CDB, etc.)
    std::string GetDebuggerType() const;

    // Get debugger execution state as a human-readable string
    std::string GetTargetState() const;

    // Get the current process ID (0 if not available)
    ULONG GetProcessId() const;

    // Check if user requested interrupt (e.g., Ctrl+C)
    bool IsInterrupted() const;

  private:
    IDebugClient* client_;
    IDebugControl* control_;
    std::unique_ptr<DmlOutput> dml_;
};

} // namespace windbg_agent
