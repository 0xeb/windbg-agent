#pragma once

#include <dbgeng.h>
#include <string>
#include <windows.h>

namespace windbg_copilot
{

// Captures debugger output while optionally forwarding to original callbacks
class OutputCapture : public IDebugOutputCallbacks
{
  public:
    OutputCapture();
    ~OutputCapture();

    // Install this capture on the given client, saving the original callbacks
    HRESULT Install(IDebugClient* client);

    // Uninstall and restore original callbacks
    HRESULT Uninstall();

    // Get the captured output and clear the buffer
    std::string GetAndClear();

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID InterfaceId, PVOID* Interface) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // IDebugOutputCallbacks
    STDMETHOD(Output)(ULONG Mask, PCSTR Text) override;

  private:
    LONG ref_count_;
    IDebugClient* client_;
    IDebugOutputCallbacks* original_callbacks_;
    std::string captured_output_;
};

} // namespace windbg_copilot
