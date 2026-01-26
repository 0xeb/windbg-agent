#include "output_capture.hpp"

namespace windbg_agent
{

OutputCapture::OutputCapture() : ref_count_(1), client_(nullptr), original_callbacks_(nullptr) {}

OutputCapture::~OutputCapture()
{
    if (client_)
        Uninstall();
}

HRESULT OutputCapture::Install(IDebugClient* client)
{
    if (client_)
        return E_FAIL; // Already installed

    client_ = client;
    client_->AddRef();

    // Save original callbacks
    client_->GetOutputCallbacks(&original_callbacks_);

    // Install ourselves
    return client_->SetOutputCallbacks(this);
}

HRESULT OutputCapture::Uninstall()
{
    if (!client_)
        return E_FAIL;

    // Restore original callbacks
    HRESULT hr = client_->SetOutputCallbacks(original_callbacks_);

    if (original_callbacks_)
    {
        original_callbacks_->Release();
        original_callbacks_ = nullptr;
    }

    client_->Release();
    client_ = nullptr;

    return hr;
}

std::string OutputCapture::GetAndClear()
{
    std::string result = std::move(captured_output_);
    captured_output_.clear();
    return result;
}

// IUnknown implementation
STDMETHODIMP OutputCapture::QueryInterface(REFIID InterfaceId, PVOID* Interface)
{
    if (InterfaceId == __uuidof(IUnknown) || InterfaceId == __uuidof(IDebugOutputCallbacks))
    {
        *Interface = static_cast<IDebugOutputCallbacks*>(this);
        AddRef();
        return S_OK;
    }
    *Interface = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) OutputCapture::AddRef()
{
    return InterlockedIncrement(&ref_count_);
}

STDMETHODIMP_(ULONG) OutputCapture::Release()
{
    ULONG count = InterlockedDecrement(&ref_count_);
    if (count == 0)
        delete this;
    return count;
}

// IDebugOutputCallbacks implementation
STDMETHODIMP OutputCapture::Output(ULONG Mask, PCSTR Text)
{
    // Stack-friendly capture: accumulate nested output and flush once at the outermost call.
    struct StackState
    {
        int depth = 0;
        std::string buffer;
        bool mask_set = false;
        ULONG mask = 0;
    };
    thread_local StackState state;
    state.depth++;

    // Capture the output
    if (Text)
    {
        captured_output_ += Text;
        state.buffer += Text;
    }
    if (!state.mask_set)
    {
        state.mask = Mask;
        state.mask_set = true;
    }

    // Nested call: just accumulate and return.
    if (state.depth > 1)
    {
        state.depth--;
        return S_OK;
    }

    // Outermost call: flush accumulated buffer to original callbacks.
    HRESULT hr = S_OK;
    if (original_callbacks_)
        hr = original_callbacks_->Output(state.mask_set ? state.mask : Mask, state.buffer.c_str());

    state.buffer.clear();
    state.mask_set = false;
    state.depth--;

    return hr;
}

} // namespace windbg_agent
