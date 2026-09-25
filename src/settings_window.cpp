#include "settings_window.h"
#include <objidl.h>
#include <shobjidl.h>
#include <WebView2.h>
#include <wrl.h>
#include <wrl/event.h>
#include <atomic>
#include <string>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {
constexpr wchar_t kSettingsClass[] = L"RearSilverAvatarSettingsWindow";
constexpr wchar_t kSettingsTitle[] = L"RearSilver Avatar Settings";
HWND g_settingsWindow = nullptr;
HWND g_ownerWindow = nullptr;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webView;
bool g_initialising = false;
std::atomic<bool> g_settingsVisible{false};

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
                                            else if (wcscmp(message, L"choose-avatar-png") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsChoosePngMessage,
                                                             0, reinterpret_cast<LPARAM>(g_settingsWindow));
                                            else if (wcscmp(message, L"avatar-settings-ready") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsReadyMessage, 0, 0);
                                            else if (wcscmp(message, L"choose-reaction-png") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsChooseReactionPngMessage,
                                                             0, reinterpret_cast<LPARAM>(g_settingsWindow));
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

bool showAvatarSettingsWindow(HWND owner)
{
    g_ownerWindow = owner;
    if (!g_settingsWindow) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpfnWndProc = settingsWindowProcedure;
        windowClass.lpszClassName = kSettingsClass;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
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
                                          : L"primary.png";
    const std::wstring cachedPath = previewDirectory() + L"\\" + fileName;
    if (!CopyFileW(path.c_str(), cachedPath.c_str(), FALSE))
        return;
    const wchar_t *message = slot == 1 ? L"reaction-preview-image\t"
                             : slot == 2 ? L"primary-blink-preview-image\t"
                             : slot == 3 ? L"reaction-blink-preview-image\t"
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
    if (g_settingsWindow) {
        DestroyWindow(g_settingsWindow);
        g_settingsWindow = nullptr;
    }
    g_ownerWindow = nullptr;
}
