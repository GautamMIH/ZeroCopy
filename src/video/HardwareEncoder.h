#pragma once
#include <d3d11.h>
#include <functional>
#include <vector>
#include "VideoProcessor.h"

// Media Foundation Headers
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h> 
#include <wrl/client.h> 

using EncodedPacketCallback = std::function<void(const uint8_t* data, size_t size)>;

enum class EncoderVendor {
    NVIDIA,
    AMD,
    MF_GENERIC, 
    UNKNOWN
};

class HardwareEncoder {
public:
    HardwareEncoder();
    ~HardwareEncoder();

    bool Initialize(ID3D11Device* device, int width, int height);
    void EncodeFrame(ID3D11Texture2D* texture, ID3D11DeviceContext* context, EncodedPacketCallback onPacketReady);
    void Cleanup();

private:
    EncoderVendor vendor = EncoderVendor::UNKNOWN;
    int width = 0;
    int height = 0;
    VideoProcessor converter;
    ID3D11Device* devicePtr = nullptr; // Original capture device
    
    // Cross-GPU support
    ID3D11Device* encoderDevice = nullptr; // May differ from capture device
    ID3D11DeviceContext* encoderContext = nullptr;
    ID3D11Texture2D* crossGPUTexture = nullptr; // Shared texture on capture device
    ID3D11Texture2D* crossGPUTextureEncoder = nullptr; // Same texture opened on encoder device
    ID3D11Texture2D* stagingTextureCrossGPU = nullptr; // CPU-accessible staging for cross-GPU copy

    // NVIDIA
    void* nvEncoder = nullptr;
    void* nvFunctionList = nullptr;
    void* nvRegisteredResource = nullptr;
    ID3D11Texture2D* nvInputTexture = nullptr; // Dedicated NV12 texture for NVENC
    bool InitNVIDIA(ID3D11Device* device);
    void EncodeNVIDIA(ID3D11Texture2D* texture, EncodedPacketCallback callback);

    // AMD
    void* amfContext = nullptr;
    void* amfComponent = nullptr;
    void* amfCachedSurface = nullptr;
    bool InitAMD(ID3D11Device* device);
    void EncodeAMD(ID3D11Texture2D* texture, EncodedPacketCallback callback);

    // GENERIC (Media Foundation / Intel)
    Microsoft::WRL::ComPtr<IMFTransform> mfTransform;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> mfDeviceManager;
    
    // Staging Texture for Safe Mode (CPU Readback)
    ID3D11Texture2D* stagingTexture = nullptr;
    bool useCPUConversion = false; // Intel workaround flag

    bool InitMF(ID3D11Device* device);
    // Updated signature to accept DeviceContext for CPU readback
    void EncodeMF(ID3D11Texture2D* texture, ID3D11DeviceContext* ctx, EncodedPacketCallback callback);
};