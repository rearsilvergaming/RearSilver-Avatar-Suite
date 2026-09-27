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
constexpr UINT kAvatarSettingsBounceEnabledMessage = WM_APP + 25;
constexpr UINT kAvatarSettingsBounceHeightMessage = WM_APP + 26;
constexpr UINT kAvatarSettingsBounceDurationMessage = WM_APP + 27;
constexpr UINT kAvatarSettingsPreviewBounceMessage = WM_APP + 28;
constexpr UINT kAvatarSettingsCalibrateNoiseMessage = WM_APP + 29;
constexpr UINT kAvatarSettingsNoiseSensitivityMessage = WM_APP + 30;
constexpr UINT kAvatarSettingsUseDefaultPrimaryMessage = WM_APP + 31;
constexpr UINT kAvatarSettingsUseDefaultReactionMessage = WM_APP + 32;
constexpr UINT kAvatarSettingsRemovePrimaryBlinkMessage = WM_APP + 33;
constexpr UINT kAvatarSettingsRemoveReactionBlinkMessage = WM_APP + 34;
constexpr UINT kAvatarSettingsEffectStackMessage = WM_APP + 35;
constexpr UINT kAvatarSettingsBreathingEnabledMessage = WM_APP + 36;
constexpr UINT kAvatarSettingsBreathingModeMessage = WM_APP + 37;
constexpr UINT kAvatarSettingsBreathingIdleMessage = WM_APP + 38;
constexpr UINT kAvatarSettingsBreathingReactionMessage = WM_APP + 39;
constexpr UINT kAvatarSettingsBreathingCycleMessage = WM_APP + 40;
constexpr UINT kAvatarSettingsSquashEnabledMessage = WM_APP + 41;
constexpr UINT kAvatarSettingsSquashIntensityMessage = WM_APP + 42;
constexpr UINT kAvatarSettingsSquashDurationMessage = WM_APP + 43;
constexpr UINT kAvatarSettingsPreviewSquashMessage = WM_APP + 44;
constexpr UINT kAvatarSettingsShakeEnabledMessage = WM_APP + 45;
constexpr UINT kAvatarSettingsShakeIntensityMessage = WM_APP + 46;
constexpr UINT kAvatarSettingsShakeSpeedMessage = WM_APP + 47;
constexpr UINT kAvatarSettingsShakeDirectionMessage = WM_APP + 48;
constexpr UINT kAvatarSettingsShakeWobbleMessage = WM_APP + 49;
constexpr UINT kAvatarSettingsPreviewShakeMessage = WM_APP + 50;

bool showAvatarSettingsWindow(HWND owner);
bool isAvatarSettingsWindowVisible();
void postAvatarSettingsMessage(const std::wstring &message);
void setAvatarSettingsPreviewImage(unsigned slot, const std::wstring &path);
void shutdownAvatarSettingsWindow();
