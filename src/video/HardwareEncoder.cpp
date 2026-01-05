#include "HardwareEncoder.h"
#include <iostream>
#include <string>

// --- Microsoft Headers ---
#include <wrl/client.h> 
#include <dxgi.h>      

using Microsoft::WRL::ComPtr;

// --- NVIDIA Headers ---
#include "nvEncodeAPI.h"

// --- AMD Headers ---
#include "AMD/include/core/Factory.h"
#include "AMD/include/core/Context.h"
#include "AMD/include/components/VideoEncoderVCE.h"
#include "AMD/common/AMFSTL.h" 

#pragma comment(lib, "ole32.lib")

// #pragma comment(lib, "amfrt64.lib")

// --- Vendor IDs ---
const int VENDOR_ID_NVIDIA = 0x10DE;
const int VENDOR_ID_AMD    = 0x1002;

// --- GUID FIX ---
static const GUID NV_ENC_PRESET_LOW_LATENCY_HQ_GUID = 
{ 0x60e44551, 0x22c4, 0x4d31, { 0x8b, 0x24, 0x50, 0xab, 0x76, 0x3d, 0x9f, 0x91 } };

HardwareEncoder::HardwareEncoder() {}

HardwareEncoder::~HardwareEncoder() {
    Cleanup();
}

void HardwareEncoder::Cleanup() {
if (vendor == EncoderVendor::NVIDIA && nvEncoder) {
        auto funcs = static_cast<NV_ENCODE_API_FUNCTION_LIST*>(nvFunctionList);
        if (funcs) {
            // NEW: Unregister the persistent resource
            if (nvRegisteredResource) {
                funcs->nvEncUnregisterResource(nvEncoder, nvRegisteredResource);
                nvRegisteredResource = nullptr;
            }
            funcs->nvEncDestroyEncoder(nvEncoder);
        }
    }
    if (vendor == EncoderVendor::AMD) {
        // Clean up the cached surface
        if (amfCachedSurface) {
            delete static_cast<amf::AMFSurfacePtr*>(amfCachedSurface);
            amfCachedSurface = nullptr;
        }
        if (amfComponent) {
            static_cast<amf::AMFComponent*>(amfComponent)->Terminate();
        }
    }
}


// 1. INITIALIZATION & DETECTION
bool HardwareEncoder::Initialize(ID3D11Device* device, int w, int h) {
    width = w;
    height = h;

    // 1. Initialize the GPU Converter (BGRA -> NV12)
    if (!converter.Initialize(device, width, height)) {
        std::cerr << "[Encoder] Failed to initialize GPU Color Converter!" << std::endl;
        return false;
    }
    
    ComPtr<ID3D11Device> dev = device;
    ComPtr<IDXGIDevice> dxgiDevice; dev.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter; dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC desc; adapter->GetDesc(&desc);

    if (desc.VendorId == VENDOR_ID_NVIDIA) {
        vendor = EncoderVendor::NVIDIA;
        return InitNVIDIA(device);
    } else if (desc.VendorId == VENDOR_ID_AMD) {
        vendor = EncoderVendor::AMD;
        return InitAMD(device);
    }
    return false;
}

// 2. MAIN ENCODE LOOP
std::vector<uint8_t> HardwareEncoder::EncodeFrame(ID3D11Texture2D* texture, ID3D11DeviceContext* context) {
    
    ID3D11Texture2D* targetInternalTexture = nullptr;

    if (texture) {
        // CASE A: NEW FRAME
        // We have fresh desktop data. Run the conversion (Blt).
        // This updates the VideoProcessor's internal output texture.
        targetInternalTexture = converter.Convert(texture);
    } 
    else {
        // CASE B: STATIC SCREEN (Heartbeat)
        // No new desktop data. 
        // We just grab the pointer to the internal texture (which holds the LAST frame).
        targetInternalTexture = converter.GetOutputTexture();
    }

    // Safety check: If we time out on the VERY first frame, this might be null.
    if (!targetInternalTexture) {
        return {}; 
    }

    // 2. ENCODE 
    // The encoder doesn't know the difference. It just sees valid NV12 data.
    if (vendor == EncoderVendor::NVIDIA) {
        return EncodeNVIDIA(targetInternalTexture);
    } else if (vendor == EncoderVendor::AMD) {
        return EncodeAMD(targetInternalTexture);
    }
    return {};
}


// 3. NVIDIA IMPLEMENTATION
bool HardwareEncoder::InitNVIDIA(ID3D11Device* device) {
    nvFunctionList = new NV_ENCODE_API_FUNCTION_LIST();
    auto nv = static_cast<NV_ENCODE_API_FUNCTION_LIST*>(nvFunctionList);
    nv->version = NV_ENCODE_API_FUNCTION_LIST_VER;

    HMODULE hLib = LoadLibrary("nvEncodeAPI64.dll");
    if (!hLib) { std::cerr << "Failed to load nvEncodeAPI64.dll" << std::endl; return false; }

    typedef NVENCSTATUS(NVENCAPI *NvEncCreate)(NV_ENCODE_API_FUNCTION_LIST*);
    auto createInstance = (NvEncCreate)GetProcAddress(hLib, "NvEncodeAPICreateInstance");
    if (!createInstance || createInstance(nv) != NV_ENC_SUCCESS) return false;

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    params.device = device;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.apiVersion = NVENCAPI_VERSION;

    if (nv->nvEncOpenEncodeSessionEx(&params, &nvEncoder) != NV_ENC_SUCCESS) return false;

    // --- CONFIGURATION FIX ---
    NV_ENC_INITIALIZE_PARAMS initParams = { NV_ENC_INITIALIZE_PARAMS_VER };
    initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initParams.presetGUID = NV_ENC_PRESET_LOW_LATENCY_HQ_GUID;
    initParams.encodeWidth = width;
    initParams.encodeHeight = height;
    initParams.darWidth = width;
    initParams.darHeight = height;
    initParams.frameRateNum = 60;
    initParams.frameRateDen = 1;
    initParams.enablePTD = 1;

    // Corrected: Use NV_ENC_PRESET_CONFIG wrapper
    NV_ENC_PRESET_CONFIG presetConfig = { NV_ENC_PRESET_CONFIG_VER };
    
    // Retrieve the preset config into the wrapper
    nv->nvEncGetEncodePresetConfig(nvEncoder, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_LOW_LATENCY_HQ_GUID, &presetConfig);
    
    // Get a pointer to the actual config inside the wrapper
    NV_ENC_CONFIG* pConfig = &presetConfig.presetCfg;

    // Modify the config
    pConfig->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    pConfig->rcParams.averageBitRate = 5000000;
    pConfig->rcParams.vbvBufferSize = 5000000;
    pConfig->gopLength = NVENC_INFINITE_GOPLENGTH; 

    // Point initParams to the modified config
    initParams.encodeConfig = pConfig;

    return nv->nvEncInitializeEncoder(nvEncoder, &initParams) == NV_ENC_SUCCESS;
}

std::vector<uint8_t> HardwareEncoder::EncodeNVIDIA(ID3D11Texture2D* texture) {
    auto nv = static_cast<NV_ENCODE_API_FUNCTION_LIST*>(nvFunctionList);
    std::vector<uint8_t> packet;

    // --- OPTIMIZATION: REGISTER ONCE ---
    if (!nvRegisteredResource) {
        NV_ENC_REGISTER_RESOURCE reg = { NV_ENC_REGISTER_RESOURCE_VER };
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        reg.resourceToRegister = texture; // This is our stable NV12 texture
        reg.width = width;
        reg.height = height;
        reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12; 
        
        if (nv->nvEncRegisterResource(nvEncoder, &reg) != NV_ENC_SUCCESS) {
            std::cerr << "[NVIDIA] Registration Failed!" << std::endl;
            return packet;
        }
        
        // Save the handle for all future frames
        nvRegisteredResource = reg.registeredResource;
    }

    // --- 1. MAP (Fast) ---
    NV_ENC_MAP_INPUT_RESOURCE map = { NV_ENC_MAP_INPUT_RESOURCE_VER };
    map.registeredResource = nvRegisteredResource; // Use the cached handle
    
    if (nv->nvEncMapInputResource(nvEncoder, &map) != NV_ENC_SUCCESS) {
        return packet; 
    }

    // --- 2. ENCODE ---
    NV_ENC_CREATE_BITSTREAM_BUFFER bitbuf = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
    nv->nvEncCreateBitstreamBuffer(nvEncoder, &bitbuf);

    NV_ENC_PIC_PARAMS pic = { NV_ENC_PIC_PARAMS_VER };
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.inputWidth = width;
    pic.inputHeight = height;
    pic.outputBitstream = bitbuf.bitstreamBuffer;
    nv->nvEncEncodePicture(nvEncoder, &pic);

    // --- 3. GET DATA ---
    NV_ENC_LOCK_BITSTREAM lock = { NV_ENC_LOCK_BITSTREAM_VER };
    lock.outputBitstream = bitbuf.bitstreamBuffer;
    nv->nvEncLockBitstream(nvEncoder, &lock);

    packet.resize(lock.bitstreamSizeInBytes);
    memcpy(packet.data(), lock.bitstreamBufferPtr, lock.bitstreamSizeInBytes);

    nv->nvEncUnlockBitstream(nvEncoder, lock.outputBitstream);

    // --- 4. CLEANUP (But DO NOT Unregister) ---
    nv->nvEncDestroyBitstreamBuffer(nvEncoder, bitbuf.bitstreamBuffer);
    
    // Unmap is required every frame, but Unregister is NOT.
    nv->nvEncUnmapInputResource(nvEncoder, map.mappedResource); 

    return packet;
}


// 4. AMD IMPLEMENTATION
static amf::AMFFactory* g_AMFFactory = nullptr;

bool HardwareEncoder::InitAMD(ID3D11Device* device) {
    HMODULE hLib = LoadLibraryW(AMF_DLL_NAME); 
    if (!hLib) {
        std::cerr << "Failed to load amfrt64.dll" << std::endl;
        return false;
    }

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
    component->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, 5000000);
    component->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, ::AMFConstructSize(width, height));
    component->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, ::AMFConstructRate(60, 1));
    
    if (component->Init(amf::AMF_SURFACE_NV12, width, height) != AMF_OK) {
            std::cerr << "[AMD] Init NV12 Failed." << std::endl;
            return false;
        }
        
    amfComponent = new amf::AMFComponentPtr(component);
    return true;
}

std::vector<uint8_t> HardwareEncoder::EncodeAMD(ID3D11Texture2D* texture) {
    auto comp = *(static_cast<amf::AMFComponentPtr*>(amfComponent));
    auto ctx = *(static_cast<amf::AMFContextPtr*>(amfContext));
    std::vector<uint8_t> packet;

    // --- OPTIMIZATION: CACHE THE WRAPPER ---
    // We cast our void* back to a smart pointer pointer
    amf::AMFSurfacePtr* pSurface = static_cast<amf::AMFSurfacePtr*>(amfCachedSurface);

    // If cache is empty, create it
    if (!pSurface) {
        amf::AMFSurfacePtr newSurface;
        
        // Use our (now safe) NV12 texture
        AMF_RESULT res = ctx->CreateSurfaceFromDX11Native(texture, &newSurface, nullptr); 
        
        if (res != AMF_OK) {
            std::cerr << "[AMD] Surface Creation Failed: " << res << std::endl;
            return packet;
        }

        // Store it on the heap so it persists (and we can store as void*)
        amfCachedSurface = new amf::AMFSurfacePtr(newSurface);
        pSurface = static_cast<amf::AMFSurfacePtr*>(amfCachedSurface);
    }

    // Dereference the pointer to get the actual Smart Pointer object
    amf::AMFSurfacePtr surface = *pSurface;

    // Update Timestamp (Important so encoder knows it's a new frame)
    static amf_pts pts = 0;
    surface->SetPts(pts++);

    // --- SUBMIT ---
    AMF_RESULT res = comp->SubmitInput(surface);
    
    if (res == AMF_INPUT_FULL) {
        return packet; // Drop frame if too fast
    } 

    // --- QUERY ---
    amf::AMFDataPtr data;
    res = comp->QueryOutput(&data);

    if (res == AMF_OK && data) {
        amf::AMFBufferPtr buffer(data);
        if (buffer) {
            packet.resize(buffer->GetSize());
            memcpy(packet.data(), buffer->GetNative(), buffer->GetSize());
        }
    }

    return packet;
}