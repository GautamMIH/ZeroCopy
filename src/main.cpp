#include <iostream>
#include <WinSock2.h>
#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <vector>
#include <string>

// ImGui Includes
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"

// Your Systems
#include "common/NetworkManager.h"
#include "video/DXGICapturer.h"
#include "video/HardwareEncoder.h"
#include "video/HardwareDecoder.h" 
#include "video/VideoProcessor.h"
#include "audio/AudioCapturer.h" 
#include "audio/AudioPlayer.h"   

#pragma comment(lib, "d3d11.lib")

// --- Global State ---
enum class AppState { MENU, HOSTING, CONNECTING, STREAMING };
AppState g_State = AppState::MENU;

// D3D11 Globals
ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// Logic Globals
NetworkManager g_Net;
int g_Socket = -1;
std::string g_StatusMsg = "Ready";

// Host
DXGICapturer g_Capturer;
HardwareEncoder g_Encoder;
AudioCapturer g_AudioCap;
size_t g_BytesSent = 0;


// Audio Selection
std::vector<AudioDeviceInfo> g_AudioDevices;
int g_SelectedAudioIndex = 0;


// Client
HardwareDecoder g_Decoder;
VideoProcessor g_Converter;
AudioPlayer g_AudioPlay;
ID3D11Texture2D* g_DisplayTexture = nullptr; 
ID3D11ShaderResourceView* g_DisplaySRV = nullptr; 
POINT g_RemoteCursor = { -1, -1 };
bool g_ClientInit = false;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, 
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext)))
        return false;

    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
    return true;
}

void CleanupDeviceD3D() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
    if (g_DisplaySRV) { g_DisplaySRV->Release(); g_DisplaySRV = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            ID3D11Texture2D* pBackBuffer;
            g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
            g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
            pBackBuffer->Release();
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int main(int, char**) {
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, _T("DXGI Streamer"), nullptr };
    RegisterClassEx(&wc);
    HWND hwnd = CreateWindow(wc.lpszClassName, _T("DXGI Zero Copy Streamer"), WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) { CleanupDeviceD3D(); UnregisterClass(wc.lpszClassName, wc.hInstance); return 1; }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Initial Audio Enumeration
    g_AudioDevices = g_AudioCap.EnumerateDevices();

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // --- LOGIC ---
        if (g_State == AppState::CONNECTING) {
             int sock = -1;
             if (g_Net.FindAndConnect(sock)) {
                 g_Socket = sock;
                 g_State = AppState::STREAMING;
                 g_StatusMsg = "Connected!";
                 // Init Audio Player
                 g_AudioPlay.Initialize();
             } else {
                 g_State = AppState::MENU; 
                 g_StatusMsg = "Connection timeout.";
             }
        }
        else if (g_State == AppState::STREAMING) {
            // Check for data loop
            while (g_Net.IsDataAvailable(g_Socket)) {
                PacketHeader header;
                if (g_Net.ReceiveHeader(g_Socket, header)) {
                    static std::vector<uint8_t> buffer;
                    if (g_Net.ReceiveBody(g_Socket, buffer, header.payloadSize)) {
                        
                        if (header.packetType == PACKET_TYPE_VIDEO) {
                            // --- HANDLE VIDEO ---
                            g_RemoteCursor.x = header.cursorX;
                            g_RemoteCursor.y = header.cursorY;

                            if (!g_ClientInit) {
                                g_Decoder.Initialize(g_pd3dDevice, 1920, 1080);
                                g_Converter.Initialize(g_pd3dDevice, 1920, 1080);
                                g_ClientInit = true;
                            }

                            ID3D11Texture2D* decoded = g_Decoder.Decode(buffer.data(), header.payloadSize, g_pd3dDeviceContext);
                            if (decoded) {
                                ID3D11Texture2D* newTex = g_Converter.ConvertNV12ToBGRA(decoded);
                                if (newTex && newTex != g_DisplayTexture) {
                                    g_DisplayTexture = newTex;
                                    if (g_DisplaySRV) { g_DisplaySRV->Release(); g_DisplaySRV = nullptr; }
                                    g_pd3dDevice->CreateShaderResourceView(g_DisplayTexture, nullptr, &g_DisplaySRV);
                                }
                            }
                        } 
                        else if (header.packetType == PACKET_TYPE_AUDIO) {
                            // --- HANDLE AUDIO ---
                            g_AudioPlay.QueueAudio(buffer.data(), header.payloadSize);
                        }
                    }
                } else {
                    g_State = AppState::MENU;
                    g_StatusMsg = "Host disconnected.";
                    closesocket(g_Socket); g_Socket = -1;
                    break;
                }
            }
        }

        // --- RENDER ---
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // 1. Draw Video
        if (g_State == AppState::STREAMING && g_DisplaySRV) {
            RECT rect; GetClientRect(hwnd, &rect);
            float w = (float)(rect.right - rect.left);
            float h = (float)(rect.bottom - rect.top);
            auto drawList = ImGui::GetBackgroundDrawList();
            drawList->AddImage((void*)g_DisplaySRV, ImVec2(0,0), ImVec2(w,h));

            if (g_RemoteCursor.x != -1) {
                float scaleX = w / 1920.0f; 
                float scaleY = h / 1080.0f;
                float bx = g_RemoteCursor.x * scaleX;
                float by = g_RemoteCursor.y * scaleY;
                float cs = 16.0f;
                drawList->AddTriangleFilled(ImVec2(bx, by), ImVec2(bx, by+cs), ImVec2(bx+cs*0.75f, by+cs*0.75f), IM_COL32(255,255,255,255));
                drawList->AddTriangle(ImVec2(bx, by), ImVec2(bx, by+cs), ImVec2(bx+cs*0.75f, by+cs*0.75f), IM_COL32(0,0,0,255), 1.5f);
            }
        }

        // 2. Draw UI
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(350, 250), ImGuiCond_FirstUseEver);
        
        ImGui::Begin("Control Panel");
        ImGui::Text("Status: %s", g_StatusMsg.c_str());
        ImGui::Separator();

        if (g_State == AppState::MENU) {
            // --- AUDIO SELECTOR ---
            ImGui::Text("Audio Source:");
            if (!g_AudioDevices.empty()) {
                if (ImGui::BeginCombo("##AudioDev", g_AudioDevices[g_SelectedAudioIndex].name.c_str())) {
                    for (int i = 0; i < g_AudioDevices.size(); i++) {
                        bool isSelected = (g_SelectedAudioIndex == i);
                        if (ImGui::Selectable(g_AudioDevices[i].name.c_str(), isSelected)) {
                            g_SelectedAudioIndex = i;
                        }
                        if (isSelected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            } else {
                ImGui::TextDisabled("No Audio Devices Found");
            }
            ImGui::Separator();
            // ---------------------

            if (ImGui::Button("HOST STREAM", ImVec2(330, 50))) {
                g_StatusMsg = "Waiting for client...";
                
                std::thread hostThread([&]() {
                    int clientSock = -1;
                    if (g_Net.WaitForReceiver(clientSock)) {
                        g_Socket = clientSock;
                        g_State = AppState::HOSTING;
                        g_StatusMsg = "Streaming...";
                        
                        // Start Video
                        static bool encInit = false;
                        g_Capturer.Initialize();
                        g_Capturer.Start([&](ID3D11Texture2D* tex, ID3D11DeviceContext* ctx, POINT pt) {
                            if (!encInit) {
                                D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
                                ComPtr<ID3D11Device> dev; ctx->GetDevice(&dev);
                                g_Encoder.Initialize(dev.Get(), d.Width, d.Height);
                                encInit = true;
                            }
                            g_Encoder.EncodeFrame(tex, ctx, [&](const uint8_t* data, size_t size) {
                                g_Net.SendPacket(g_Socket, PACKET_TYPE_VIDEO, data, size, pt.x, pt.y);
                                g_BytesSent += size;
                            });
                        });

                        // Start Audio
                        if (!g_AudioDevices.empty()) {
                            g_AudioCap.Start(g_AudioDevices[g_SelectedAudioIndex].id, [&](const uint8_t* data, size_t size) {
                                g_Net.SendPacket(g_Socket, PACKET_TYPE_AUDIO, data, size);
                            });
                        }

                    } else {
                        g_StatusMsg = "Hosting failed.";
                    }
                });
                hostThread.detach();
            }

            if (ImGui::Button("JOIN STREAM", ImVec2(330, 50))) {
                g_StatusMsg = "Searching...";
                g_State = AppState::CONNECTING; 
            }
        }
        else if (g_State == AppState::HOSTING) {
            ImGui::Text("Sent: %.2f MB", g_BytesSent / 1024.0f / 1024.0f);
            if (ImGui::Button("Stop Hosting")) {
                g_Capturer.Stop();
                g_AudioCap.Stop(); // Stop Audio
                g_State = AppState::MENU;
                g_BytesSent = 0;
            }
        }
        else if (g_State == AppState::STREAMING) {
            if (ImGui::Button("Disconnect")) {
                closesocket(g_Socket); g_Socket = -1;
                g_State = AppState::MENU;
            }
        }

        ImGui::End();
        ImGui::Render();

        float clearColor[] = { 0.1f, 0.1f, 0.1f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    CleanupDeviceD3D(); DestroyWindow(hwnd); UnregisterClass(wc.lpszClassName, wc.hInstance);
    return 0;
}