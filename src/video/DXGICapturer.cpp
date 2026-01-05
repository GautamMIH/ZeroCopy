#include "DXGICapturer.h"
#include <iostream>
#include <chrono>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")

using namespace Microsoft::WRL;

DXGICapturer::DXGICapturer() : capturing(false) {
    timeBeginPeriod(1);
}

DXGICapturer::~DXGICapturer() {
    Stop();
    timeEndPeriod(1);
}

bool DXGICapturer::Initialize() {
    HRESULT hr;
    
    // Create Device
    // Note: We need BGRA support for DDA
    UINT createDeviceFlags = 0; 
    // createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG; // Uncomment for debug layer

    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, 
                           featureLevels, 2, D3D11_SDK_VERSION, &device, &featureLevel, &context);
    if (FAILED(hr)) return false;

    // Get DXGI Output
    ComPtr<IDXGIDevice> dxgiDevice; device.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> dxgiAdapter; dxgiDevice->GetAdapter(&dxgiAdapter);
    ComPtr<IDXGIOutput> dxgiOutput; dxgiAdapter->EnumOutputs(0, &dxgiOutput);
    ComPtr<IDXGIOutput1> dxgiOutput1; dxgiOutput.As(&dxgiOutput1);
    
    // Duplicate Output
    hr = dxgiOutput1->DuplicateOutput(device.Get(), &deskDupl);
    return SUCCEEDED(hr);
}

void DXGICapturer::Start(FrameCallback onFrameCaptured) {
    if (capturing) return;
    capturing = true;
    captureThread = std::thread(&DXGICapturer::CaptureLoop, this, onFrameCaptured);
}

void DXGICapturer::Stop() {
    capturing = false;
    if (captureThread.joinable()) {
        captureThread.join();
    }
}

void DXGICapturer::CaptureLoop(FrameCallback onFrameCaptured) {
    // 60 FPS Target
    const auto FRAME_DURATION = std::chrono::microseconds(1000000 / 60);
    auto nextFrameTime = std::chrono::steady_clock::now();

    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    ComPtr<IDXGIResource> desktopResource;
    ComPtr<ID3D11Texture2D> acquiredImage;

    while (capturing) {
        nextFrameTime += FRAME_DURATION;

        // 1. Acquire the raw desktop frame
        // 0 timeout ensures we don't hang if the screen is static, 
        // but for a game/video, usually use ~10ms. 
        // We use 0 here to keep the loop tight and handle "no update" logic if needed.
        HRESULT hr = deskDupl->AcquireNextFrame(0, &frameInfo, &desktopResource);

        if (SUCCEEDED(hr)) {
            // 2. Get the Texture Interface
            if (SUCCEEDED(desktopResource.As(&acquiredImage))) {
                
                // 3. ZERO COPY SHORTCUT:
                // We pass the raw desktop texture directly to the callback.
                // The callback runs the Converter (Blt) immediately.
                // The GPU command is queued.
                onFrameCaptured(acquiredImage.Get(), context.Get());
            }

            // 4. Release (Give it back to the OS)
            // It is safe to release here because the GPU commands (Conversion) 
            // from the callback have already been submitted to the Context.
            deskDupl->ReleaseFrame();
        }
        else if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // No screen update (desktop is static). 
            // In a real stream, you might want to resend the previous frame 
            // or send a "keep alive" packet.
            onFrameCaptured(nullptr, context.Get());
        }
        else if (hr == DXGI_ERROR_ACCESS_LOST) {
            // Resolution changed or UAC prompt. Logic to reset would go here.
        }

        std::this_thread::sleep_until(nextFrameTime);
    }
}