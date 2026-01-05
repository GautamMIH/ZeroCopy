#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class WindowRenderer {
public:
    WindowRenderer(HWND targetWindow) : hwnd(targetWindow) {}

    bool Initialize(ID3D11Device* device, int width, int height) {
        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), &dxgiDevice))) return false;

        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;

        ComPtr<IDXGIFactory2> factory;
        if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), &factory))) return false;

        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = width;
        sd.Height = height;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        return SUCCEEDED(factory->CreateSwapChainForHwnd(device, hwnd, &sd, nullptr, nullptr, &swapChain));
    }

    void Draw(ID3D11Texture2D* srcTexture, ID3D11DeviceContext* ctx) {
        if (!swapChain) return;
        ComPtr<ID3D11Texture2D> backBuffer;
        if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backBuffer))) {
            ctx->CopyResource(backBuffer.Get(), srcTexture);
            swapChain->Present(1, 0); 
        }
    }

private:
    HWND hwnd;
    ComPtr<IDXGISwapChain1> swapChain;
};