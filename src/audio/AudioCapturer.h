#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <vector>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <iostream>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib") 

using AudioCallback = std::function<void(const uint8_t*, size_t)>;

struct AudioDeviceInfo {
    std::string name;
    std::wstring id;
};

class AudioCapturer {
public:
    AudioCapturer() {
        CoInitialize(nullptr);
    }
    ~AudioCapturer() {
        Stop();
        CoUninitialize();
    }

    // Get list of active Output devices
    std::vector<AudioDeviceInfo> EnumerateDevices() {
        std::vector<AudioDeviceInfo> devices;
        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
        
        if (FAILED(hr)) return devices;

        IMMDeviceCollection* collection = nullptr;
        enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
        
        UINT count;
        collection->GetCount(&count);
        
        for (UINT i = 0; i < count; i++) {
            IMMDevice* device;
            collection->Item(i, &device);
            LPWSTR id;
            device->GetId(&id);
            
            IPropertyStore* props;
            device->OpenPropertyStore(STGM_READ, &props);
            
            PROPVARIANT varName;
            PropVariantInit(&varName);
            // PKEY_Device_FriendlyName
            PROPERTYKEY key = { {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14 };
            props->GetValue(key, &varName);
            
            // --- FIX START: Proper UTF-16 to UTF-8 Conversion ---
            std::wstring wname = varName.pwszVal;
            
            // Calculate required size for UTF-8 string
            int size_needed = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), (int)wname.length(), NULL, 0, NULL, NULL);
            std::string name(size_needed, 0);
            
            // Do the conversion
            WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), (int)wname.length(), &name[0], size_needed, NULL, NULL);
            // --- FIX END ---
            
            devices.push_back({ name, std::wstring(id) });
            
            PropVariantClear(&varName);
            props->Release();
            device->Release();
        }
        
        if (collection) collection->Release();
        if (enumerator) enumerator->Release();
        return devices;
    }

    bool Start(std::wstring deviceId, AudioCallback callback) {
        if (capturing) return false;
        
        IMMDeviceEnumerator* enumerator = nullptr;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
        
        IMMDevice* device = nullptr;
        if (deviceId.empty()) {
            enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        } else {
            enumerator->GetDevice(deviceId.c_str(), &device);
        }
        
        if (!device) {
            if (enumerator) enumerator->Release();
            return false;
        }

        device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
        
        WAVEFORMATEX* mixFormat;
        audioClient->GetMixFormat(&mixFormat);
        
        // Loopback Capture requires AUDCLNT_STREAMFLAGS_LOOPBACK
        HRESULT hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, mixFormat, nullptr);
        
        if (FAILED(hr)) {
            if (enumerator) enumerator->Release();
            if (device) device->Release();
            return false;
        }

        audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
        
        capturing = true;
        captureThread = std::thread(&AudioCapturer::CaptureLoop, this, callback, mixFormat);
        
        if (enumerator) enumerator->Release();
        if (device) device->Release();
        return true;
    }

    void Stop() {
        capturing = false;
        if (captureThread.joinable()) captureThread.join();
        if (audioClient) { audioClient->Stop(); audioClient->Release(); audioClient = nullptr; }
        if (captureClient) { captureClient->Release(); captureClient = nullptr; }
    }

private:
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    std::thread captureThread;
    std::atomic<bool> capturing = false;

    void CaptureLoop(AudioCallback callback, WAVEFORMATEX* mixFormat) {
        audioClient->Start();
        
        while (capturing) {
            UINT32 nextPacketSize = 0;
            HRESULT hr = captureClient->GetNextPacketSize(&nextPacketSize);
            
            if (SUCCEEDED(hr) && nextPacketSize > 0) {
                BYTE* pData;
                UINT32 numFrames;
                DWORD flags;
                
                if (SUCCEEDED(captureClient->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr))) {
                    // CONVERSION: Float32 -> Int16
                    std::vector<int16_t> pcmData;
                    pcmData.reserve(numFrames * mixFormat->nChannels);
                    
                    float* floatData = (float*)pData;
                    
                    for (UINT i = 0; i < numFrames * mixFormat->nChannels; i++) {
                        float sample = floatData[i];
                        // Hard Clip
                        if (sample > 1.0f) sample = 1.0f;
                        if (sample < -1.0f) sample = -1.0f;
                        pcmData.push_back((int16_t)(sample * 32767.0f));
                    }
                    
                    callback((uint8_t*)pcmData.data(), pcmData.size() * sizeof(int16_t));
                    
                    captureClient->ReleaseBuffer(numFrames);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
};