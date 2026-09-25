#pragma once
#include <windows.h>
#include <string>

constexpr UINT kAvatarSettingsChoosePngMessage = WM_APP + 10;
constexpr UINT kAvatarSettingsReadyMessage = WM_APP + 11;
constexpr UINT kAvatarSettingsChooseReactionPngMessage = WM_APP + 12;
constexpr UINT kAvatarSettingsPreviewReactionMessage = WM_APP + 13;

bool showAvatarSettingsWindow(HWND owner);
bool isAvatarSettingsWindowVisible();
void postAvatarSettingsMessage(const std::wstring &message);
void setAvatarSettingsPreviewImage(bool reaction, const std::wstring &path);
void shutdownAvatarSettingsWindow();
