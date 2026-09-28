#include "settings_window.h"
#include "resource.h"
#include <objidl.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <WebView2.h>
#include <wrl.h>
#include <wrl/event.h>
#include <atomic>
#include <string>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {
constexpr wchar_t kSettingsClass[] = L"RearSilverAvatarSettingsWindow";
constexpr wchar_t kSettingsTitle[] = L"RearSilver Avatar Suite Settings";
HWND g_settingsWindow = nullptr;
HWND g_ownerWindow = nullptr;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webView;
bool g_initialising = false;
bool g_webViewReady = false;
std::atomic<bool> g_settingsVisible{false};
std::wstring g_pendingPage;

void sendPendingPage()
{
    if (!g_webView || !g_webViewReady || g_pendingPage.empty())
        return;
    g_webView->PostWebMessageAsString((L"navigate-page\t" + g_pendingPage).c_str());
    g_pendingPage.clear();
}

std::wstring executableDirectory()
{
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring result(path, length);
    const size_t separator = result.find_last_of(L"\\/");
    return separator == std::wstring::npos ? L"." : result.substr(0, separator);
}

std::wstring webViewDataDirectory()
{
    wchar_t localAppData[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    const std::wstring root = std::wstring(localAppData) + L"\\RearSilver Avatar";
    CreateDirectoryW(root.c_str(), nullptr);
    const std::wstring result = root + L"\\WebView2";
    CreateDirectoryW(result.c_str(), nullptr);
    return result;
}

std::wstring previewDirectory()
{
    wchar_t localAppData[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    const std::wstring root = std::wstring(localAppData) + L"\\RearSilver Avatar";
    CreateDirectoryW(root.c_str(), nullptr);
    const std::wstring result = root + L"\\Preview";
    CreateDirectoryW(result.c_str(), nullptr);
    return result;
}

void resizeWebView()
{
    if (!g_controller || !g_settingsWindow)
        return;
    RECT bounds{};
    GetClientRect(g_settingsWindow, &bounds);
    g_controller->put_Bounds(bounds);
}

void hideSettingsWindow()
{
    g_settingsVisible.store(false);
    if (g_ownerWindow && IsWindow(g_ownerWindow))
        PostMessageW(g_ownerWindow, kAvatarSettingsPreviewReactionMessage, FALSE, 0);
    if (g_controller)
        g_controller->put_IsVisible(FALSE);
    if (g_settingsWindow)
        ShowWindow(g_settingsWindow, SW_HIDE);
    if (g_ownerWindow && IsWindow(g_ownerWindow)) {
        ShowWindow(g_ownerWindow, SW_SHOW);
        SetForegroundWindow(g_ownerWindow);
    }
}

void initialiseWebView()
{
    if (g_initialising || g_controller || !g_settingsWindow)
        return;
    g_initialising = true;
    const std::wstring assets = executableDirectory();
    const std::wstring data = webViewDataDirectory();
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [assets](HRESULT result, ICoreWebView2Environment *environment) -> HRESULT {
                if (FAILED(result) || !environment || !g_settingsWindow) {
                    g_initialising = false;
                    return result;
                }
                return environment->CreateCoreWebView2Controller(
                    g_settingsWindow,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [assets](HRESULT controllerResult, ICoreWebView2Controller *controller) -> HRESULT {
                            g_initialising = false;
                            if (FAILED(controllerResult) || !controller || !g_settingsWindow)
                                return controllerResult;
                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webView);
                            if (!g_webView)
                                return E_FAIL;
                            ComPtr<ICoreWebView2_3> webView3;
                            if (SUCCEEDED(g_webView.As(&webView3))) {
                                webView3->SetVirtualHostNameToFolderMapping(
                                    L"app.rearsilver-avatar.test", assets.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                                const std::wstring previews = previewDirectory();
                                webView3->SetVirtualHostNameToFolderMapping(
                                    L"preview.rearsilver-avatar.test", previews.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                            }
                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(g_webView->get_Settings(&settings))) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                            }
                            EventRegistrationToken token{};
                            g_webView->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                                        wchar_t *message = nullptr;
                                        if (SUCCEEDED(args->TryGetWebMessageAsString(&message)) && message) {
                                            if (wcscmp(message, L"close-settings") == 0)
                                                PostMessageW(g_settingsWindow, WM_CLOSE, 0, 0);
                                            else if (wcscmp(message, L"open-spout-plugin") == 0)
                                                ShellExecuteW(g_settingsWindow, L"open",
                                                    L"https://github.com/Off-World-Live/obs-spout2-plugin/releases",
                                                    nullptr, nullptr, SW_SHOWNORMAL);
                                            else if (wcscmp(message, L"choose-avatar-png") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsChoosePngMessage,
                                                             0, reinterpret_cast<LPARAM>(g_settingsWindow));
                                            else if (wcscmp(message, L"avatar-settings-ready") == 0) {
                                                g_webViewReady = true;
                                                sendPendingPage();
                                                if (g_ownerWindow && IsWindow(g_ownerWindow))
                                                    PostMessageW(g_ownerWindow, kAvatarSettingsReadyMessage, 0, 0);
                                            }
                                            else if (wcsncmp(message, L"avatar-scale\t", 13) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsScaleMessage,
                                                             wcstoul(message + 13, nullptr, 10), 0);
                                            else if (wcscmp(message, L"choose-reaction-png") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsChooseReactionPngMessage,
                                                             0, reinterpret_cast<LPARAM>(g_settingsWindow));
                                            else if (wcscmp(message, L"use-default-primary") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsUseDefaultPrimaryMessage,
                                                             0, 0);
                                            else if (wcscmp(message, L"use-default-reaction") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsUseDefaultReactionMessage,
                                                             0, 0);
                                            else if (wcsncmp(message, L"preview-reaction:", 17) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsPreviewReactionMessage,
                                                             wcscmp(message + 17, L"on") == 0, 0);
                                            else if (wcsncmp(message, L"select-microphone\t", 18) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsSelectMicrophoneMessage,
                                                             0, reinterpret_cast<LPARAM>(
                                                                    new std::wstring(message + 18)));
                                            else if (wcsncmp(message, L"reaction-threshold\t", 19) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsReactionThresholdMessage,
                                                             wcstoul(message + 19, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"release-delay\t", 14) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsReleaseDelayMessage,
                                                             wcstoul(message + 14, nullptr, 10), 0);
                                            else if (wcscmp(message, L"choose-primary-blink") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsChoosePrimaryBlinkMessage,
                                                             0, reinterpret_cast<LPARAM>(g_settingsWindow));
                                            else if (wcscmp(message, L"choose-reaction-blink") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsChooseReactionBlinkMessage,
                                                             0, reinterpret_cast<LPARAM>(g_settingsWindow));
                                            else if (wcscmp(message, L"remove-primary-blink") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsRemovePrimaryBlinkMessage,
                                                             0, 0);
                                            else if (wcscmp(message, L"remove-reaction-blink") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsRemoveReactionBlinkMessage,
                                                             0, 0);
                                            else if (wcsncmp(message, L"blink-enabled\t", 14) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsBlinkEnabledMessage,
                                                             wcscmp(message + 14, L"1") == 0, 0);
                                            else if (wcsncmp(message, L"blink-minimum\t", 14) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsBlinkMinimumMessage,
                                                             wcstoul(message + 14, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"blink-maximum\t", 14) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsBlinkMaximumMessage,
                                                             wcstoul(message + 14, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"blink-duration\t", 15) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsBlinkDurationMessage,
                                                             wcstoul(message + 15, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"bounce-enabled\t", 15) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsBounceEnabledMessage,
                                                             wcscmp(message + 15, L"1") == 0, 0);
                                            else if (wcsncmp(message, L"bounce-height\t", 14) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsBounceHeightMessage,
                                                             wcstoul(message + 14, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"bounce-duration\t", 16) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsBounceDurationMessage,
                                                             wcstoul(message + 16, nullptr, 10), 0);
                                            else if (wcscmp(message, L"preview-bounce") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsPreviewBounceMessage,
                                                             0, 0);
                                            else if (wcsncmp(message, L"effect-stack\t", 13) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsEffectStackMessage,
                                                             0, reinterpret_cast<LPARAM>(
                                                                    new std::wstring(message + 13)));
                                            else if (wcsncmp(message, L"breathing-enabled\t", 18) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsBreathingEnabledMessage,
                                                             wcscmp(message + 18, L"1") == 0, 0);
                                            else if (wcsncmp(message, L"breathing-mode\t", 15) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsBreathingModeMessage,
                                                             wcstoul(message + 15, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"breathing-idle\t", 15) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsBreathingIdleMessage,
                                                             wcstoul(message + 15, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"breathing-reaction\t", 19) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsBreathingReactionMessage,
                                                             wcstoul(message + 19, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"breathing-cycle\t", 16) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsBreathingCycleMessage,
                                                             wcstoul(message + 16, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"squash-enabled\t", 15) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsSquashEnabledMessage,
                                                             wcscmp(message + 15, L"1") == 0, 0);
                                            else if (wcsncmp(message, L"squash-intensity\t", 17) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsSquashIntensityMessage,
                                                             wcstoul(message + 17, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"squash-duration\t", 16) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsSquashDurationMessage,
                                                             wcstoul(message + 16, nullptr, 10), 0);
                                            else if (wcscmp(message, L"preview-squash") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsPreviewSquashMessage,
                                                             0, 0);
                                            else if (wcsncmp(message, L"shake-enabled\t", 14) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsShakeEnabledMessage,
                                                             wcscmp(message + 14, L"1") == 0, 0);
                                            else if (wcsncmp(message, L"shake-intensity\t", 16) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsShakeIntensityMessage,
                                                             wcstoul(message + 16, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"shake-speed\t", 12) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsShakeSpeedMessage,
                                                             wcstoul(message + 12, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"shake-direction\t", 16) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsShakeDirectionMessage,
                                                             wcstoul(message + 16, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"shake-wobble\t", 13) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsShakeWobbleMessage,
                                                             wcscmp(message + 13, L"1") == 0, 0);
                                            else if (wcscmp(message, L"preview-shake") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsPreviewShakeMessage,
                                                             0, 0);
                                            else if (wcsncmp(message, L"brightness-enabled\t", 19) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsBrightnessEnabledMessage,
                                                             wcscmp(message + 19, L"1") == 0, 0);
                                            else if (wcsncmp(message, L"brightness-idle\t", 16) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsBrightnessIdleMessage,
                                                             wcstoul(message + 16, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"brightness-reaction\t", 20) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsBrightnessReactionMessage,
                                                             wcstoul(message + 20, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"brightness-transition\t", 22) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsBrightnessTransitionMessage,
                                                             wcstoul(message + 22, nullptr, 10), 0);
                                            else if (wcscmp(message, L"preview-brightness") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsPreviewBrightnessMessage,
                                                             0, 0);
                                            else if (wcsncmp(message, L"float-enabled\t", 14) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsFloatEnabledMessage,
                                                             wcscmp(message + 14, L"1") == 0, 0);
                                            else if (wcsncmp(message, L"float-mode\t", 11) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsFloatModeMessage,
                                                             wcstoul(message + 11, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"float-height\t", 13) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsFloatHeightMessage,
                                                             wcstoul(message + 13, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"float-cycle\t", 12) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsFloatCycleMessage,
                                                             wcstoul(message + 12, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"float-direction\t", 16) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsFloatDirectionMessage,
                                                             wcstoul(message + 16, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"float-drift\t", 12) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsFloatDriftMessage,
                                                             wcstoul(message + 12, nullptr, 10), 0);
                                            else if (wcscmp(message, L"preview-float") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsPreviewFloatMessage,
                                                             0, 0);
                                            else if (wcsncmp(message, L"tilt-enabled\t", 13) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsTiltEnabledMessage,
                                                             wcscmp(message + 13, L"1") == 0, 0);
                                            else if (wcsncmp(message, L"tilt-angle\t", 11) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsTiltAngleMessage,
                                                             wcstoul(message + 11, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"tilt-direction\t", 15) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsTiltDirectionMessage,
                                                             wcstoul(message + 15, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"tilt-transition\t", 16) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsTiltTransitionMessage,
                                                             wcstoul(message + 16, nullptr, 10), 0);
                                            else if (wcscmp(message, L"preview-tilt") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsPreviewTiltMessage,
                                                             0, 0);
                                            else if (wcsncmp(message, L"background-mode\t", 16) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsBackgroundModeMessage,
                                                             wcstoul(message + 16, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"background-solid-colour\t", 24) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow,
                                                             kAvatarSettingsBackgroundSolidColourMessage, 0,
                                                             reinterpret_cast<LPARAM>(new std::wstring(message + 24)));
                                            else if (wcsncmp(message, L"background-chroma-colour\t", 25) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow,
                                                             kAvatarSettingsBackgroundChromaColourMessage, 0,
                                                             reinterpret_cast<LPARAM>(new std::wstring(message + 25)));
                                            else if (wcscmp(message, L"choose-background-image") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow,
                                                             kAvatarSettingsChooseBackgroundImageMessage, 0,
                                                             reinterpret_cast<LPARAM>(g_settingsWindow));
                                            else if (wcscmp(message, L"remove-background-image") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow,
                                                             kAvatarSettingsRemoveBackgroundImageMessage, 0, 0);
                                            else if (wcsncmp(message, L"background-fit\t", 15) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsBackgroundFitMessage,
                                                             wcstoul(message + 15, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"capture-method\t", 15) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsCaptureMethodMessage,
                                                             wcstoul(message + 15, nullptr, 10), 0);
                                            else if (wcscmp(message, L"calibrate-noise") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsCalibrateNoiseMessage,
                                                             0, 0);
                                            else if (wcsncmp(message, L"noise-sensitivity\t", 18) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsNoiseSensitivityMessage,
                                                             wcstoul(message + 18, nullptr, 10), 0);
                                            CoTaskMemFree(message);
                                        }
                                        return S_OK;
                                    }).Get(), &token);
                            resizeWebView();
                            g_controller->put_IsVisible(TRUE);
                            return g_webView->Navigate(L"https://app.rearsilver-avatar.test/avatar-settings.html");
                        }).Get());
            }).Get());
}

LRESULT CALLBACK settingsWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE: initialiseWebView(); return 0;
    case WM_SIZE: resizeWebView(); return 0;
    case WM_CLOSE: hideSettingsWindow(); return 0;
    case WM_DESTROY: g_settingsWindow = nullptr; return 0;
    default: return DefWindowProcW(window, message, wParam, lParam);
    }
}
} // namespace

bool showAvatarSettingsWindow(HWND owner, const std::wstring &page)
{
    g_ownerWindow = owner;
    if (!page.empty())
        g_pendingPage = page;
    if (!g_settingsWindow) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpfnWndProc = settingsWindowProcedure;
        windowClass.lpszClassName = kSettingsClass;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hIcon = LoadIconW(windowClass.hInstance,
            MAKEINTRESOURCEW(IDI_REARSILVER_AVATAR_SUITE));
        windowClass.hIconSm = static_cast<HICON>(LoadImageW(windowClass.hInstance,
            MAKEINTRESOURCEW(IDI_REARSILVER_AVATAR_SUITE), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        RegisterClassExW(&windowClass);
        g_settingsWindow = CreateWindowExW(0, kSettingsClass, kSettingsTitle,
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820, owner, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        if (!g_settingsWindow)
            return false;
    }
    ShowWindow(g_settingsWindow, SW_SHOW);
    g_settingsVisible.store(true);
    SetForegroundWindow(g_settingsWindow);
    if (g_controller)
        g_controller->put_IsVisible(TRUE);
    else
        initialiseWebView();
    sendPendingPage();
    return true;
}

bool isAvatarSettingsWindowVisible()
{
    return g_settingsVisible.load();
}

void postAvatarSettingsMessage(const std::wstring &message)
{
    if (g_webView)
        g_webView->PostWebMessageAsString(message.c_str());
}

void setAvatarSettingsPreviewImage(unsigned slot, const std::wstring &path)
{
    if (!g_webView || path.empty())
        return;
    const wchar_t *fileName = slot == 1 ? L"reaction.png"
                              : slot == 2 ? L"primary-blink.png"
                              : slot == 3 ? L"reaction-blink.png"
                              : slot == 4 ? L"background.png"
                                          : L"primary.png";
    const std::wstring cachedPath = previewDirectory() + L"\\" + fileName;
    if (!CopyFileW(path.c_str(), cachedPath.c_str(), FALSE))
        return;
    const wchar_t *message = slot == 1 ? L"reaction-preview-image\t"
                             : slot == 2 ? L"primary-blink-preview-image\t"
                             : slot == 3 ? L"reaction-blink-preview-image\t"
                             : slot == 4 ? L"background-preview-image\t"
                                         : L"primary-preview-image\t";
    postAvatarSettingsMessage(std::wstring(message) +
                              fileName + L"?revision=" + std::to_wstring(GetTickCount64()));
}

void shutdownAvatarSettingsWindow()
{
    g_settingsVisible.store(false);
    if (g_controller) {
        g_controller->put_IsVisible(FALSE);
        g_controller->Close();
    }
    g_webView.Reset();
    g_controller.Reset();
    g_webViewReady = false;
    if (g_settingsWindow) {
        DestroyWindow(g_settingsWindow);
        g_settingsWindow = nullptr;
    }
    g_ownerWindow = nullptr;
    g_pendingPage.clear();
}
