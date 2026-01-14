#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <vector>
#include <thread>
#include <atomic>

class AudioPlayer {
public:
    AudioPlayer() { CoInitialize(nullptr); }
    ~AudioPlayer() { Cleanup(); CoUninitialize(); }

    void Initialize() {
        IMMDeviceEnumerator* enumerator = nullptr;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
        
        IMMDevice* device = nullptr;
        enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        
        device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
        
        // Define standard format matching our Sender (48kHz, Stereo, 16-bit)
        WAVEFORMATEX fmt = {};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = 48000;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = (fmt.nChannels * fmt.wBitsPerSample) / 8;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        fmt.cbSize = 0;

        // Initialize Shared Mode
        audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, &fmt, nullptr);
        audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient);
        audioClient->Start();
        
        enumerator->Release();
        device->Release();
    }

    void QueueAudio(const uint8_t* data, size_t size) {
        if (!renderClient) return;

        // Simple "Push" playback (Note: In production, you'd use a circular buffer to handle jitter)
        // Here we just write whatever fits in the buffer to minimize latency
        
        UINT32 bufferSize;
        audioClient->GetBufferSize(&bufferSize);
        
        UINT32 padding;
        audioClient->GetCurrentPadding(&padding);
        
        UINT32 available = bufferSize - padding;
        UINT32 incomingFrames = (UINT32)size / 4; // 16-bit stereo = 4 bytes per frame

        if (incomingFrames > available) incomingFrames = available; // Drop excess to catch up

        if (incomingFrames > 0) {
            BYTE* pBuffer;
            if (SUCCEEDED(renderClient->GetBuffer(incomingFrames, &pBuffer))) {
                memcpy(pBuffer, data, incomingFrames * 4);
                renderClient->ReleaseBuffer(incomingFrames, 0);
            }
        }
    }

    void Cleanup() {
        if (audioClient) { audioClient->Stop(); audioClient->Release(); audioClient = nullptr; }
        if (renderClient) { renderClient->Release(); renderClient = nullptr; }
    }

private:
    IAudioClient* audioClient = nullptr;
    IAudioRenderClient* renderClient = nullptr;
};