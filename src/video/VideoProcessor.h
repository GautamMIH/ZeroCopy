#pragma once
#include <d3d11.h>
#include <d3d11_1.h> 
#include <wrl/client.h>
#include <map> 
#include <iostream>

using Microsoft::WRL::ComPtr;

class VideoProcessor {
public:
    bool Initialize(ID3D11Device* device, int width, int height) {
        inputViewCache.clear();
        devicePtr = device; 

        if (FAILED(device->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&videoDevice))) return false;
        
        ComPtr<ID3D11DeviceContext> ctx;
        device->GetImmediateContext(&ctx);
        if (FAILED(ctx.As(&videoContext))) return false;

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
        contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        contentDesc.InputWidth = width;
        contentDesc.InputHeight = height;
        contentDesc.OutputWidth = width;
        contentDesc.OutputHeight = height;
        contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        if (FAILED(videoDevice->CreateVideoProcessorEnumerator(&contentDesc, &videoEnum))) return false;
        if (FAILED(videoDevice->CreateVideoProcessor(videoEnum.Get(), 0, &processor))) return false;

        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_NV12; 
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; 
        if (FAILED(device->CreateTexture2D(&texDesc, nullptr, &outputTexture))) return false;

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outViewDesc = {};
        outViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(videoDevice->CreateVideoProcessorOutputView(outputTexture.Get(), videoEnum.Get(), &outViewDesc, &outputView))) return false;

        return true;
    }

    ID3D11Texture2D* Convert(ID3D11Texture2D* inputTexture) {
        if (!videoContext || !processor || !inputTexture) {
            std::cerr << "[VideoProcessor] Convert: NULL input detected" << std::endl;
            return nullptr;
        }
        
        ComPtr<ID3D11Device> texDevice;
        inputTexture->GetDevice(&texDevice);
        if (texDevice.Get() != devicePtr.Get()) { // Texture must be on same device
            std::cerr << "[VideoProcessor] ERROR: Input texture is on different device!" << std::endl;
            return nullptr;
        }
        
        ID3D11VideoProcessorInputView* pInputView = GetCachedInputView(inputTexture);
        if (!pInputView) {
            std::cerr << "[VideoProcessor] Failed to create/get input view" << std::endl;
            return nullptr;
        }
        
        if (!videoContext) {
            std::cerr << "[VideoProcessor] videoContext is NULL!" << std::endl;
            return nullptr;
        }
        if (!processor) {
            std::cerr << "[VideoProcessor] processor is NULL!" << std::endl;
            return nullptr;
        }
        if (!outputView) {
            std::cerr << "[VideoProcessor] outputView is NULL!" << std::endl;
            return nullptr;
        }

        D3D11_VIDEO_PROCESSOR_STREAM stream = {};
        stream.Enable = TRUE;
        stream.pInputSurface = pInputView;
        
        HRESULT hr = videoContext->VideoProcessorBlt(processor.Get(), outputView.Get(), 0, 1, &stream);
        
        if (FAILED(hr)) {
            std::cerr << "[VideoProcessor] Blt FAILED: 0x" << std::hex << hr << std::dec << std::endl;
            return nullptr;
        }
        
        ComPtr<ID3D11DeviceContext> ctx;
        devicePtr->GetImmediateContext(&ctx);
        ctx->Flush(); // Ensure GPU commands complete
        
        return outputTexture.Get(); 
    }

    ID3D11Texture2D* ConvertNV12ToBGRA(ID3D11Texture2D* nv12Texture) {
        if (!videoContext || !processor || !nv12Texture) return nullptr;

        if (!bgraTexture) {
            D3D11_TEXTURE2D_DESC srcDesc;
            nv12Texture->GetDesc(&srcDesc);

            D3D11_TEXTURE2D_DESC newDesc = {}; 
            newDesc.Width = srcDesc.Width;
            newDesc.Height = srcDesc.Height;
            newDesc.MipLevels = 1;
            newDesc.ArraySize = 1;
            newDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; 
            newDesc.SampleDesc.Count = 1;
            newDesc.Usage = D3D11_USAGE_DEFAULT;
            newDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            newDesc.MiscFlags = 0;

            if (FAILED(devicePtr->CreateTexture2D(&newDesc, nullptr, &bgraTexture))) {
                std::cerr << "Failed to create BGRA" << std::endl;
                return nullptr;
            }

            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outViewDesc = {};
            outViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            videoDevice->CreateVideoProcessorOutputView(bgraTexture.Get(), videoEnum.Get(), &outViewDesc, &bgraOutputView);
        }

        ID3D11VideoProcessorInputView* pInputView = GetCachedInputView(nv12Texture);
        if (!pInputView) return nullptr;

        D3D11_VIDEO_PROCESSOR_STREAM stream = {};
        stream.Enable = TRUE;
        stream.pInputSurface = pInputView; 
        videoContext->VideoProcessorBlt(processor.Get(), bgraOutputView.Get(), 0, 1, &stream);
        return bgraTexture.Get();
    }

    ID3D11Texture2D* GetOutputTexture() { return outputTexture.Get(); }

private:
    ComPtr<ID3D11Device> devicePtr;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> videoEnum;
    ComPtr<ID3D11VideoProcessor> processor;
    
    ComPtr<ID3D11Texture2D> outputTexture; 
    ComPtr<ID3D11VideoProcessorOutputView> outputView;

    ComPtr<ID3D11Texture2D> bgraTexture;
    ComPtr<ID3D11VideoProcessorOutputView> bgraOutputView;

    std::map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorInputView>> inputViewCache;

    ID3D11VideoProcessorInputView* GetCachedInputView(ID3D11Texture2D* tex) {
        auto it = inputViewCache.find(tex);
        if (it != inputViewCache.end()) {
            return it->second.Get();
        }
        if (inputViewCache.size() > 4) inputViewCache.clear();

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc = {};
        inDesc.FourCC = 0;
        inDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inDesc.Texture2D.MipSlice = 0;
        ComPtr<ID3D11VideoProcessorInputView> newView;
        
        HRESULT hr = videoDevice->CreateVideoProcessorInputView(tex, videoEnum.Get(), &inDesc, &newView);
        if (FAILED(hr)) {
            std::cerr << "[VideoProcessor] CreateVideoProcessorInputView failed: 0x" << std::hex << hr << std::dec << std::endl;
            return nullptr;
        }
        
        inputViewCache[tex] = newView;
        return newView.Get();
    }
};