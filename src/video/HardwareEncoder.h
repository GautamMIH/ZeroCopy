#pragma once
#include <d3d11.h>
#include <vector>
#include <cstdint>
#include "VideoProcessor.h"

enum class EncoderVendor {
    NONE,
    NVIDIA, // NVENC
    AMD     // AMF
};

class HardwareEncoder {
public:
    HardwareEncoder();
    ~HardwareEncoder();

    // Detects GPU vendor and loads the appropriate driver
    bool Initialize(ID3D11Device* device, int width, int height);

    // Routes to the active encoder
    std::vector<uint8_t> EncodeFrame(ID3D11Texture2D* texture, ID3D11DeviceContext* context);

private:
    EncoderVendor vendor = EncoderVendor::NONE;
    int width = 0;
    int height = 0;

    VideoProcessor converter;

    // --- NVIDIA State ---
    void* nvEncoder = nullptr; // NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS*
    void* nvFunctionList = nullptr; // NV_ENCODE_API_FUNCTION_LIST*
    
    // --- AMD State ---
    void* amfContext = nullptr; // amf::AMFContext*
    void* amfComponent = nullptr; // amf::AMFComponent*

    // Internal Helpers
    bool InitNVIDIA(ID3D11Device* device);
    bool InitAMD(ID3D11Device* device);
    std::vector<uint8_t> EncodeNVIDIA(ID3D11Texture2D* texture);
    std::vector<uint8_t> EncodeAMD(ID3D11Texture2D* texture);
    void Cleanup();

    void* nvRegisteredResource = nullptr;
    void* amfCachedSurface = nullptr;
};