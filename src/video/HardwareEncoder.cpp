#include "HardwareEncoder.h"
#include <iostream>
#include <string>
#include <wrl/client.h> 
#include <dxgi.h>      

// Headers
#include <wmcodecdsp.h> 
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h> 

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")
#pragma comment(lib, "ole32.lib") 

using Microsoft::WRL::ComPtr;

#include "nvEncodeAPI.h"
#include "AMD/include/core/Factory.h"
#include "AMD/include/core/Context.h"
#include "AMD/include/components/VideoEncoderVCE.h"
#include "AMD/common/AMFSTL.h" 

const int VENDOR_ID_NVIDIA = 0x10DE;
const int VENDOR_ID_AMD    = 0x1002;
const int VENDOR_ID_INTEL  = 0x8086;

// P4 preset = balanced quality/performance (defined in nvEncodeAPI.h)

inline int Align16(int value) {
    return (value + 15) & ~15;
}

HardwareEncoder::HardwareEncoder() { 
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) std::cerr << "Warning: CoInitializeEx failed: " << std::hex << hr << std::endl;
    
    hr = MFStartup(MF_VERSION); 
    if (FAILED(hr)) std::cerr << "CRITICAL: MFStartup failed: " << std::hex << hr << std::endl;
}

HardwareEncoder::~HardwareEncoder() { 
    Cleanup(); 
    MFShutdown(); 
    CoUninitialize();
}

void HardwareEncoder::Cleanup() {
    if (vendor == EncoderVendor::NVIDIA && nvEncoder) {
        auto funcs = static_cast<NV_ENCODE_API_FUNCTION_LIST*>(nvFunctionList);
        if (funcs && nvRegisteredResource) funcs->nvEncUnregisterResource(nvEncoder, nvRegisteredResource);
        if (funcs) funcs->nvEncDestroyEncoder(nvEncoder);
    }
    if (nvInputTexture) {
        nvInputTexture->Release();
        nvInputTexture = nullptr;
    }
    if (vendor == EncoderVendor::AMD) {
        if (amfCachedSurface) delete static_cast<amf::AMFSurfacePtr*>(amfCachedSurface);
        if (amfComponent) static_cast<amf::AMFComponent*>(amfComponent)->Terminate();
    }
    if (stagingTexture) {
        stagingTexture->Release();
        stagingTexture = nullptr;
    }
    if (stagingTextureCrossGPU) {
        stagingTextureCrossGPU->Release();
        stagingTextureCrossGPU = nullptr;
    }
    if (crossGPUTextureEncoder) {
        crossGPUTextureEncoder->Release();
        crossGPUTextureEncoder = nullptr;
    }
    if (crossGPUTexture) {
        crossGPUTexture->Release();
        crossGPUTexture = nullptr;
    }
    if (encoderContext) {
        encoderContext->Release();
        encoderContext = nullptr;
    }
    if (encoderDevice) {
        encoderDevice->Release();
        encoderDevice = nullptr;
    }
}

bool HardwareEncoder::Initialize(ID3D11Device* device, int w, int h) {
    width = w;
    height = h;
    devicePtr = device;

    ComPtr<ID3D11Device> dev = device; 
    ComPtr<IDXGIDevice> dxgiDevice; dev.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter; dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC desc; adapter->GetDesc(&desc);

    std::cout << "[Encoder] Capture GPU Vendor ID: " << std::hex << desc.VendorId << std::dec << std::endl;

    if (desc.VendorId == VENDOR_ID_NVIDIA) {
        vendor = EncoderVendor::NVIDIA;
        std::cout << "[Encoder] Running on NVIDIA GPU - using NVENC directly" << std::endl;
    } 
    else if (desc.VendorId == VENDOR_ID_AMD) {
        vendor = EncoderVendor::AMD;
        std::cout << "[Encoder] Running on AMD GPU - using AMF directly" << std::endl;
    }
    else if (desc.VendorId == VENDOR_ID_INTEL) {
        // INTEL HYBRID GPU FALLBACK:
        // Try to find NVIDIA discrete GPU for encoding
        std::cout << "[Encoder] Intel GPU detected - searching for discrete GPU..." << std::endl;
        
        ComPtr<IDXGIFactory1> factory;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) {
            for (UINT i = 0; ; i++) {
                ComPtr<IDXGIAdapter1> candidateAdapter;
                if (factory->EnumAdapters1(i, &candidateAdapter) == DXGI_ERROR_NOT_FOUND) break;
                
                DXGI_ADAPTER_DESC1 candidateDesc;
                candidateAdapter->GetDesc1(&candidateDesc);
                
                if (candidateDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
                
                // Found NVIDIA or AMD discrete GPU
                if (candidateDesc.VendorId == VENDOR_ID_NVIDIA || candidateDesc.VendorId == VENDOR_ID_AMD) {
                    std::wcout << L"[Encoder] Found discrete GPU: " << candidateDesc.Description << std::endl;
                    
                    // Create encoder device on discrete GPU
                    D3D_FEATURE_LEVEL featureLevel;
                    HRESULT hr = D3D11CreateDevice(
                        candidateAdapter.Get(),
                        D3D_DRIVER_TYPE_UNKNOWN,
                        nullptr,
                        0,
                        nullptr, 0,
                        D3D11_SDK_VERSION,
                        &encoderDevice,
                        &featureLevel,
                        &encoderContext
                    );
                    
                    if (SUCCEEDED(hr)) {
                        vendor = (candidateDesc.VendorId == VENDOR_ID_NVIDIA) ? EncoderVendor::NVIDIA : EncoderVendor::AMD;
                        std::cout << "[Encoder] Will use discrete GPU for encoding (requires texture copy)" << std::endl;
                        
                        // Create staging texture on CAPTURE device (Intel) for CPU readback
                        D3D11_TEXTURE2D_DESC stagingDesc = {};
                        stagingDesc.Width = width;
                        stagingDesc.Height = height;
                        stagingDesc.MipLevels = 1;
                        stagingDesc.ArraySize = 1;
                        stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                        stagingDesc.SampleDesc.Count = 1;
                        stagingDesc.Usage = D3D11_USAGE_STAGING;
                        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        
                        if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &stagingTextureCrossGPU))) {
                            std::cerr << "[Encoder] Failed to create staging texture" << std::endl;
                            encoderDevice->Release();
                            encoderDevice = nullptr;
                            encoderContext->Release();
                            encoderContext = nullptr;
                            vendor = EncoderVendor::MF_GENERIC;
                            continue;
                        }
                        
                        // Create upload texture on ENCODER device (NVIDIA)
                        D3D11_TEXTURE2D_DESC uploadDesc = {};
                        uploadDesc.Width = width;
                        uploadDesc.Height = height;
                        uploadDesc.MipLevels = 1;
                        uploadDesc.ArraySize = 1;
                        uploadDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                        uploadDesc.SampleDesc.Count = 1;
                        uploadDesc.Usage = D3D11_USAGE_DEFAULT;
                        uploadDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                        
                        if (FAILED(encoderDevice->CreateTexture2D(&uploadDesc, nullptr, &crossGPUTextureEncoder))) {
                            std::cerr << "[Encoder] Failed to create encoder upload texture" << std::endl;
                            stagingTextureCrossGPU->Release();
                            stagingTextureCrossGPU = nullptr;
                            encoderDevice->Release();
                            encoderDevice = nullptr;
                            encoderContext->Release();
                            encoderContext = nullptr;
                            vendor = EncoderVendor::MF_GENERIC;
                            continue;
                        }
                        
                        std::cout << "[Encoder] Cross-GPU staging buffer created successfully" << std::endl;
                        break; // Success!
                    }
                }
            }
        }
        
        if (vendor == EncoderVendor::UNKNOWN) {
            std::cout << "[Encoder] No discrete GPU found, falling back to Intel Media Foundation" << std::endl;
            vendor = EncoderVendor::MF_GENERIC;
        }
    }
    else {
        vendor = EncoderVendor::MF_GENERIC;
    }

    // CRITICAL: VideoProcessor must match ACTUAL frame dimensions (not aligned)
    // Alignment is applied inside the encoder (Media Foundation) if needed
    ID3D11Device* converterDevice = encoderDevice ? encoderDevice : device;
    if (!converter.Initialize(converterDevice, width, height)) {
        std::cerr << "[Encoder] Converter Init Failed" << std::endl;
        return false;
    }

    if (vendor == EncoderVendor::NVIDIA) {
        std::cout << "[Encoder] Initializing NVENC..." << std::endl;
        ID3D11Device* nvDevice = encoderDevice ? encoderDevice : device;
        if (!InitNVIDIA(nvDevice)) {
            std::cerr << "[Encoder] NVENC initialization failed!" << std::endl;
            
            // Clean up cross-GPU resources
            if (stagingTextureCrossGPU) {
                stagingTextureCrossGPU->Release();
                stagingTextureCrossGPU = nullptr;
            }
            if (crossGPUTextureEncoder) {
                crossGPUTextureEncoder->Release();
                crossGPUTextureEncoder = nullptr;
            }
            if (encoderContext) {
                encoderContext->Release();
                encoderContext = nullptr;
            }
            if (encoderDevice) {
                encoderDevice->Release();
                encoderDevice = nullptr;
            }
            
            // Fall back to Intel Media Foundation
            vendor = EncoderVendor::MF_GENERIC;
            std::cout << "[Encoder] Falling back to Intel Media Foundation..." << std::endl;
            
            // Re-initialize converter on capture device
            if (!converter.Initialize(device, width, height)) {
                std::cerr << "[Encoder] Converter re-init failed" << std::endl;
                return false;
            }
            
            return InitMF(device);
        }
        return true;
    } 
    else if (vendor == EncoderVendor::AMD) {
        std::cout << "[Encoder] Initializing AMF..." << std::endl;
        ID3D11Device* amdDevice = encoderDevice ? encoderDevice : device;
        return InitAMD(amdDevice);
    }
    else {
        std::cout << "[Encoder] Using Generic/Intel Media Foundation..." << std::endl;
        // This is where it crashed before.
        return InitMF(device);
    }
}

void HardwareEncoder::EncodeFrame(ID3D11Texture2D* texture, ID3D11DeviceContext* context, EncodedPacketCallback onPacketReady) {
    ID3D11Texture2D* target = nullptr;
    
    if (stagingTextureCrossGPU && crossGPUTextureEncoder && encoderDevice) { // Cross-GPU copy via CPU staging
        static bool firstFrame = true;
        
        context->CopyResource(stagingTextureCrossGPU, texture); // GPU -> CPU
        
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hrMap = context->Map(stagingTextureCrossGPU, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hrMap)) {
            if (firstFrame) {
                std::cerr << "[Encoder] Failed to map staging texture: 0x" << std::hex << hrMap << std::dec << std::endl;
            }
            return;
        }
        
        if (firstFrame) {
            std::cout << "[Encoder] Cross-GPU copy starting | RowPitch: " << mapped.RowPitch << std::endl;
        }
        
        D3D11_BOX srcBox; // CPU -> encoder GPU
        srcBox.left = 0;
        srcBox.top = 0;
        srcBox.front = 0;
        srcBox.right = width;
        srcBox.bottom = height;
        srcBox.back = 1;
        
        encoderContext->UpdateSubresource(crossGPUTextureEncoder, 0, &srcBox, mapped.pData, mapped.RowPitch, 0);
        context->Unmap(stagingTextureCrossGPU, 0);
        
        if (firstFrame) {
            std::cout << "[Encoder] Cross-GPU copy completed, converting..." << std::endl;
            firstFrame = false;
        }
        
        // Use converter on encoder GPU side
        target = converter.Convert(crossGPUTextureEncoder);
        if (!target) {
            std::cerr << "[Encoder] Converter failed to convert texture" << std::endl;
            return;
        }
        
        if (firstFrame) {
            std::cout << "[Encoder] Conversion successful, calling NVENC..." << std::endl;
        }
    }
    else if (vendor == EncoderVendor::MF_GENERIC) { // Intel: skip VideoProcessor, use BGRA directly
        target = texture;
    } else {
        // NVIDIA/AMD: Use VideoProcessor for BGRA->NV12 conversion
        target = converter.Convert(texture);
        if (!target) return;
    }

    if (vendor == EncoderVendor::NVIDIA) {
        // std::cout << "[Encoder] Encoding with NVENC..." << std::endl;
        EncodeNVIDIA(target, onPacketReady);
        // std::cout << "[Encoder] NVENC encode completed" << std::endl;
    }
    else if (vendor == EncoderVendor::AMD) EncodeAMD(target, onPacketReady);
    else if (vendor == EncoderVendor::MF_GENERIC) EncodeMF(target, context, onPacketReady);
}

bool HardwareEncoder::InitNVIDIA(ID3D11Device* device) {
    std::cout << "[NVENC] InitNVIDIA starting..." << std::endl;
    
    nvFunctionList = new NV_ENCODE_API_FUNCTION_LIST();
    auto nv = static_cast<NV_ENCODE_API_FUNCTION_LIST*>(nvFunctionList);
    nv->version = NV_ENCODE_API_FUNCTION_LIST_VER;
    
    std::cout << "[NVENC] Loading nvEncodeAPI64.dll..." << std::endl;
    
    // Try multiple loading strategies
    HMODULE hLib = LoadLibraryA("nvEncodeAPI64.dll");
    if (!hLib) {
        DWORD error = GetLastError();
        std::cerr << "[NVENC] LoadLibrary failed with error: " << error << std::endl;
        
        // Try with full path
        std::cout << "[NVENC] Trying full system32 path..." << std::endl;
        char sysPath[MAX_PATH];
        GetSystemDirectoryA(sysPath, MAX_PATH);
        std::string fullPath = std::string(sysPath) + "\\nvEncodeAPI64.dll";
        std::cout << "[NVENC] Path: " << fullPath << std::endl;
        
        hLib = LoadLibraryA(fullPath.c_str());
        if (!hLib) {
            error = GetLastError();
            std::cerr << "[NVENC] LoadLibrary (full path) failed with error: " << error << std::endl;
            
            if (error == 126) {
                std::cerr << "[NVENC] Error 126: DLL or its dependencies not found" << std::endl;
                std::cerr << "[NVENC] Install latest NVIDIA drivers from nvidia.com/drivers" << std::endl;
            } else if (error == 193) {
                std::cerr << "[NVENC] Error 193: Not a valid Win64 application (architecture mismatch)" << std::endl;
            }
            
            return false;
        }
    }
    
    typedef NVENCSTATUS(NVENCAPI *NvEncCreate)(NV_ENCODE_API_FUNCTION_LIST*);
    auto createInstance = (NvEncCreate)GetProcAddress(hLib, "NvEncodeAPICreateInstance");
    if (!createInstance || createInstance(nv) != NV_ENC_SUCCESS) {
        std::cerr << "[NVENC] Failed to create encoder instance" << std::endl;
        return false;
    }
    
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    params.device = device;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.apiVersion = NVENCAPI_VERSION;
    NVENCSTATUS openStatus = nv->nvEncOpenEncodeSessionEx(&params, &nvEncoder);
    if (openStatus != NV_ENC_SUCCESS) {
        std::cerr << "[NVENC] Failed to open encode session: " << openStatus << std::endl;
        return false;
    }
    
    NV_ENC_CONFIG encodeConfig = {}; // Manual config for better compatibility
    memset(&encodeConfig, 0, sizeof(NV_ENC_CONFIG));
    encodeConfig.version = NV_ENC_CONFIG_VER;
    encodeConfig.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
    encodeConfig.gopLength = NVENC_INFINITE_GOPLENGTH;
    encodeConfig.frameIntervalP = 1;
    encodeConfig.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    encodeConfig.mvPrecision = NV_ENC_MV_PRECISION_QUARTER_PEL;
    
    // Rate control
    encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    encodeConfig.rcParams.averageBitRate = 30000000;
    encodeConfig.rcParams.maxBitRate = 30000000;
    encodeConfig.rcParams.vbvBufferSize = 30000000;
    encodeConfig.rcParams.vbvInitialDelay = 30000000;
    
    // H.264 specific settings
    encodeConfig.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    encodeConfig.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    encodeConfig.encodeCodecConfig.h264Config.outputBufferingPeriodSEI = 1;
    encodeConfig.encodeCodecConfig.h264Config.outputPictureTimingSEI = 1;
    encodeConfig.encodeCodecConfig.h264Config.level = NV_ENC_LEVEL_AUTOSELECT;
    encodeConfig.encodeCodecConfig.h264Config.chromaFormatIDC = 1;
    
    NV_ENC_INITIALIZE_PARAMS initParams = {};
    memset(&initParams, 0, sizeof(NV_ENC_INITIALIZE_PARAMS));
    initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initParams.presetGUID = NV_ENC_PRESET_P4_GUID;
    initParams.encodeWidth = width;
    initParams.encodeHeight = height;
    initParams.darWidth = width;
    initParams.darHeight = height;
    initParams.frameRateNum = 60;
    initParams.frameRateDen = 1;
    initParams.enablePTD = 1;
    initParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    initParams.encodeConfig = &encodeConfig;
    
    std::cout << "[NVENC] Initializing encoder..." << std::endl;
    NVENCSTATUS initStatus = nv->nvEncInitializeEncoder(nvEncoder, &initParams);
    if (initStatus != NV_ENC_SUCCESS) {
        std::cerr << "[NVENC] nvEncInitializeEncoder FAILED: " << initStatus << std::endl;
        return false;
    }
    
    std::cout << "[NVENC] Encoder initialized, creating input texture..." << std::endl;
    
    D3D11_TEXTURE2D_DESC nvInputDesc = {};
    nvInputDesc.Width = width;
    nvInputDesc.Height = height;
    nvInputDesc.MipLevels = 1;
    nvInputDesc.ArraySize = 1;
    nvInputDesc.Format = DXGI_FORMAT_NV12;
    nvInputDesc.SampleDesc.Count = 1;
    nvInputDesc.Usage = D3D11_USAGE_DEFAULT;
    nvInputDesc.BindFlags = 0;
    nvInputDesc.CPUAccessFlags = 0;
    
    HRESULT hrTex = device->CreateTexture2D(&nvInputDesc, nullptr, &nvInputTexture);
    if (FAILED(hrTex)) {
        std::cerr << "[NVENC] Failed to create input texture: 0x" << std::hex << hrTex << std::dec << std::endl;
        return false;
    }
    
    // Register the dedicated texture
    NV_ENC_REGISTER_RESOURCE reg = { NV_ENC_REGISTER_RESOURCE_VER };
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.resourceToRegister = nvInputTexture;
    reg.width = width;
    reg.height = height;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    
    NVENCSTATUS regStatus = nv->nvEncRegisterResource(nvEncoder, &reg);
    if (regStatus != NV_ENC_SUCCESS) {
        std::cerr << "[NVENC] Register resource failed: " << regStatus << std::endl;
        return false;
    }
    nvRegisteredResource = reg.registeredResource;
    
    std::cout << "[NVENC] Initialized successfully" << std::endl;
    return true;
}

void HardwareEncoder::EncodeNVIDIA(ID3D11Texture2D* texture, EncodedPacketCallback callback) {
    auto nv = static_cast<NV_ENCODE_API_FUNCTION_LIST*>(nvFunctionList);
    
    // Copy converted NV12 texture to dedicated NVENC input texture
    if (encoderDevice) {
        encoderContext->CopyResource(nvInputTexture, texture);
    } else {
        ComPtr<ID3D11DeviceContext> ctx;
        devicePtr->GetImmediateContext(&ctx);
        ctx->CopyResource(nvInputTexture, texture);
    }
    
    NV_ENC_MAP_INPUT_RESOURCE map = { NV_ENC_MAP_INPUT_RESOURCE_VER };
    map.registeredResource = nvRegisteredResource;
    
    NVENCSTATUS mapStatus = nv->nvEncMapInputResource(nvEncoder, &map);
    if (mapStatus != NV_ENC_SUCCESS) {
        std::cerr << "[NVENC] Map failed: " << mapStatus << std::endl;
        return;
    }
    
    NV_ENC_CREATE_BITSTREAM_BUFFER bitbuf = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
    nv->nvEncCreateBitstreamBuffer(nvEncoder, &bitbuf);
    
    NV_ENC_PIC_PARAMS pic = { NV_ENC_PIC_PARAMS_VER };
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.inputWidth = width;
    pic.inputHeight = height;
    pic.outputBitstream = bitbuf.bitstreamBuffer;
    static int nvFrameCount = 0;
    nvFrameCount++;
    if (nvFrameCount == 1) pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
    
    NVENCSTATUS encStatus = nv->nvEncEncodePicture(nvEncoder, &pic);
    // std::cout << "[NVENC] nvEncEncodePicture returned: " << encStatus << std::endl;
    
    // std::cout << "[NVENC] Locking bitstream..." << std::endl;
    
    NV_ENC_LOCK_BITSTREAM lock = { NV_ENC_LOCK_BITSTREAM_VER };
    lock.outputBitstream = bitbuf.bitstreamBuffer;
    if (nv->nvEncLockBitstream(nvEncoder, &lock) == NV_ENC_SUCCESS) {
        // std::cout << "[NVENC] Bitstream locked, size: " << lock.bitstreamSizeInBytes << " bytes" << std::endl;
        if (lock.bitstreamSizeInBytes > 0) callback((const uint8_t*)lock.bitstreamBufferPtr, lock.bitstreamSizeInBytes);
        nv->nvEncUnlockBitstream(nvEncoder, lock.outputBitstream);
    }
    nv->nvEncDestroyBitstreamBuffer(nvEncoder, bitbuf.bitstreamBuffer);
    nv->nvEncUnmapInputResource(nvEncoder, map.mappedResource); 
}

// =============================================================
// AMD IMPL (Preserved)
// =============================================================
static amf::AMFFactory* g_AMFFactory = nullptr;

bool HardwareEncoder::InitAMD(ID3D11Device* device) {
    HMODULE hLib = LoadLibraryW(AMF_DLL_NAME); 
    if (!hLib) return false;
    typedef AMF_RESULT (AMF_CDECL_CALL *AMFInit_Fn)(amf_uint64, amf::AMFFactory**);
    AMFInit_Fn initFn = (AMFInit_Fn)GetProcAddress(hLib, AMF_INIT_FUNCTION_NAME);
    if (!initFn || initFn(AMF_FULL_VERSION, &g_AMFFactory) != AMF_OK) return false;
    amf::AMFContextPtr context;
    g_AMFFactory->CreateContext(&context);
    context->InitDX11(device);
    amfContext = new amf::AMFContextPtr(context); 
    amf::AMFComponentPtr component;
    g_AMFFactory->CreateComponent(context, AMFVideoEncoderVCE_AVC, &component);
    component->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY);
    component->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, 30000000); 
    component->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, 30000000);
    component->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, ::AMFConstructSize(width, height));
    component->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, ::AMFConstructRate(60, 1));
    component->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, 60); // Insert IDR with headers every 60 frames
    component->SetProperty(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, 60); // Insert headers every 60 frames
    if (component->Init(amf::AMF_SURFACE_NV12, width, height) != AMF_OK) return false;
    amfComponent = new amf::AMFComponentPtr(component);
    std::cout << "[Encoder] AMF encoder initialized with header insertion" << std::endl;
    return true;
}

void HardwareEncoder::EncodeAMD(ID3D11Texture2D* texture, EncodedPacketCallback callback) {
    auto comp = *(static_cast<amf::AMFComponentPtr*>(amfComponent));
    auto ctx = *(static_cast<amf::AMFContextPtr*>(amfContext));
    amf::AMFSurfacePtr* pSurface = static_cast<amf::AMFSurfacePtr*>(amfCachedSurface);
    if (!pSurface) {
        amf::AMFSurfacePtr newSurface;
        if (ctx->CreateSurfaceFromDX11Native(texture, &newSurface, nullptr) != AMF_OK) return;
        amfCachedSurface = new amf::AMFSurfacePtr(newSurface);
        pSurface = static_cast<amf::AMFSurfacePtr*>(amfCachedSurface);
    }
    amf::AMFSurfacePtr surface = *pSurface;
    static amf_pts pts = 0;
    surface->SetPts(pts++);
    static int amfFrameCount = 0;
    amfFrameCount++;
    if (amfFrameCount == 1) surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
    if (comp->SubmitInput(surface) == AMF_INPUT_FULL) return;
    amf::AMFDataPtr data;
    if (comp->QueryOutput(&data) == AMF_OK && data) {
        amf::AMFBufferPtr buffer(data);
        if (buffer && buffer->GetSize() > 0) callback((const uint8_t*)buffer->GetNative(), buffer->GetSize());
    }
}

// =============================================================
// INTEL / MEDIA FOUNDATION IMPLEMENTATION (DEBUG MODE)
// =============================================================

// ... (Keep existing headers/includes) ...

// ... Keep headers ...

bool HardwareEncoder::InitMF(ID3D11Device* device) {
    std::cout << "MF: InitMF Start (Auto-Discovery with Loop)" << std::endl;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IMFActivate** ppActivate = nullptr;
    UINT32 count = 0;
    
    MFT_REGISTER_TYPE_INFO info = { 0 };
    info.guidMajorType = MFMediaType_Video;
    info.guidSubtype = MFVideoFormat_H264;

    UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
    
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, NULL, &info, &ppActivate, &count);

    if (FAILED(hr) || count == 0) {
        std::cerr << "MF: No Encoders found." << std::endl;
        return false;
    }

    std::cout << "MF: Found " << count << " encoder(s)" << std::endl;

    bool activated = false;
    int workingEncoderIndex = -1;
    
    for (UINT32 i = 0; i < count; i++) {
        std::cout << "MF: Trying Encoder #" << i << "..." << std::endl;
        
        ComPtr<IMFTransform> testTransform;
        if (FAILED(ppActivate[i]->ActivateObject(IID_PPV_ARGS(&testTransform)))) {
            std::cerr << "MF: Encoder #" << i << " failed to activate." << std::endl;
            continue;
        }
        
        // Test if this encoder can actually be configured
        ComPtr<IMFMediaType> testOutputType;
        MFCreateMediaType(&testOutputType);
        testOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        testOutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        testOutputType->SetUINT32(MF_MT_AVG_BITRATE, 5000000);
        MFSetAttributeSize(testOutputType.Get(), MF_MT_FRAME_SIZE, Align16(width), Align16(height));
        MFSetAttributeRatio(testOutputType.Get(), MF_MT_FRAME_RATE, 60, 1);
        
        if (SUCCEEDED(testTransform->SetOutputType(0, testOutputType.Get(), 0))) {
            // Check if it exposes input types
            ComPtr<IMFMediaType> testInputType;
            if (SUCCEEDED(testTransform->GetInputAvailableType(0, 0, &testInputType))) {
                std::cout << "MF: Encoder #" << i << " is functional!" << std::endl;
                mfTransform = testTransform;
                activated = true;
                workingEncoderIndex = i;
                break;
            } else {
                std::cerr << "MF: Encoder #" << i << " has no input types (broken)." << std::endl;
            }
        } else {
            std::cerr << "MF: Encoder #" << i << " rejected output type." << std::endl;
        }
    }

    for (UINT32 i = 0; i < count; i++) ppActivate[i]->Release();
    CoTaskMemFree(ppActivate);

    if (!activated) return false;

    int alignedWidth = Align16(width);
    int alignedHeight = Align16(height);
    std::cout << "MF: Aligned Size: " << alignedWidth << "x" << alignedHeight << std::endl;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // Always stage as BGRA from capture
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &stagingTexture))) return false;

    std::cout << "MF: Setting Output Type..." << std::endl;
    ComPtr<IMFMediaType> outputType;
    
    if (SUCCEEDED(mfTransform->GetOutputAvailableType(0, 0, &outputType))) { // Use encoder defaults
        std::cout << "MF: Using encoder's default output type" << std::endl;
        // Override only essential properties
        MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, alignedWidth, alignedHeight);
        MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, 60, 1);
        outputType->SetUINT32(MF_MT_AVG_BITRATE, 5000000);
    } else {
        std::cout << "MF: Creating custom output type" << std::endl;
        MFCreateMediaType(&outputType);
        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, alignedWidth, alignedHeight);
        MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, 60, 1);
        outputType->SetUINT32(MF_MT_AVG_BITRATE, 5000000);
        outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    }

    if (FAILED(mfTransform->SetOutputType(0, outputType.Get(), 0))) {
        std::cerr << "MF: SetOutputType failed!" << std::endl;
        return false;
    }
    
    std::cout << "MF: Output type set successfully!" << std::endl;

    std::cout << "MF: Negotiating Input Type..." << std::endl;
    ComPtr<IMFMediaType> inputType;
    
    int alignedW = Align16(width);
    int alignedH = Align16(height);
    
    std::cout << "MF: Querying supported input formats..." << std::endl;
    for (DWORD i = 0; i < 10; i++) {
        ComPtr<IMFMediaType> availType;
        if (SUCCEEDED(mfTransform->GetInputAvailableType(0, i, &availType))) {
            GUID subtype;
            availType->GetGUID(MF_MT_SUBTYPE, &subtype);
            
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(availType.Get(), MF_MT_FRAME_SIZE, &w, &h);
            
            if (i < 5) { // Print first few formats
                char subtypeStr[40];
                if (subtype == MFVideoFormat_NV12) strcpy(subtypeStr, "NV12");
                else if (subtype == MFVideoFormat_RGB32) strcpy(subtypeStr, "RGB32");
                else if (subtype == MFVideoFormat_YUY2) strcpy(subtypeStr, "YUY2");
                else if (subtype == MFVideoFormat_IYUV) strcpy(subtypeStr, "IYUV");
                else sprintf(subtypeStr, "Unknown");
                
                std::cout << "  Type " << i << ": " << subtypeStr << " " << w << "x" << h << std::endl;
            }
            
            // Try to use the first one that matches our aligned size or close
            if (!inputType) {
                inputType = availType;
                std::cout << "MF: Selecting first available type" << std::endl;
            }
        } else {
            break;
        }
    }
    
    if (!inputType) {
        std::cerr << "MF: No input types available!" << std::endl;
        return false;
    }
    
    // Override with our dimensions
    MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, alignedW, alignedH);
    MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, 60, 1);
    
    if (FAILED(mfTransform->SetInputType(0, inputType.Get(), 0))) {
        std::cerr << "MF: SetInputType failed with available type." << std::endl;
        return false;
    }
    
    GUID selectedSubtype;
    inputType->GetGUID(MF_MT_SUBTYPE, &selectedSubtype);
    useCPUConversion = (selectedSubtype == MFVideoFormat_NV12);
    
    std::cout << "MF: Input type accepted! CPU conversion: " << (useCPUConversion ? "YES" : "NO") << std::endl;

    std::cout << "MF: Begin Streaming..." << std::endl;
    if (FAILED(mfTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))) return false;
    
    std::cout << "MF: Init Success!" << std::endl;
    return true;
}

void HardwareEncoder::EncodeMF(ID3D11Texture2D* texture, ID3D11DeviceContext* ctx, EncodedPacketCallback callback) {
    if (!stagingTexture || !mfTransform) return;

    // 1. Copy GPU -> CPU Staging
    ctx->CopyResource(stagingTexture, texture);
    
    // --- FORCE GPU SYNC ---
    // Ensure the copy completes before we map it
    ctx->Flush(); 

    // 2. Map
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(ctx->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &map))) {
        
        // --- DATA INTEGRITY CHECK (Run once per second) ---
        static int debugFrame = 0;
        debugFrame++;
        if (debugFrame % 60 == 0) {
            BYTE* check = (BYTE*)map.pData;
            int sum = 0;
            // Sum first 100 pixels to see if they are empty
            for(int i=0; i<100; i++) sum += check[i];
            
            std::cout << "MF: Frame " << debugFrame 
                      << " | Pitch: " << map.RowPitch 
                      << " | 1st Byte: " << (int)check[0] 
                      << " | Sum(100): " << sum << std::endl;
        }
        // --------------------------------------------------

        ComPtr<IMFMediaBuffer> buffer;
        
        if (!useCPUConversion) {
            // PATH 1: MF accepts RGB32 (BGRA) directly - need to pad to aligned size
            int alignedW = Align16(width);
            int alignedH = Align16(height);
            DWORD bufLen = alignedW * alignedH * 4;
            MFCreateMemoryBuffer(bufLen, &buffer);
            
            BYTE* pBufData = nullptr;
            if (SUCCEEDED(buffer->Lock(&pBufData, nullptr, nullptr))) {
                 BYTE* src = (BYTE*)map.pData;
                 BYTE* dst = pBufData;

                 // Copy actual frame rows
                 for (int y = 0; y < height; y++) {
                     memcpy(dst, src, width * 4);
                     src += map.RowPitch;
                     dst += alignedW * 4; // Use aligned width for destination
                 }
                 
                 // Pad remaining rows with black (0,0,0,255)
                 for (int y = height; y < alignedH; y++) {
                     memset(dst, 0, alignedW * 4);
                     dst += alignedW * 4;
                 }
                 
                 buffer->Unlock();
                 buffer->SetCurrentLength(bufLen);
            }
        } else {
            // PATH 2: CPU BGRA->NV12 conversion with padding
            int alignedW = Align16(width);
            int alignedH = Align16(height);
            DWORD bufLen = alignedW * alignedH * 3 / 2;
            MFCreateMemoryBuffer(bufLen, &buffer);
            
            BYTE* pBufData = nullptr;
            if (SUCCEEDED(buffer->Lock(&pBufData, nullptr, nullptr))) {
                BYTE* src = (BYTE*)map.pData;
                BYTE* yPlane = pBufData;
                BYTE* uvPlane = pBufData + (alignedW * alignedH);
                
                // DEBUG: Check first few source pixels
                static int convDebug = 0;
                convDebug++;
                if (convDebug <= 5 || convDebug % 60 == 0) {
                    std::cout << "[CPU-Conv] Frame " << convDebug << " - First 5 BGRA pixels:" << std::endl;
                    for (int i = 0; i < 5; i++) {
                        int idx = i * 4;
                        std::cout << "  Pixel " << i << ": B=" << (int)src[idx] 
                                 << " G=" << (int)src[idx+1] 
                                 << " R=" << (int)src[idx+2] 
                                 << " A=" << (int)src[idx+3] << std::endl;
                    }
                    // Also check center of screen
                    int centerIdx = (height/2) * map.RowPitch + (width/2) * 4;
                    std::cout << "  Center pixel: B=" << (int)src[centerIdx] 
                             << " G=" << (int)src[centerIdx+1] 
                             << " R=" << (int)src[centerIdx+2] << std::endl;
                }
                
                // Convert BGRA to NV12 (CPU) - actual frame area
                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < width; x++) {
                        int idx = y * map.RowPitch + x * 4;
                        BYTE B = src[idx];
                        BYTE G = src[idx + 1];
                        BYTE R = src[idx + 2];
                        
                        // BT.709 for HD
                        int Y = (66 * R + 129 * G + 25 * B + 128) / 256 + 16;
                        yPlane[y * alignedW + x] = (BYTE)Y;
                        
                        // UV subsampling (4:2:0)
                        if (y % 2 == 0 && x % 2 == 0) {
                            int U = (-38 * R - 74 * G + 112 * B + 128) / 256 + 128;
                            int V = (112 * R - 94 * G - 18 * B + 128) / 256 + 128;
                            int uvIdx = (y / 2) * alignedW + (x & ~1);
                            uvPlane[uvIdx] = (BYTE)U;
                            uvPlane[uvIdx + 1] = (BYTE)V;
                        }
                    }
                    // Pad right edge of each row
                    for (int x = width; x < alignedW; x++) {
                        yPlane[y * alignedW + x] = 16; // Black Y
                    }
                }
                
                // Pad bottom rows
                for (int y = height; y < alignedH; y++) {
                    for (int x = 0; x < alignedW; x++) {
                        yPlane[y * alignedW + x] = 16;
                        if (y % 2 == 0 && x % 2 == 0) {
                            int uvIdx = (y / 2) * alignedW + x;
                            uvPlane[uvIdx] = 128; // Neutral UV
                            uvPlane[uvIdx + 1] = 128;
                        }
                    }
                }
                
                // DEBUG: Verify NV12 output
                static int nv12DebugFrame = 0;
                nv12DebugFrame++;
                if (nv12DebugFrame % 60 == 0) {
                    int sum = 0;
                    for (int i = 0; i < 100; i++) sum += yPlane[i];
                    std::cout << "MF-NV12: Frame " << nv12DebugFrame 
                             << " | 1st Y: " << (int)yPlane[0] 
                             << " | Sum(100): " << sum << std::endl;
                }
                
                buffer->Unlock();
                buffer->SetCurrentLength(bufLen);
            }
        }

        ctx->Unmap(stagingTexture, 0);

        // 3. Feed Encoder
        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buffer.Get());

        static LONGLONG pts = 10000000;
        sample->SetSampleTime(pts);
        sample->SetSampleDuration(166666);
        pts += 166666;

        mfTransform->ProcessInput(0, sample.Get(), 0);
    }

    // 4. Drain Output
    while (true) {
        MFT_OUTPUT_STREAM_INFO info;
        mfTransform->GetOutputStreamInfo(0, &info);

        ComPtr<IMFSample> outSample;
        ComPtr<IMFMediaBuffer> outBuffer;
        MFCreateMemoryBuffer(info.cbSize, &outBuffer);
        MFCreateSample(&outSample);
        outSample->AddBuffer(outBuffer.Get());

        MFT_OUTPUT_DATA_BUFFER outputData = {};
        outputData.dwStreamID = 0;
        outputData.pSample = outSample.Get();

        DWORD status = 0;
        HRESULT hr = mfTransform->ProcessOutput(0, 1, &outputData, &status);

        if (SUCCEEDED(hr)) {
            BYTE* pData = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(outBuffer->Lock(&pData, nullptr, &len))) {
                if (len > 0) {
                    static int pktCount = 0;
                    pktCount++;
                    if (pktCount <= 5 || pktCount % 60 == 0) {
                        std::cout << "[MF-Output] Packet #" << pktCount << " size: " << len << std::endl;
                    }
                    callback(pData, len);
                }
                outBuffer->Unlock();
            }
            if (outputData.pSample) outputData.pSample->Release();
            if (outputData.pEvents) outputData.pEvents->Release();
        } 
        else {
            static bool errorLogged = false;
            if (!errorLogged && hr != MF_E_TRANSFORM_NEED_MORE_INPUT) {
                std::cerr << "[MF-Output] ProcessOutput failed: 0x" << std::hex << hr << std::dec << std::endl;
                if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                    std::cout << "[MF-Output] (Need more input - normal)" << std::endl;
                }
                errorLogged = true;
            }
            break; 
        }
    }
}