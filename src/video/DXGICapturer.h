#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <functional>
#include <thread>

// Windows.Graphics.Capture API (C++/WinRT)
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

// Minimal includes in header to speed up compilation
// "Forward Declarations" could be used here to reduce dependencies further

using FrameCallback = std::function<void(ID3D11Texture2D*, ID3D11DeviceContext*)>;

class DXGICapturer {
public:
    DXGICapturer();
    ~DXGICapturer();

    // Returns true if initialization succeeded
    bool Initialize();

    // Starts the capture thread
    void Start(FrameCallback onFrameCaptured);

    // Stops the capture thread
    void Stop();

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> deskDupl;

    std::thread captureThread;
    bool capturing;
    bool useWGC; // Use Windows.Graphics.Capture instead of DXGI (for Intel)

    // Windows.Graphics.Capture objects
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem captureItem{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession captureSession{ nullptr };
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;

    // The internal loop functions
    void CaptureLoop(FrameCallback onFrameCaptured);
    void CaptureLoopWGC(FrameCallback onFrameCaptured);
};
