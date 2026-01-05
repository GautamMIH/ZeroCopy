#include "HardwareDecoder.h"
#include <iostream>
#include <dxgi.h>
#include <vector>

// Media Foundation includes (for NVIDIA)
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <wmcodecdsp.h>

// AMD AMF includes
#include "AMD/include/core/Factory.h"
#include "AMD/include/core/Context.h"
#include "AMD/include/components/VideoDecoderUVD.h"
#include "AMD/common/AMFSTL.h"

const int VENDOR_ID_NVIDIA = 0x10DE;
const int VENDOR_ID_AMD    = 0x1002;
const int VENDOR_ID_INTEL  = 0x8086;

static amf::AMFFactory* g_AMFDecoderFactory = nullptr;

HardwareDecoder::HardwareDecoder() {
}

HardwareDecoder::~HardwareDecoder() {
    Cleanup();
}

void HardwareDecoder::Cleanup() {
    if (vendor == DecoderVendor::AMD) {
        if (amfCachedSurface) {
            delete static_cast<amf::AMFSurfacePtr*>(amfCachedSurface);
            amfCachedSurface = nullptr;
        }
        if (amfComponent) {
            static_cast<amf::AMFComponent*>(amfComponent)->Terminate();
            delete static_cast<amf::AMFComponentPtr*>(amfComponent);
            amfComponent = nullptr;
        }
        if (amfContext) {
            static_cast<amf::AMFContext*>(amfContext)->Terminate();
            delete static_cast<amf::AMFContextPtr*>(amfContext);
            amfContext = nullptr;
        }
    }
    else if (vendor == DecoderVendor::NVIDIA) {
        if (mfTransform) {
            static_cast<IMFTransform*>(mfTransform)->Release();
            mfTransform = nullptr;
        }
        if (mfStagingTexture) {
            mfStagingTexture->Release();
            mfStagingTexture = nullptr;
        }
        if (mfOutputTexture) {
            mfOutputTexture->Release();
            mfOutputTexture = nullptr;
        }
        MFShutdown();
    }
    
    if (outputTexture) {
        outputTexture->Release();
        outputTexture = nullptr;
    }
}

bool HardwareDecoder::Initialize(ID3D11Device* device, int w, int h) {
    width = w;
    height = h;
    devicePtr = device;

    // Detect GPU vendor
    ComPtr<ID3D11Device> dev = device;
    ComPtr<IDXGIDevice> dxgiDevice;
    dev.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC desc;
    adapter->GetDesc(&desc);

    std::cout << "[Decoder] GPU Vendor ID: " << std::hex << desc.VendorId << std::dec << std::endl;

    if (desc.VendorId == VENDOR_ID_AMD) {
        vendor = DecoderVendor::AMD;
        std::wcout << L"[Decoder] Running on AMD GPU (" << desc.Description << L") - using AMF VideoDecoderUVD" << std::endl;
        return InitAMD(device);
    } 
    else if (desc.VendorId == VENDOR_ID_NVIDIA) {
        vendor = DecoderVendor::NVIDIA;
        std::wcout << L"[Decoder] Running on NVIDIA GPU (" << desc.Description << L") - using Media Foundation H/W decoder" << std::endl;
        return InitNVIDIA(device);
    }
    else if (desc.VendorId == VENDOR_ID_INTEL) {
        vendor = DecoderVendor::INTEL;
        std::cout << "[Decoder] Intel GPU detected - zero-copy decode not yet implemented for Intel" << std::endl;
        return false;
    }
    else {
        std::cout << "[Decoder] Unknown GPU vendor - zero-copy decode not available" << std::endl;
        return false;
    }
}

bool HardwareDecoder::InitAMD(ID3D11Device* device) {
    std::cout << "[Decoder] Initializing AMD AMF decoder..." << std::endl;

    // Load AMF library
    HMODULE hLib = LoadLibraryW(AMF_DLL_NAME);
    if (!hLib) {
        std::cerr << "[Decoder] Failed to load AMF library" << std::endl;
        return false;
    }

    // Initialize AMF factory
    typedef AMF_RESULT (AMF_CDECL_CALL *AMFInit_Fn)(amf_uint64, amf::AMFFactory**);
    AMFInit_Fn initFn = (AMFInit_Fn)GetProcAddress(hLib, AMF_INIT_FUNCTION_NAME);
    if (!initFn) {
        std::cerr << "[Decoder] Failed to get AMF init function" << std::endl;
        return false;
    }

    if (initFn(AMF_FULL_VERSION, &g_AMFDecoderFactory) != AMF_OK) {
        std::cerr << "[Decoder] Failed to initialize AMF factory" << std::endl;
        return false;
    }

    // Create AMF context
    amf::AMFContextPtr context;
    if (g_AMFDecoderFactory->CreateContext(&context) != AMF_OK) {
        std::cerr << "[Decoder] Failed to create AMF context" << std::endl;
        return false;
    }

    // Initialize context with D3D11 device
    if (context->InitDX11(device) != AMF_OK) {
        std::cerr << "[Decoder] Failed to initialize AMF context with D3D11" << std::endl;
        return false;
    }

    amfContext = new amf::AMFContextPtr(context);

    // Create decoder component (H.264/AVC)
    amf::AMFComponentPtr component;
    if (g_AMFDecoderFactory->CreateComponent(context, AMFVideoDecoderUVD_H264_AVC, &component) != AMF_OK) {
        std::cerr << "[Decoder] Failed to create AMF decoder component" << std::endl;
        return false;
    }

    // Configure decoder for low latency and Annex B format (raw stream with start codes)
    component->SetProperty(AMF_VIDEO_DECODER_REORDER_MODE, AMF_VIDEO_DECODER_MODE_LOW_LATENCY);
    component->SetProperty(AMF_TIMESTAMP_MODE, AMF_TS_DECODE);
    component->SetProperty(AMF_VIDEO_DECODER_SURFACE_COPY, false); // Zero-copy mode!
    // No EXTRADATA needed - Annex B format includes SPS/PPS inline
    
    // Initialize decoder with NV12 output format
    std::cout << "[Decoder] Initializing decoder: " << width << "x" << height << " NV12" << std::endl;
    if (component->Init(amf::AMF_SURFACE_NV12, width, height) != AMF_OK) {
        std::cerr << "[Decoder] Failed to initialize AMF decoder" << std::endl;
        return false;
    }

    amfComponent = new amf::AMFComponentPtr(component);

    // AMF pads height to 16-pixel alignment (1080 -> 1088)
    int alignedHeight = (height + 15) & ~15;
    
    // Create a compatible output texture for VideoProcessor
    D3D11_TEXTURE2D_DESC outputDesc = {};
    outputDesc.Width = width;
    outputDesc.Height = alignedHeight; // Use aligned height!
    outputDesc.MipLevels = 1;
    outputDesc.ArraySize = 1;
    outputDesc.Format = DXGI_FORMAT_NV12;
    outputDesc.SampleDesc.Count = 1;
    outputDesc.Usage = D3D11_USAGE_DEFAULT;
    outputDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    outputDesc.CPUAccessFlags = 0;
    
    if (FAILED(device->CreateTexture2D(&outputDesc, nullptr, &outputTexture))) {
        std::cerr << "[Decoder] Failed to create output texture" << std::endl;
        return false;
    }

    std::cout << "[Decoder] AMD AMF decoder initialized successfully (Zero-Copy Mode)" << std::endl;
    std::cout << "[Decoder] Resolution: " << width << "x" << height << " (aligned: " << alignedHeight << ")" << std::endl;
    return true;
}

ID3D11Texture2D* HardwareDecoder::Decode(const uint8_t* data, size_t size, ID3D11DeviceContext* ctx) {
    if (!data || size == 0) return nullptr;

    if (vendor == DecoderVendor::AMD) {
        return DecodeAMD(data, size);
    }
    else if (vendor == DecoderVendor::NVIDIA) {
        return DecodeNVIDIA(data, size, ctx);
    }

    return nullptr;
}

ID3D11Texture2D* HardwareDecoder::DecodeAMD(const uint8_t* data, size_t size) {
    auto comp = *(static_cast<amf::AMFComponentPtr*>(amfComponent));
    auto ctx = *(static_cast<amf::AMFContextPtr*>(amfContext));

    // Debug: Check NAL type for keyframes only
    int nalType = 0;
    if (size > 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        nalType = data[4] & 0x1F;
    }
    
    if (nalType == 7 || nalType == 5) {
        std::cout << "[Decoder] Keyframe received: NAL=" << nalType << std::endl;
    }

    // Create AMF buffer from input data
    amf::AMFBufferPtr buffer;
    if (ctx->AllocBuffer(amf::AMF_MEMORY_HOST, size, &buffer) != AMF_OK) {
        std::cerr << "[Decoder] Failed to allocate AMF buffer" << std::endl;
        return nullptr;
    }

    // Copy compressed data to AMF buffer
    memcpy(buffer->GetNative(), data, size);
    buffer->SetSize(size);

    // Set timestamp
    static amf_pts pts = 0;
    buffer->SetPts(pts++);

    // Submit compressed data to decoder
    AMF_RESULT result = comp->SubmitInput(buffer);
    if (result == AMF_INPUT_FULL) {
        // Input full, skip submitting but still try to get output
        if (frameCount < 5) {
            std::cout << "[Decoder] Input buffer full, draining output" << std::endl;
        }
    }
    else if (result != AMF_OK) {
        std::cerr << "[Decoder] SubmitInput failed: " << result << std::endl;
    }

    // ALWAYS try to query output, even if submit failed
    // This drains the decoder pipeline and reduces latency
    amf::AMFDataPtr outputData;
    result = comp->QueryOutput(&outputData);
    
    if (result == AMF_REPEAT) {
        // No output ready yet - this is normal with decode latency
        return nullptr;
    }
    else if (result == AMF_EOF) {
        std::cerr << "[Decoder] AMF_EOF received" << std::endl;
        return nullptr;
    }
    else if (result != AMF_OK || !outputData) {
        return nullptr;
    }

    // Convert AMFData to AMFSurface
    amf::AMFSurfacePtr surface(outputData);
    if (!surface) {
        std::cerr << "[Decoder] Failed to get surface from output data" << std::endl;
        return nullptr;
    }

    // Get D3D11 texture from AMF surface (ZERO-COPY!)
    ID3D11Texture2D* d3d11Texture = (ID3D11Texture2D*)surface->GetPlaneAt(0)->GetNative();
    
    if (!d3d11Texture) {
        std::cerr << "[Decoder] Failed to get D3D11 texture from surface" << std::endl;
        return nullptr;
    }
    
    // Copy AMF texture to compatible output texture (still GPU-only, no CPU copy)
    ComPtr<ID3D11DeviceContext> d3dContext;
    devicePtr->GetImmediateContext(&d3dContext);
    d3dContext->CopyResource(outputTexture, d3d11Texture);

    // Cache the surface to keep it alive
    if (amfCachedSurface) {
        delete static_cast<amf::AMFSurfacePtr*>(amfCachedSurface);
    }
    amfCachedSurface = new amf::AMFSurfacePtr(surface);

    frameCount++;
    if (frameCount == 1) {
        std::cout << "[Decoder] ✓ First frame decoded successfully (Zero-Copy)" << std::endl;
    } else if (frameCount % 60 == 0) {
        std::cout << "[Decoder] " << frameCount << " frames decoded" << std::endl;
    }

    // Return the compatible texture (GPU-to-GPU copy, still zero CPU copy!)
    return outputTexture;
}

ID3D11Texture2D* HardwareDecoder::DrainOutput() {
    if (vendor != DecoderVendor::AMD || !amfComponent) return nullptr;
    
    auto comp = *(static_cast<amf::AMFComponentPtr*>(amfComponent));
    
    // Try to get buffered output
    amf::AMFDataPtr outputData;
    AMF_RESULT result = comp->QueryOutput(&outputData);
    
    if (result != AMF_OK || !outputData) {
        return nullptr;
    }
    
    amf::AMFSurfacePtr surface(outputData);
    if (!surface) return nullptr;
    
    ID3D11Texture2D* d3d11Texture = (ID3D11Texture2D*)surface->GetPlaneAt(0)->GetNative();
    if (!d3d11Texture) return nullptr;
    
    // Cache the surface
    if (amfCachedSurface) {
        delete static_cast<amf::AMFSurfacePtr*>(amfCachedSurface);
    }
    amfCachedSurface = new amf::AMFSurfacePtr(surface);
    
    frameCount++;
    
    return d3d11Texture;
}

// =============================================================
// NVIDIA MEDIA FOUNDATION DECODER IMPLEMENTATION
// =============================================================

inline int Align16(int value) {
    return (value + 15) & ~15;
}

bool HardwareDecoder::InitNVIDIA(ID3D11Device* device) {
    std::cout << "[Decoder] Initializing NVIDIA Media Foundation decoder..." << std::endl;

    MFStartup(MF_VERSION);

    // Create H.264 decoder MFT
    IMFTransform* transform = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, 
                                IID_PPV_ARGS(&transform)))) {
        std::cerr << "[Decoder] Failed to create H.264 decoder MFT" << std::endl;
        return false;
    }

    // Enable low latency mode
    ICodecAPI* codecApi = nullptr;
    if (SUCCEEDED(transform->QueryInterface(IID_PPV_ARGS(&codecApi)))) {
        VARIANT var = {};
        var.vt = VT_UI4;
        var.ulVal = 1;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &var);
        codecApi->Release();
    }

    // Configure input type (H.264)
    IMFMediaType* inputType = nullptr;
    MFCreateMediaType(&inputType);
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    inputType->SetUINT64(MF_MT_FRAME_SIZE, ((UINT64)width << 32) | height);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    
    if (FAILED(transform->SetInputType(0, inputType, 0))) {
        std::cerr << "[Decoder] Failed to set input type" << std::endl;
        inputType->Release();
        transform->Release();
        return false;
    }
    inputType->Release();

    // Configure output type (NV12)
    IMFMediaType* outputType = nullptr;
    MFCreateMediaType(&outputType);
    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    outputType->SetUINT64(MF_MT_FRAME_SIZE, ((UINT64)width << 32) | height);
    
    if (FAILED(transform->SetOutputType(0, outputType, 0))) {
        std::cerr << "[Decoder] Failed to set output type" << std::endl;
        outputType->Release();
        transform->Release();
        return false;
    }
    outputType->Release();

    // Start streaming
    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    mfTransform = transform;

    // Create staging texture (DYNAMIC for CPU write)
    int alignedHeight = Align16(height);
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = width;
    stagingDesc.Height = alignedHeight;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_NV12;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_DYNAMIC;
    stagingDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &mfStagingTexture))) {
        std::cerr << "[Decoder] Failed to create staging texture" << std::endl;
        return false;
    }

    // Create output texture (DEFAULT for GPU usage) - use aligned height
    D3D11_TEXTURE2D_DESC outputDesc = {};
    outputDesc.Width = width;
    outputDesc.Height = alignedHeight; // Use aligned height for proper storage
    outputDesc.MipLevels = 1;
    outputDesc.ArraySize = 1;
    outputDesc.Format = DXGI_FORMAT_NV12;
    outputDesc.SampleDesc.Count = 1;
    outputDesc.Usage = D3D11_USAGE_DEFAULT;
    outputDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    if (FAILED(device->CreateTexture2D(&outputDesc, nullptr, &mfOutputTexture))) {
        std::cerr << "[Decoder] Failed to create output texture" << std::endl;
        return false;
    }
    
    // Also store the aligned output texture in the base class member
    outputTexture = mfOutputTexture;
    outputTexture->AddRef();

    std::cout << "[Decoder] NVIDIA decoder initialized successfully (Hardware Accelerated)" << std::endl;
    std::cout << "[Decoder] Resolution: " << width << "x" << height << " (aligned: " << alignedHeight << ")" << std::endl;
    return true;
}

ID3D11Texture2D* HardwareDecoder::DecodeNVIDIA(const uint8_t* data, size_t size, ID3D11DeviceContext* ctx) {
    IMFTransform* transform = static_cast<IMFTransform*>(mfTransform);
    if (!transform) return nullptr;

    // Create input sample
    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;
    MFCreateMemoryBuffer((DWORD)size, &buffer);
    
    BYTE* pData = nullptr;
    buffer->Lock(&pData, nullptr, nullptr);
    memcpy(pData, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength((DWORD)size);

    MFCreateSample(&sample);
    sample->AddBuffer(buffer);
    
    static LONGLONG timestamp = 0;
    sample->SetSampleTime(timestamp);
    sample->SetSampleDuration(166666);
    timestamp += 166666;

    // Submit input
    HRESULT hr = transform->ProcessInput(0, sample, 0);
    sample->Release();
    buffer->Release();
    
    if (FAILED(hr)) return nullptr;

    // Query output
    while (true) {
        MFT_OUTPUT_STREAM_INFO info;
        transform->GetOutputStreamInfo(0, &info);

        IMFSample* outSample = nullptr;
        IMFMediaBuffer* outBuffer = nullptr;
        MFCreateMemoryBuffer(info.cbSize, &outBuffer);
        MFCreateSample(&outSample);
        outSample->AddBuffer(outBuffer);

        MFT_OUTPUT_DATA_BUFFER outputData = {};
        outputData.dwStreamID = 0;
        outputData.pSample = outSample;
        
        DWORD status = 0;
        hr = transform->ProcessOutput(0, 1, &outputData, &status);

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            outSample->Release();
            outBuffer->Release();
            
            IMFMediaType* type = nullptr;
            MFCreateMediaType(&type);
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            type->SetUINT64(MF_MT_FRAME_SIZE, ((UINT64)width << 32) | height);
            transform->SetOutputType(0, type, 0);
            type->Release();
            continue;
        }
        else if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            outSample->Release();
            outBuffer->Release();
            return nullptr;
        }
        else if (SUCCEEDED(hr)) {
            // Copy decoded data to staging texture
            BYTE* srcPtr = nullptr;
            LONG srcStride = 0;
            
            IMF2DBuffer* buffer2D = nullptr;
            if (SUCCEEDED(outBuffer->QueryInterface(IID_PPV_ARGS(&buffer2D)))) {
                buffer2D->Lock2D(&srcPtr, &srcStride);
            } else {
                outBuffer->Lock(&srcPtr, nullptr, nullptr);
                srcStride = width;
            }

            if (srcPtr) {
                D3D11_MAPPED_SUBRESOURCE map;
                if (SUCCEEDED(ctx->Map(mfStagingTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                    BYTE* dstPtr = (BYTE*)map.pData;
                    UINT dstStride = map.RowPitch;
                    int alignedHeight = Align16(height);
                    
                    // Copy Y plane (only actual height, not aligned)
                    for (int y = 0; y < height; ++y) {
                        memcpy(dstPtr + y * dstStride, srcPtr + y * srcStride, width);
                    }

                    // Copy UV plane - starts at alignedHeight in both source and dest
                    BYTE* srcUV = srcPtr + (srcStride * alignedHeight);
                    BYTE* dstUV = dstPtr + (dstStride * alignedHeight);
                    for (int y = 0; y < height / 2; ++y) {
                        memcpy(dstUV + y * dstStride, srcUV + y * srcStride, width);
                    }

                    ctx->Unmap(mfStagingTexture, 0);
                }

                if (buffer2D) {
                    buffer2D->Unlock2D();
                    buffer2D->Release();
                } else {
                    outBuffer->Unlock();
                }

                // GPU copy to output texture
                ctx->CopyResource(mfOutputTexture, mfStagingTexture);
            }

            outSample->Release();
            outBuffer->Release();
            if (outputData.pEvents) outputData.pEvents->Release();

            frameCount++;
            if (frameCount == 1) {
                std::cout << "[Decoder] ✓ First frame decoded successfully (NVIDIA)" << std::endl;
            } else if (frameCount % 60 == 0) {
                std::cout << "[Decoder] " << frameCount << " frames decoded" << std::endl;
            }

            return mfOutputTexture;
        }
        else {
            outSample->Release();
            outBuffer->Release();
            return nullptr;
        }
    }
}
