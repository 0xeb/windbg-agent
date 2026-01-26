#include "windbg_client.hpp"
#include "output_capture.hpp"
#include <cctype>
#include <wrl/client.h>

namespace windbg_agent
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

std::string WinDbgClient::GetTargetArchitecture() const
{
    if (!control_)
        return "";

    ULONG proc_type = 0;
    if (SUCCEEDED(control_->GetActualProcessorType(&proc_type)))
    {
        switch (proc_type)
        {
        case IMAGE_FILE_MACHINE_I386:
            return "x86";
        case IMAGE_FILE_MACHINE_AMD64:
            return "x64";
        case IMAGE_FILE_MACHINE_ARM64:
            return "ARM64";
        case IMAGE_FILE_MACHINE_ARM:
        case IMAGE_FILE_MACHINE_ARMNT:
            return "ARM";
        default:
            return "Unknown (" + std::to_string(proc_type) + ")";
        }
    }
    return "";
}

std::string WinDbgClient::GetDebuggerType() const
{
    // Detect debugger by examining host process name
    char module_path[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, module_path, MAX_PATH))
    {
        std::string path = module_path;
        // Convert to lowercase for comparison
        for (auto& c : path)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

        if (path.find("dbgx") != std::string::npos || path.find("windbg") != std::string::npos)
            return "WinDbg";
        if (path.find("cdb") != std::string::npos)
            return "CDB";
        if (path.find("ntsd") != std::string::npos)
            return "NTSD";
        if (path.find("kd") != std::string::npos)
            return "KD";
    }
    return "Windows Debugger";
}

bool WinDbgClient::IsInterrupted() const
{
    if (!control_)
        return false;

    // Check if user pressed Ctrl+C or Ctrl+Break
    HRESULT hr = control_->GetInterrupt();
    return hr == S_OK;
}

std::string WinDbgClient::GetTargetState() const
{
    if (!control_)
        return "Unknown";

    ULONG status = 0;
    HRESULT hr = control_->GetExecutionStatus(&status);
    if (FAILED(hr))
        return "Unknown";

    switch (status)
    {
    case DEBUG_STATUS_NO_DEBUGGEE:
        return "No target";
    case DEBUG_STATUS_STEP_INTO:
    case DEBUG_STATUS_STEP_OVER:
    case DEBUG_STATUS_STEP_BRANCH:
        return "Stepping";
    case DEBUG_STATUS_GO:
    case DEBUG_STATUS_GO_HANDLED:
    case DEBUG_STATUS_GO_NOT_HANDLED:
        return "Running";
    case DEBUG_STATUS_BREAK:
        return "Break";
    case DEBUG_STATUS_OUT_OF_SYNC:
        return "Out of sync";
    case DEBUG_STATUS_WAIT_INPUT:
        return "Waiting for input";
    case DEBUG_STATUS_TIMEOUT:
        return "Timeout";
    default:
        return "Unknown";
    }
}

ULONG WinDbgClient::GetProcessId() const
{
    if (!client_)
        return 0;

    Microsoft::WRL::ComPtr<IDebugSystemObjects> sys;
    if (SUCCEEDED(client_->QueryInterface(__uuidof(IDebugSystemObjects),
                                          reinterpret_cast<void**>(sys.GetAddressOf()))))
    {
        ULONG pid = 0;
        if (SUCCEEDED(sys->GetCurrentProcessSystemId(&pid)))
            return pid;
    }
    return 0;
}

} // namespace windbg_agent
