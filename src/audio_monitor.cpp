#include "audio_monitor.h"

#include <audioclient.h>
#include <endpointvolume.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace {
float sampleRms(const BYTE *data, UINT32 frames, const WAVEFORMATEX *format)
{
    if (!data || !format || frames == 0)
        return 0.0f;
    const UINT channels = std::max<UINT>(1, format->nChannels);
    const UINT bits = format->wBitsPerSample;
    bool floatingPoint = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool pcm = format->wFormatTag == WAVE_FORMAT_PCM;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto *extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        floatingPoint = IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        pcm = IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
    }
    double sumSquares = 0.0;
    const size_t sampleCount = static_cast<size_t>(frames) * channels;
    if (floatingPoint && bits == 32) {
        const float *samples = reinterpret_cast<const float *>(data);
        for (size_t index = 0; index < sampleCount; ++index)
            sumSquares += static_cast<double>(samples[index]) * samples[index];
    } else if (pcm && bits == 16) {
        const int16_t *samples = reinterpret_cast<const int16_t *>(data);
        for (size_t index = 0; index < sampleCount; ++index) {
            const double sample = static_cast<double>(samples[index]) / 32768.0;
            sumSquares += sample * sample;
        }
    } else if (pcm && bits == 24) {
        for (size_t index = 0; index < sampleCount; ++index) {
            const BYTE *sample = data + index * 3;
            int32_t value = sample[0] | (sample[1] << 8) | (sample[2] << 16);
            if (value & 0x00800000)
                value |= static_cast<int32_t>(0xff000000);
            const double normalized = static_cast<double>(value) / 8388608.0;
            sumSquares += normalized * normalized;
        }
    } else if (pcm && bits == 32) {
        const int32_t *samples = reinterpret_cast<const int32_t *>(data);
        for (size_t index = 0; index < sampleCount; ++index) {
            const double sample = static_cast<double>(samples[index]) / 2147483648.0;
            sumSquares += sample * sample;
        }
    } else if (pcm && bits == 8) {
        for (size_t index = 0; index < sampleCount; ++index) {
            const double sample = (static_cast<double>(data[index]) - 128.0) / 128.0;
            sumSquares += sample * sample;
        }
    }
    return std::clamp(static_cast<float>(std::sqrt(sumSquares / sampleCount)), 0.0f, 1.0f);
}
} // namespace

std::vector<AudioInputDevice> enumerateAudioInputDevices()
{
    std::vector<AudioInputDevice> result;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator))))
        return result;
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection)))
        return result;
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        LPWSTR id = nullptr;
        ComPtr<IPropertyStore> properties;
        PROPVARIANT name{};
        PropVariantInit(&name);
        if (SUCCEEDED(collection->Item(index, &device)) &&
            SUCCEEDED(device->GetId(&id)) &&
            SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) &&
            SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) &&
            name.vt == VT_LPWSTR) {
            result.push_back({id, name.pwszVal});
        }
        if (id)
            CoTaskMemFree(id);
        PropVariantClear(&name);
    }
    return result;
}

AudioInputMonitor::~AudioInputMonitor()
{
    stop();
}

void AudioInputMonitor::start(HWND notificationWindow, const std::wstring &deviceId)
{
    stop();
    notificationWindow_ = notificationWindow;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_)
        return;
    thread_ = std::thread(&AudioInputMonitor::run, this, deviceId);
}

void AudioInputMonitor::stop()
{
    if (stopEvent_)
        SetEvent(stopEvent_);
    if (thread_.joinable())
        thread_.join();
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
}

void AudioInputMonitor::run(std::wstring deviceId)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HANDLE audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    ComPtr<IAudioEndpointVolume> endpointVolume;
    WAVEFORMATEX *format = nullptr;
    HRESULT result = audioEvent ? CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                                   IID_PPV_ARGS(&enumerator))
                                : E_FAIL;
    if (SUCCEEDED(result)) {
        result = deviceId.empty()
                     ? enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device)
                     : enumerator->GetDevice(deviceId.c_str(), &device);
    }
    if (SUCCEEDED(result))
        result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void **>(client.GetAddressOf()));
    if (SUCCEEDED(result))
        device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                         reinterpret_cast<void **>(endpointVolume.GetAddressOf()));
    if (SUCCEEDED(result))
        result = client->GetMixFormat(&format);
    if (SUCCEEDED(result))
        result = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                                    0, 0, format, nullptr);
    if (SUCCEEDED(result))
        result = client->SetEventHandle(audioEvent);
    if (SUCCEEDED(result))
        result = client->GetService(IID_PPV_ARGS(&capture));
    if (SUCCEEDED(result))
        result = client->Start();
    if (FAILED(result)) {
        PostMessageW(notificationWindow_, kAudioMonitorStatusMessage, 2, 0);
    } else {
        PostMessageW(notificationWindow_, kAudioMonitorStatusMessage, 1, 0);
        HANDLE events[] = {stopEvent_, audioEvent};
        bool captureFailed = false;
        unsigned lastPostedLevel = ~0u;
        ULONGLONG lastLevelPostAt = 0;
        float smoothedRms = 0.0f;
        while (!captureFailed) {
            const DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, 250);
            if (waitResult == WAIT_OBJECT_0)
                break;
            if (waitResult == WAIT_FAILED) {
                captureFailed = true;
                break;
            }
            float rms = 0.0f;
            UINT32 packetFrames = 0;
            HRESULT packetResult = capture->GetNextPacketSize(&packetFrames);
            while (SUCCEEDED(packetResult) && packetFrames > 0) {
                BYTE *data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                    captureFailed = true;
                    break;
                }
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                    rms = std::max(rms, sampleRms(data, frames, format));
                capture->ReleaseBuffer(frames);
                packetResult = capture->GetNextPacketSize(&packetFrames);
            }
            if (FAILED(packetResult))
                captureFailed = true;
            BOOL muted = FALSE;
            if (endpointVolume)
                endpointVolume->GetMute(&muted);
            if (muted)
                smoothedRms = 0.0f;
            else
                smoothedRms += (rms - smoothedRms) * 0.22f;
            unsigned level = muted ? 0 : static_cast<unsigned>(std::sqrt(smoothedRms) * 1000.0f);
            if (level < 10)
                level = 0;
            const ULONGLONG now = GetTickCount64();
            const bool becameSilent = level == 0 && lastPostedLevel != 0;
            if (becameSilent || lastLevelPostAt == 0 || now - lastLevelPostAt >= 50) {
                PostMessageW(notificationWindow_, kAudioMonitorLevelMessage, level, 0);
                lastPostedLevel = level;
                lastLevelPostAt = now;
            }
        }
        client->Stop();
        if (captureFailed) {
            PostMessageW(notificationWindow_, kAudioMonitorLevelMessage, 0, 0);
            PostMessageW(notificationWindow_, kAudioMonitorStatusMessage, 2, 0);
        }
    }
    if (format)
        CoTaskMemFree(format);
    if (audioEvent)
        CloseHandle(audioEvent);
    if (SUCCEEDED(com))
        CoUninitialize();
}
