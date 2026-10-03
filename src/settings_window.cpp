#include "settings_window.h"
#include "resource.h"
#include "rs_build_config.hpp"
#include <commdlg.h>
#include <objidl.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <WebView2.h>
#include <wrl.h>
#include <wrl/event.h>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

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
bool g_settingsWasMaximised = false;
std::atomic<bool> g_settingsVisible{false};
std::wstring g_pendingPage;
std::wstring g_lastSettingsPage = L"avatar";
bool g_pendingPostUpgradeReview = false;

int hexadecimalValue(wchar_t character)
{
    if (character >= L'0' && character <= L'9') return character - L'0';
    if (character >= L'a' && character <= L'f') return character - L'a' + 10;
    if (character >= L'A' && character <= L'F') return character - L'A' + 10;
    return -1;
}

std::string decodeUriComponent(const wchar_t *encoded)
{
    std::string decoded;
    for (size_t index = 0; encoded[index]; ++index) {
        if (encoded[index] == L'%' && encoded[index + 1] && encoded[index + 2]) {
            const int high = hexadecimalValue(encoded[index + 1]);
            const int low = hexadecimalValue(encoded[index + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        if (encoded[index] <= 0x7f)
            decoded.push_back(static_cast<char>(encoded[index]));
    }
    return decoded;
}

bool exportDiagnosticReport(const wchar_t *encodedReport)
{
    SYSTEMTIME timestamp{};
    GetSystemTime(&timestamp);
    wchar_t path[MAX_PATH]{};
    swprintf_s(path, L"RearSilver-Avatar-Suite-Feedback-%04u-%02u-%02uT%02u-%02u-%02u-%03uZ.txt",
               timestamp.wYear, timestamp.wMonth, timestamp.wDay, timestamp.wHour,
               timestamp.wMinute, timestamp.wSecond, timestamp.wMilliseconds);
    OPENFILENAMEW picker{};
    picker.lStructSize = sizeof(picker);
    picker.hwndOwner = g_settingsWindow;
    picker.lpstrFilter = L"Text report (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0";
    picker.lpstrFile = path;
    picker.nMaxFile = ARRAYSIZE(path);
    picker.lpstrDefExt = L"txt";
    picker.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&picker)) return false;

    const std::string report = decodeUriComponent(encodedReport);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    constexpr unsigned char bom[] = {0xef, 0xbb, 0xbf};
    DWORD written = 0;
    bool succeeded = WriteFile(file, bom, sizeof(bom), &written, nullptr) != FALSE;
    if (succeeded && !report.empty())
        succeeded = WriteFile(file, report.data(), static_cast<DWORD>(report.size()), &written, nullptr) != FALSE;
    CloseHandle(file);
    return succeeded;
}

struct StreamSuiteState {
    bool registered = false;
    bool installValid = false;
    bool companionSupported = false;
    std::wstring executable;
};

std::wstring avatarSettingsFilePath()
{
    wchar_t localAppData[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData, ARRAYSIZE(localAppData));
    if (!length || length >= ARRAYSIZE(localAppData))
        return {};
    const std::wstring directory = std::wstring(localAppData, length) + L"\\RearSilver Avatar";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory + L"\\settings.ini";
}

void ensureUnicodeSettingsFile(const std::wstring &path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart == 0) {
        const wchar_t bom = 0xfeff;
        DWORD written = 0;
        WriteFile(file, &bom, sizeof(bom), &written, nullptr);
    }
    CloseHandle(file);
}

bool openWithStreamSuiteEnabled()
{
    const std::wstring settings = avatarSettingsFilePath();
    if (settings.empty())
        return false;
    wchar_t value[16]{};
    GetPrivateProfileStringW(L"Avatar", L"OpenWithStreamSuite", L"0", value,
                             ARRAYSIZE(value), settings.c_str());
    return wcscmp(value, L"1") == 0;
}

void saveOpenWithStreamSuite(bool enabled)
{
    const std::wstring settings = avatarSettingsFilePath();
    if (!settings.empty()) {
        ensureUnicodeSettingsFile(settings);
        WritePrivateProfileStringW(L"Avatar", L"OpenWithStreamSuite",
                                   enabled ? L"1" : L"0", settings.c_str());
    }
}

constexpr unsigned kGuidedSetupSchemaVersion = 2;
constexpr unsigned kGuidedSetupLastStep = 4;

struct GuidedSetupState {
    bool completed = false;
    unsigned step = 0;
    unsigned schemaVersion = kGuidedSetupSchemaVersion;
};

GuidedSetupState guidedSetupState()
{
    GuidedSetupState state;
    const std::wstring settings = avatarSettingsFilePath();
    if (settings.empty()) return state;
    const unsigned storedSchema = GetPrivateProfileIntW(L"Avatar", L"SetupSchemaVersion", 0, settings.c_str());
    if (storedSchema < kGuidedSetupSchemaVersion) return state;
    state.completed = GetPrivateProfileIntW(L"Avatar", L"SetupCompleted", 0, settings.c_str()) != 0;
    state.step = std::min<unsigned>(GetPrivateProfileIntW(L"Avatar", L"SetupStep", 0, settings.c_str()),
                                    kGuidedSetupLastStep);
    return state;
}

void saveGuidedSetupState(const GuidedSetupState &state)
{
    const std::wstring settings = avatarSettingsFilePath();
    if (settings.empty()) return;
    ensureUnicodeSettingsFile(settings);
    WritePrivateProfileStringW(L"Avatar", L"SetupCompleted", state.completed ? L"1" : L"0", settings.c_str());
    WritePrivateProfileStringW(L"Avatar", L"SetupStep", std::to_wstring(state.step).c_str(), settings.c_str());
    WritePrivateProfileStringW(L"Avatar", L"SetupSchemaVersion", std::to_wstring(state.schemaVersion).c_str(), settings.c_str());
}

void sendGuidedSetupState()
{
    if (!g_webView) return;
    const GuidedSetupState state = guidedSetupState();
    g_webView->PostWebMessageAsString((L"setup-state\t" + std::wstring(state.completed ? L"1" : L"0") + L"\t" +
        std::to_wstring(state.step) + L"\t" + std::to_wstring(state.schemaVersion)).c_str());
}

std::wstring widenAscii(const char *text)
{
    std::wstring result;
    while (*text) result.push_back(static_cast<unsigned char>(*text++));
    return result;
}

std::wstring widenUtf8(const std::string &text)
{
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                             static_cast<int>(text.size()), result.data(), count)) return {};
    return result;
}

void sendBuildState()
{
    if (!g_webView) return;
    const auto state = RsBuild::currentState();
    g_webView->PostWebMessageAsString((L"build-state\t" + widenAscii(RsBuild::kChannel) + L"\t" +
        widenAscii(RsBuild::kVersion) + L"\t" + widenAscii(RsBuild::kBuildId) + L"\t" +
        widenAscii(RsBuild::kBuildDate) + L"\t" + (RsBuild::kExpiryEnabled ? L"1" : L"0") + L"\t" +
        widenAscii(RsBuild::kExpiryDisplay) + L"\t" + std::to_wstring(state.daysRemaining) + L"\t" +
        (state.expired ? L"1" : L"0")).c_str());
}

void sendUpdateConfiguration()
{
    if (!g_webView) return;
    g_webView->PostWebMessageAsString((L"update-config\t" +
        std::wstring(RsBuild::kUpdateCheckEnabled ? L"1" : L"0") + L"\t" +
        widenAscii(RsBuild::kChannel) + L"\t" + widenAscii(RsBuild::kVersion)).c_str());
}

StreamSuiteState streamSuiteState()
{
    StreamSuiteState state;
    constexpr wchar_t uninstallKey[] =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RearSilver Stream Suite";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, uninstallKey, 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return state;
    state.registered = true;

    wchar_t installLocation[32768]{};
    DWORD bytes = sizeof(installLocation);
    const LSTATUS locationResult = RegGetValueW(
        key, nullptr, L"InstallLocation", RRF_RT_REG_SZ, nullptr, installLocation, &bytes);
    DWORD schema = 0;
    DWORD schemaBytes = sizeof(schema);
    const LSTATUS schemaResult = RegGetValueW(
        key, nullptr, L"AvatarCompanionSchema", RRF_RT_REG_DWORD, nullptr,
        &schema, &schemaBytes);
    RegCloseKey(key);
    state.companionSupported = schemaResult == ERROR_SUCCESS && schema >= 1;

    if (locationResult == ERROR_SUCCESS && installLocation[0]) {
        const std::filesystem::path installPath(installLocation);
        if (installPath.is_absolute()) {
            const std::filesystem::path executable =
                installPath / L"Control Hub" / L"RearSilver-Stream-Suite-Control-Hub.exe";
            const DWORD attributes = GetFileAttributesW(executable.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                state.installValid = true;
                state.executable = executable.wstring();
            }
        }
    }
    return state;
}

void sendGeneralState()
{
    if (!g_webView)
        return;
    const StreamSuiteState state = streamSuiteState();
    const wchar_t *status = !state.registered ? L"missing" :
                            !state.installValid ? L"invalid" :
                            !state.companionSupported ? L"unsupported" : L"ready";
    g_webView->PostWebMessageAsString(
        (L"stream-suite-state\t" + std::wstring(status)).c_str());
    g_webView->PostWebMessageAsString(
        (L"open-with-stream-suite\t" + std::wstring(openWithStreamSuiteEnabled() ? L"1" : L"0")).c_str());
}

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
    if (g_settingsWindow)
        g_settingsWasMaximised = IsZoomed(g_settingsWindow) != FALSE;
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
    if (!std::filesystem::is_regular_file(std::filesystem::path(assets) / L"avatar-settings.html")) {
        g_initialising = false;
        MessageBoxW(g_settingsWindow,
                    L"Avatar Suite Settings cannot open because avatar-settings.html is missing.\n\nRepair or reinstall RearSilver Avatar Suite.",
                    L"Settings files are incomplete", MB_OK | MB_ICONERROR);
        return;
    }
    const std::wstring data = webViewDataDirectory();
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [assets](HRESULT result, ICoreWebView2Environment *environment) -> HRESULT {
                if (FAILED(result) || !environment || !g_settingsWindow) {
                    g_initialising = false;
                    if (g_settingsWindow)
                        MessageBoxW(g_settingsWindow,
                                    L"Avatar Suite Settings could not start Microsoft Edge WebView2.\n\nRepair the WebView2 Runtime or reinstall RearSilver Avatar Suite.",
                                    L"Settings could not open", MB_OK | MB_ICONERROR);
                    return result;
                }
                return environment->CreateCoreWebView2Controller(
                    g_settingsWindow,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [assets](HRESULT controllerResult, ICoreWebView2Controller *controller) -> HRESULT {
                            g_initialising = false;
                            if (FAILED(controllerResult) || !controller || !g_settingsWindow) {
                                if (g_settingsWindow)
                                    MessageBoxW(g_settingsWindow,
                                                L"Avatar Suite Settings could not create its WebView2 window.\n\nRestart Avatar Suite. If this continues, repair the WebView2 Runtime.",
                                                L"Settings could not open", MB_OK | MB_ICONERROR);
                                return controllerResult;
                            }
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
                                            else if (wcsncmp(message, L"page-changed\t", 13) == 0) {
                                                const std::wstring page(message + 13);
                                                if (page != L"feedback" && page != L"updates" && page != L"help")
                                                    g_lastSettingsPage = page;
                                                if (page == L"general" || page == L"feedback" || page == L"updates")
                                                    sendGeneralState();
                                            }
                                            else if (wcscmp(message, L"diagnostics-refresh") == 0) {
                                                sendGeneralState();
                                                if (g_ownerWindow && IsWindow(g_ownerWindow))
                                                    PostMessageW(g_ownerWindow, kAvatarSettingsReadyMessage, 0, 0);
                                            }
                                            else if (wcscmp(message, L"update-check") == 0 && g_ownerWindow)
                                                PostMessageW(g_ownerWindow, kAvatarSettingsUpdateCheckMessage, 0, 0);
                                            else if (wcscmp(message, L"update-download") == 0 && g_ownerWindow)
                                                PostMessageW(g_ownerWindow, kAvatarSettingsUpdateDownloadMessage, 0, 0);
                                            else if (wcscmp(message, L"update-cancel") == 0 && g_ownerWindow)
                                                PostMessageW(g_ownerWindow, kAvatarSettingsUpdateCancelMessage, 0, 0);
                                            else if (wcscmp(message, L"update-install") == 0 && g_ownerWindow)
                                                PostMessageW(g_ownerWindow, kAvatarSettingsUpdateInstallMessage, 0, 0);
                                            else if (wcscmp(message, L"open-stream-suite") == 0) {
                                                const StreamSuiteState state = streamSuiteState();
                                                if (state.installValid && state.companionSupported)
                                                    ShellExecuteW(g_settingsWindow, L"open", state.executable.c_str(),
                                                                  nullptr, nullptr, SW_SHOWNORMAL);
                                                sendGeneralState();
                                            }
                                            else if (wcsncmp(message, L"open-with-stream-suite\t", 23) == 0) {
                                                const StreamSuiteState state = streamSuiteState();
                                                if (state.installValid && state.companionSupported)
                                                    saveOpenWithStreamSuite(wcscmp(message + 23, L"1") == 0);
                                                sendGeneralState();
                                            }
                                            else if (wcscmp(message, L"open-spout-plugin") == 0)
                                                ShellExecuteW(g_settingsWindow, L"open",
                                                    L"https://github.com/Off-World-Live/obs-spout2-plugin/releases",
                                                    nullptr, nullptr, SW_SHOWNORMAL);
                                            else if (wcscmp(message, L"open-logs-folder") == 0) {
                                                wchar_t localAppData[32768]{};
                                                if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, ARRAYSIZE(localAppData))) {
                                                    const std::filesystem::path logs = std::filesystem::path(localAppData) / L"RearSilver Avatar" / L"Logs";
                                                    std::error_code error;
                                                    std::filesystem::create_directories(logs, error);
                                                    if (!error) ShellExecuteW(g_settingsWindow, L"open", logs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                                }
                                            }
                                            else if (wcscmp(message, L"open-application-data") == 0) {
                                                wchar_t localAppData[32768]{};
                                                if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, ARRAYSIZE(localAppData))) {
                                                    const std::filesystem::path applicationData = std::filesystem::path(localAppData) / L"RearSilver Avatar";
                                                    std::error_code error;
                                                    std::filesystem::create_directories(applicationData, error);
                                                    if (!error) ShellExecuteW(g_settingsWindow, L"open", applicationData.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                                }
                                            }
                                            else if (wcsncmp(message, L"export-diagnostics\t", 19) == 0) {
                                                const bool exported = exportDiagnosticReport(message + 19);
                                                g_webView->PostWebMessageAsString(exported ? L"diagnostics-export\t1"
                                                                                         : L"diagnostics-export\t0");
                                            }
                                            else if (wcsncmp(message, L"setup-step\t", 11) == 0) {
                                                GuidedSetupState state = guidedSetupState();
                                                state.completed = false;
                                                state.step = std::min<unsigned>(wcstoul(message + 11, nullptr, 10),
                                                                                kGuidedSetupLastStep);
                                                saveGuidedSetupState(state);
                                                sendGuidedSetupState();
                                            }
                                            else if (wcscmp(message, L"setup-complete") == 0 ||
                                                     wcscmp(message, L"setup-skip") == 0) {
                                                GuidedSetupState state = guidedSetupState();
                                                state.completed = true;
                                                saveGuidedSetupState(state);
                                                sendGuidedSetupState();
                                            }
                                            else if (wcscmp(message, L"setup-reset") == 0) {
                                                GuidedSetupState state; saveGuidedSetupState(state); sendGuidedSetupState();
                                            }
                                            else if (wcscmp(message, L"update-current-preset") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsUpdatePresetMessage, 0, 0);
                                            else if (wcscmp(message, L"revert-current-preset") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsRevertPresetMessage, 0, 0);
                                            else if (wcscmp(message, L"add-layer") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsAddLayerMessage, 0,
                                                             reinterpret_cast<LPARAM>(g_settingsWindow));
                                            else if (wcsncmp(message, L"layer-command\t", 14) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsLayerCommandMessage, 0,
                                                             reinterpret_cast<LPARAM>(new std::wstring(message + 14)));
                                            else if (wcsncmp(message, L"layer-preview\t", 14) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsLayerCommandMessage, 1,
                                                             reinterpret_cast<LPARAM>(new std::wstring(message + 14)));
                                            else if (wcsncmp(message, L"preset-command\t", 15) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsPresetCommandMessage, 0,
                                                             reinterpret_cast<LPARAM>(new std::wstring(message + 15)));
                                            else if (wcscmp(message, L"websocket-state-request") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsWebSocketStateMessage, 0, 0);
                                            else if (wcscmp(message, L"choose-avatar-png") == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsChoosePngMessage,
                                                             0, reinterpret_cast<LPARAM>(g_settingsWindow));
                                            else if (wcscmp(message, L"avatar-settings-ready") == 0) {
                                                g_webViewReady = true;
                                                sendPendingPage();
                                                sendGeneralState();
                                                sendGuidedSetupState();
                                                sendBuildState();
                                                sendUpdateConfiguration();
                                                if (g_pendingPostUpgradeReview) {
                                                    g_pendingPostUpgradeReview = false;
                                                    g_webView->PostWebMessageAsString(L"setup-review-start");
                                                }
                                                if (g_ownerWindow && IsWindow(g_ownerWindow))
                                                    PostMessageW(g_ownerWindow, kAvatarSettingsReadyMessage, 0, 0);
                                            }
                                            else if (wcsncmp(message, L"avatar-scale\t", 13) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsScaleMessage,
                                                             wcstoul(message + 13, nullptr, 10), 0);
                                            else if (wcsncmp(message, L"avatar-flip\t", 12) == 0 &&
                                                     g_ownerWindow && IsWindow(g_ownerWindow))
                                                PostMessageW(g_ownerWindow, kAvatarSettingsFlipMessage,
                                                             wcscmp(message + 12, L"1") == 0, 0);
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
                            const std::wstring settingsUrl = L"https://app.rearsilver-avatar.test/avatar-settings.html?v=" +
                                                             std::to_wstring(GetTickCount64());
                            return g_webView->Navigate(settingsUrl.c_str());
                        }).Get());
            }).Get());
}

LRESULT CALLBACK settingsWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE: initialiseWebView(); return 0;
    case WM_SIZE:
        if (wParam == SIZE_MAXIMIZED) g_settingsWasMaximised = true;
        else if (wParam == SIZE_RESTORED) g_settingsWasMaximised = false;
        resizeWebView(); return 0;
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
    else
        g_pendingPage = g_lastSettingsPage;
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
    ShowWindow(g_settingsWindow, g_settingsWasMaximised ? SW_SHOWMAXIMIZED : SW_SHOW);
    g_settingsVisible.store(true);
    SetForegroundWindow(g_settingsWindow);
    if (g_controller)
        g_controller->put_IsVisible(TRUE);
    else
        initialiseWebView();
    sendPendingPage();
    return true;
}

bool showAvatarPostUpgradeReview(HWND owner)
{
    saveGuidedSetupState(GuidedSetupState{false, 0, kGuidedSetupSchemaVersion});
    g_pendingPostUpgradeReview = true;
    if (!showAvatarSettingsWindow(owner, L"presets")) {
        g_pendingPostUpgradeReview = false;
        return false;
    }
    if (g_webViewReady && g_webView) {
        g_pendingPostUpgradeReview = false;
        g_webView->PostWebMessageAsString(L"setup-review-start");
    }
    return true;
}

bool isAvatarSettingsWindowVisible()
{
    return g_settingsVisible.load();
}

HWND avatarSettingsWindowHandle()
{
    return g_settingsWindow && IsWindow(g_settingsWindow) ? g_settingsWindow : nullptr;
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
