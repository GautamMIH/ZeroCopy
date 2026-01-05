#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <functional>
#include <thread>

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

    // The internal loop function
    void CaptureLoop(FrameCallback onFrameCaptured);
};