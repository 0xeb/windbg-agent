#include "output_capture.hpp"

namespace windbg_copilot
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
    // Capture the output
    if (Text)
        captured_output_ += Text;

    // Forward to original callbacks (so user still sees output)
    if (original_callbacks_)
        return original_callbacks_->Output(Mask, Text);

    return S_OK;
}

} // namespace windbg_copilot
