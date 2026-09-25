#pragma once
#include <windows.h>
#include <string>

constexpr UINT kAvatarSettingsChoosePngMessage = WM_APP + 10;
constexpr UINT kAvatarSettingsReadyMessage = WM_APP + 11;
constexpr UINT kAvatarSettingsChooseReactionPngMessage = WM_APP + 12;
constexpr UINT kAvatarSettingsPreviewReactionMessage = WM_APP + 13;
constexpr UINT kAvatarSettingsSelectMicrophoneMessage = WM_APP + 14;
constexpr UINT kAvatarSettingsReactionThresholdMessage = WM_APP + 15;
constexpr UINT kAvatarSettingsReleaseDelayMessage = WM_APP + 16;
constexpr UINT kAvatarSettingsChoosePrimaryBlinkMessage = WM_APP + 17;
constexpr UINT kAvatarSettingsChooseReactionBlinkMessage = WM_APP + 18;
constexpr UINT kAvatarSettingsBlinkEnabledMessage = WM_APP + 19;
constexpr UINT kAvatarSettingsBlinkMinimumMessage = WM_APP + 22;
constexpr UINT kAvatarSettingsBlinkMaximumMessage = WM_APP + 23;
constexpr UINT kAvatarSettingsBlinkDurationMessage = WM_APP + 24;

bool showAvatarSettingsWindow(HWND owner);
bool isAvatarSettingsWindowVisible();
void postAvatarSettingsMessage(const std::wstring &message);
void setAvatarSettingsPreviewImage(unsigned slot, const std::wstring &path);
void shutdownAvatarSettingsWindow();
