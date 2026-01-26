#pragma once

#include <dbgeng.h>
#include <string>
#include <windows.h>

namespace windbg_agent
{

// DML (Debugger Markup Language) output helper
// Provides colored output that works in WinDbg and degrades gracefully in CDB
class DmlOutput
{
  public:
    explicit DmlOutput(IDebugControl* control);

    // Check if DML is preferred/supported
    bool IsDmlSupported() const { return dml_supported_; }

    // Output with specific colors (uses DML fg color names)
    // Colors: empfg (emphasis/blue), errfg (error/red), warnfg (warning),
    //         subfg (subdued/gray), changed (red for changes)
    void OutputColored(const char* color, const char* format, ...);

    // Convenience methods for common message types
    void OutputCommand(const char* cmd);          // Command being run (subdued)
    void OutputCommandResult(const char* result); // Command output (normal)
    void OutputAgentThinking(const char* msg);    // Agent status (emphasis)
    void OutputAgentResponse(const char* msg);    // Agent response (normal)
    void OutputError(const char* msg);            // Error messages (red)
    void OutputWarning(const char* msg);          // Warning messages (yellow)

    // Raw output (no DML)
    void Output(const char* format, ...);

  private:
    IDebugControl* control_;
    bool dml_supported_ = false;

    // Escape special characters for DML
    static std::string EscapeDml(const std::string& text);
};

} // namespace windbg_agent
