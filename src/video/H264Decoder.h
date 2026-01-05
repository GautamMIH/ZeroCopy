#pragma once
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wrl/client.h>
#include <codecapi.h> 
#include <wmcodecdsp.h> 
#include <iostream>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

using Microsoft::WRL::ComPtr;

// Helper to align dimensions to 16 pixels (Standard H.264 Macroblock size)
inline int Align16(int value) {
    return (value + 15) & ~15;
}

class H264Decoder {
public:
    H264Decoder() { MFStartup(MF_VERSION); }
    ~H264Decoder() { MFShutdown(); }

    bool Initialize(ID3D11Device* device, int width, int height) {
        this->width = width;
        this->height = height;
        this->d3dDevice = device;

        if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown, (void**)&transform))) return false;

        ComPtr<ICodecAPI> codecApi;
        if (SUCCEEDED(transform.As(&codecApi))) {
            VARIANT var = {};
            var.vt = VT_UI4;
            var.ulVal = 1; 
            codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &var);
        }

        ComPtr<IMFMediaType> inputType;
        MFCreateMediaType(&inputType);
        inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        inputType->SetUINT64(MF_MT_FRAME_SIZE, ((UINT64)width << 32) | height);
        inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); 
        if (FAILED(transform->SetInputType(0, inputType.Get(), 0))) return false;

        ComPtr<IMFMediaType> outputType;
        MFCreateMediaType(&outputType);
        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        outputType->SetUINT64(MF_MT_FRAME_SIZE, ((UINT64)width << 32) | height);
        if (FAILED(transform->SetOutputType(0, outputType.Get(), 0))) return false;

        // Output Texture (DEFAULT)
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT; 
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; 
        desc.MiscFlags = 0; 
        if (FAILED(d3dDevice->CreateTexture2D(&desc, nullptr, &sharedTexture))) return false;

        // Staging (DYNAMIC)
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; 
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(d3dDevice->CreateTexture2D(&desc, nullptr, &stagingTexture))) return false;

        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0); 
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        return true;
    }

    ID3D11Texture2D* Decode(const uint8_t* data, size_t size, ID3D11DeviceContext* ctx) {
        if (!data || size == 0) return nullptr;

        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        MFCreateMemoryBuffer((DWORD)size, &buffer);
        BYTE* pData = nullptr;
        buffer->Lock(&pData, nullptr, nullptr);
        memcpy(pData, data, size);
        buffer->Unlock();
        buffer->SetCurrentLength((DWORD)size);

        MFCreateSample(&sample);
        sample->AddBuffer(buffer.Get());

        static LONGLONG timestamp = 10000000;
        sample->SetSampleTime(timestamp);
        sample->SetSampleDuration(166666); 
        timestamp += 166666;

        if (FAILED(transform->ProcessInput(0, sample.Get(), 0))) return nullptr;

        HRESULT hr = S_OK;
        while (true) {
            MFT_OUTPUT_STREAM_INFO info;
            transform->GetOutputStreamInfo(0, &info);

            ComPtr<IMFSample> outSample;
            ComPtr<IMFMediaBuffer> outBuffer;
            MFCreateMemoryBuffer(info.cbSize, &outBuffer);
            MFCreateSample(&outSample);
            outSample->AddBuffer(outBuffer.Get());

            MFT_OUTPUT_DATA_BUFFER outputData = {};
            outputData.dwStreamID = 0;
            outputData.pSample = outSample.Get();
            
            DWORD status = 0;
            hr = transform->ProcessOutput(0, 1, &outputData, &status);

            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                 ComPtr<IMFMediaType> type;
                 MFCreateMediaType(&type);
                 type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                 type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                 type->SetUINT64(MF_MT_FRAME_SIZE, ((UINT64)width << 32) | height);
                 transform->SetOutputType(0, type.Get(), 0);
                 continue; 
            }
            else if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                return nullptr;
            }
            else if (SUCCEEDED(hr)) {
                
                ComPtr<IMF2DBuffer> buffer2D;
                BYTE* srcPtr = nullptr;
                LONG srcStride = 0;
                
                if (SUCCEEDED(outBuffer.As(&buffer2D))) {
                    buffer2D->Lock2D(&srcPtr, &srcStride);
                } else {
                    outBuffer->Lock(&srcPtr, nullptr, nullptr);
                    srcStride = width; 
                }

                if (srcPtr) {
                    D3D11_MAPPED_SUBRESOURCE map;
                    if (SUCCEEDED(ctx->Map(stagingTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                        
                        BYTE* dstPtr = (BYTE*)map.pData;
                        UINT dstStride = map.RowPitch;

                        int alignedHeight = Align16(height); // Decoder pads to 16-pixel alignment
                        
                        // Copy Y plane
                        BYTE* srcY = srcPtr;
                        BYTE* dstY = dstPtr;
                        for (int y = 0; y < height; ++y) {
                            memcpy(dstY, srcY, width);
                            dstY += dstStride;
                            srcY += srcStride;
                        }

                        // Copy UV plane (starts at aligned height)
                        BYTE* srcUV = srcPtr + (srcStride * alignedHeight);
                        BYTE* dstUV_Base = (BYTE*)map.pData + (dstStride * height);
                        
                        for (int y = 0; y < height / 2; ++y) {
                            memcpy(dstUV_Base, srcUV, width);
                            dstUV_Base += dstStride;
                            srcUV += srcStride;
                        }

                        ctx->Unmap(stagingTexture.Get(), 0);
                    }

                    if (buffer2D) buffer2D->Unlock2D();
                    else outBuffer->Unlock();

                    ctx->CopyResource(sharedTexture.Get(), stagingTexture.Get());
                }
                
                if (outputData.pEvents) outputData.pEvents->Release();
                return sharedTexture.Get();
            }
            else {
                return nullptr;
            }
        }
    }

private:
    ComPtr<IMFTransform> transform;
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11Texture2D> sharedTexture; 
    ComPtr<ID3D11Texture2D> stagingTexture;
    int width, height;
};