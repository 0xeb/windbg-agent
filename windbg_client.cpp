#include "windbg_client.hpp"
#include "output_capture.hpp"
#include <wrl/client.h>

namespace windbg_copilot
{

WinDbgClient::WinDbgClient(IDebugClient* client) : client_(client), control_(nullptr)
{
    if (client_)
    {
        client_->QueryInterface(__uuidof(IDebugControl), (void**)&control_);
        if (control_)
            dml_ = std::make_unique<DmlOutput>(control_);
    }
}

WinDbgClient::~WinDbgClient()
{
    if (control_)
    {
        control_->Release();
        control_ = nullptr;
    }
}

std::string WinDbgClient::ExecuteCommand(const std::string& command)
{
    if (!control_ || !client_)
        return "Error: No debugger control available";

    // Show user what command is being executed
    OutputCommand(command);

    // Install output capture
    OutputCapture capture;
    capture.Install(client_);

    // Execute the command
    HRESULT hr =
        control_->Execute(DEBUG_OUTCTL_THIS_CLIENT, command.c_str(), DEBUG_EXECUTE_DEFAULT);

    // Get captured output
    std::string result = capture.GetAndClear();
    capture.Uninstall();

    if (FAILED(hr))
    {
        result = "Error executing command: hr=" + std::to_string(hr);
        OutputError(result);
    }
    else if (result.empty())
    {
        result = "(No output)";
    }
    else
    {
        // Show the command output to the user
        OutputCommandResult(result);
    }

    return result;
}

void WinDbgClient::Output(const std::string& message)
{
    if (control_)
        control_->Output(DEBUG_OUTPUT_NORMAL, "%s", message.c_str());
}

void WinDbgClient::OutputError(const std::string& message)
{
    if (dml_)
        dml_->OutputError(message.c_str());
    else if (control_)
        control_->Output(DEBUG_OUTPUT_ERROR, "%s\n", message.c_str());
}

void WinDbgClient::OutputWarning(const std::string& message)
{
    if (dml_)
        dml_->OutputWarning(message.c_str());
    else if (control_)
        control_->Output(DEBUG_OUTPUT_WARNING, "%s\n", message.c_str());
}

void WinDbgClient::OutputCommand(const std::string& command)
{
    if (dml_)
        dml_->OutputCommand(command.c_str());
    else if (control_)
        control_->Output(DEBUG_OUTPUT_NORMAL, "$ %s\n", command.c_str());
}

void WinDbgClient::OutputCommandResult(const std::string& result)
{
    if (dml_)
        dml_->OutputCommandResult(result.c_str());
    else if (control_)
        control_->Output(DEBUG_OUTPUT_NORMAL, "%s\n", result.c_str());
}

void WinDbgClient::OutputThinking(const std::string& message)
{
    if (dml_)
        dml_->OutputAgentThinking(message.c_str());
    else if (control_)
        control_->Output(DEBUG_OUTPUT_NORMAL, "%s\n", message.c_str());
}

void WinDbgClient::OutputResponse(const std::string& response)
{
    if (dml_)
        dml_->OutputAgentResponse(response.c_str());
    else if (control_)
        control_->Output(DEBUG_OUTPUT_NORMAL, "%s\n", response.c_str());
}

bool WinDbgClient::SupportsColor() const
{
    return dml_ && dml_->IsDmlSupported();
}

std::string WinDbgClient::GetTargetName() const
{
    if (!client_)
        return "";

    // Try to get dump file name first
    char dump_file[MAX_PATH] = {0};
    ULONG dump_file_size = 0;

    Microsoft::WRL::ComPtr<IDebugClient4> client4;
    if (SUCCEEDED(client_->QueryInterface(__uuidof(IDebugClient4),
                                          reinterpret_cast<void**>(client4.GetAddressOf()))))
    {
        // GetDumpFile returns the dump file name if debugging a dump
        // Note: Handle and Type must not be nullptr - the API writes to them
        ULONG64 handle = 0;
        ULONG type = 0;
        HRESULT hr =
            client4->GetDumpFile(0, dump_file, sizeof(dump_file), &dump_file_size, &handle, &type);

        if (SUCCEEDED(hr) && dump_file[0] != '\0')
            return dump_file;
    }

    // Fall back to getting process name via system objects
    Microsoft::WRL::ComPtr<IDebugSystemObjects> sys;
    if (SUCCEEDED(client_->QueryInterface(__uuidof(IDebugSystemObjects),
                                          reinterpret_cast<void**>(sys.GetAddressOf()))))
    {
        char exe_name[MAX_PATH] = {0};
        ULONG exe_size = 0;
        HRESULT hr = sys->GetCurrentProcessExecutableName(exe_name, sizeof(exe_name), &exe_size);
        if (SUCCEEDED(hr) && exe_name[0] != '\0')
            return exe_name;
    }

    return "";
}

bool WinDbgClient::IsInterrupted() const
{
    if (!control_)
        return false;

    // Check if user pressed Ctrl+C or Ctrl+Break
    HRESULT hr = control_->GetInterrupt();
    return hr == S_OK;
}

} // namespace windbg_copilot
