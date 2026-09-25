#pragma once
#include <windows.h>
#include <string>

constexpr UINT kAvatarSettingsChoosePngMessage = WM_APP + 10;
constexpr UINT kAvatarSettingsReadyMessage = WM_APP + 11;

bool showAvatarSettingsWindow(HWND owner);
bool isAvatarSettingsWindowVisible();
void postAvatarSettingsMessage(const std::wstring &message);
void shutdownAvatarSettingsWindow();
