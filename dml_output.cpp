#include "dml_output.hpp"
#include <cstdarg>
#include <vector>

namespace windbg_agent
{

DmlOutput::DmlOutput(IDebugControl* control) : control_(control)
{
    if (!control_)
        return;

    // Check if DML is preferred via engine options
    ULONG options = 0;
    if (SUCCEEDED(control_->GetEngineOptions(&options)))
    {
        // DEBUG_ENGOPT_PREFER_DML = 0x00040000
        dml_supported_ = (options & 0x00040000) != 0;
    }
}

std::string DmlOutput::EscapeDml(const std::string& text)
{
    std::string result;
    result.reserve(text.size() * 1.1);

    for (char c : text)
    {
        switch (c)
        {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        default:
            result += c;
            break;
        }
    }
    return result;
}

void DmlOutput::Output(const char* format, ...)
{
    if (!control_)
        return;

    va_list args;
    va_start(args, format);

    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), format, args);
    control_->Output(DEBUG_OUTPUT_NORMAL, "%s", buffer);

    va_end(args);
}

void DmlOutput::OutputColored(const char* color, const char* format, ...)
{
    if (!control_)
        return;

    va_list args;
    va_start(args, format);

    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), format, args);

    if (dml_supported_)
    {
        std::string escaped = EscapeDml(buffer);
        control_->ControlledOutput(DEBUG_OUTCTL_AMBIENT_DML, DEBUG_OUTPUT_NORMAL,
                                   "<col fg=\"%s\">%s</col>", color, escaped.c_str());
    }
    else
    {
        control_->Output(DEBUG_OUTPUT_NORMAL, "%s", buffer);
    }

    va_end(args);
}

void DmlOutput::OutputCommand(const char* cmd)
{
    if (!control_)
        return;

    if (dml_supported_)
    {
        std::string escaped = EscapeDml(cmd);
        control_->ControlledOutput(DEBUG_OUTCTL_AMBIENT_DML, DEBUG_OUTPUT_NORMAL,
                                   "<col fg=\"subfg\">$ %s</col>\n", escaped.c_str());
    }
    else
    {
        control_->Output(DEBUG_OUTPUT_NORMAL, "$ %s\n", cmd);
    }
}

void DmlOutput::OutputCommandResult(const char* result)
{
    if (!control_)
        return;

    if (dml_supported_)
    {
        std::string escaped = EscapeDml(result);
        control_->ControlledOutput(DEBUG_OUTCTL_AMBIENT_DML, DEBUG_OUTPUT_NORMAL,
                                   "<col fg=\"subfg\">%s</col>\n", escaped.c_str());
    }
    else
    {
        control_->Output(DEBUG_OUTPUT_NORMAL, "%s\n", result);
    }
}

void DmlOutput::OutputAgentThinking(const char* msg)
{
    if (!control_)
        return;

    if (dml_supported_)
    {
        std::string escaped = EscapeDml(msg);
        control_->ControlledOutput(DEBUG_OUTCTL_AMBIENT_DML, DEBUG_OUTPUT_NORMAL,
                                   "<col fg=\"empfg\"><i>%s</i></col>\n", escaped.c_str());
    }
    else
    {
        control_->Output(DEBUG_OUTPUT_NORMAL, "%s\n", msg);
    }
}

void DmlOutput::OutputAgentResponse(const char* msg)
{
    if (!control_)
        return;

    // Agent response is normal text, no special coloring needed
    control_->Output(DEBUG_OUTPUT_NORMAL, "%s\n", msg);
}

void DmlOutput::OutputError(const char* msg)
{
    if (!control_)
        return;

    if (dml_supported_)
    {
        std::string escaped = EscapeDml(msg);
        control_->ControlledOutput(DEBUG_OUTCTL_AMBIENT_DML, DEBUG_OUTPUT_ERROR,
                                   "<col fg=\"errfg\">%s</col>\n", escaped.c_str());
    }
    else
    {
        control_->Output(DEBUG_OUTPUT_ERROR, "%s\n", msg);
    }
}

void DmlOutput::OutputWarning(const char* msg)
{
    if (!control_)
        return;

    if (dml_supported_)
    {
        std::string escaped = EscapeDml(msg);
        control_->ControlledOutput(DEBUG_OUTCTL_AMBIENT_DML, DEBUG_OUTPUT_WARNING,
                                   "<col fg=\"warnfg\">%s</col>\n", escaped.c_str());
    }
    else
    {
        control_->Output(DEBUG_OUTPUT_WARNING, "%s\n", msg);
    }
}

} // namespace windbg_agent
