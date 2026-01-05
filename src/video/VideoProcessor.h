#pragma once
#include <d3d11.h>
#include <d3d11_1.h> // Required for Video Context
#include <wrl/client.h>
#include <iostream>

using Microsoft::WRL::ComPtr;

class VideoProcessor {
public:
    bool Initialize(ID3D11Device* device, int width, int height) {
        // 1. Get Video Device Interfaces
        if (FAILED(device->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&videoDevice))) return false;
        
        ComPtr<ID3D11DeviceContext> ctx;
        device->GetImmediateContext(&ctx);
        if (FAILED(ctx.As(&videoContext))) return false;

        // 2. Create Video Processor Enumerator
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
        contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        contentDesc.InputWidth = width;
        contentDesc.InputHeight = height;
        contentDesc.OutputWidth = width;
        contentDesc.OutputHeight = height;
        contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        if (FAILED(videoDevice->CreateVideoProcessorEnumerator(&contentDesc, &videoEnum))) return false;

        // 3. Create Video Processor
        if (FAILED(videoDevice->CreateVideoProcessor(videoEnum.Get(), 0, &processor))) return false;

        // 4. Create Output Texture (NV12)
        // This is the destination memory on the GPU where the converted image lives
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_NV12; // <--- The Target Format
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; 
        texDesc.MiscFlags = 0;

        if (FAILED(device->CreateTexture2D(&texDesc, nullptr, &outputTexture))) return false;

        // 5. Create Output View
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outViewDesc = {};
        outViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(videoDevice->CreateVideoProcessorOutputView(outputTexture.Get(), videoEnum.Get(), &outViewDesc, &outputView))) return false;

        this->width = width;
        this->height = height;
        return true;
    }

    // Converts Input (BGRA) -> Output (NV12) entirely on GPU
    ID3D11Texture2D* Convert(ID3D11Texture2D* inputTexture) {
        if (!videoContext || !processor) return nullptr;

        // 1. Create Input View (We do this every frame or cache it if the input texture pointer never changes)
        // Ideally cache this, but for simplicity we create/release (lightweight object)
        ComPtr<ID3D11VideoProcessorInputView> inputView;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inViewDesc = {};
        inViewDesc.FourCC = 0;
        inViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inViewDesc.Texture2D.MipSlice = 0;
        
        if (FAILED(videoDevice->CreateVideoProcessorInputView(inputTexture, videoEnum.Get(), &inViewDesc, &inputView))) {
            return nullptr;
        }

        // 2. Perform the Blit (Conversion)
        D3D11_VIDEO_PROCESSOR_STREAM stream = {};
        stream.Enable = TRUE;
        stream.pInputSurface = inputView.Get();

        // Pass the output view
        HRESULT hr = videoContext->VideoProcessorBlt(
            processor.Get(), 
            outputView.Get(), 
            0, 1, &stream);

        if (FAILED(hr)) return nullptr;

        return outputTexture.Get(); // Return the NV12 Texture
    }

    ID3D11Texture2D* GetOutputTexture() { 
        return outputTexture.Get(); 
    }

private:
    int width = 0, height = 0;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> videoEnum;
    ComPtr<ID3D11VideoProcessor> processor;
    
    ComPtr<ID3D11Texture2D> outputTexture; // Holds the NV12 result
    ComPtr<ID3D11VideoProcessorOutputView> outputView;
};