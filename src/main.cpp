#include <iostream>
#include <windows.h>
#include "video/DXGICapturer.h"
#include "video/HardwareEncoder.h"
#include "video/HardwareDecoder.h" 
#include "WindowRenderer.h"
#include "video/VideoProcessor.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); 

    WNDCLASS wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "LoopbackPreview";
    RegisterClass(&wc);
    HWND hwnd = CreateWindowEx(0, "LoopbackPreview", "Zero Copy Loopback", 
                               WS_OVERLAPPEDWINDOW, 100, 100, 1280, 720, 
                               nullptr, nullptr, nullptr, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    DXGICapturer capturer;
    HardwareEncoder encoder;
    HardwareDecoder decoder;
    WindowRenderer renderer(hwnd);
    VideoProcessor displayConverter; 

    if (!capturer.Initialize()) return -1;
    bool initDone = false;

    auto onFrame = [&](ID3D11Texture2D* tex, ID3D11DeviceContext* ctx) {
        if (!initDone && tex) {
            D3D11_TEXTURE2D_DESC desc; tex->GetDesc(&desc);
            ComPtr<ID3D11Device> dev; ctx->GetDevice(&dev);
            
            encoder.Initialize(dev.Get(), desc.Width, desc.Height);
            decoder.Initialize(dev.Get(), desc.Width, desc.Height);
            renderer.Initialize(dev.Get(), desc.Width, desc.Height);
            displayConverter.Initialize(dev.Get(), desc.Width, desc.Height); // Display size
            
            std::cout << "[System] Ready. Display: " << desc.Width << "x" << desc.Height << std::endl;
            initDone = true;
        }

        if (initDone) {
            // Space to Bypass (Pure Capture Test)
            if (GetAsyncKeyState(VK_SPACE) & 0x8000) {
                ID3D11Texture2D* bgra = displayConverter.ConvertNV12ToBGRA(displayConverter.Convert(tex));
                if (bgra) renderer.Draw(bgra, ctx);
                return;
            }

            encoder.EncodeFrame(tex, ctx, [&](const uint8_t* data, size_t size) {
            // STATUS: Print Size
            int nalType = 0;
            if (size > 4) nalType = data[4] & 0x1F;

            std::cout << "\rPkt(" << size << "|T:" << nalType << ")";

                // Decode returns output immediately (after pipeline is primed)
                ID3D11Texture2D* decodedNV12 = decoder.Decode(data, size, ctx);
                
                if (decodedNV12) {
                    // STATUS: Decoded
                    std::cout << "-DEC"; 

                    ID3D11Texture2D* displayTex = displayConverter.ConvertNV12ToBGRA(decodedNV12);
                    
                    if (displayTex) {
                        // STATUS: Converted
                        std::cout << "-CNV "; 
                        renderer.Draw(displayTex, ctx);
                    } else {
                        std::cout << "-XXX "; // Converter Failed
                    }
                } else {
                     std::cout << ".... "; // Waiting for Keyframe/Init
                }
            });
        }
    };

    capturer.Start(onFrame);
    // Static pointer for the callback to access

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    capturer.Stop();
    return 0;
}