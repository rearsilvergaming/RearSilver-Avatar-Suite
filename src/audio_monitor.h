#pragma once

#include <windows.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

constexpr UINT kAudioMonitorLevelMessage = WM_APP + 20;
constexpr UINT kAudioMonitorStatusMessage = WM_APP + 21;

struct AudioInputDevice {
    std::wstring id;
    std::wstring name;
};

std::vector<AudioInputDevice> enumerateAudioInputDevices();

class AudioInputMonitor {
public:
    ~AudioInputMonitor();
    void start(HWND notificationWindow, const std::wstring &deviceId);
    void stop();

private:
    void run(std::wstring deviceId);

    HWND notificationWindow_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    std::thread thread_;
};
