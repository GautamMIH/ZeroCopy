#include "DXGICapturer.h"
#include <iostream>
#include <chrono>
#include <objbase.h>
#include <dxgi1_2.h> // For IDXGIFactory1
#include <DispatcherQueue.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "windowsapp.lib") // For WinRT APIs

using namespace Microsoft::WRL;
namespace winrt {
    using namespace winrt::Windows::Graphics::Capture;
    using namespace winrt::Windows::Graphics::DirectX;
    using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
}

DXGICapturer::DXGICapturer() : capturing(false), useWGC(false) {
    timeBeginPeriod(1);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
}

DXGICapturer::~DXGICapturer() {
    Stop();
    
    if (captureSession) captureSession.Close();
    if (framePool) framePool.Close();
    
    winrt::uninit_apartment();
    timeEndPeriod(1);
}

bool DXGICapturer::Initialize() {
    HRESULT hr;
    
    // Find adapter with active desktop output (handles Optimus/dual GPU setups)
    ComPtr<IDXGIFactory1> factory;
    hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter1> selectedAdapter;
    ComPtr<IDXGIOutput> selectedOutput;
    
    std::cout << "\n=== ADAPTER ENUMERATION ===" << std::endl;
    
    for (UINT i = 0; ; i++) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;

        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue; // Skip software adapters

        std::wcout << L"[Capturer] Adapter " << i << L": " << desc.Description << std::endl;
        std::wcout << L"           VendorID: 0x" << std::hex << desc.VendorId << std::dec << std::endl;
        std::wcout << L"           VRAM: " << (desc.DedicatedVideoMemory / 1024 / 1024) << L" MB" << std::endl;

        for (UINT outputIdx = 0; outputIdx < 4; outputIdx++) { // Check all outputs
            ComPtr<IDXGIOutput> output;
            if (SUCCEEDED(adapter->EnumOutputs(outputIdx, &output))) {
                DXGI_OUTPUT_DESC outDesc;
                output->GetDesc(&outDesc);
                std::wcout << L"           Output " << outputIdx << L": " << outDesc.DeviceName 
                          << (outDesc.AttachedToDesktop ? L" (ACTIVE DESKTOP)" : L" (inactive)") << std::endl;
                
                if (!selectedAdapter && outDesc.AttachedToDesktop) { // Select first adapter with active desktop
                    selectedAdapter = adapter;
                    selectedOutput = output;
                    std::wcout << L"[Capturer] ✓ SELECTED THIS ADAPTER" << std::endl;
                }
            }
        }
        std::cout << std::endl;
    }

    if (!selectedAdapter) {
        std::cerr << "[Capturer] ERROR: No adapter with active desktop output found!" << std::endl;
        return false;
    }

    DXGI_ADAPTER_DESC1 finalDesc;
    selectedAdapter->GetDesc1(&finalDesc);
    std::wcout << L"[Capturer] Final Selection: " << finalDesc.Description << std::endl;
    std::cout << "===========================\n" << std::endl;

    // Create Device on the CORRECT adapter
    UINT createDeviceFlags = 0; 
    // createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG; // Uncomment for debug layer

    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    
    hr = D3D11CreateDevice(selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, createDeviceFlags, 
                           featureLevels, 2, D3D11_SDK_VERSION, &device, &featureLevel, &context);
    if (FAILED(hr)) return false;

    // CHECK IF INTEL - Use Windows.Graphics.Capture for Intel GPUs ONLY
    // On NVIDIA/AMD, use Desktop Duplication (more reliable on discrete GPUs)
    useWGC = (finalDesc.VendorId == 0x8086); // Intel only
    
    if (useWGC) {
        std::cout << "[Capturer] Using Windows.Graphics.Capture API (Intel GPU)" << std::endl;
        
        // Get DXGI device for WinRT interop
        hr = device.As(&dxgiDevice);
        if (FAILED(hr)) return false;
        
        // Create WinRT Direct3D11 device wrapper
        winrt::com_ptr<::IInspectable> inspectable;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put());
        if (FAILED(hr)) {
            std::cerr << "[Capturer] Failed to create WinRT device wrapper: 0x" << std::hex << hr << std::endl;
            return false;
        }
        
        auto d3dDevice = inspectable.as<winrt::IDirect3DDevice>();
        
        // Get primary monitor
        HMONITOR hMonitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        
        // Create GraphicsCaptureItem for the monitor
        auto interop = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::guid itemGuid = winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>();
        
        hr = interop->CreateForMonitor(hMonitor, itemGuid, winrt::put_abi(captureItem));
        if (FAILED(hr)) {
            std::cerr << "[Capturer] Failed to create GraphicsCaptureItem: 0x" << std::hex << hr << std::endl;
            return false;
        }
        
        // Create frame pool
        framePool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
            d3dDevice,
            winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2, // Number of buffers
            captureItem.Size()
        );
        
        if (!framePool) {
            std::cerr << "[Capturer] Failed to create frame pool" << std::endl;
            return false;
        }
        
        // Create capture session
        captureSession = framePool.CreateCaptureSession(captureItem);
        
        std::cout << "[Capturer] Windows.Graphics.Capture initialized successfully" << std::endl;
        return true;
    }
    
    // NVIDIA/AMD: Use DXGI Desktop Duplication
    std::cout << "[Capturer] Using DXGI Desktop Duplication (NVIDIA/AMD GPU)" << std::endl;

    // Get Output1 Interface for Duplication
    ComPtr<IDXGIOutput1> dxgiOutput1; 
    hr = selectedOutput.As(&dxgiOutput1);
    if (FAILED(hr)) {
        std::cerr << "[Capturer] Failed to get IDXGIOutput1: 0x" << std::hex << hr << std::dec << std::endl;
        std::cout << "[Capturer] Falling back to Windows.Graphics.Capture..." << std::endl;
        useWGC = true;
        goto USE_WGC_FALLBACK;
    }
    
    // Duplicate Output
    hr = dxgiOutput1->DuplicateOutput(device.Get(), &deskDupl);
    if (FAILED(hr)) {
        std::cerr << "[Capturer] DuplicateOutput FAILED: 0x" << std::hex << hr << std::dec << std::endl;
        std::cout << "[Capturer] This GPU can't access the desktop (Optimus?)" << std::endl;
        std::cout << "[Capturer] Falling back to Windows.Graphics.Capture..." << std::endl;
        useWGC = true;
        goto USE_WGC_FALLBACK;
    }
    
    std::cout << "[Capturer] DXGI Desktop Duplication initialized successfully" << std::endl;
    return true;

USE_WGC_FALLBACK:
    // Fallback for Optimus laptops where DXGI Desktop Duplication isn't available
    std::cout << "[Capturer] Initializing Windows.Graphics.Capture fallback..." << std::endl;
    
    // Get DXGI device for WinRT interop
    hr = device.As(&dxgiDevice);
    if (FAILED(hr)) return false;
    
    // Create WinRT Direct3D11 device wrapper
    winrt::com_ptr<::IInspectable> inspectable;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put());
    if (FAILED(hr)) {
        std::cerr << "[Capturer] Failed to create WinRT device wrapper: 0x" << std::hex << hr << std::endl;
        return false;
    }
    
    auto d3dDevice = inspectable.as<winrt::IDirect3DDevice>();
    
    // Get primary monitor
    HMONITOR hMonitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    
    // Create GraphicsCaptureItem for the monitor
    auto interop = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    winrt::guid itemGuid = winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>();
    
    hr = interop->CreateForMonitor(hMonitor, itemGuid, winrt::put_abi(captureItem));
    if (FAILED(hr)) {
        std::cerr << "[Capturer] Failed to create GraphicsCaptureItem: 0x" << std::hex << hr << std::endl;
        return false;
    }
    
    // Create frame pool
    framePool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
        d3dDevice,
        winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2, // Number of buffers
        captureItem.Size()
    );
    
    if (!framePool) {
        std::cerr << "[Capturer] Failed to create frame pool" << std::endl;
        return false;
    }
    
    // Create capture session
    captureSession = framePool.CreateCaptureSession(captureItem);
    
    std::cout << "[Capturer] Windows.Graphics.Capture initialized successfully" << std::endl;
    return true;
}

void DXGICapturer::Start(FrameCallback onFrameCaptured) {
    if (capturing) return;
    capturing = true;
    
    if (useWGC) {
        captureThread = std::thread(&DXGICapturer::CaptureLoopWGC, this, onFrameCaptured);
    } else {
        captureThread = std::thread(&DXGICapturer::CaptureLoop, this, onFrameCaptured);
    }
}

void DXGICapturer::Stop() {
    capturing = false;
    if (captureThread.joinable()) {
        captureThread.join();
    }
}

void DXGICapturer::CaptureLoop(FrameCallback onFrameCaptured) {
    HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // 60 FPS Target
    const auto FRAME_DURATION = std::chrono::microseconds(1000000 / 60);
    auto nextFrameTime = std::chrono::steady_clock::now();

    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    ComPtr<IDXGIResource> desktopResource;
    ComPtr<ID3D11Texture2D> acquiredImage;
    
    int successCount = 0;
    int timeoutCount = 0;
    int errorCount = 0;

    std::cout << "[Capturer] Capture loop started..." << std::endl;

    while (capturing) {
        nextFrameTime += FRAME_DURATION;

        // 1. Acquire the raw desktop frame
        // Use 16ms timeout (one frame) to get updates
        HRESULT hr = deskDupl->AcquireNextFrame(16, &frameInfo, &desktopResource);

        if (SUCCEEDED(hr)) {
            successCount++;
            
            // SKIP THE FIRST FRAME (it's often empty/uninitialized)
            if (successCount == 1) {
                std::cout << "[Capturer] Skipping first frame (usually empty)" << std::endl;
                deskDupl->ReleaseFrame();
                continue;
            }
            
            // Log first few successful captures
            if (successCount <= 6) {
                std::cout << "[Capturer] Frame acquired #" << successCount 
                         << " | LastPresentTime: " << frameInfo.LastPresentTime.QuadPart
                         << " | AccumulatedFrames: " << frameInfo.AccumulatedFrames << std::endl;
            }
            
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
            timeoutCount++;
            if (timeoutCount <= 3) {
                std::cout << "[Capturer] Timeout (no screen update)" << std::endl;
            }
            // No screen update (desktop is static). 
            // Send nullptr to re-encode last frame
            onFrameCaptured(nullptr, context.Get());
        }
        else if (hr == DXGI_ERROR_ACCESS_LOST) {
            std::cerr << "[Capturer] ERROR: Access lost (resolution change or UAC?)" << std::endl;
            errorCount++;
        }
        else {
            if (errorCount < 5) {
                std::cerr << "[Capturer] AcquireNextFrame failed: 0x" << std::hex << hr << std::dec << std::endl;
            }
            errorCount++;
        }

        std::this_thread::sleep_until(nextFrameTime);
    }
    if (SUCCEEDED(coInit)) {
        CoUninitialize();
    }
}

void DXGICapturer::CaptureLoopWGC(FrameCallback onFrameCaptured) {
    HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    
    // 60 FPS Target
    const auto FRAME_DURATION = std::chrono::microseconds(1000000 / 60);
    auto nextFrameTime = std::chrono::steady_clock::now();
    
    int frameCount = 0;
    
    std::cout << "[Capturer-WGC] Capture loop started..." << std::endl;
    
    // Start the capture session
    captureSession.StartCapture();
    
    while (capturing) {
        nextFrameTime += FRAME_DURATION;
        
        // Try to get next frame from pool
        auto frame = framePool.TryGetNextFrame();
        
        if (frame) {
            frameCount++;
            
            // Get the Direct3D11 surface
            auto frameSurface = frame.Surface();
            
            // Get the underlying ID3D11Texture2D from the WinRT surface
            auto surfaceInterop = frameSurface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            
            ComPtr<ID3D11Texture2D> texture;
            HRESULT hr = surfaceInterop->GetInterface(__uuidof(ID3D11Texture2D), &texture);
            
            if (SUCCEEDED(hr) && texture) {
                if (frameCount % 60 == 1) {
                    std::cout << "[Capturer-WGC] Frame #" << frameCount << std::endl;
                }
                
                // ZERO COPY: Pass texture directly to encoder
                onFrameCaptured(texture.Get(), context.Get());
            }
            
            frame.Close();
        } else {
            // No new frame available, send nullptr to re-encode last frame
            onFrameCaptured(nullptr, context.Get());
        }
        
        std::this_thread::sleep_until(nextFrameTime);
    }
    
    if (SUCCEEDED(coInit)) {
        CoUninitialize();
    }
}

