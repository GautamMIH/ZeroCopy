#pragma once
#include <d3d11.h>
#include <functional>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class HardwareDecoder {
public:
    HardwareDecoder();
    ~HardwareDecoder();

    bool Initialize(ID3D11Device* device, int width, int height);
    ID3D11Texture2D* Decode(const uint8_t* data, size_t size, ID3D11DeviceContext* ctx);
    ID3D11Texture2D* DrainOutput(); // Get buffered output without submitting new input
    void Cleanup();

private:
    enum class DecoderVendor {
        UNKNOWN,
        AMD,
        NVIDIA,
        INTEL
    };

    bool InitAMD(ID3D11Device* device);
    ID3D11Texture2D* DecodeAMD(const uint8_t* data, size_t size);
    
    bool InitNVIDIA(ID3D11Device* device);
    ID3D11Texture2D* DecodeNVIDIA(const uint8_t* data, size_t size, ID3D11DeviceContext* ctx);

    DecoderVendor vendor = DecoderVendor::UNKNOWN;
    ID3D11Device* devicePtr = nullptr;
    int width = 0;
    int height = 0;

    // AMD AMF decoder members
    void* amfContext = nullptr;      // amf::AMFContextPtr*
    void* amfComponent = nullptr;    // amf::AMFComponentPtr*
    void* amfCachedSurface = nullptr; // amf::AMFSurfacePtr*
    ID3D11Texture2D* outputTexture = nullptr;
    
    // NVIDIA Media Foundation decoder members
    void* mfTransform = nullptr;     // IMFTransform*
    ID3D11Texture2D* mfStagingTexture = nullptr;
    ID3D11Texture2D* mfOutputTexture = nullptr;
    
    bool firstFrame = true;
    int frameCount = 0;
};
