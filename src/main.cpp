#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d2d1.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <tlhelp32.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <SpoutDX.h>

#include "settings_window.h"
#include "audio_monitor.h"
#include "layer_model.h"
#include "preset_store.h"
#include "resource.h"
#include "websocket_server.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"RearSilverAvatarWindow";
constexpr wchar_t kWindowTitle[] = L"RearSilver Avatar Suite";
constexpr wchar_t kSingleInstanceMutex[] = L"Local\\RearSilverAvatarSuite.SingleInstance";
constexpr UINT kRenderFailureMessage = WM_APP + 1;
constexpr UINT kImageUploadFailureMessage = WM_APP + 2;
constexpr UINT kImageUploadSuccessMessage = WM_APP + 3;
constexpr UINT kBlinkStateMessage = WM_APP + 4;
constexpr UINT kSpoutStatusChangedMessage = WM_APP + 5;
constexpr UINT kWebSocketCommandMessage = WM_APP + 6;
constexpr UINT kOutputWidth = 1920;
constexpr UINT kOutputHeight = 1080;
constexpr UINT kOutputUiDpi = 192;

HWND g_mainWindow = nullptr;
std::unique_ptr<WebSocketServer> g_webSocketServer;
std::mutex g_webSocketStateMutex;
std::wstring g_webSocketLastAction;
std::atomic<bool> g_running{false};
std::atomic<unsigned long long> g_presetGeneration{1};
std::atomic<bool> g_applicationActive{true};
std::atomic<bool> g_dialogOpen{false};
std::atomic<bool> g_reactionsEnabled{true};
std::atomic<unsigned> g_captureMethod{0};
std::atomic<unsigned> g_fixedBackgroundMode{0};
std::atomic<unsigned> g_windowBackgroundMode{2};
std::atomic<unsigned> g_backgroundSolidColour{0xffffff};
std::atomic<unsigned> g_backgroundChromaColour{0x00ff00};
std::atomic<unsigned> g_backgroundFit{0};
std::atomic<unsigned> g_avatarScalePercent{100};
std::atomic<bool> g_avatarFlipHorizontal{false};
std::atomic<unsigned> g_spoutStatus{0};
std::atomic<UINT> g_clientWidth{960};
std::atomic<UINT> g_clientHeight{720};
std::atomic<bool> g_transformPending{true};
bool g_borderlessFullscreen = false;
WINDOWPLACEMENT g_windowedPlacement{sizeof(WINDOWPLACEMENT)};
LONG_PTR g_windowedStyle = WS_OVERLAPPEDWINDOW;

std::mutex g_logMutex;
HANDLE g_logFile = INVALID_HANDLE_VALUE;

struct WebSocketCommandRequest {
    std::string payload;
    std::string response;
};

std::wstring fromUtf8(const std::string &value)
{
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string toUtf8(const std::wstring &value)
{
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
}

std::string jsonQuoted(const std::string &value)
{
    std::string result = "\"";
    for (const unsigned char character : value) {
        switch (character) {
        case '\"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20) {
                char escaped[7]{};
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", character);
                result += escaped;
            } else result.push_back(static_cast<char>(character));
        }
    }
    result.push_back('\"');
    return result;
}

std::string jsonQuoted(const std::wstring &value) { return jsonQuoted(toUtf8(value)); }

std::string jsonStringValue(const std::string &json, const std::string &key)
{
    const std::string marker = "\"" + key + "\"";
    size_t at = json.find(marker);
    if (at == std::string::npos) return {};
    at = json.find(':', at + marker.size());
    if (at == std::string::npos) return {};
    at = json.find('"', at + 1);
    if (at == std::string::npos) return {};
    std::string result;
    for (++at; at < json.size(); ++at) {
        const char character = json[at];
        if (character == '"') return result;
        if (character == '\\' && at + 1 < json.size()) {
            const char escaped = json[++at];
            if (escaped == '"' || escaped == '\\' || escaped == '/') result.push_back(escaped);
            else if (escaped == 'n') result.push_back('\n');
            else if (escaped == 'r') result.push_back('\r');
            else if (escaped == 't') result.push_back('\t');
            else return {};
        } else result.push_back(character);
    }
    return {};
}

bool jsonBooleanValue(const std::string &json, const std::string &key, bool &value)
{
    const std::string marker = "\"" + key + "\"";
    size_t at = json.find(marker);
    if (at == std::string::npos || (at = json.find(':', at + marker.size())) == std::string::npos)
        return false;
    ++at;
    while (at < json.size() && std::isspace(static_cast<unsigned char>(json[at]))) ++at;
    if (json.compare(at, 4, "true") == 0) { value = true; return true; }
    if (json.compare(at, 5, "false") == 0) { value = false; return true; }
    return false;
}

std::vector<std::string> jsonObjectArray(const std::string &json, const std::string &key)
{
    std::vector<std::string> result;
    const std::string marker = "\"" + key + "\"";
    size_t at = json.find(marker);
    if (at == std::string::npos || (at = json.find('[', at + marker.size())) == std::string::npos)
        return result;
    bool quoted = false, escaped = false;
    int depth = 0;
    size_t start = std::string::npos;
    for (++at; at < json.size(); ++at) {
        const char character = json[at];
        if (quoted) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') quoted = false;
            continue;
        }
        if (character == '"') { quoted = true; continue; }
        if (character == '{') { if (depth++ == 0) start = at; }
        else if (character == '}' && depth > 0 && --depth == 0 && start != std::string::npos) {
            result.push_back(json.substr(start, at - start + 1));
            start = std::string::npos;
        } else if (character == ']' && depth == 0) break;
    }
    return result;
}

enum class ImageSlot : WPARAM {
    Primary = 0,
    Reaction = 1,
    PrimaryBlink = 2,
    ReactionBlink = 3,
    Background = 4,
    Layer = 5,
};

struct PendingImage {
    std::vector<unsigned char> rgba;
    UINT width = 0;
    UINT height = 0;
    std::wstring path;
    ImageSlot slot = ImageSlot::Primary;
    bool persistSelection = true;
    bool clearSlot = false;
    unsigned long long presetGeneration = 0;
    std::wstring displayName;
    std::wstring layerId;
};

std::mutex g_pendingImageMutex;
std::deque<PendingImage> g_pendingImages;
std::mutex g_layerStateMutex;
layer_model::Composition g_composition;

std::mutex g_primaryImageStateMutex;
std::wstring g_primaryImagePath;
bool g_primaryImageLoaded = false;
std::wstring g_reactionImagePath;
bool g_reactionImageLoaded = false;
std::wstring g_defaultPrimaryImagePath;
std::wstring g_defaultReactionImagePath;
std::wstring g_primaryBlinkImagePath;
bool g_primaryBlinkImageLoaded = false;
std::wstring g_reactionBlinkImagePath;
bool g_reactionBlinkImageLoaded = false;
std::wstring g_backgroundImagePath;
bool g_backgroundImageLoaded = false;
std::mutex g_windowBackgroundMutex;
std::vector<unsigned char> g_windowBackgroundBgra;
UINT g_windowBackgroundWidth = 0;
UINT g_windowBackgroundHeight = 0;
std::atomic<bool> g_previewReaction{false};
std::atomic<bool> g_microphoneReaction{false};
std::atomic<bool> g_reactionAvailable{false};
std::atomic<unsigned> g_reactionThreshold{180};
std::atomic<unsigned> g_releaseDelayMs{100};
std::atomic<unsigned> g_noiseFloor{0};
std::atomic<unsigned> g_noiseSensitivity{60};
bool g_noiseCalibrationActive = false;
ULONGLONG g_noiseCalibrationStartedAt = 0;
std::uint64_t g_noiseCalibrationSum = 0;
unsigned g_noiseCalibrationSamples = 0;
ULONGLONG g_lastAboveThreshold = 0;
std::unique_ptr<AudioInputMonitor> g_audioMonitor;
std::wstring g_selectedMicrophoneId;
std::atomic<int> g_audioMonitorStatus{0};
std::atomic<unsigned> g_audioMonitorLevel{0};
std::atomic<bool> g_blinkEnabled{true};
std::atomic<unsigned> g_blinkMinimumMs{3000};
std::atomic<unsigned> g_blinkMaximumMs{6000};
std::atomic<unsigned> g_blinkDurationMs{150};
std::atomic<bool> g_bounceEnabled{true};
std::atomic<bool> g_bounceAdded{true};
std::atomic<unsigned> g_bounceHeightPixels{40};
std::atomic<unsigned> g_bounceDurationMs{350};
std::atomic<ULONGLONG> g_bounceStartedAt{0};
std::atomic<bool> g_breathingAdded{false};
std::atomic<bool> g_breathingEnabled{true};
std::atomic<unsigned> g_breathingMode{1};
std::atomic<unsigned> g_breathingIdleAmount{20};
std::atomic<unsigned> g_breathingReactionAmount{30};
std::atomic<unsigned> g_breathingCycleMs{2500};
std::atomic<bool> g_squashAdded{false};
std::atomic<bool> g_squashEnabled{true};
std::atomic<unsigned> g_squashIntensity{70};
std::atomic<unsigned> g_squashDurationMs{500};
std::atomic<ULONGLONG> g_squashStartedAt{0};
std::atomic<bool> g_shakeAdded{false};
std::atomic<bool> g_shakeEnabled{true};
std::atomic<unsigned> g_shakeIntensity{70};
std::atomic<unsigned> g_shakeSpeed{45};
std::atomic<unsigned> g_shakeDirection{0};
std::atomic<bool> g_shakeWobble{true};
std::atomic<ULONGLONG> g_shakePreviewUntil{0};
std::atomic<bool> g_brightnessAdded{false};
std::atomic<bool> g_brightnessEnabled{true};
std::atomic<unsigned> g_brightnessIdle{70};
std::atomic<unsigned> g_brightnessReaction{115};
std::atomic<unsigned> g_brightnessTransitionMs{150};
std::atomic<ULONGLONG> g_brightnessPreviewUntil{0};
std::atomic<bool> g_floatAdded{false};
std::atomic<bool> g_floatEnabled{true};
std::atomic<unsigned> g_floatMode{1};
std::atomic<unsigned> g_floatHeightPixels{35};
std::atomic<unsigned> g_floatCycleMs{4000};
std::atomic<unsigned> g_floatDirection{0};
std::atomic<unsigned> g_floatDriftPixels{30};
std::atomic<ULONGLONG> g_floatPreviewUntil{0};
std::atomic<bool> g_tiltAdded{false};
std::atomic<bool> g_tiltEnabled{true};
std::atomic<unsigned> g_tiltAngleDegrees{12};
std::atomic<unsigned> g_tiltDirection{2};
std::atomic<unsigned> g_tiltTransitionMs{225};
std::atomic<int> g_tiltActiveSign{-1};
std::atomic<bool> g_tiltAlternateRight{false};
std::atomic<ULONGLONG> g_tiltPreviewUntil{0};
std::mutex g_effectStackMutex;
std::wstring g_effectStack = L"bounce";

struct RectF {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;

    bool contains(float px, float py) const
    {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};

struct UiLayout {
    RectF presets;
    RectF reactions;
    RectF websocket;
    RectF backgrounds;
    RectF tools;
    RectF settings;
};

UiLayout calculateUiLayout(UINT, UINT, UINT dpi)
{
    const float scale = std::max(1.0f, static_cast<float>(dpi) / 96.0f);
    const float margin = 16.0f * scale;
    const float gap = 8.0f * scale;
    const float iconSize = 44.0f * scale;

    UiLayout result;
    result.presets = {margin, margin, iconSize, iconSize};
    result.reactions = {margin, margin + (iconSize + gap), iconSize, iconSize};
    result.websocket = {margin, margin + 2.0f * (iconSize + gap), iconSize, iconSize};
    result.backgrounds = {margin, margin + 3.0f * (iconSize + gap), iconSize, iconSize};
    result.tools = {margin, margin + 4.0f * (iconSize + gap), iconSize, iconSize};
    result.settings = {margin, margin + 5.0f * (iconSize + gap), iconSize, iconSize};
    return result;
}

void logMessage(const std::wstring &message)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t stamp[64]{};
    swprintf_s(stamp, L"%02u:%02u:%02u.%03u | ", time.wHour, time.wMinute, time.wSecond,
               time.wMilliseconds);
    const std::wstring line = std::wstring(stamp) + message + L"\r\n";
    OutputDebugStringW(line.c_str());
    if (g_logFile != INVALID_HANDLE_VALUE) {
        const int count = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                                              nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(count), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), count,
                            nullptr, nullptr);
        DWORD written = 0;
        WriteFile(g_logFile, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
}

void startLog()
{
    wchar_t tempPath[MAX_PATH + 1]{};
    if (GetTempPathW(MAX_PATH, tempPath) == 0)
        return;
    const std::wstring path = std::wstring(tempPath) + L"RearSilverAvatar-baseline.log";
    g_logFile = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    logMessage(L"RearSilver Avatar Suite starting");
}

std::wstring settingsFilePath()
{
    wchar_t localAppData[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) == 0)
        return {};
    const std::wstring directory = std::wstring(localAppData) + L"\\RearSilver Avatar";
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

std::wstring loadSetting(const wchar_t *key)
{
    const std::wstring path = settingsFilePath();
    if (path.empty())
        return {};
    if (preset_store::isPresetScopedKey(key)) {
        preset_store::initialise(path);
        return preset_store::loadDraftValue(key);
    }
    wchar_t value[32768]{};
    GetPrivateProfileStringW(L"Avatar", key, L"", value,
                             static_cast<DWORD>(std::size(value)), path.c_str());
    return value;
}

bool settingExists(const wchar_t *key)
{
    if (preset_store::isPresetScopedKey(key))
        return !loadSetting(key).empty();
    const std::wstring path = settingsFilePath();
    if (path.empty())
        return false;
    constexpr wchar_t missing[] = {1, 0};
    wchar_t value[2]{};
    const DWORD length = GetPrivateProfileStringW(L"Avatar", key, missing, value,
                                                   static_cast<DWORD>(std::size(value)), path.c_str());
    return !(length == 1 && value[0] == missing[0]);
}

void saveSetting(const wchar_t *key, const std::wstring &value)
{
    const std::wstring path = settingsFilePath();
    if (path.empty())
        return;
    if (preset_store::isPresetScopedKey(key)) {
        preset_store::initialise(path);
        preset_store::saveDraftValue(key, value);
        postAvatarSettingsMessage(L"preset-dirty\t1");
        return;
    }
    ensureUnicodeSettingsFile(path);
    WritePrivateProfileStringW(L"Avatar", key, value.c_str(), path.c_str());
}

std::wstring fileNameFromPath(const std::wstring &path)
{
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

std::wstring imageDisplayName(const wchar_t *key, const std::wstring &path)
{
    const std::wstring saved = loadSetting(key);
    if (!saved.empty()) return saved;
    const std::wstring fileName = fileNameFromPath(path);
    const size_t dot = fileName.rfind(L".png");
    if (dot == 64 && fileName.substr(0, dot).find_first_not_of(L"0123456789abcdef") == std::wstring::npos)
        return L"Imported PNG";
    return fileName;
}

std::wstring executableDirectory()
{
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring result(path, length);
    const size_t separator = result.find_last_of(L"\\/");
    return separator == std::wstring::npos ? L"." : result.substr(0, separator);
}

void sendPrimaryImageState()
{
    std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
    if (g_primaryImagePath.empty()) {
        setAvatarSettingsPreviewImage(static_cast<unsigned>(ImageSlot::Primary),
                                      g_defaultPrimaryImagePath);
        postAvatarSettingsMessage(L"avatar-image-default");
    } else if (g_primaryImageLoaded) {
        setAvatarSettingsPreviewImage(static_cast<unsigned>(ImageSlot::Primary), g_primaryImagePath);
        postAvatarSettingsMessage(L"avatar-image-current\t" + imageDisplayName(L"PrimaryImageDisplayName", g_primaryImagePath));
    } else {
        setAvatarSettingsPreviewImage(static_cast<unsigned>(ImageSlot::Primary),
                                      g_defaultPrimaryImagePath);
        postAvatarSettingsMessage(L"avatar-image-unavailable\t" + fileNameFromPath(g_primaryImagePath));
    }
}

void sendReactionImageState()
{
    std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
    if (g_reactionImagePath.empty()) {
        setAvatarSettingsPreviewImage(static_cast<unsigned>(ImageSlot::Reaction),
                                      g_defaultReactionImagePath);
        postAvatarSettingsMessage(L"reaction-image-default");
    } else if (g_reactionImageLoaded) {
        setAvatarSettingsPreviewImage(static_cast<unsigned>(ImageSlot::Reaction), g_reactionImagePath);
        postAvatarSettingsMessage(L"reaction-image-current\t" + imageDisplayName(L"ReactionImageDisplayName", g_reactionImagePath));
    } else {
        setAvatarSettingsPreviewImage(static_cast<unsigned>(ImageSlot::Reaction),
                                      g_defaultReactionImagePath);
        postAvatarSettingsMessage(L"reaction-image-unavailable\t" + fileNameFromPath(g_reactionImagePath));
    }
}

void sendBlinkImageState(ImageSlot slot)
{
    std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
    const bool reaction = slot == ImageSlot::ReactionBlink;
    const std::wstring &path = reaction ? g_reactionBlinkImagePath : g_primaryBlinkImagePath;
    const bool loaded = reaction ? g_reactionBlinkImageLoaded : g_primaryBlinkImageLoaded;
    const wchar_t *prefix = reaction ? L"reaction-blink-image" : L"primary-blink-image";
    if (path.empty()) {
        postAvatarSettingsMessage(std::wstring(prefix) + L"-empty");
    } else if (loaded) {
        setAvatarSettingsPreviewImage(static_cast<unsigned>(slot), path);
        postAvatarSettingsMessage(std::wstring(prefix) + L"-current\t" +
                                  imageDisplayName(reaction ? L"ReactionBlinkImageDisplayName" :
                                                              L"PrimaryBlinkImageDisplayName", path));
    } else {
        postAvatarSettingsMessage(std::wstring(prefix) + L"-unavailable\t" + fileNameFromPath(path));
    }
}

std::wstring colourText(unsigned colour)
{
    wchar_t value[8]{};
    swprintf_s(value, L"#%06X", colour & 0xffffff);
    return value;
}

unsigned activeBackgroundMode()
{
    return g_captureMethod.load() == 1 ? g_windowBackgroundMode.load()
                                       : g_fixedBackgroundMode.load();
}

void cacheWindowBackground(const PendingImage &image)
{
    std::vector<unsigned char> bgra = image.rgba;
    for (size_t index = 0; index + 3 < bgra.size(); index += 4) {
        const unsigned alpha = bgra[index + 3];
        bgra[index] = static_cast<unsigned char>(bgra[index] * alpha / 255);
        bgra[index + 1] = static_cast<unsigned char>(bgra[index + 1] * alpha / 255);
        bgra[index + 2] = static_cast<unsigned char>(bgra[index + 2] * alpha / 255);
        std::swap(bgra[index], bgra[index + 2]);
    }
    {
        std::lock_guard<std::mutex> lock(g_windowBackgroundMutex);
        g_windowBackgroundBgra = std::move(bgra);
        g_windowBackgroundWidth = image.width;
        g_windowBackgroundHeight = image.height;
    }
    if (g_mainWindow)
        InvalidateRect(g_mainWindow, nullptr, TRUE);
}

unsigned parseColour(const std::wstring &text, unsigned fallback)
{
    const wchar_t *value = text.c_str();
    if (*value == L'#')
        ++value;
    wchar_t *end = nullptr;
    const unsigned long parsed = wcstoul(value, &end, 16);
    return end && *end == L'\0' && end != value ? static_cast<unsigned>(parsed) & 0xffffff
                                                : fallback;
}

void sendBackgroundSettings()
{
    postAvatarSettingsMessage(L"capture-method\t" + std::to_wstring(g_captureMethod.load()));
    postAvatarSettingsMessage(L"background-mode\t" + std::to_wstring(activeBackgroundMode()));
    postAvatarSettingsMessage(L"background-solid-colour\t" +
                              colourText(g_backgroundSolidColour.load()));
    postAvatarSettingsMessage(L"background-chroma-colour\t" +
                              colourText(g_backgroundChromaColour.load()));
    postAvatarSettingsMessage(L"background-fit\t" + std::to_wstring(g_backgroundFit.load()));
    std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
    if (g_backgroundImagePath.empty())
        postAvatarSettingsMessage(L"background-image-empty");
    else if (g_backgroundImageLoaded)
        postAvatarSettingsMessage(L"background-image-current\t" +
                                  imageDisplayName(L"BackgroundImageDisplayName", g_backgroundImagePath));
    else
        postAvatarSettingsMessage(L"background-image-unavailable\t" +
                                  imageDisplayName(L"BackgroundImageDisplayName", g_backgroundImagePath));
}

void sendAvatarTransformSettings()
{
    postAvatarSettingsMessage(L"avatar-scale\t" +
                              std::to_wstring(g_avatarScalePercent.load()));
    postAvatarSettingsMessage(g_avatarFlipHorizontal.load() ? L"avatar-flip\t1" : L"avatar-flip\t0");
}

void sendSpoutSettings()
{
    postAvatarSettingsMessage(L"spout-status\t" + std::to_wstring(g_spoutStatus.load()));
}

void setSpoutStatus(HWND window, unsigned status)
{
    if (g_spoutStatus.exchange(status) != status)
        PostMessageW(window, kSpoutStatusChangedMessage, status, 0);
}

const wchar_t *imageMessagePrefix(ImageSlot slot)
{
    switch (slot) {
    case ImageSlot::Reaction: return L"reaction-image";
    case ImageSlot::PrimaryBlink: return L"primary-blink-image";
    case ImageSlot::ReactionBlink: return L"reaction-blink-image";
    case ImageSlot::Background: return L"background-image";
    default: return L"avatar-image";
    }
}

void sendBlinkSettings()
{
    postAvatarSettingsMessage(g_blinkEnabled.load() ? L"blink-enabled\t1" : L"blink-enabled\t0");
    postAvatarSettingsMessage(L"blink-minimum\t" + std::to_wstring(g_blinkMinimumMs.load()));
    postAvatarSettingsMessage(L"blink-maximum\t" + std::to_wstring(g_blinkMaximumMs.load()));
    postAvatarSettingsMessage(L"blink-duration\t" + std::to_wstring(g_blinkDurationMs.load()));
}

void sendBounceSettings()
{
    {
        std::lock_guard<std::mutex> lock(g_effectStackMutex);
        postAvatarSettingsMessage(L"effect-stack\t" + g_effectStack);
    }
    postAvatarSettingsMessage(g_bounceEnabled.load() ? L"bounce-enabled\t1" : L"bounce-enabled\t0");
    postAvatarSettingsMessage(L"bounce-height\t" + std::to_wstring(g_bounceHeightPixels.load()));
    postAvatarSettingsMessage(L"bounce-duration\t" + std::to_wstring(g_bounceDurationMs.load()));
    postAvatarSettingsMessage(g_breathingEnabled.load() ? L"breathing-enabled\t1"
                                                        : L"breathing-enabled\t0");
    postAvatarSettingsMessage(L"breathing-mode\t" + std::to_wstring(g_breathingMode.load()));
    postAvatarSettingsMessage(L"breathing-idle\t" +
                              std::to_wstring(g_breathingIdleAmount.load()));
    postAvatarSettingsMessage(L"breathing-reaction\t" +
                              std::to_wstring(g_breathingReactionAmount.load()));
    postAvatarSettingsMessage(L"breathing-cycle\t" +
                              std::to_wstring(g_breathingCycleMs.load()));
    postAvatarSettingsMessage(g_squashEnabled.load() ? L"squash-enabled\t1"
                                                      : L"squash-enabled\t0");
    postAvatarSettingsMessage(L"squash-intensity\t" +
                              std::to_wstring(g_squashIntensity.load()));
    postAvatarSettingsMessage(L"squash-duration\t" +
                              std::to_wstring(g_squashDurationMs.load()));
    postAvatarSettingsMessage(g_shakeEnabled.load() ? L"shake-enabled\t1"
                                                     : L"shake-enabled\t0");
    postAvatarSettingsMessage(L"shake-intensity\t" +
                              std::to_wstring(g_shakeIntensity.load()));
    postAvatarSettingsMessage(L"shake-speed\t" + std::to_wstring(g_shakeSpeed.load()));
    postAvatarSettingsMessage(L"shake-direction\t" +
                              std::to_wstring(g_shakeDirection.load()));
    postAvatarSettingsMessage(g_shakeWobble.load() ? L"shake-wobble\t1"
                                                    : L"shake-wobble\t0");
    postAvatarSettingsMessage(g_brightnessEnabled.load() ? L"brightness-enabled\t1"
                                                          : L"brightness-enabled\t0");
    postAvatarSettingsMessage(L"brightness-idle\t" +
                              std::to_wstring(g_brightnessIdle.load()));
    postAvatarSettingsMessage(L"brightness-reaction\t" +
                              std::to_wstring(g_brightnessReaction.load()));
    postAvatarSettingsMessage(L"brightness-transition\t" +
                              std::to_wstring(g_brightnessTransitionMs.load()));
    postAvatarSettingsMessage(g_floatEnabled.load() ? L"float-enabled\t1" : L"float-enabled\t0");
    postAvatarSettingsMessage(L"float-mode\t" + std::to_wstring(g_floatMode.load()));
    postAvatarSettingsMessage(L"float-height\t" + std::to_wstring(g_floatHeightPixels.load()));
    postAvatarSettingsMessage(L"float-cycle\t" + std::to_wstring(g_floatCycleMs.load()));
    postAvatarSettingsMessage(L"float-direction\t" +
                              std::to_wstring(g_floatDirection.load()));
    postAvatarSettingsMessage(L"float-drift\t" +
                              std::to_wstring(g_floatDriftPixels.load()));
    postAvatarSettingsMessage(g_tiltEnabled.load() ? L"tilt-enabled\t1" : L"tilt-enabled\t0");
    postAvatarSettingsMessage(L"tilt-angle\t" + std::to_wstring(g_tiltAngleDegrees.load()));
    postAvatarSettingsMessage(L"tilt-direction\t" + std::to_wstring(g_tiltDirection.load()));
    postAvatarSettingsMessage(L"tilt-transition\t" +
                              std::to_wstring(g_tiltTransitionMs.load()));
}

int selectTiltDirection()
{
    const unsigned direction = g_tiltDirection.load();
    if (direction == 0)
        return -1;
    if (direction == 1)
        return 1;
    const bool right = g_tiltAlternateRight.load();
    g_tiltAlternateRight.store(!right);
    return right ? 1 : -1;
}

void triggerReactionEffects()
{
    const ULONGLONG now = GetTickCount64();
    if (g_bounceAdded.load() && g_bounceEnabled.load()) {
        g_bounceStartedAt.store(now);
        postAvatarSettingsMessage(L"bounce-triggered");
    }
    if (g_squashAdded.load() && g_squashEnabled.load()) {
        g_squashStartedAt.store(now);
        postAvatarSettingsMessage(L"squash-triggered");
    }
    if (g_tiltAdded.load() && g_tiltEnabled.load()) {
        const int sign = selectTiltDirection();
        g_tiltActiveSign.store(sign);
        postAvatarSettingsMessage(L"tilt-triggered\t" + std::to_wstring(sign));
    }
}

void sendMicrophoneState()
{
    const std::vector<AudioInputDevice> devices = enumerateAudioInputDevices();
    std::wstring message = L"microphone-devices\t" + g_selectedMicrophoneId;
    for (const AudioInputDevice &device : devices)
        message += L"\n" + device.id + L"\t" + device.name;
    postAvatarSettingsMessage(message);
    const int status = g_audioMonitorStatus.load();
    postAvatarSettingsMessage(status == 1 ? L"microphone-status\tListening"
                                          : status == 2 ? L"microphone-status\tInput unavailable"
                                                        : L"microphone-status\tConnecting…");
    postAvatarSettingsMessage(L"microphone-level\t" +
                              std::to_wstring(g_audioMonitorLevel.load()));
    postAvatarSettingsMessage(L"reaction-threshold\t" +
                              std::to_wstring(g_reactionThreshold.load()));
    postAvatarSettingsMessage(L"release-delay\t" +
                              std::to_wstring(g_releaseDelayMs.load()));
    postAvatarSettingsMessage(L"noise-floor\t" + std::to_wstring(g_noiseFloor.load()));
    postAvatarSettingsMessage(L"noise-sensitivity\t" +
                              std::to_wstring(g_noiseSensitivity.load()));
    postAvatarSettingsMessage(g_microphoneReaction.load() ? L"microphone-reaction-on"
                                                           : L"microphone-reaction-off");
}

void setMicrophoneReaction(bool active)
{
    const bool previous = g_microphoneReaction.exchange(active);
    if (previous != active) {
        if (active)
            triggerReactionEffects();
        postAvatarSettingsMessage(active ? L"microphone-reaction-on"
                                         : L"microphone-reaction-off");
    }
}

void processMicrophoneLevel(unsigned level)
{
    if (!g_reactionsEnabled.load()) {
        setMicrophoneReaction(false);
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (g_noiseCalibrationActive) {
        g_noiseCalibrationSum += level;
        ++g_noiseCalibrationSamples;
        if (now - g_noiseCalibrationStartedAt >= 3000) {
            const unsigned floor = g_noiseCalibrationSamples > 0
                                       ? static_cast<unsigned>(g_noiseCalibrationSum /
                                                               g_noiseCalibrationSamples)
                                       : 0;
            g_noiseFloor.store(std::clamp<unsigned>(floor, 0, 950));
            g_reactionThreshold.store(std::clamp<unsigned>(
                g_noiseFloor.load() + g_noiseSensitivity.load(), 1, 1000));
            saveSetting(L"NoiseFloor", std::to_wstring(g_noiseFloor.load()));
            saveSetting(L"NoiseSensitivity", std::to_wstring(g_noiseSensitivity.load()));
            saveSetting(L"ReactionThreshold", std::to_wstring(g_reactionThreshold.load()));
            g_noiseCalibrationActive = false;
            postAvatarSettingsMessage(L"noise-floor\t" + std::to_wstring(g_noiseFloor.load()));
            postAvatarSettingsMessage(L"noise-sensitivity\t" +
                                      std::to_wstring(g_noiseSensitivity.load()));
            postAvatarSettingsMessage(L"reaction-threshold\t" +
                                      std::to_wstring(g_reactionThreshold.load()));
            postAvatarSettingsMessage(L"noise-calibration-complete");
        }
        return;
    }
    const unsigned threshold = g_reactionThreshold.load();
    const unsigned hysteresis = 20;
    if (level >= threshold) {
        g_lastAboveThreshold = now;
        setMicrophoneReaction(true);
    } else if (g_microphoneReaction.load() && level + hysteresis < threshold &&
               now - g_lastAboveThreshold >= g_releaseDelayMs.load()) {
        setMicrophoneReaction(false);
    }
}

void check(HRESULT result)
{
    if (FAILED(result))
        throw result;
}

bool sameLuid(const LUID &a, const LUID &b)
{
    return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
}

bool sameAdapterIdentity(const DXGI_ADAPTER_DESC1 &a, const DXGI_ADAPTER_DESC1 &b)
{
    return a.VendorId == b.VendorId && a.DeviceId == b.DeviceId && a.SubSysId == b.SubSysId;
}

struct AdapterCandidate {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    DXGI_ADAPTER_DESC1 description{};
    UINT relatedOpen = 0;
    UINT relatedTotal = 0;
    bool viable = false;
    bool selfOpen = false;
};

ComPtr<IDXGIAdapter1> chooseInteroperableAdapter()
{
    ComPtr<ID3D11Device> defaultDevice;
    ComPtr<ID3D11DeviceContext> defaultContext;
    D3D_FEATURE_LEVEL defaultFeature{};
    LUID defaultLuid{};
    bool haveDefault = false;
    const HRESULT defaultResult = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        defaultDevice.GetAddressOf(), &defaultFeature, defaultContext.GetAddressOf());
    if (SUCCEEDED(defaultResult)) {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(defaultDevice.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetDesc(&description))) {
            defaultLuid = description.AdapterLuid;
            haveDefault = true;
        }
    }

    ComPtr<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    std::vector<AdapterCandidate> candidates;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT enumerate = factory->EnumAdapters1(index, adapter.GetAddressOf());
        if (enumerate == DXGI_ERROR_NOT_FOUND)
            break;
        check(enumerate);

        AdapterCandidate candidate;
        candidate.adapter = adapter;
        check(adapter->GetDesc1(&candidate.description));
        if ((candidate.description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            D3D_FEATURE_LEVEL feature{};
            candidate.viable = SUCCEEDED(D3D11CreateDevice(
                adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                candidate.device.GetAddressOf(), &feature, candidate.context.GetAddressOf()));
        }
        logMessage(L"Adapter candidate " + std::to_wstring(index) + L": " +
                   candidate.description.Description + L"; LUID=" +
                   std::to_wstring(candidate.description.AdapterLuid.HighPart) + L":" +
                   std::to_wstring(candidate.description.AdapterLuid.LowPart) + L"; viable=" +
                   std::to_wstring(candidate.viable));
        candidates.push_back(std::move(candidate));
    }

    std::vector<std::vector<bool>> matrix(candidates.size(),
                                          std::vector<bool>(candidates.size(), false));
    for (size_t from = 0; from < candidates.size(); ++from) {
        if (!candidates[from].viable)
            continue;
        D3D11_TEXTURE2D_DESC description{};
        description.Width = 64;
        description.Height = 64;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        description.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        ComPtr<ID3D11Texture2D> sharedTexture;
        ComPtr<IDXGIResource> sharedResource;
        HANDLE sharedHandle = nullptr;
        if (SUCCEEDED(candidates[from].device->CreateTexture2D(&description, nullptr, &sharedTexture)) &&
            SUCCEEDED(sharedTexture.As(&sharedResource)) &&
            SUCCEEDED(sharedResource->GetSharedHandle(&sharedHandle))) {
            for (size_t to = 0; to < candidates.size(); ++to) {
                if (!candidates[to].viable)
                    continue;
                ComPtr<ID3D11Texture2D> opened;
                matrix[from][to] = SUCCEEDED(candidates[to].device->OpenSharedResource(
                    sharedHandle, IID_PPV_ARGS(&opened)));
                logMessage(L"Adapter matrix " + std::to_wstring(from) + L" -> " +
                           std::to_wstring(to) + L" = " + std::to_wstring(matrix[from][to]));
            }
        }
    }

    int defaultIndex = -1;
    for (size_t index = 0; index < candidates.size(); ++index) {
        if (haveDefault && sameLuid(candidates[index].description.AdapterLuid, defaultLuid))
            defaultIndex = static_cast<int>(index);
        if (!candidates[index].viable)
            continue;
        candidates[index].selfOpen = matrix[index][index];
        for (size_t target = 0; target < candidates.size(); ++target) {
            if (candidates[target].viable &&
                sameAdapterIdentity(candidates[index].description, candidates[target].description)) {
                ++candidates[index].relatedTotal;
                if (matrix[index][target])
                    ++candidates[index].relatedOpen;
            }
        }
    }

    int choice = defaultIndex >= 0 && candidates[defaultIndex].viable &&
                         candidates[defaultIndex].selfOpen
                     ? defaultIndex
                     : -1;
    if (choice >= 0) {
        for (size_t index = 0; index < candidates.size(); ++index) {
            if (candidates[index].viable && candidates[index].selfOpen &&
                sameAdapterIdentity(candidates[index].description, candidates[choice].description) &&
                candidates[index].relatedOpen > candidates[choice].relatedOpen)
                choice = static_cast<int>(index);
        }
    } else {
        for (size_t index = 0; index < candidates.size(); ++index) {
            if (candidates[index].viable && candidates[index].selfOpen &&
                (choice < 0 || candidates[index].relatedOpen > candidates[choice].relatedOpen))
                choice = static_cast<int>(index);
        }
    }
    if (choice < 0)
        throw E_FAIL;

    logMessage(L"Selected adapter candidate " + std::to_wstring(choice) + L"; LUID=" +
               std::to_wstring(candidates[choice].description.AdapterLuid.HighPart) + L":" +
               std::to_wstring(candidates[choice].description.AdapterLuid.LowPart) +
               L"; related-open=" + std::to_wstring(candidates[choice].relatedOpen) + L"/" +
               std::to_wstring(candidates[choice].relatedTotal));
    return candidates[choice].adapter;
}

bool decodePng(const wchar_t *path, PendingImage &decoded, UINT bundledTextureLimit = 0)
{
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&factory)));
    ComPtr<IWICBitmapDecoder> decoder;
    check(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                             WICDecodeMetadataCacheOnLoad, &decoder));
    ComPtr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, &frame));
    UINT width = 0;
    UINT height = 0;
    check(frame->GetSize(&width, &height));
    if (width == 0 || height == 0 ||
        (!bundledTextureLimit && (width > 8192 || height > 8192)))
        return false;

    ComPtr<IWICBitmapSource> source;
    check(frame.As(&source));
    if (bundledTextureLimit && (width > bundledTextureLimit || height > bundledTextureLimit)) {
        const double scale = std::min(static_cast<double>(bundledTextureLimit) / width,
                                      static_cast<double>(bundledTextureLimit) / height);
        width = std::max<UINT>(1, static_cast<UINT>(std::round(width * scale)));
        height = std::max<UINT>(1, static_cast<UINT>(std::round(height * scale)));
        ComPtr<IWICBitmapScaler> scaler;
        check(factory->CreateBitmapScaler(&scaler));
        check(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant));
        source = scaler;
    }

    ComPtr<IWICFormatConverter> converter;
    check(factory->CreateFormatConverter(&converter));
    check(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppRGBA,
                                WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom));
    decoded.rgba.resize(static_cast<size_t>(width) * height * 4);
    check(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(decoded.rgba.size()),
                                decoded.rgba.data()));
    decoded.width = width;
    decoded.height = height;
    decoded.path = path;
    return true;
}

void reloadDraftLayers()
{
    layer_model::Composition composition = layer_model::loadDraftComposition();
    {
        std::lock_guard<std::mutex> lock(g_layerStateMutex);
        g_composition = composition;
    }
    PendingImage clear;
    clear.slot = ImageSlot::Layer;
    clear.clearSlot = true;
    clear.persistSelection = false;
    {
        std::lock_guard<std::mutex> lock(g_pendingImageMutex);
        g_pendingImages.push_back(std::move(clear));
    }
    for (const layer_model::Layer &layer : composition.layers) {
        if (layer.imagePath.empty()) continue;
        try {
            PendingImage decoded;
            if (!decodePng(layer.imagePath.c_str(), decoded)) throw E_INVALIDARG;
            decoded.slot = ImageSlot::Layer;
            decoded.layerId = layer.id;
            decoded.persistSelection = false;
            std::lock_guard<std::mutex> lock(g_pendingImageMutex);
            g_pendingImages.push_back(std::move(decoded));
        } catch (...) {
            logMessage(L"Layer image is unavailable: " + layer.imagePath);
        }
        for (const auto &effect : layer.effects) {
            if (effect.type != layer_model::LocalEffectType::ArtworkStateChange || effect.imagePath.empty()) continue;
            try {
                PendingImage decoded;
                if (!decodePng(effect.imagePath.c_str(), decoded)) throw E_INVALIDARG;
                decoded.slot = ImageSlot::Layer;
                decoded.layerId = layer.id + L"::effect::" + effect.id;
                decoded.persistSelection = false;
                std::lock_guard<std::mutex> lock(g_pendingImageMutex);
                g_pendingImages.push_back(std::move(decoded));
            } catch (...) {
                logMessage(L"Layer effect image is unavailable: " + effect.imagePath);
            }
        }
    }
}

void applyPresetDraftToRuntime()
{
    auto number = [](const wchar_t *key, unsigned fallback, unsigned minimum, unsigned maximum) {
        const std::wstring value = loadSetting(key);
        return value.empty() ? fallback : std::clamp<unsigned>(wcstoul(value.c_str(), nullptr, 10), minimum, maximum);
    };
    auto flag = [](const wchar_t *key, bool fallback) {
        const std::wstring value = loadSetting(key);
        return value.empty() ? fallback : value != L"0";
    };
    auto queueImage = [](const wchar_t *key, ImageSlot slot, const std::wstring &fallback,
                         std::wstring &statePath, bool &stateLoaded) {
        const std::wstring path = loadSetting(key);
        {
            std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
            statePath = path;
            stateLoaded = false;
        }
        const std::wstring source = path.empty() ? fallback : path;
        if (source.empty()) {
            if (slot == ImageSlot::Background) {
                std::lock_guard<std::mutex> lock(g_windowBackgroundMutex);
                g_windowBackgroundBgra.clear();
                g_windowBackgroundWidth = 0;
                g_windowBackgroundHeight = 0;
            }
            PendingImage clear;
            clear.slot = slot;
            clear.clearSlot = true;
            clear.persistSelection = false;
            std::lock_guard<std::mutex> lock(g_pendingImageMutex);
            g_pendingImages.push_back(std::move(clear));
            return;
        }
        try {
            PendingImage decoded;
            const UINT bundledLimit = source == fallback && !fallback.empty() ? 4096 : 0;
            if (!decodePng(source.c_str(), decoded, bundledLimit)) throw E_INVALIDARG;
            decoded.slot = slot;
            decoded.persistSelection = false;
            if (slot == ImageSlot::Background) cacheWindowBackground(decoded);
            std::lock_guard<std::mutex> lock(g_pendingImageMutex);
            g_pendingImages.push_back(std::move(decoded));
        } catch (...) {
            if (!fallback.empty() && source != fallback) {
                try {
                    PendingImage decoded;
                    if (!decodePng(fallback.c_str(), decoded, 4096)) throw E_INVALIDARG;
                    decoded.slot = slot;
                    decoded.persistSelection = false;
                    std::lock_guard<std::mutex> lock(g_pendingImageMutex);
                    g_pendingImages.push_back(std::move(decoded));
                } catch (...) {}
            }
        }
    };

    queueImage(L"PrimaryImage", ImageSlot::Primary, g_defaultPrimaryImagePath,
               g_primaryImagePath, g_primaryImageLoaded);
    queueImage(L"ReactionImage", ImageSlot::Reaction, g_defaultReactionImagePath,
               g_reactionImagePath, g_reactionImageLoaded);
    queueImage(L"PrimaryBlinkImage", ImageSlot::PrimaryBlink, L"",
               g_primaryBlinkImagePath, g_primaryBlinkImageLoaded);
    queueImage(L"ReactionBlinkImage", ImageSlot::ReactionBlink, L"",
               g_reactionBlinkImagePath, g_reactionBlinkImageLoaded);
    queueImage(L"BackgroundImage", ImageSlot::Background, L"",
               g_backgroundImagePath, g_backgroundImageLoaded);
    reloadDraftLayers();

    g_avatarScalePercent.store(number(L"AvatarScalePercent", 100, 25, 250));
    g_avatarFlipHorizontal.store(flag(L"AvatarFlipHorizontal", false));
    g_reactionsEnabled.store(flag(L"ReactionsEnabled", true));
    g_blinkEnabled.store(flag(L"BlinkEnabled", true));
    g_blinkMinimumMs.store(number(L"BlinkMinimumMs", 3000, 250, 30000));
    g_blinkMaximumMs.store(number(L"BlinkMaximumMs", 6000, g_blinkMinimumMs.load(), 30000));
    g_blinkDurationMs.store(number(L"BlinkDurationMs", 150, 50, 1000));
    g_windowBackgroundMode.store(number(L"WindowBackgroundMode", 2, 1, 3));
    g_fixedBackgroundMode.store(number(L"FixedBackgroundMode", 0, 0, 3) == 3 ? 3 : 0);
    g_backgroundFit.store(number(L"BackgroundFit", 0, 0, 3));

    const std::wstring stack = settingExists(L"EffectStack") ? loadSetting(L"EffectStack") : L"bounce";
    {
        std::lock_guard<std::mutex> lock(g_effectStackMutex);
        g_effectStack = stack;
    }
    const std::wstring padded = L"," + stack + L",";
    g_bounceAdded.store(padded.find(L",bounce,") != std::wstring::npos);
    g_breathingAdded.store(padded.find(L",breathing,") != std::wstring::npos);
    g_squashAdded.store(padded.find(L",squash,") != std::wstring::npos);
    g_shakeAdded.store(padded.find(L",shake,") != std::wstring::npos);
    g_brightnessAdded.store(padded.find(L",brightness,") != std::wstring::npos);
    g_floatAdded.store(padded.find(L",float,") != std::wstring::npos);
    g_tiltAdded.store(padded.find(L",tilt,") != std::wstring::npos);

    g_bounceEnabled.store(flag(L"BounceEnabled", true));
    g_bounceHeightPixels.store(number(L"BounceHeightPixels", 40, 5, 160));
    g_bounceDurationMs.store(number(L"BounceDurationMs", 350, 100, 1200));
    g_breathingEnabled.store(flag(L"BreathingEnabled", true));
    g_breathingMode.store(number(L"BreathingMode", 1, 0, 2));
    g_breathingIdleAmount.store(number(L"BreathingIdleAmount", 20, 0, 100));
    g_breathingReactionAmount.store(number(L"BreathingReactionAmount", 30, 0, 100));
    g_breathingCycleMs.store(number(L"BreathingCycleMs", 2500, 500, 10000));
    g_squashEnabled.store(flag(L"SquashEnabled", true));
    g_squashIntensity.store(number(L"SquashIntensity", 70, 0, 300));
    g_squashDurationMs.store(number(L"SquashDurationMs", 500, 200, 1600));
    g_shakeEnabled.store(flag(L"ShakeEnabled", true));
    g_shakeIntensity.store(number(L"ShakeIntensity", 70, 0, 300));
    g_shakeSpeed.store(number(L"ShakeSpeed", 45, 8, 120));
    g_shakeDirection.store(number(L"ShakeDirection", 0, 0, 2));
    g_shakeWobble.store(flag(L"ShakeWobble", true));
    g_brightnessEnabled.store(flag(L"BrightnessEnabled", true));
    g_brightnessIdle.store(number(L"BrightnessIdle", 70, 10, 100));
    g_brightnessReaction.store(number(L"BrightnessReaction", 115, 100, 200));
    g_brightnessTransitionMs.store(number(L"BrightnessTransitionMs", 150, 0, 2000));
    g_floatEnabled.store(flag(L"FloatEnabled", true));
    g_floatMode.store(number(L"FloatMode", 1, 0, 2));
    const std::wstring floatHeight = loadSetting(L"FloatHeightPixels");
    const std::wstring legacyFloatHeight = loadSetting(L"FloatIdlePixels");
    g_floatHeightPixels.store(std::clamp<unsigned>(
        wcstoul((!floatHeight.empty() ? floatHeight : legacyFloatHeight).c_str(), nullptr, 10), 0, 160));
    if (floatHeight.empty() && legacyFloatHeight.empty()) g_floatHeightPixels.store(35);
    g_floatCycleMs.store(number(L"FloatCycleMs", 4000, 1000, 12000));
    g_floatDirection.store(number(L"FloatDirection", 0, 0, 2));
    g_floatDriftPixels.store(number(L"FloatDriftPixels", 30, 0, 120));
    g_tiltEnabled.store(flag(L"TiltEnabled", true));
    g_tiltAngleDegrees.store(number(L"TiltAngleDegrees", 12, 0, 60));
    g_tiltDirection.store(number(L"TiltDirection", 2, 0, 2));
    g_tiltTransitionMs.store(number(L"TiltTransitionMs", 225, 0, 2000));
    if (g_tiltDirection.load() < 2)
        g_tiltActiveSign.store(g_tiltDirection.load() == 0 ? -1 : 1);

    sendPrimaryImageState();
    sendReactionImageState();
    sendBlinkImageState(ImageSlot::PrimaryBlink);
    sendBlinkImageState(ImageSlot::ReactionBlink);
    sendBlinkSettings();
    sendBounceSettings();
    sendBackgroundSettings();
    sendAvatarTransformSettings();
    g_transformPending.store(true);
    if (g_mainWindow) InvalidateRect(g_mainWindow, nullptr, TRUE);
}

void openPngPicker(HWND owner = nullptr, ImageSlot slot = ImageSlot::Primary)
{
    wchar_t path[32768]{};
    OPENFILENAMEW picker{};
    picker.lStructSize = sizeof(picker);
    picker.hwndOwner = owner && IsWindow(owner) ? owner : g_mainWindow;
    picker.lpstrFilter = L"PNG images\0*.png\0All files\0*.*\0";
    picker.lpstrFile = path;
    picker.nMaxFile = static_cast<DWORD>(std::size(path));
    picker.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    g_dialogOpen.store(true);
    const BOOL selected = GetOpenFileNameW(&picker);
    if (selected) {
        try {
            PendingImage decoded;
            if (!decodePng(path, decoded))
                throw E_INVALIDARG;
            const std::wstring managedPath = preset_store::importPngAsset(path);
            if (!managedPath.empty())
                decoded.path = managedPath;
            decoded.slot = slot;
            decoded.presetGeneration = g_presetGeneration.load();
            decoded.displayName = fileNameFromPath(path);
            if (slot == ImageSlot::Background)
                cacheWindowBackground(decoded);
            {
                std::lock_guard<std::mutex> lock(g_pendingImageMutex);
                g_pendingImages.push_back(std::move(decoded));
            }
            logMessage(L"PNG decoded and queued for render-thread upload: " + std::wstring(path));
            const wchar_t *fileName = wcsrchr(path, L'\\');
            postAvatarSettingsMessage(std::wstring(imageMessagePrefix(slot)) + L"-selected\t" +
                                      (fileName ? fileName + 1 : path));
        } catch (...) {
            postAvatarSettingsMessage(std::wstring(imageMessagePrefix(slot)) + L"-error");
            MessageBoxW(picker.hwndOwner,
                        L"Could not decode this image. Choose a valid PNG no larger than 8192 × 8192 pixels. The current avatar is unchanged.",
                        L"RearSilver Avatar Suite — PNG loading", MB_OK | MB_ICONERROR);
        }
    } else if (CommDlgExtendedError() == 0) {
        postAvatarSettingsMessage(std::wstring(imageMessagePrefix(slot)) + L"-cancelled");
    }
    g_dialogOpen.store(false);
}

void sendLayerState()
{
    const layer_model::Composition composition = layer_model::loadDraftComposition();
    postAvatarSettingsMessage(L"layer-list-reset\t" + std::to_wstring(composition.layers.size()));
    auto sendEffects = [](const std::wstring &ownerKind, const std::wstring &ownerId,
                          const std::vector<layer_model::LocalEffect> &effects) {
        for (const auto &effect : effects) {
            const wchar_t *type = L"sway";
            switch (effect.type) {
            case layer_model::LocalEffectType::Sway: type=L"sway"; break;
            case layer_model::LocalEffectType::BoundedMovement: type=L"bounded-movement"; break;
            case layer_model::LocalEffectType::Orbit: type=L"orbit"; break;
            case layer_model::LocalEffectType::LocalPulse: type=L"local-pulse"; break;
            case layer_model::LocalEffectType::LocalSpin: type=L"local-spin"; break;
            case layer_model::LocalEffectType::ReactionNudge: type=L"reaction-nudge"; break;
            case layer_model::LocalEffectType::StateVisibility: type=L"state-visibility"; break;
            case layer_model::LocalEffectType::DangleSpring: type=L"dangle-spring"; break;
            case layer_model::LocalEffectType::Flutter: type=L"flutter"; break;
            case layer_model::LocalEffectType::ArtworkStateChange: type=L"artwork-state-change"; break;
            }
            postAvatarSettingsMessage(L"local-effect\t" + ownerKind + L"\t" + ownerId + L"\t" + effect.id + L"\t" +
                type + L"\t" +
                (effect.enabled ? L"1" : L"0") + L"\t" + std::to_wstring(effect.amountX) + L"\t" +
                std::to_wstring(effect.amountY) + L"\t" + std::to_wstring(effect.cycleMs) + L"\t" +
                std::to_wstring(effect.reactionBoost) + L"\t" + std::to_wstring(effect.pivot) + L"\t" +
                std::to_wstring(effect.activeDuring) + L"\t" +
                (effect.imageDisplayName.empty() && !effect.imagePath.empty() ? L"Selected PNG" : effect.imageDisplayName) + L"\t" +
                std::to_wstring(effect.artworkScaleX) + L"\t" + std::to_wstring(effect.artworkScaleY) + L"\t" +
                (effect.artworkScaleLinked ? L"1" : L"0"));
        }
    };
    auto sendLayer = [&](const layer_model::Layer &layer, const std::wstring &groupId) {
        postAvatarSettingsMessage(L"layer-item\t" + layer.id + L"\t" + layer.name + L"\t" +
                                  fileNameFromPath(layer.imagePath) + L"\t" +
                                  (layer.visible ? L"1" : L"0") + L"\t" +
                                  (layer.abovePrimary ? L"1" : L"0") + L"\t" + layer.purpose + L"\t" +
                                  (layer.inheritAvatarEffects ? L"1" : L"0") + L"\t" +
                                  std::to_wstring(layer.positionX) + L"\t" + std::to_wstring(layer.positionY) + L"\t" +
                                  std::to_wstring(layer.pivotX) + L"\t" + std::to_wstring(layer.pivotY) + L"\t" +
                                  std::to_wstring(layer.scaleX) + L"\t" + std::to_wstring(layer.scaleY) + L"\t" +
                                  std::to_wstring(layer.rotationTenths) + L"\t" + std::to_wstring(layer.opacity) + L"\t" +
                                  (layer.scaleLinked ? L"1" : L"0") + L"\t" +
                                  (layer.tailWagAdded ? L"1" : L"0") + L"\t" +
                                  (layer.tailWagEnabled ? L"1" : L"0") + L"\t" +
                                  std::to_wstring(layer.tailWagAngle) + L"\t" +
                                  std::to_wstring(layer.tailWagCycleMs) + L"\t" +
                                  std::to_wstring(layer.tailWagReactionBoost) + L"\t" +
                                  std::to_wstring(layer.tailWagPivot) + L"\t" +
                                  (layer.flipHorizontal ? L"1" : L"0") + L"\t" +
                                  (layer.swayAdded ? L"1" : L"0") + L"\t" +
                                  (layer.swayEnabled ? L"1" : L"0") + L"\t" +
                                  std::to_wstring(layer.swayAngle) + L"\t" +
                                  std::to_wstring(layer.swayCycleMs) + L"\t" +
                                  std::to_wstring(layer.swayReactionBoost) + L"\t" +
                                  std::to_wstring(layer.swayPivot) + L"\t" +
                                  (layer.eyeMovementAdded ? L"1" : L"0") + L"\t" +
                                  (layer.eyeMovementEnabled ? L"1" : L"0") + L"\t" +
                                  std::to_wstring(layer.eyeRangeX) + L"\t" +
                                  std::to_wstring(layer.eyeRangeY) + L"\t" +
                                  std::to_wstring(layer.eyeCycleMs) + L"\t" +
                                  std::to_wstring(layer.eyeActiveDuring) + L"\t" + groupId);
        sendEffects(L"layer", layer.id, layer.effects);
    };
    for (const layer_model::StackItem &item : composition.rootOrder) {
        if (item.type == layer_model::StackItemType::Primary) {
            postAvatarSettingsMessage(L"layer-primary");
        } else if (item.type == layer_model::StackItemType::Layer) {
            const auto layer = std::find_if(composition.layers.begin(), composition.layers.end(),
                [&](const layer_model::Layer &candidate) { return candidate.id == item.id; });
            if (layer != composition.layers.end()) sendLayer(*layer, L"");
        } else if (item.type == layer_model::StackItemType::Group) {
            const auto group = std::find_if(composition.groups.begin(), composition.groups.end(),
                [&](const layer_model::Group &candidate) { return candidate.id == item.id; });
            if (group == composition.groups.end()) continue;
            postAvatarSettingsMessage(L"layer-group\t" + group->id + L"\t" + group->name + L"\t" +
                (group->visible ? L"1" : L"0") + L"\t" + std::to_wstring(group->positionX) + L"\t" +
                std::to_wstring(group->positionY) + L"\t" + std::to_wstring(group->scaleX) + L"\t" +
                std::to_wstring(group->scaleY) + L"\t" + std::to_wstring(group->rotationTenths) + L"\t" +
                std::to_wstring(group->opacity) + L"\t" + (group->scaleLinked ? L"1" : L"0") + L"\t" +
                (group->flipHorizontal ? L"1" : L"0") + L"\t" +
                (group->inheritAvatarEffects ? L"1" : L"0"));
            sendEffects(L"group", group->id, group->effects);
            for (const std::wstring &layerId : group->layerOrder) {
                const auto layer = std::find_if(composition.layers.begin(), composition.layers.end(),
                    [&](const layer_model::Layer &candidate) { return candidate.id == layerId; });
                if (layer != composition.layers.end()) sendLayer(*layer, group->id);
            }
            postAvatarSettingsMessage(L"layer-group-complete\t" + group->id);
        }
    }
    postAvatarSettingsMessage(L"layer-list-complete");
}

void sendPresetState()
{
    postAvatarSettingsMessage(L"preset-state\t" + preset_store::activePresetId() + L"\t" +
                              preset_store::activePresetName() + L"\t" +
                              (preset_store::hasUnsavedChanges() ? L"1" : L"0"));
    const auto presets = preset_store::listPresets();
    postAvatarSettingsMessage(L"preset-list-reset\t" + std::to_wstring(presets.size()));
    for (const auto &preset : presets)
        postAvatarSettingsMessage(L"preset-item\t" + preset.id + L"\t" + preset.name + L"\t" +
                                  (preset.active ? L"1" : L"0") + L"\t" +
                                  (preset.dirty ? L"1" : L"0"));
    postAvatarSettingsMessage(L"preset-list-complete");
}

void sendWebSocketState()
{
    const bool listening = g_webSocketServer && g_webSocketServer->running();
    const unsigned clients = listening ? g_webSocketServer->connectedClients() : 0;
    const unsigned long long messages = listening ? g_webSocketServer->messagesReceived() : 0;
    std::wstring lastAction;
    {
        std::lock_guard<std::mutex> lock(g_webSocketStateMutex);
        lastAction = g_webSocketLastAction;
    }
    postAvatarSettingsMessage(L"websocket-state\t" + std::wstring(listening ? L"1" : L"0") + L"\t" +
                              std::to_wstring(clients) + L"\t" + std::to_wstring(messages) + L"\t" + lastAction);
}

std::vector<std::wstring> splitPresetCommand(const std::wstring &command)
{
    std::vector<std::wstring> fields;
    size_t start = 0;
    while (start <= command.size()) {
        const size_t end = command.find(L'\t', start);
        fields.push_back(command.substr(start, end == std::wstring::npos ? end : end - start));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return fields;
}

bool resolveDirtyPresetSwitch(const std::wstring &disposition)
{
    if (!preset_store::hasUnsavedChanges()) return true;
    if (disposition == L"update") return preset_store::updateActivePreset();
    if (disposition == L"discard") return preset_store::revertActivePreset();
    return false;
}

void exportPresetWithPicker(const std::wstring &id)
{
    std::wstring name = L"Avatar preset";
    for (const auto &preset : preset_store::listPresets()) if (preset.id == id) name = preset.name;
    for (wchar_t &character : name)
        if (wcschr(L"<>:\"/\\|?*", character)) character = L'_';
    wchar_t path[32768]{};
    wcsncpy_s(path, (name + L".rasavatar").c_str(), _TRUNCATE);
    OPENFILENAMEW picker{}; picker.lStructSize = sizeof(picker); picker.hwndOwner = g_mainWindow;
    picker.lpstrFilter = L"RearSilver Avatar preset\0*.rasavatar\0All files\0*.*\0";
    picker.lpstrFile = path; picker.nMaxFile = static_cast<DWORD>(std::size(path));
    picker.lpstrDefExt = L"rasavatar"; picker.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    g_dialogOpen.store(true);
    if (GetSaveFileNameW(&picker) && !preset_store::exportPreset(id, path))
        MessageBoxW(g_mainWindow, L"The preset could not be exported.",
                    L"RearSilver Avatar Suite — Presets", MB_OK | MB_ICONERROR);
    g_dialogOpen.store(false);
}

bool importPresetWithPicker()
{
    wchar_t path[32768]{};
    OPENFILENAMEW picker{}; picker.lStructSize = sizeof(picker); picker.hwndOwner = g_mainWindow;
    picker.lpstrFilter = L"RearSilver Avatar preset\0*.rasavatar\0All files\0*.*\0";
    picker.lpstrFile = path; picker.nMaxFile = static_cast<DWORD>(std::size(path));
    picker.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    g_dialogOpen.store(true);
    bool imported = false;
    if (GetOpenFileNameW(&picker)) {
        std::wstring created;
        if (preset_store::importPreset(path, created)) {
            g_presetGeneration.fetch_add(1);
            imported = preset_store::selectPreset(created);
        } else {
            MessageBoxW(g_mainWindow, L"This preset package is invalid or could not be imported.",
                        L"RearSilver Avatar Suite — Presets", MB_OK | MB_ICONERROR);
        }
    }
    g_dialogOpen.store(false);
    return imported;
}

void handlePresetCommand(const std::wstring &command)
{
    const auto fields = splitPresetCommand(command);
    if (fields.empty()) return;
    bool reload = false;
    if (fields[0] == L"select" && fields.size() >= 2) {
        if (fields[1] == preset_store::activePresetId()) return;
        const std::wstring disposition = fields.size() >= 3 ? fields[2] : L"";
        if (!resolveDirtyPresetSwitch(disposition)) {
            postAvatarSettingsMessage(L"preset-switch-required\tselect\t" + fields[1]);
            return;
        }
        g_presetGeneration.fetch_add(1);
        reload = preset_store::selectPreset(fields[1]);
    } else if ((fields[0] == L"create" || fields[0] == L"duplicate") && fields.size() >= 2) {
        const std::wstring disposition = fields.size() >= 4 ? fields[3] : L"";
        if (!resolveDirtyPresetSwitch(disposition)) {
            postAvatarSettingsMessage(L"preset-switch-required\t" + fields[0] + L"\t" + fields[1] +
                                      L"\t" + (fields.size() >= 3 ? fields[2] : L""));
            return;
        }
        std::wstring created;
        const std::wstring source = fields[0] == L"duplicate" && fields.size() >= 3 ? fields[2] : L"";
        if (preset_store::createPreset(fields[1], source, created)) {
            g_presetGeneration.fetch_add(1);
            reload = preset_store::selectPreset(created);
        }
    } else if (fields[0] == L"rename" && fields.size() >= 3) {
        preset_store::renamePreset(fields[1], fields[2]);
    } else if (fields[0] == L"delete" && fields.size() >= 2) {
        const bool wasActive = fields[1] == preset_store::activePresetId();
        if (wasActive) g_presetGeneration.fetch_add(1);
        if (preset_store::deletePreset(fields[1])) reload = wasActive;
    } else if (fields[0] == L"export" && fields.size() >= 2) {
        exportPresetWithPicker(fields[1]);
    } else if (fields[0] == L"import") {
        const std::wstring disposition = fields.size() >= 2 ? fields[1] : L"";
        if (!resolveDirtyPresetSwitch(disposition)) {
            postAvatarSettingsMessage(L"preset-switch-required\timport");
            return;
        }
        reload = importPresetWithPicker();
    }
    if (reload) {
        applyPresetDraftToRuntime();
        sendLayerState();
    }
    sendPresetState();
}

void addLayerFromPicker(HWND owner)
{
    if (layer_model::loadDraftLayers().size() >= layer_model::kMaximumLayers) {
        MessageBoxW(owner, L"A preset can contain up to 16 additional layers.",
                    L"RearSilver Avatar Suite — Layers", MB_OK | MB_ICONINFORMATION);
        return;
    }
    wchar_t path[32768]{};
    OPENFILENAMEW picker{};
    picker.lStructSize = sizeof(picker);
    picker.hwndOwner = owner && IsWindow(owner) ? owner : g_mainWindow;
    picker.lpstrFilter = L"PNG images\0*.png\0All files\0*.*\0";
    picker.lpstrFile = path;
    picker.nMaxFile = static_cast<DWORD>(std::size(path));
    picker.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    g_dialogOpen.store(true);
    if (GetOpenFileNameW(&picker)) {
        try {
            PendingImage validation;
            if (!decodePng(path, validation)) throw E_INVALIDARG;
            const std::wstring managed = preset_store::importPngAsset(path);
            if (managed.empty()) throw E_FAIL;
            layer_model::Composition composition = layer_model::loadDraftComposition();
            layer_model::Layer layer = layer_model::makeLayer(fileNameFromPath(path), managed);
            composition.layers.push_back(layer);
            composition.rootOrder.insert(composition.rootOrder.begin(),
                                         {layer_model::StackItemType::Layer, layer.id});
            if (!layer_model::saveDraftComposition(composition)) throw E_FAIL;
            reloadDraftLayers();
            postAvatarSettingsMessage(L"preset-dirty\t1");
            sendLayerState();
        } catch (...) {
            MessageBoxW(picker.hwndOwner,
                        L"Could not add this layer. Choose a valid PNG no larger than 8192 × 8192 pixels.",
                        L"RearSilver Avatar Suite — Layers", MB_OK | MB_ICONERROR);
        }
    }
    g_dialogOpen.store(false);
}

std::wstring chooseManagedLayerEffectPng(std::wstring &displayName)
{
    wchar_t path[32768]{};
    OPENFILENAMEW picker{}; picker.lStructSize=sizeof(picker);
    picker.hwndOwner=avatarSettingsWindowHandle() ? avatarSettingsWindowHandle() : g_mainWindow;
    picker.lpstrFilter=L"PNG images\0*.png\0All files\0*.*\0"; picker.lpstrFile=path;
    picker.nMaxFile=static_cast<DWORD>(std::size(path));
    picker.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    g_dialogOpen.store(true);
    const BOOL selected=GetOpenFileNameW(&picker);
    g_dialogOpen.store(false);
    if (!selected) return {};
    try {
        PendingImage validation;
        if (!decodePng(path,validation)) throw E_INVALIDARG;
        const std::wstring managed=preset_store::importPngAsset(path);
        if (managed.empty()) throw E_FAIL;
        displayName=fileNameFromPath(path);
        return managed;
    } catch (...) {
        MessageBoxW(g_mainWindow,L"Could not use this artwork. Choose a valid PNG no larger than 8192 × 8192 pixels.",
                    L"RearSilver Avatar Suite — Artwork State Change",MB_OK|MB_ICONERROR);
        return {};
    }
}

void handleLayerCommand(const std::wstring &command, bool previewOnly)
{
    const size_t first = command.find(L'\t');
    const size_t second = first == std::wstring::npos ? std::wstring::npos : command.find(L'\t', first + 1);
    const std::wstring action = command.substr(0, first);
    const std::wstring id = first == std::wstring::npos ? L"" : command.substr(first + 1, second - first - 1);
    const std::wstring value = second == std::wstring::npos ? L"" : command.substr(second + 1);
    layer_model::Composition composition = layer_model::loadDraftComposition();
    if (action == L"builtin-add") {
        const auto assetPath=[](const wchar_t *file){return executableDirectory()+L"\\built-in-layers\\"+file;};
        const auto required = id == L"cartoon-eyes" || id == L"googly-eyes" ? 4u :
                              id == L"blush" || id == L"tears" ? 2u : 1u;
        if (composition.layers.size()+required>layer_model::kMaximumLayers) {
            MessageBoxW(g_mainWindow,L"This template would exceed the 16-layer preset limit.",
                        L"RearSilver Avatar Suite — Built-in layers",MB_OK|MB_ICONINFORMATION); return;
        }
        auto addLayer=[&](const std::wstring &name,const wchar_t *file,const std::wstring &purpose,
                          int x,int y,int scale){
            const std::wstring managed=preset_store::importPngAsset(assetPath(file));
            if(managed.empty()) throw E_FAIL;
            auto layer=layer_model::makeLayer(name,managed); layer.purpose=purpose; layer.positionX=x;
            layer.positionY=y; layer.scaleX=scale; layer.scaleY=scale; composition.layers.push_back(layer);
            return layer.id;
        };
        auto addRootLayer=[&](const std::wstring &layerId){composition.rootOrder.insert(composition.rootOrder.begin(),
            {layer_model::StackItemType::Layer,layerId});};
        try {
            if (id==L"cartoon-eyes" || id==L"googly-eyes") {
                const bool cartoon=id==L"cartoon-eyes";
                const wchar_t *housingLeft=cartoon?L"cartoon-eyes-left.png":L"googly-eye.png";
                const wchar_t *housingRight=cartoon?L"cartoon-eyes-right.png":L"googly-eye.png";
                const wchar_t *pupilLeft=cartoon?L"cartoon-eyes-pupil-left.png":L"googly-pupil.png";
                const wchar_t *pupilRight=cartoon?L"cartoon-eyes-pupil-right.png":L"googly-pupil.png";
                for(int side=0;side<2;++side){const bool left=side==0;auto group=layer_model::makeGroup(left?L"Left Eye":L"Right Eye");
                    group.positionX=left?-85:85;group.positionY=-70;group.scaleX=70;group.scaleY=70;
                    const auto housing=addLayer(left?L"Left eye housing":L"Right eye housing",left?housingLeft:housingRight,L"eyes",0,0,100);
                    const auto pupil=addLayer(left?L"Left pupil":L"Right pupil",left?pupilLeft:pupilRight,L"eyes",0,0,100);
                    auto pupilLayer=std::find_if(composition.layers.begin(),composition.layers.end(),[&](const auto &item){return item.id==pupil;});
                    auto movement=layer_model::makeLocalEffect(layer_model::LocalEffectType::BoundedMovement);
                    movement.amountX=cartoon?12:10;movement.amountY=8;movement.cycleMs=3200;movement.activeDuring=0;
                    pupilLayer->effects.push_back(movement);group.layerOrder={pupil,housing};composition.groups.push_back(group);
                    composition.rootOrder.insert(composition.rootOrder.begin(),{layer_model::StackItemType::Group,group.id});}
            } else if (id==L"blush" || id==L"tears") {
                auto group=layer_model::makeGroup(id==L"blush"?L"Blush":L"Tears");group.positionY=id==L"blush"?40:10;
                const auto left=addLayer(id==L"blush"?L"Left blush":L"Left tears",id==L"blush"?L"blush-left.png":L"tears-left.png",L"effect",-85,0,100);
                const auto right=addLayer(id==L"blush"?L"Right blush":L"Right tears",id==L"blush"?L"blush-right.png":L"tears-right.png",L"effect",85,0,100);
                group.layerOrder={left,right};composition.groups.push_back(group);composition.rootOrder.insert(composition.rootOrder.begin(),{layer_model::StackItemType::Group,group.id});
            } else if (id==L"reaction-heart") {
                const auto layerId=addLayer(L"Reaction heart",L"heart-empty.png",L"effect",210,-170,180);
                auto layer=std::find_if(composition.layers.begin(),composition.layers.end(),[&](const auto &item){return item.id==layerId;});
                auto artwork=layer_model::makeLocalEffect(layer_model::LocalEffectType::ArtworkStateChange);
                artwork.imagePath=preset_store::importPngAsset(assetPath(L"heart-full.png"));artwork.imageDisplayName=L"heart-full.png";artwork.activeDuring=1;
                layer->effects.push_back(artwork);addRootLayer(layerId);
            } else {
                const wchar_t *file=id==L"star"?L"star.png":id==L"sweat"?L"sweat-droplet.png":L"zzz.png";
                const std::wstring name=id==L"star"?L"Star":id==L"sweat"?L"Sweat drop":L"Zzz";
                const int scale=id==L"star"?500:id==L"sweat"?180:250;
                const auto layerId=addLayer(name,file,L"effect",210,-160,scale);addRootLayer(layerId);
            }
            if(!layer_model::saveDraftComposition(composition))throw E_FAIL;
            reloadDraftLayers();postAvatarSettingsMessage(L"preset-dirty\t1");sendLayerState();
        } catch (...) { MessageBoxW(g_mainWindow,L"The built-in template could not be added.",
            L"RearSilver Avatar Suite — Built-in layers",MB_OK|MB_ICONERROR); }
        return;
    }
    auto updateEffect = [&](std::vector<layer_model::LocalEffect> &effects, const std::wstring &effectAction) {
        const size_t separator = value.find(L'|');
        const std::wstring effectId = separator == std::wstring::npos ? value : value.substr(0, separator);
        const std::wstring setting = separator == std::wstring::npos ? L"" : value.substr(separator + 1);
        const auto addUnique = [&](layer_model::LocalEffectType type) {
            if (std::any_of(effects.begin(), effects.end(), [&](const auto &effect) { return effect.type == type; })) return false;
            effects.push_back(layer_model::makeLocalEffect(type)); return true;
        };
        if (effectAction == L"effect-add-sway") return addUnique(layer_model::LocalEffectType::Sway);
        if (effectAction == L"effect-add-bounded") return addUnique(layer_model::LocalEffectType::BoundedMovement);
        if (effectAction == L"effect-add-orbit") return addUnique(layer_model::LocalEffectType::Orbit);
        if (effectAction == L"effect-add-pulse") return addUnique(layer_model::LocalEffectType::LocalPulse);
        if (effectAction == L"effect-add-spin") return addUnique(layer_model::LocalEffectType::LocalSpin);
        if (effectAction == L"effect-add-nudge") return addUnique(layer_model::LocalEffectType::ReactionNudge);
        if (effectAction == L"effect-add-visibility") return addUnique(layer_model::LocalEffectType::StateVisibility);
        if (effectAction == L"effect-add-spring") return addUnique(layer_model::LocalEffectType::DangleSpring);
        if (effectAction == L"effect-add-flutter") return addUnique(layer_model::LocalEffectType::Flutter);
        if (effectAction == L"effect-add-artwork") return addUnique(layer_model::LocalEffectType::ArtworkStateChange);
        const auto effect = std::find_if(effects.begin(), effects.end(), [&](const auto &candidate) { return candidate.id == effectId; });
        if (effect == effects.end()) return false;
        if (effectAction == L"effect-remove") effects.erase(effect);
        else if (effectAction == L"effect-up") { if (effect != effects.begin()) std::iter_swap(effect, effect - 1); }
        else if (effectAction == L"effect-down") { if (effect + 1 != effects.end()) std::iter_swap(effect, effect + 1); }
        else if (effectAction == L"effect-enabled") effect->enabled = setting != L"0";
        else if (effectAction == L"effect-amount-x") effect->amountX = std::clamp(_wtoi(setting.c_str()), effect->type == layer_model::LocalEffectType::ReactionNudge ? -512 : 0, 512);
        else if (effectAction == L"effect-amount-y") effect->amountY = std::clamp(_wtoi(setting.c_str()), effect->type == layer_model::LocalEffectType::ReactionNudge ? -512 : 0, 512);
        else if (effectAction == L"effect-cycle") effect->cycleMs = std::clamp(_wtoi(setting.c_str()), 200, 20000);
        else if (effectAction == L"effect-boost") effect->reactionBoost = std::clamp(_wtoi(setting.c_str()), 0, 300);
        else if (effectAction == L"effect-pivot") effect->pivot = std::clamp(_wtoi(setting.c_str()), 0, 9);
        else if (effectAction == L"effect-active") effect->activeDuring = std::clamp(_wtoi(setting.c_str()), 0, 2);
        else if (effectAction == L"effect-artwork-choose") {
            std::wstring displayName; const std::wstring path=chooseManagedLayerEffectPng(displayName);
            if (path.empty()) return false; effect->imagePath=path; effect->imageDisplayName=displayName;
        }
        else if (effectAction == L"effect-artwork-remove") { effect->imagePath.clear(); effect->imageDisplayName.clear(); }
        else if (effectAction == L"effect-artwork-scale-x") { effect->artworkScaleX=std::clamp(_wtoi(setting.c_str()),1,2000); if(effect->artworkScaleLinked)effect->artworkScaleY=effect->artworkScaleX; }
        else if (effectAction == L"effect-artwork-scale-y") { effect->artworkScaleY=std::clamp(_wtoi(setting.c_str()),1,2000); if(effect->artworkScaleLinked)effect->artworkScaleX=effect->artworkScaleY; }
        else if (effectAction == L"effect-artwork-scale-link") effect->artworkScaleLinked=setting!=L"0";
        else return false;
        return true;
    };
    if (action == L"group-add") {
        layer_model::Group group = layer_model::makeGroup(L"New group");
        composition.groups.push_back(group);
        composition.rootOrder.insert(composition.rootOrder.begin(),
                                     {layer_model::StackItemType::Group, group.id});
        if (layer_model::saveDraftComposition(composition)) {
            { std::lock_guard<std::mutex> lock(g_layerStateMutex); g_composition = composition; }
            postAvatarSettingsMessage(L"preset-dirty\t1");
            const bool structuralEffectChange = action.rfind(L"group-effect-add-", 0) == 0 || action == L"group-effect-remove" ||
                action == L"group-effect-up" || action == L"group-effect-down";
            if (action.rfind(L"group-effect-", 0) != 0 || structuralEffectChange) sendLayerState();
        }
        return;
    }
    if (action.rfind(L"group-", 0) == 0) {
        const auto group = std::find_if(composition.groups.begin(), composition.groups.end(),
            [&](const layer_model::Group &candidate) { return candidate.id == id; });
        if (group == composition.groups.end()) return;
        const auto rootItem = std::find_if(composition.rootOrder.begin(), composition.rootOrder.end(),
            [&](const layer_model::StackItem &item) {
                return item.type == layer_model::StackItemType::Group && item.id == id;
            });
        if (action == L"group-remove") {
            if (rootItem == composition.rootOrder.end()) return;
            const size_t insertAt = static_cast<size_t>(rootItem - composition.rootOrder.begin());
            composition.rootOrder.erase(rootItem);
            size_t offset = 0;
            for (const std::wstring &layerId : group->layerOrder)
                composition.rootOrder.insert(composition.rootOrder.begin() + insertAt + offset++,
                                             {layer_model::StackItemType::Layer, layerId});
            composition.groups.erase(group);
        } else if (action == L"group-up" && rootItem != composition.rootOrder.end() &&
                   rootItem != composition.rootOrder.begin()) std::iter_swap(rootItem, rootItem - 1);
        else if (action == L"group-down" && rootItem != composition.rootOrder.end() &&
                 rootItem + 1 != composition.rootOrder.end()) std::iter_swap(rootItem, rootItem + 1);
        else if (action == L"group-name") group->name = value.empty() ? group->name : value.substr(0, 80);
        else if (action == L"group-visible") group->visible = !group->visible;
        else if (action == L"group-x") group->positionX = std::clamp(_wtoi(value.c_str()), -4096, 4096);
        else if (action == L"group-y") group->positionY = std::clamp(_wtoi(value.c_str()), -4096, 4096);
        else if (action == L"group-scale-x") { group->scaleX = std::clamp(_wtoi(value.c_str()), 1, 1000); if (group->scaleLinked) group->scaleY = group->scaleX; }
        else if (action == L"group-scale-y") { group->scaleY = std::clamp(_wtoi(value.c_str()), 1, 1000); if (group->scaleLinked) group->scaleX = group->scaleY; }
        else if (action == L"group-scale-link") group->scaleLinked = value != L"0";
        else if (action == L"group-rotation") group->rotationTenths = std::clamp(_wtoi(value.c_str()), -3600, 3600);
        else if (action == L"group-opacity") group->opacity = std::clamp(_wtoi(value.c_str()), 0, 100);
        else if (action == L"group-flip") group->flipHorizontal = value != L"0";
        else if (action == L"group-inherit") group->inheritAvatarEffects = value != L"0";
        else if (action.rfind(L"group-effect-", 0) == 0) {
            if (action == L"group-effect-add-artwork") return;
            if (!updateEffect(group->effects, action.substr(6))) return;
        }
        else return;
        if (previewOnly) {
            std::lock_guard<std::mutex> lock(g_layerStateMutex);
            g_composition = composition;
            return;
        }
        if (layer_model::saveDraftComposition(composition)) {
            { std::lock_guard<std::mutex> lock(g_layerStateMutex); g_composition = composition; }
            postAvatarSettingsMessage(L"preset-dirty\t1");
            sendLayerState();
        }
        return;
    }
    auto &layers = composition.layers;
    const auto found = std::find_if(layers.begin(), layers.end(), [&](const auto &layer) { return layer.id == id; });
    if (found == layers.end()) return;
    const auto stackItem = std::find_if(composition.rootOrder.begin(), composition.rootOrder.end(),
        [&](const layer_model::StackItem &item) {
            return item.type == layer_model::StackItemType::Layer && item.id == id;
        });
    const auto parentGroup = std::find_if(composition.groups.begin(), composition.groups.end(),
        [&](const layer_model::Group &group) {
            return std::find(group.layerOrder.begin(), group.layerOrder.end(), id) != group.layerOrder.end();
        });
    const bool reloadsTextures = action == L"remove" || action == L"effect-artwork-choose" ||
        action == L"effect-artwork-remove";
    if (action == L"remove") {
        layers.erase(found);
        if (parentGroup != composition.groups.end()) {
            const auto child = std::find(parentGroup->layerOrder.begin(), parentGroup->layerOrder.end(), id);
            if (child != parentGroup->layerOrder.end()) parentGroup->layerOrder.erase(child);
        } else if (stackItem != composition.rootOrder.end()) composition.rootOrder.erase(stackItem);
    }
    else if (action == L"visible") found->visible = !found->visible;
    else if (action == L"up") {
        if (parentGroup != composition.groups.end()) {
            const auto child = std::find(parentGroup->layerOrder.begin(), parentGroup->layerOrder.end(), id);
            if (child != parentGroup->layerOrder.begin()) std::iter_swap(child, child - 1);
        } else if (stackItem != composition.rootOrder.end() && stackItem != composition.rootOrder.begin())
            std::iter_swap(stackItem, stackItem - 1);
    }
    else if (action == L"down") {
        if (parentGroup != composition.groups.end()) {
            const auto child = std::find(parentGroup->layerOrder.begin(), parentGroup->layerOrder.end(), id);
            if (child + 1 != parentGroup->layerOrder.end()) std::iter_swap(child, child + 1);
        } else if (stackItem != composition.rootOrder.end() && stackItem + 1 != composition.rootOrder.end())
            std::iter_swap(stackItem, stackItem + 1);
    }
    else if (action == L"parent") {
        if (parentGroup != composition.groups.end()) parentGroup->layerOrder.erase(
            std::find(parentGroup->layerOrder.begin(), parentGroup->layerOrder.end(), id));
        else if (stackItem != composition.rootOrder.end()) composition.rootOrder.erase(stackItem);
        if (value.empty()) composition.rootOrder.insert(composition.rootOrder.begin(),
            {layer_model::StackItemType::Layer, id});
        else {
            const auto target = std::find_if(composition.groups.begin(), composition.groups.end(),
                [&](const layer_model::Group &group) { return group.id == value; });
            if (target == composition.groups.end()) return;
            target->layerOrder.insert(target->layerOrder.begin(), id);
        }
    }
    else if (action == L"name") found->name = value.empty() ? found->name : value.substr(0, 80);
    else if (action == L"purpose") found->purpose = value;
    else if (action == L"inherit") found->inheritAvatarEffects = value != L"0";
    else if (action == L"x") found->positionX = std::clamp(_wtoi(value.c_str()), -4096, 4096);
    else if (action == L"y") found->positionY = std::clamp(_wtoi(value.c_str()), -4096, 4096);
    else if (action == L"pivot-x") found->pivotX = std::clamp(_wtoi(value.c_str()), 0, 100);
    else if (action == L"pivot-y") found->pivotY = std::clamp(_wtoi(value.c_str()), 0, 100);
    else if (action == L"scale-x") { found->scaleX = std::clamp(_wtoi(value.c_str()), 1, 1000); if (found->scaleLinked) found->scaleY = found->scaleX; }
    else if (action == L"scale-y") { found->scaleY = std::clamp(_wtoi(value.c_str()), 1, 1000); if (found->scaleLinked) found->scaleX = found->scaleY; }
    else if (action == L"scale-link") found->scaleLinked = value != L"0";
    else if (action == L"flip") found->flipHorizontal = value != L"0";
    else if (action == L"rotation") found->rotationTenths = std::clamp(_wtoi(value.c_str()), -3600, 3600);
    else if (action == L"opacity") found->opacity = std::clamp(_wtoi(value.c_str()), 0, 100);
    else if (action == L"tail-add") found->tailWagAdded = true;
    else if (action == L"tail-remove") found->tailWagAdded = false;
    else if (action == L"tail-enabled") found->tailWagEnabled = value != L"0";
    else if (action == L"tail-angle") found->tailWagAngle = std::clamp(_wtoi(value.c_str()), 0, 90);
    else if (action == L"tail-cycle") found->tailWagCycleMs = std::clamp(_wtoi(value.c_str()), 200, 10000);
    else if (action == L"tail-boost") found->tailWagReactionBoost = std::clamp(_wtoi(value.c_str()), 0, 300);
    else if (action == L"tail-pivot") found->tailWagPivot = std::clamp(_wtoi(value.c_str()), 0, 9);
    else if (action == L"sway-add") found->swayAdded = true;
    else if (action == L"sway-remove") found->swayAdded = false;
    else if (action == L"sway-enabled") found->swayEnabled = value != L"0";
    else if (action == L"sway-angle") found->swayAngle = std::clamp(_wtoi(value.c_str()), 0, 45);
    else if (action == L"sway-cycle") found->swayCycleMs = std::clamp(_wtoi(value.c_str()), 400, 20000);
    else if (action == L"sway-boost") found->swayReactionBoost = std::clamp(_wtoi(value.c_str()), 0, 300);
    else if (action == L"sway-pivot") found->swayPivot = std::clamp(_wtoi(value.c_str()), 0, 9);
    else if (action == L"eye-add") found->eyeMovementAdded = true;
    else if (action == L"eye-remove") found->eyeMovementAdded = false;
    else if (action == L"eye-enabled") found->eyeMovementEnabled = value != L"0";
    else if (action == L"eye-range-x") found->eyeRangeX = std::clamp(_wtoi(value.c_str()), 0, 512);
    else if (action == L"eye-range-y") found->eyeRangeY = std::clamp(_wtoi(value.c_str()), 0, 512);
    else if (action == L"eye-cycle") found->eyeCycleMs = std::clamp(_wtoi(value.c_str()), 500, 20000);
    else if (action == L"eye-active") found->eyeActiveDuring = std::clamp(_wtoi(value.c_str()), 0, 2);
    else if (action.rfind(L"effect-", 0) == 0) { if (!updateEffect(found->effects, action)) return; }
    else return;
    if (previewOnly) {
        std::lock_guard<std::mutex> lock(g_layerStateMutex);
        g_composition = composition;
        return;
    }
    if (layer_model::saveDraftComposition(composition)) {
        if (reloadsTextures) reloadDraftLayers();
        else { std::lock_guard<std::mutex> lock(g_layerStateMutex); g_composition = composition; }
        postAvatarSettingsMessage(L"preset-dirty\t1");
        if (action == L"remove" || action == L"up" || action == L"down" ||
            action == L"visible" || action == L"purpose" || action == L"parent" ||
            action == L"tail-add" || action == L"tail-remove" ||
            action == L"sway-add" || action == L"sway-remove" ||
            action == L"eye-add" || action == L"eye-remove" ||
            action.rfind(L"effect-add-", 0) == 0 ||
            action == L"effect-remove" || action == L"effect-up" || action == L"effect-down" ||
            action == L"effect-artwork-choose" || action == L"effect-artwork-remove")
            sendLayerState();
    }
}

bool isProcessRunning(const wchar_t *name)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

struct Vertex {
    float x;
    float y;
    float u;
    float v;
};

struct TextureAsset {
    ComPtr<ID3D11ShaderResourceView> view;
    UINT width = 1;
    UINT height = 1;
};

class Renderer {
public:
    ~Renderer()
    {
        if (spoutDeviceOpen_) {
            spoutSender_.ReleaseSender();
            spoutSender_.CloseDirectX11();
        }
    }

    void initialize(HWND window)
    {
        window_ = window;
        width_ = kOutputWidth;
        height_ = kOutputHeight;

        ComPtr<IDXGIAdapter1> selectedAdapter = chooseInteroperableAdapter();
        const D3D_FEATURE_LEVEL requestedLevels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL createdLevel{};
        check(D3D11CreateDevice(
            selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, requestedLevels,
            static_cast<UINT>(std::size(requestedLevels)), D3D11_SDK_VERSION,
            device_.GetAddressOf(), &createdLevel, context_.GetAddressOf()));

        ComPtr<IDXGIFactory2> factory;
        check(selectedAdapter->GetParent(IID_PPV_ARGS(&factory)));
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = width_;
        description.Height = height_;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        check(factory->CreateSwapChainForComposition(device_.Get(), &description, nullptr,
                                                      swapChain_.GetAddressOf()));
        createRenderTarget();

        ComPtr<IDXGIDevice> dxgiDevice;
        check(device_.As(&dxgiDevice));
        check(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&compositionDevice_)));
        check(compositionDevice_->CreateTargetForHwnd(window_, TRUE,
                                                       compositionTarget_.GetAddressOf()));
        check(compositionDevice_->CreateVisual(compositionVisual_.GetAddressOf()));
        check(compositionDevice_->CreateMatrixTransform(compositionTransform_.GetAddressOf()));
        check(compositionVisual_->SetContent(swapChain_.Get()));
        check(compositionVisual_->SetTransform(compositionTransform_.Get()));
        check(compositionTarget_->SetRoot(compositionVisual_.Get()));
        updateLocalTransform(g_clientWidth.load(), g_clientHeight.load());

        createPipeline();
        createAssets();

        ComPtr<IDXGIDevice> activeDxgiDevice;
        ComPtr<IDXGIAdapter> activeAdapter;
        DXGI_ADAPTER_DESC activeDescription{};
        if (SUCCEEDED(device_.As(&activeDxgiDevice)) &&
            SUCCEEDED(activeDxgiDevice->GetAdapter(activeAdapter.GetAddressOf())) &&
            SUCCEEDED(activeAdapter->GetDesc(&activeDescription))) {
            logMessage(L"Active GPU: " + std::wstring(activeDescription.Description) + L"; LUID=" +
                       std::to_wstring(activeDescription.AdapterLuid.HighPart) + L":" +
                       std::to_wstring(activeDescription.AdapterLuid.LowPart));
        }
        logMessage(L"Fixed 1920 x 1080 composition swap chain attached through DirectComposition");
    }

    void updateLocalTransform(UINT clientWidth, UINT clientHeight)
    {
        if (clientWidth == 0 || clientHeight == 0)
            return;
        const float scale = std::min(static_cast<float>(clientWidth) / kOutputWidth,
                                     static_cast<float>(clientHeight) / kOutputHeight);
        const float offsetX = (static_cast<float>(clientWidth) - kOutputWidth * scale) * 0.5f;
        const float offsetY = (static_cast<float>(clientHeight) - kOutputHeight * scale) * 0.5f;
        const D2D_MATRIX_3X2_F matrix{scale, 0.0f, 0.0f, scale, offsetX, offsetY};
        check(compositionTransform_->SetMatrix(matrix));
        check(compositionDevice_->Commit());
    }

    void uploadPendingImage()
    {
        PendingImage pending;
        {
            std::lock_guard<std::mutex> lock(g_pendingImageMutex);
            if (g_pendingImages.empty())
                return;
            pending = std::move(g_pendingImages.front());
            g_pendingImages.pop_front();
        }

        try {
            if (pending.clearSlot) {
                if (pending.slot == ImageSlot::Layer) {
                    layerImages_.clear();
                } else if (pending.slot == ImageSlot::Background) {
                    backgroundImage_ = {};
                    backgroundImageLoaded_ = false;
                } else if (pending.slot == ImageSlot::PrimaryBlink) {
                    primaryBlinkAvatar_ = {};
                    primaryBlinkAvatarLoaded_ = false;
                } else if (pending.slot == ImageSlot::ReactionBlink) {
                    reactionBlinkAvatar_ = {};
                    reactionBlinkAvatarLoaded_ = false;
                }
                return;
            }
            TextureAsset replacement = createTexture(pending.rgba.data(), pending.width, pending.height);
            if (pending.slot == ImageSlot::Layer) {
                const auto found = std::find_if(layerImages_.begin(), layerImages_.end(),
                    [&](const LayerTexture &item) { return item.id == pending.layerId; });
                if (found == layerImages_.end())
                    layerImages_.push_back({pending.layerId, std::move(replacement)});
                else
                    found->texture = std::move(replacement);
            } else if (pending.slot == ImageSlot::Background) {
                backgroundImage_ = std::move(replacement);
                backgroundImageLoaded_ = true;
                {
                    std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
                    g_backgroundImagePath = pending.path;
                    g_backgroundImageLoaded = true;
                }
                if (pending.persistSelection && pending.presetGeneration == g_presetGeneration.load()) {
                    saveSetting(L"BackgroundImage", pending.path);
                    saveSetting(L"BackgroundImageDisplayName", pending.displayName);
                }
            } else if (pending.slot == ImageSlot::Reaction) {
                reactionAvatar_ = std::move(replacement);
                reactionAvatarLoaded_ = true;
                g_reactionAvailable.store(true);
                {
                    std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
                    g_reactionImageLoaded = true;
                }
                if (pending.persistSelection && pending.presetGeneration == g_presetGeneration.load()) {
                    std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
                    g_reactionImagePath = pending.path;
                    saveSetting(L"ReactionImage", pending.path);
                    saveSetting(L"ReactionImageDisplayName", pending.displayName);
                }
            } else if (pending.slot == ImageSlot::PrimaryBlink) {
                primaryBlinkAvatar_ = std::move(replacement);
                primaryBlinkAvatarLoaded_ = true;
                {
                    std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
                    g_primaryBlinkImagePath = pending.path;
                    g_primaryBlinkImageLoaded = true;
                }
                if (pending.persistSelection && pending.presetGeneration == g_presetGeneration.load()) {
                    saveSetting(L"PrimaryBlinkImage", pending.path);
                    saveSetting(L"PrimaryBlinkImageDisplayName", pending.displayName);
                }
            } else if (pending.slot == ImageSlot::ReactionBlink) {
                reactionBlinkAvatar_ = std::move(replacement);
                reactionBlinkAvatarLoaded_ = true;
                {
                    std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
                    g_reactionBlinkImagePath = pending.path;
                    g_reactionBlinkImageLoaded = true;
                }
                if (pending.persistSelection && pending.presetGeneration == g_presetGeneration.load()) {
                    saveSetting(L"ReactionBlinkImage", pending.path);
                    saveSetting(L"ReactionBlinkImageDisplayName", pending.displayName);
                }
            } else {
                primaryAvatar_ = std::move(replacement);
                {
                    std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
                    g_primaryImageLoaded = true;
                }
                if (pending.persistSelection && pending.presetGeneration == g_presetGeneration.load()) {
                    std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
                    g_primaryImagePath = pending.path;
                    saveSetting(L"PrimaryImage", pending.path);
                    saveSetting(L"PrimaryImageDisplayName", pending.displayName);
                }
            }
            logMessage(L"Image atomically replaced after GPU upload: " + pending.path);
            if (pending.persistSelection && pending.presetGeneration == g_presetGeneration.load())
                PostMessageW(window_, kImageUploadSuccessMessage, static_cast<WPARAM>(pending.slot), 0);
        } catch (...) {
            PostMessageW(window_, kImageUploadFailureMessage, static_cast<WPARAM>(pending.slot), 0);
        }
    }

    unsigned nextBlinkDelay()
    {
        blinkRandomState_ = blinkRandomState_ * 1664525u + 1013904223u;
        const unsigned minimum = g_blinkMinimumMs.load();
        const unsigned maximum = std::max(minimum, g_blinkMaximumMs.load());
        return minimum + (maximum > minimum ? blinkRandomState_ % (maximum - minimum + 1) : 0);
    }

    void updateBlinkState(ULONGLONG now)
    {
        if (!g_blinkEnabled.load()) {
            if (blinking_) {
                blinking_ = false;
                PostMessageW(window_, kBlinkStateMessage, FALSE, 0);
            }
            nextBlinkAt_ = 0;
            return;
        }
        if (nextBlinkAt_ == 0)
            nextBlinkAt_ = now + nextBlinkDelay();
        if (!blinking_ && now >= nextBlinkAt_) {
            blinking_ = true;
            blinkEndsAt_ = now + g_blinkDurationMs.load();
            PostMessageW(window_, kBlinkStateMessage, TRUE, 0);
        } else if (blinking_ && now >= blinkEndsAt_) {
            blinking_ = false;
            nextBlinkAt_ = now + nextBlinkDelay();
            PostMessageW(window_, kBlinkStateMessage, FALSE, 0);
        }
    }

    void render()
    {
        uploadPendingImage();
        const ULONGLONG now = GetTickCount64();
        updateBlinkState(now);

        float clear[4] = {0, 0, 0, 0};
        const unsigned background = g_captureMethod.load() == 1 ? 0 : g_fixedBackgroundMode.load();
        if (background != 0) {
            const unsigned colour = background == 2 ? g_backgroundChromaColour.load()
                                                     : g_backgroundSolidColour.load();
            clear[0] = static_cast<float>((colour >> 16) & 0xff) / 255.0f;
            clear[1] = static_cast<float>((colour >> 8) & 0xff) / 255.0f;
            clear[2] = static_cast<float>(colour & 0xff) / 255.0f;
            clear[3] = 1.0f;
        }
        context_->ClearRenderTargetView(target_.Get(), clear);
        ID3D11RenderTargetView *target = target_.Get();
        context_->OMSetRenderTargets(1, &target, nullptr);
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
        context_->RSSetViewports(1, &viewport);
        bindPipeline();

        if (background == 3 && backgroundImageLoaded_) {
            setPixelBrightness(1.0f);
            const float imageWidth = static_cast<float>(backgroundImage_.width);
            const float imageHeight = static_cast<float>(backgroundImage_.height);
            const unsigned fit = g_backgroundFit.load();
            if (fit == 2) {
                draw(backgroundImage_, 0, 0, static_cast<float>(width_), static_cast<float>(height_));
            } else if (fit == 3) {
                float tileWidth = imageWidth;
                float tileHeight = imageHeight;
                const float columns = std::ceil(static_cast<float>(width_) / tileWidth);
                const float rows = std::ceil(static_cast<float>(height_) / tileHeight);
                if (columns * rows > 1024.0f) {
                    const float expansion = std::sqrt(columns * rows / 1024.0f);
                    tileWidth *= expansion;
                    tileHeight *= expansion;
                }
                for (float y = 0; y < height_; y += tileHeight)
                    for (float x = 0; x < width_; x += tileWidth)
                        draw(backgroundImage_, x, y, tileWidth, tileHeight);
            } else {
                const float scaleX = static_cast<float>(width_) / imageWidth;
                const float scaleY = static_cast<float>(height_) / imageHeight;
                const float scale = fit == 1 ? std::max(scaleX, scaleY) : std::min(scaleX, scaleY);
                const float drawWidth = imageWidth * scale;
                const float drawHeight = imageHeight * scale;
                draw(backgroundImage_, (static_cast<float>(width_) - drawWidth) * 0.5f,
                     (static_cast<float>(height_) - drawHeight) * 0.5f, drawWidth, drawHeight);
            }
        }

        const float maxWidth = static_cast<float>(width_) * 0.68f;
        const float maxHeight = static_cast<float>(height_) * 0.68f;
        const bool reactionActive = g_previewReaction.load() || g_microphoneReaction.load();
        const bool reactionState = reactionActive && reactionAvatarLoaded_;
        const TextureAsset *activeAvatar = reactionState ? &reactionAvatar_ : &primaryAvatar_;
        if (blinking_) {
            if (reactionState && reactionBlinkAvatarLoaded_)
                activeAvatar = &reactionBlinkAvatar_;
            else if (!reactionState && primaryBlinkAvatarLoaded_)
                activeAvatar = &primaryBlinkAvatar_;
        }
        const float scale = std::min(maxWidth / activeAvatar->width, maxHeight / activeAvatar->height) *
                            static_cast<float>(g_avatarScalePercent.load()) / 100.0f;
        float avatarWidth = activeAvatar->width * scale;
        float avatarHeight = activeAvatar->height * scale;
        const float baseAvatarWidth = avatarWidth;
        const float baseAvatarHeight = avatarHeight;
        const float primaryScale = std::min(maxWidth / primaryAvatar_.width,
                                            maxHeight / primaryAvatar_.height) *
                                   static_cast<float>(g_avatarScalePercent.load()) / 100.0f;
        const float baseBottom = (static_cast<float>(height_) + avatarHeight) * 0.5f;
        float targetBreathAmount = 0.0f;
        if (g_breathingAdded.load() && g_breathingEnabled.load()) {
            const unsigned mode = g_breathingMode.load();
            const bool applies = mode == 1 || (mode == 0 && !reactionState) ||
                                 (mode == 2 && reactionState);
            if (applies)
                targetBreathAmount = static_cast<float>(reactionState
                                         ? g_breathingReactionAmount.load()
                                         : g_breathingIdleAmount.load()) / 1000.0f;
        }
        if (lastBreathUpdateAt_ == 0) {
            currentBreathAmount_ = targetBreathAmount;
        } else {
            const float elapsed = static_cast<float>(std::min<ULONGLONG>(now - lastBreathUpdateAt_, 250));
            const float blend = std::min(1.0f, elapsed / 180.0f);
            currentBreathAmount_ += (targetBreathAmount - currentBreathAmount_) * blend;
            if (std::abs(targetBreathAmount - currentBreathAmount_) < 0.00001f)
                currentBreathAmount_ = targetBreathAmount;
        }
        lastBreathUpdateAt_ = now;
        if (currentBreathAmount_ > 0.00001f) {
            const unsigned cycle = std::max(500u, g_breathingCycleMs.load());
            const float wave = std::sin(static_cast<float>(now % cycle) /
                                        static_cast<float>(cycle) * 6.28318530718f);
            avatarWidth *= 1.0f - wave * currentBreathAmount_ * 0.25f;
            avatarHeight *= 1.0f + wave * currentBreathAmount_;
        }
        const ULONGLONG squashStartedAt = g_squashStartedAt.load();
        const unsigned squashDuration = g_squashDurationMs.load();
        if (g_squashAdded.load() && g_squashEnabled.load() && squashStartedAt > 0 &&
            now >= squashStartedAt && now - squashStartedAt < squashDuration) {
            const float progress = static_cast<float>(now - squashStartedAt) /
                                   static_cast<float>(squashDuration);
            const float amount = static_cast<float>(g_squashIntensity.load()) / 100.0f;
            auto smoothStep = [](float value) {
                value = std::clamp(value, 0.0f, 1.0f);
                return value * value * (3.0f - 2.0f * value);
            };
            float scaleX = 1.0f;
            float scaleY = 1.0f;
            if (progress < 0.22f) {
                const float phase = smoothStep(progress / 0.22f);
                scaleX = 1.0f + 0.16f * amount * phase;
                scaleY = 1.0f - 0.12f * amount * phase;
            } else if (progress < 0.48f) {
                const float phase = smoothStep((progress - 0.22f) / 0.26f);
                scaleX = 1.0f + (0.16f - 0.26f * phase) * amount;
                scaleY = 1.0f + (-0.12f + 0.34f * phase) * amount;
            } else {
                const float phase = smoothStep((progress - 0.48f) / 0.52f);
                scaleX = 1.0f - 0.10f * amount * (1.0f - phase);
                scaleY = 1.0f + 0.22f * amount * (1.0f - phase);
            }
            avatarWidth *= std::max(0.55f, scaleX);
            avatarHeight *= std::max(0.55f, scaleY);
        }
        const unsigned floatMode = g_floatMode.load();
        const bool floatApplies = g_floatAdded.load() && g_floatEnabled.load() &&
                                  (now < g_floatPreviewUntil.load() || floatMode == 1 ||
                                   (floatMode == 0 && !reactionActive) ||
                                   (floatMode == 2 && reactionActive));
        const float targetFloatHeight = floatApplies
                                            ? static_cast<float>(g_floatHeightPixels.load())
                                            : 0.0f;
        const float targetFloatDrift = floatApplies && g_floatDirection.load() != 0
                                           ? static_cast<float>(g_floatDriftPixels.load())
                                           : 0.0f;
        const unsigned floatCycle = std::max(1000u, g_floatCycleMs.load());
        const float floatPhase = static_cast<float>(now % floatCycle) /
                                 static_cast<float>(floatCycle) * 6.28318530718f;
        auto calculateFloatPosition = [floatPhase](float height, float drift, unsigned direction) {
            const float y = -(std::sin(floatPhase) + 1.0f) * 0.5f * height;
            const float directionSign = direction == 1 ? -1.0f : 1.0f;
            const float x = direction == 0
                                ? 0.0f
                                : (1.0f - std::cos(floatPhase)) * 0.5f * drift * directionSign;
            return std::pair<float, float>{x, y};
        };
        const float correctionProgress = floatCorrectionStartedAt_ == 0
                                             ? 1.0f
                                             : std::clamp(static_cast<float>(now - floatCorrectionStartedAt_) /
                                                              500.0f,
                                                          0.0f, 1.0f);
        const float correctionEase = correctionProgress * correctionProgress *
                                     (3.0f - 2.0f * correctionProgress);
        currentFloatCorrectionX_ = floatCorrectionStartX_ * (1.0f - correctionEase);
        currentFloatCorrectionY_ = floatCorrectionStartY_ * (1.0f - correctionEase);
        const unsigned targetFloatDirection = targetFloatDrift > 0.0f ? g_floatDirection.load() : 0;
        if (std::abs(targetFloatHeight - currentFloatHeight_) > 0.001f ||
            std::abs(targetFloatDrift - currentFloatDrift_) > 0.001f ||
            targetFloatDirection != currentFloatDirection_) {
            const auto oldPosition = calculateFloatPosition(currentFloatHeight_, currentFloatDrift_,
                                                            currentFloatDirection_);
            const float displayedX = oldPosition.first + currentFloatCorrectionX_;
            const float displayedY = oldPosition.second + currentFloatCorrectionY_;
            currentFloatHeight_ = targetFloatHeight;
            currentFloatDrift_ = targetFloatDrift;
            currentFloatDirection_ = targetFloatDirection;
            const auto newPosition = calculateFloatPosition(currentFloatHeight_, currentFloatDrift_,
                                                            currentFloatDirection_);
            floatCorrectionStartX_ = displayedX - newPosition.first;
            floatCorrectionStartY_ = displayedY - newPosition.second;
            currentFloatCorrectionX_ = floatCorrectionStartX_;
            currentFloatCorrectionY_ = floatCorrectionStartY_;
            floatCorrectionStartedAt_ = now;
        }
        const auto floatPosition = calculateFloatPosition(currentFloatHeight_, currentFloatDrift_,
                                                          currentFloatDirection_);
        const float floatX = floatPosition.first + currentFloatCorrectionX_;
        const float floatY = floatPosition.second + currentFloatCorrectionY_;
        float bounce = 0.0f;
        const ULONGLONG bounceStartedAt = g_bounceStartedAt.load();
        const unsigned bounceDuration = g_bounceDurationMs.load();
        if (g_bounceEnabled.load() && bounceStartedAt > 0 && now >= bounceStartedAt &&
            now - bounceStartedAt < bounceDuration) {
            const float progress = static_cast<float>(now - bounceStartedAt) /
                                   static_cast<float>(bounceDuration);
            bounce = -std::sin(progress * 3.14159265359f) *
                     static_cast<float>(g_bounceHeightPixels.load());
        }
        const bool shakeTarget = g_shakeAdded.load() && g_shakeEnabled.load() &&
                                 (reactionActive || now < g_shakePreviewUntil.load());
        if (lastShakeUpdateAt_ == 0) {
            currentShakeMix_ = shakeTarget ? 1.0f : 0.0f;
        } else {
            const float elapsed = static_cast<float>(std::min<ULONGLONG>(now - lastShakeUpdateAt_, 250));
            const float duration = shakeTarget ? 60.0f : 160.0f;
            const float blend = std::min(1.0f, elapsed / duration);
            currentShakeMix_ += ((shakeTarget ? 1.0f : 0.0f) - currentShakeMix_) * blend;
            if (currentShakeMix_ < 0.0001f)
                currentShakeMix_ = 0.0f;
        }
        lastShakeUpdateAt_ = now;
        float shakeX = 0.0f;
        float shakeY = 0.0f;
        float shakeRotation = 0.0f;
        if (currentShakeMix_ > 0.0f) {
            const float intensity = static_cast<float>(g_shakeIntensity.load()) / 100.0f;
            const float phase = static_cast<float>(now) *
                                (static_cast<float>(g_shakeSpeed.load()) / 10.0f) *
                                0.00628318530718f;
            const float amplitude = 12.0f * intensity * currentShakeMix_;
            const unsigned direction = g_shakeDirection.load();
            if (direction != 2)
                shakeX = (std::sin(phase) * 0.68f + std::sin(phase * 2.13f + 1.4f) * 0.32f) * amplitude;
            if (direction != 1)
                shakeY = (std::sin(phase * 1.37f + 2.1f) * 0.72f + std::sin(phase * 2.71f) * 0.28f) * amplitude;
            if (g_shakeWobble.load())
                shakeRotation = std::sin(phase * 1.17f + 0.8f) * intensity *
                                currentShakeMix_ * 0.0261799388f;
        }
        const bool tiltActive = g_tiltAdded.load() && g_tiltEnabled.load() &&
                                (reactionActive || now < g_tiltPreviewUntil.load());
        const float targetTilt = tiltActive
                                     ? static_cast<float>(g_tiltActiveSign.load()) *
                                           static_cast<float>(g_tiltAngleDegrees.load()) *
                                           0.0174532925199f
                                     : 0.0f;
        if (std::abs(targetTilt - tiltTarget_) > 0.00001f) {
            tiltStart_ = currentTilt_;
            tiltTarget_ = targetTilt;
            tiltTransitionStartedAt_ = now;
        }
        const unsigned tiltDuration = g_tiltTransitionMs.load();
        const float tiltProgress = tiltDuration == 0
                                       ? 1.0f
                                       : std::clamp(static_cast<float>(now - tiltTransitionStartedAt_) /
                                                        static_cast<float>(tiltDuration),
                                                    0.0f, 1.0f);
        const float tiltEase = tiltProgress * tiltProgress * (3.0f - 2.0f * tiltProgress);
        currentTilt_ = tiltStart_ + (tiltTarget_ - tiltStart_) * tiltEase;
        const bool brightnessReaction = reactionActive || now < g_brightnessPreviewUntil.load();
        const float targetBrightness = g_brightnessAdded.load() && g_brightnessEnabled.load()
                                           ? static_cast<float>(brightnessReaction
                                                 ? g_brightnessReaction.load()
                                                 : g_brightnessIdle.load()) / 100.0f
                                           : 1.0f;
        if (std::abs(targetBrightness - brightnessTarget_) > 0.0001f) {
            brightnessStart_ = currentBrightness_;
            brightnessTarget_ = targetBrightness;
            brightnessTransitionStartedAt_ = now;
        }
        const unsigned brightnessDuration = g_brightnessTransitionMs.load();
        const float brightnessProgress = brightnessDuration == 0
                                             ? 1.0f
                                             : std::clamp(static_cast<float>(now - brightnessTransitionStartedAt_) /
                                                              static_cast<float>(brightnessDuration),
                                                          0.0f, 1.0f);
        const float brightnessEase = brightnessProgress * brightnessProgress *
                                     (3.0f - 2.0f * brightnessProgress);
        currentBrightness_ = brightnessStart_ +
                             (brightnessTarget_ - brightnessStart_) * brightnessEase;
        setPixelBrightness(currentBrightness_);
        layer_model::Composition composition;
        {
            std::lock_guard<std::mutex> lock(g_layerStateMutex);
            composition = g_composition;
        }
        auto drawLayer = [&](const layer_model::Layer &layer, const layer_model::Group *group = nullptr) {
                if (!layer.visible) return;
                if (group && !group->visible) return;
                const auto found = std::find_if(layerImages_.begin(), layerImages_.end(),
                    [&](const LayerTexture &item) { return item.id == layer.id; });
                if (found == layerImages_.end()) return;
                const LayerTexture *rendered=&*found;
                float artworkScaleX=1.0f, artworkScaleY=1.0f;
                for (const auto &effect : layer.effects) {
                    if (effect.type != layer_model::LocalEffectType::ArtworkStateChange ||
                        !effect.enabled || effect.imagePath.empty()) continue;
                    const bool useAlternate=effect.activeDuring == 1 ? reactionActive : !reactionActive;
                    if (!useAlternate) continue;
                    const std::wstring alternateId=layer.id+L"::effect::"+effect.id;
                    const auto alternate=std::find_if(layerImages_.begin(),layerImages_.end(),
                        [&](const LayerTexture &item){return item.id==alternateId;});
                    if (alternate!=layerImages_.end()) {
                        rendered=&*alternate;
                        artworkScaleX=static_cast<float>(effect.artworkScaleX)/100.0f;
                        artworkScaleY=static_cast<float>(effect.artworkScaleY)/100.0f;
                    }
                }
                auto evaluateEffects = [&](const std::wstring &ownerId,
                                           const std::vector<layer_model::LocalEffect> &effects,
                                           float driverX, float driverY,
                                           float &offsetX, float &offsetY, float &rotation,
                                           float &scale, float &opacity, int &pivot) {
                    for (const auto &effect : effects) {
                        if (!effect.enabled) continue;
                        if (effect.type == layer_model::LocalEffectType::ArtworkStateChange) continue;
                        const unsigned cycle = static_cast<unsigned>(std::max(200, effect.cycleMs));
                        const float phase = static_cast<float>(now % cycle) / static_cast<float>(cycle) * 6.28318530718f;
                        if (effect.type == layer_model::LocalEffectType::Sway) {
                            auto &reaction = localEffectReaction_[ownerId + L":" + effect.id];
                            if (reaction.second == 0) reaction.second = now;
                            const float elapsed = static_cast<float>(std::min<ULONGLONG>(now - reaction.second, 100));
                            reaction.second = now;
                            const float target = reactionActive ? 1.0f : 0.0f;
                            reaction.first += (target - reaction.first) * std::min(1.0f, elapsed / 220.0f);
                            const float reactionMultiplier = 1.0f + reaction.first *
                                static_cast<float>(effect.reactionBoost) / 100.0f;
                            rotation += std::sin(phase) * static_cast<float>(effect.amountX) *
                                        reactionMultiplier * 0.0174532925199f;
                            pivot = effect.pivot;
                        } else if (effect.type == layer_model::LocalEffectType::DangleSpring) {
                            auto &spring = localEffectSprings_[ownerId + L":" + effect.id];
                            if (spring.lastUpdate == 0) { spring.lastDriverX=driverX; spring.lastDriverY=driverY; spring.lastUpdate=now; }
                            const float elapsed = static_cast<float>(std::min<ULONGLONG>(now-spring.lastUpdate, 50)) / 1000.0f;
                            const bool active = effect.activeDuring == 2 || (effect.activeDuring == 1 ? reactionActive : !reactionActive);
                            if (active) {
                                const float flexibility = static_cast<float>(effect.amountX) / 100.0f;
                                spring.offsetX -= (driverX-spring.lastDriverX) * flexibility;
                                spring.offsetY -= (driverY-spring.lastDriverY) * flexibility;
                            }
                            spring.lastDriverX=driverX; spring.lastDriverY=driverY; spring.lastUpdate=now;
                            const float settleSeconds=std::max(0.2f,static_cast<float>(cycle)/1000.0f);
                            const float stiffness=39.4784176f/(settleSeconds*settleSeconds);
                            const float damping=2.0f*std::sqrt(stiffness)*0.72f;
                            spring.velocityX+=(-stiffness*spring.offsetX-damping*spring.velocityX)*elapsed;
                            spring.velocityY+=(-stiffness*spring.offsetY-damping*spring.velocityY)*elapsed;
                            spring.offsetX+=spring.velocityX*elapsed; spring.offsetY+=spring.velocityY*elapsed;
                            const float limit=static_cast<float>(std::max(0,effect.amountY));
                            spring.offsetX=std::clamp(spring.offsetX,-limit,limit); spring.offsetY=std::clamp(spring.offsetY,-limit,limit);
                            offsetX+=spring.offsetX; offsetY+=spring.offsetY;
                        } else if (effect.type == layer_model::LocalEffectType::ReactionNudge) {
                            auto &state = localEffectTriggers_[ownerId + L":" + effect.id];
                            if (reactionActive && !state.first) state.second = now;
                            state.first = reactionActive;
                            if (state.second > 0 && now - state.second < cycle) {
                                const float progress = static_cast<float>(now - state.second) / static_cast<float>(cycle);
                                const float envelope = std::sin(progress * 3.14159265359f);
                                offsetX += static_cast<float>(effect.amountX) * envelope;
                                offsetY += static_cast<float>(effect.amountY) * envelope;
                            }
                        } else {
                            const bool active = effect.activeDuring == 2 ||
                                (effect.activeDuring == 1 ? reactionActive : !reactionActive);
                            auto &activity = localEffectActivity_[ownerId + L":" + effect.id];
                            if (activity.second == 0) activity.second = now;
                            const float elapsed = static_cast<float>(std::min<ULONGLONG>(now - activity.second, 100));
                            activity.second = now;
                            const float target = active ? 1.0f : 0.0f;
                            activity.first += (target - activity.first) * std::min(1.0f, elapsed / 220.0f);
                            if (effect.type == layer_model::LocalEffectType::BoundedMovement) {
                                float unitX = std::sin(phase);
                                float unitY = std::sin(phase * 2.0f + 1.15f);
                                const float magnitude = std::sqrt(unitX * unitX + unitY * unitY);
                                if (magnitude > 1.0f) { unitX /= magnitude; unitY /= magnitude; }
                                offsetX += unitX * static_cast<float>(effect.amountX) * activity.first;
                                offsetY += unitY * static_cast<float>(effect.amountY) * activity.first;
                            } else if (effect.type == layer_model::LocalEffectType::Orbit) {
                                offsetX += std::cos(phase) * static_cast<float>(effect.amountX) * activity.first;
                                offsetY += std::sin(phase) * static_cast<float>(effect.amountY) * activity.first;
                            } else if (effect.type == layer_model::LocalEffectType::LocalPulse) {
                                scale *= 1.0f + std::sin(phase) * static_cast<float>(effect.amountX) * 0.01f * activity.first;
                            } else if (effect.type == layer_model::LocalEffectType::LocalSpin) {
                                const float direction = effect.amountY == 1 ? -1.0f : 1.0f;
                                rotation += phase * direction * activity.first;
                                pivot = effect.pivot;
                            } else if (effect.type == layer_model::LocalEffectType::StateVisibility) {
                                opacity *= activity.first;
                            } else if (effect.type == layer_model::LocalEffectType::Flutter) {
                                const float distance=static_cast<float>(effect.amountX)*activity.first;
                                offsetX+=(std::sin(phase*1.73f+0.4f)*0.65f+std::sin(phase*3.17f+2.0f)*0.35f)*distance;
                                offsetY+=(std::sin(phase*2.11f+1.2f)*0.7f+std::sin(phase*4.03f)*0.3f)*distance*0.55f;
                                rotation+=(std::sin(phase*1.37f)+std::sin(phase*2.79f+0.8f)*0.45f)*
                                    static_cast<float>(effect.amountY)*0.0174532925199f*activity.first;
                                pivot=effect.pivot;
                            }
                        }
                    }
                };
                const bool inheritsRoot = layer.inheritAvatarEffects && (!group || group->inheritAvatarEffects);
                const float motionX = inheritsRoot ? floatX + shakeX : 0.0f;
                const float motionY = inheritsRoot ? floatY + bounce + shakeY : 0.0f;
                float groupEffectX = 0.0f, groupEffectY = 0.0f, groupEffectRotation = 0.0f;
                float groupEffectScale = 1.0f, groupEffectOpacity = 1.0f;
                int groupEffectPivot = 0;
                if (group) evaluateEffects(group->id, group->effects,
                    group->inheritAvatarEffects ? floatX+shakeX : 0.0f,
                    group->inheritAvatarEffects ? floatY+bounce+shakeY : 0.0f,
                    groupEffectX, groupEffectY,
                    groupEffectRotation, groupEffectScale, groupEffectOpacity, groupEffectPivot);
                float layerEffectX = 0.0f, layerEffectY = 0.0f, localEffectRotation = 0.0f;
                float layerEffectScale = 1.0f, layerEffectOpacity = 1.0f;
                int localEffectPivot = 0;
                evaluateEffects(layer.id, layer.effects, motionX+(group?group->positionX+groupEffectX:0.0f),
                    motionY+(group?group->positionY+groupEffectY:0.0f), layerEffectX, layerEffectY, localEffectRotation,
                    layerEffectScale, layerEffectOpacity, localEffectPivot);
                const float groupScaleX = (group ? static_cast<float>(group->scaleX) / 100.0f : 1.0f) * groupEffectScale;
                const float groupScaleY = (group ? static_cast<float>(group->scaleY) / 100.0f : 1.0f) * groupEffectScale;
                const float localX = static_cast<float>(layer.scaleX) / 100.0f * groupScaleX * layerEffectScale * artworkScaleX;
                const float localY = static_cast<float>(layer.scaleY) / 100.0f * groupScaleY * layerEffectScale * artworkScaleY;
                const float effectScaleX = inheritsRoot && baseAvatarWidth > 0.0f
                                               ? avatarWidth / baseAvatarWidth : 1.0f;
                const float effectScaleY = inheritsRoot && baseAvatarHeight > 0.0f
                                               ? avatarHeight / baseAvatarHeight : 1.0f;
                const float rootX = primaryScale * effectScaleX;
                const float rootY = primaryScale * effectScaleY;
                const float layerWidth = rendered->texture.width * rootX * localX;
                const float layerHeight = rendered->texture.height * rootY * localY;
                const float centreX = static_cast<float>(width_) * 0.5f + motionX +
                    (group ? group->positionX + groupEffectX : 0) + (layer.positionX + layerEffectX) * groupScaleX;
                const float centreY = baseBottom - avatarHeight * 0.5f + motionY +
                    (group ? group->positionY + groupEffectY : 0) + (layer.positionY + layerEffectY) * groupScaleY;
                const float rootRotation = inheritsRoot ? shakeRotation + currentTilt_ : 0.0f;
                const float groupRotation = group ? static_cast<float>(group->rotationTenths) * 0.001745329252f + groupEffectRotation : 0.0f;
                const float localRotation = static_cast<float>(layer.rotationTenths) * 0.001745329252f;
                const float pivotU = static_cast<float>(layer.pivotX) / 100.0f;
                const float pivotV = static_cast<float>(layer.pivotY) / 100.0f;
                static constexpr float anchorU[] = {0.0f,0.0f,0.5f,1.0f,0.0f,0.5f,1.0f,0.0f,0.5f,1.0f};
                static constexpr float anchorV[] = {0.0f,0.0f,0.0f,0.0f,0.5f,0.5f,0.5f,1.0f,1.0f,1.0f};
                float groupPivotX = static_cast<float>(width_) * 0.5f + motionX +
                    (group ? group->positionX + groupEffectX : 0);
                float groupPivotY = baseBottom - avatarHeight * 0.5f + motionY +
                    (group ? group->positionY + groupEffectY : 0);
                if (group && groupEffectPivot > 0) {
                    float left=0.0f, right=0.0f, top=0.0f, bottom=0.0f; bool hasBounds=false;
                    for (const std::wstring &childId : group->layerOrder) {
                        const auto child = std::find_if(composition.layers.begin(), composition.layers.end(),
                            [&](const auto &candidate) { return candidate.id == childId; });
                        const auto childImage = std::find_if(layerImages_.begin(), layerImages_.end(),
                            [&](const auto &candidate) { return candidate.id == childId; });
                        if (child == composition.layers.end() || childImage == layerImages_.end()) continue;
                        const float childWidth = childImage->texture.width * primaryScale *
                            static_cast<float>(child->scaleX) / 100.0f * groupScaleX;
                        const float childHeight = childImage->texture.height * primaryScale *
                            static_cast<float>(child->scaleY) / 100.0f * groupScaleY;
                        const float childX = static_cast<float>(child->positionX) * groupScaleX;
                        const float childY = static_cast<float>(child->positionY) * groupScaleY;
                        const float candidateLeft=childX-childWidth*0.5f, candidateRight=childX+childWidth*0.5f;
                        const float candidateTop=childY-childHeight*0.5f, candidateBottom=childY+childHeight*0.5f;
                        if (!hasBounds) { left=candidateLeft; right=candidateRight; top=candidateTop; bottom=candidateBottom; hasBounds=true; }
                        else { left=std::min(left,candidateLeft); right=std::max(right,candidateRight); top=std::min(top,candidateTop); bottom=std::max(bottom,candidateBottom); }
                    }
                    if (hasBounds) {
                        groupPivotX += left + (right-left) * anchorU[std::clamp(groupEffectPivot,1,9)];
                        groupPivotY += top + (bottom-top) * anchorV[std::clamp(groupEffectPivot,1,9)];
                    }
                }
                localEffectPivot = std::clamp(localEffectPivot, 0, 9);
                const float effectPivotU = localEffectPivot == 0 ? pivotU : anchorU[localEffectPivot];
                const float effectPivotV = localEffectPivot == 0 ? pivotV : anchorV[localEffectPivot];
                const float avatarRootPivotX = static_cast<float>(width_) * 0.5f + floatX + shakeX;
                const float avatarRootPivotY = baseBottom + floatY + bounce + shakeY;
                const float groupOpacity = (group ? static_cast<float>(group->opacity) / 100.0f : 1.0f) * groupEffectOpacity;
                setPixelAppearance(inheritsRoot ? currentBrightness_ : 1.0f,
                                   static_cast<float>(layer.opacity) / 100.0f * groupOpacity * layerEffectOpacity);
                draw(rendered->texture, centreX - layerWidth * 0.5f, centreY - layerHeight * 0.5f,
                     layerWidth, layerHeight, localRotation, pivotU, pivotV,
                     localEffectRotation, effectPivotU, effectPivotV, rootRotation,
                     avatarRootPivotX, avatarRootPivotY, layer.flipHorizontal,
                     g_avatarFlipHorizontal.load(), groupRotation, groupPivotX,
                     groupPivotY, group && group->flipHorizontal);
        };
        for (auto item = composition.rootOrder.rbegin(); item != composition.rootOrder.rend(); ++item) {
            if (item->type == layer_model::StackItemType::Primary) {
                setPixelBrightness(currentBrightness_);
                draw(*activeAvatar, (static_cast<float>(width_) - avatarWidth) * 0.5f + floatX + shakeX,
                     baseBottom - avatarHeight + floatY + bounce + shakeY, avatarWidth,
                     avatarHeight, shakeRotation + currentTilt_, 0.5f, 1.0f,
                     0.0f, 0.5f, 0.5f, 0.0f,
                     static_cast<float>(width_) * 0.5f + floatX + shakeX,
                     baseBottom + floatY + bounce + shakeY, false, g_avatarFlipHorizontal.load());
                continue;
            }
            if (item->type == layer_model::StackItemType::Layer) {
                const auto layer = std::find_if(composition.layers.begin(), composition.layers.end(),
                    [&](const layer_model::Layer &candidate) { return candidate.id == item->id; });
                if (layer != composition.layers.end()) drawLayer(*layer);
            } else if (item->type == layer_model::StackItemType::Group) {
                const auto group = std::find_if(composition.groups.begin(), composition.groups.end(),
                    [&](const layer_model::Group &candidate) { return candidate.id == item->id; });
                if (group == composition.groups.end()) continue;
                for (auto child = group->layerOrder.rbegin(); child != group->layerOrder.rend(); ++child) {
                    const auto layer = std::find_if(composition.layers.begin(), composition.layers.end(),
                        [&](const layer_model::Layer &candidate) { return candidate.id == *child; });
                    if (layer != composition.layers.end()) drawLayer(*layer, &*group);
                }
            }
        }
        setPixelBrightness(1.0f);

        const bool overlayVisible = g_applicationActive.load() && !g_dialogOpen.load() &&
                                    !isAvatarSettingsWindowVisible();
        if (overlayVisible)
            drawOverlay();

        ID3D11ShaderResourceView *empty = nullptr;
        context_->PSSetShaderResources(0, 1, &empty);
        updateSpoutOutput();
        check(swapChain_->Present(1, 0));
    }

private:
    void updateSpoutOutput()
    {
        if (g_captureMethod.load() != 2) {
            if (spoutDeviceOpen_) {
                spoutSender_.ReleaseSender();
                spoutSender_.CloseDirectX11();
                spoutDeviceOpen_ = false;
            }
            setSpoutStatus(window_, 0);
            return;
        }

        if (!spoutDeviceOpen_) {
            setSpoutStatus(window_, 1);
            if (!spoutSender_.OpenDirectX11(device_.Get())) {
                setSpoutStatus(window_, 3);
                return;
            }
            spoutSender_.SetSenderName("RearSilver Avatar Suite");
            spoutSender_.SetSenderFormat(DXGI_FORMAT_R8G8B8A8_UNORM);
            spoutDeviceOpen_ = true;
        }

        ComPtr<ID3D11Texture2D> frame;
        if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&frame))) ||
            !spoutSender_.SendTexture(frame.Get())) {
            setSpoutStatus(window_, 3);
            return;
        }
        setSpoutStatus(window_, 2);
    }

    TextureAsset createTexture(const void *pixels, UINT width, UINT height)
    {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{pixels, width * 4, 0};
        ComPtr<ID3D11Texture2D> texture;
        check(device_->CreateTexture2D(&description, &initial, texture.GetAddressOf()));
        TextureAsset asset;
        check(device_->CreateShaderResourceView(texture.Get(), nullptr, asset.view.GetAddressOf()));
        asset.width = width;
        asset.height = height;
        return asset;
    }

    TextureAsset createSolid(unsigned char red, unsigned char green, unsigned char blue,
                             unsigned char alpha)
    {
        const unsigned char rgba[] = {red, green, blue, alpha};
        return createTexture(rgba, 1, 1);
    }

    TextureAsset createText(const wchar_t *text, UINT pixelHeight, COLORREF color)
    {
        constexpr int canvasWidth = 512;
        constexpr int canvasHeight = 80;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = canvasWidth;
        info.bmiHeader.biHeight = -canvasHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *pixels = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!dc || !bitmap) {
            if (bitmap)
                DeleteObject(bitmap);
            if (dc)
                DeleteDC(dc);
            throw E_OUTOFMEMORY;
        }
        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        HFONT font = CreateFontW(-static_cast<int>(pixelHeight), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                                 FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkColor(dc, RGB(0, 0, 0));
        SetTextColor(dc, color);
        RECT measure{0, 0, canvasWidth, canvasHeight};
        DrawTextW(dc, text, -1, &measure, DT_CALCRECT | DT_SINGLELINE);
        const int textWidth = std::clamp(measure.right, 1L, static_cast<LONG>(canvasWidth));
        const int textHeight = std::clamp(measure.bottom, 1L, static_cast<LONG>(canvasHeight));
        RECT drawRect{0, 0, textWidth, textHeight};
        DrawTextW(dc, text, -1, &drawRect, DT_LEFT | DT_TOP | DT_SINGLELINE);
        GdiFlush();

        std::vector<unsigned char> rgba(static_cast<size_t>(textWidth) * textHeight * 4);
        const auto *bgra = static_cast<const unsigned char *>(pixels);
        const unsigned char targetRed = GetRValue(color);
        const unsigned char targetGreen = GetGValue(color);
        const unsigned char targetBlue = GetBValue(color);
        for (int y = 0; y < textHeight; ++y) {
            for (int x = 0; x < textWidth; ++x) {
                const size_t source = static_cast<size_t>(y * canvasWidth + x) * 4;
                const size_t destination = static_cast<size_t>(y * textWidth + x) * 4;
                const unsigned char coverage = std::max({bgra[source], bgra[source + 1], bgra[source + 2]});
                rgba[destination] = targetRed;
                rgba[destination + 1] = targetGreen;
                rgba[destination + 2] = targetBlue;
                rgba[destination + 3] = coverage;
            }
        }
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBitmap);
        DeleteObject(font);
        DeleteObject(bitmap);
        DeleteDC(dc);
        return createTexture(rgba.data(), static_cast<UINT>(textWidth), static_cast<UINT>(textHeight));
    }

    void createRenderTarget()
    {
        ComPtr<ID3D11Texture2D> backBuffer;
        check(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
        check(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, target_.GetAddressOf()));
    }

    void createPipeline()
    {
        static constexpr char shader[] =
            "struct V{float2 p:POSITION;float2 uv:TEXCOORD0;};"
            "struct P{float4 p:SV_POSITION;float2 uv:TEXCOORD0;};"
            "P vs(V i){P o;o.p=float4(i.p,0,1);o.uv=i.uv;return o;}"
            "Texture2D img:register(t0);SamplerState smp:register(s0);"
            "cbuffer B:register(b0){float brightness;float opacity;float2 appearancePad;}"
            "float4 ps(P i):SV_TARGET{float4 c=img.Sample(smp,i.uv);c.rgb*=brightness;c.a*=opacity;return c;}";
        ComPtr<ID3DBlob> vertexBlob;
        ComPtr<ID3DBlob> pixelBlob;
        ComPtr<ID3DBlob> errors;
        check(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0,
                         vertexBlob.GetAddressOf(), errors.GetAddressOf()));
        errors.Reset();
        check(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0,
                         pixelBlob.GetAddressOf(), errors.GetAddressOf()));
        check(device_->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
                                          nullptr, vertexShader_.GetAddressOf()));
        check(device_->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(),
                                         nullptr, pixelShader_.GetAddressOf()));
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        check(device_->CreateInputLayout(elements, static_cast<UINT>(std::size(elements)),
                                         vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
                                         inputLayout_.GetAddressOf()));

        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(Vertex) * 6;
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        check(device_->CreateBuffer(&buffer, nullptr, vertexBuffer_.GetAddressOf()));

        D3D11_BUFFER_DESC brightnessBuffer{};
        brightnessBuffer.ByteWidth = sizeof(float) * 4;
        brightnessBuffer.Usage = D3D11_USAGE_DEFAULT;
        brightnessBuffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device_->CreateBuffer(&brightnessBuffer, nullptr, brightnessBuffer_.GetAddressOf()));

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        check(device_->CreateSamplerState(&sampler, sampler_.GetAddressOf()));

        D3D11_BLEND_DESC blend{};
        auto &target = blend.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_ONE;
        target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        check(device_->CreateBlendState(&blend, blend_.GetAddressOf()));
    }

    void createAssets()
    {
        constexpr UINT size = 512;
        std::vector<unsigned char> pixels(static_cast<size_t>(size) * size * 4, 0);
        auto paint = [&](int x, int y, unsigned char r, unsigned char g, unsigned char b,
                         unsigned char a) {
            if (x < 0 || y < 0 || x >= static_cast<int>(size) || y >= static_cast<int>(size))
                return;
            const size_t index = static_cast<size_t>(y * size + x) * 4;
            pixels[index] = r;
            pixels[index + 1] = g;
            pixels[index + 2] = b;
            pixels[index + 3] = a;
        };
        for (int y = 0; y < static_cast<int>(size); ++y) {
            for (int x = 0; x < static_cast<int>(size); ++x) {
                const float dx = (x - 256.0f) / 180.0f;
                const float dy = (y - 285.0f) / 190.0f;
                const bool head = dx * dx + dy * dy <= 1.0f;
                const bool leftEar = y < 190 && x > 105 && x < 245 && y > 0.82f * x - 55;
                const bool rightEar = y < 190 && x > 267 && x < 407 && y > -0.82f * x + 365;
                if (head || leftEar || rightEar)
                    paint(x, y, 89, 181, 226, 255);
            }
        }
        primaryAvatar_ = createTexture(pixels.data(), size, size);
        control_ = createSolid(30, 36, 48, 255);
        border_ = createSolid(48, 59, 74, 255);
        accent_ = createSolid(0, 212, 255, 255);
        disabledOverlay_ = createSolid(4, 8, 12, 150);
        const std::wstring directory = executableDirectory();
        auto loadRailIcon = [this, &directory](const wchar_t *fileName) {
            PendingImage decoded;
            if (!decodePng((directory + L"\\" + fileName).c_str(), decoded))
                throw E_INVALIDARG;
            return createTexture(decoded.rgba.data(), decoded.width, decoded.height);
        };
        presetsIcon_ = loadRailIcon(L"rail-presets.png");
        reactionsOnIcon_ = loadRailIcon(L"rail-reactions-on.png");
        reactionsOffIcon_ = loadRailIcon(L"rail-reactions-off.png");
        websocketIcon_ = loadRailIcon(L"rail-websocket.png");
        backgroundsIcon_ = loadRailIcon(L"rail-background.png");
        toolsIcon_ = loadRailIcon(L"rail-tools.png");
        settingsIcon_ = loadRailIcon(L"rail-settings.png");
    }

    void bindPipeline()
    {
        const UINT stride = sizeof(Vertex);
        const UINT offset = 0;
        ID3D11Buffer *vertexBuffer = vertexBuffer_.Get();
        context_->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        context_->IASetInputLayout(inputLayout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        ID3D11Buffer *brightnessBuffer = brightnessBuffer_.Get();
        context_->PSSetConstantBuffers(0, 1, &brightnessBuffer);
        ID3D11SamplerState *sampler = sampler_.Get();
        context_->PSSetSamplers(0, 1, &sampler);
        context_->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
    }

    void setPixelBrightness(float brightness)
    {
        const float values[4] = {brightness, 1.0f, 0.0f, 0.0f};
        context_->UpdateSubresource(brightnessBuffer_.Get(), 0, nullptr, values, 0, 0);
    }

    void setPixelAppearance(float brightness, float opacity)
    {
        const float values[4] = {brightness, std::clamp(opacity, 0.0f, 1.0f), 0.0f, 0.0f};
        context_->UpdateSubresource(brightnessBuffer_.Get(), 0, nullptr, values, 0, 0);
    }

    void draw(const TextureAsset &texture, float x, float y, float width, float height,
              float rotation = 0.0f, float pivotU = 0.5f, float pivotV = 1.0f,
              float secondaryRotation = 0.0f, float secondaryPivotU = 0.5f,
              float secondaryPivotV = 0.5f, float rootRotation = 0.0f,
              float rootPivotX = 0.0f, float rootPivotY = 0.0f,
              bool flipTextureHorizontal = false, bool mirrorRootHorizontal = false,
              float groupRotation = 0.0f, float groupPivotX = 0.0f,
              float groupPivotY = 0.0f, bool mirrorGroupHorizontal = false)
    {
        if (!texture.view || width <= 0 || height <= 0)
            return;
        auto toClip = [this](float pixelX, float pixelY) {
            return std::pair<float, float>{2.0f * pixelX / width_ - 1.0f,
                                           1.0f - 2.0f * pixelY / height_};
        };
        auto rotate = [rotation, pivotX = x + width * pivotU, pivotY = y + height * pivotV](float px, float py) {
            if (std::abs(rotation) < 0.000001f)
                return std::pair<float, float>{px, py};
            const float cosine = std::cos(rotation);
            const float sine = std::sin(rotation);
            const float dx = px - pivotX;
            const float dy = py - pivotY;
            return std::pair<float, float>{pivotX + dx * cosine - dy * sine,
                                           pivotY + dx * sine + dy * cosine};
        };
        auto rotateSecondary = [secondaryRotation, pivotX = x + width * secondaryPivotU,
                                pivotY = y + height * secondaryPivotV](std::pair<float,float> point) {
            if (std::abs(secondaryRotation) < 0.000001f) return point;
            const float cosine = std::cos(secondaryRotation);
            const float sine = std::sin(secondaryRotation);
            const float dx = point.first - pivotX;
            const float dy = point.second - pivotY;
            return std::pair<float,float>{pivotX + dx * cosine - dy * sine,
                                          pivotY + dx * sine + dy * cosine};
        };
        auto composeRotations = [&](float px, float py) {
            const auto local = rotateSecondary({px, py});
            const auto layerPoint = rotate(local.first, local.second);
            std::pair<float,float> groupPoint = layerPoint;
            if (std::abs(groupRotation) >= 0.000001f) {
                const float cosine = std::cos(groupRotation);
                const float sine = std::sin(groupRotation);
                const float dx = layerPoint.first - groupPivotX;
                const float dy = layerPoint.second - groupPivotY;
                groupPoint = {groupPivotX + dx * cosine - dy * sine,
                              groupPivotY + dx * sine + dy * cosine};
            }
            if (mirrorGroupHorizontal) groupPoint.first = groupPivotX * 2.0f - groupPoint.first;
            std::pair<float,float> rootPoint = groupPoint;
            if (std::abs(rootRotation) >= 0.000001f) {
                const float cosine = std::cos(rootRotation);
                const float sine = std::sin(rootRotation);
                const float dx = groupPoint.first - rootPivotX;
                const float dy = groupPoint.second - rootPivotY;
                rootPoint = {rootPivotX + dx * cosine - dy * sine,
                             rootPivotY + dx * sine + dy * cosine};
            }
            if (mirrorRootHorizontal) rootPoint.first = rootPivotX * 2.0f - rootPoint.first;
            return rootPoint;
        };
        const auto topLeftPixel = composeRotations(x, y);
        const auto topRightPixel = composeRotations(x + width, y);
        const auto bottomLeftPixel = composeRotations(x, y + height);
        const auto bottomRightPixel = composeRotations(x + width, y + height);
        const auto topLeft = toClip(topLeftPixel.first, topLeftPixel.second);
        const auto topRight = toClip(topRightPixel.first, topRightPixel.second);
        const auto bottomLeft = toClip(bottomLeftPixel.first, bottomLeftPixel.second);
        const auto bottomRight = toClip(bottomRightPixel.first, bottomRightPixel.second);
        const float leftU = flipTextureHorizontal ? 1.0f : 0.0f;
        const float rightU = flipTextureHorizontal ? 0.0f : 1.0f;
        Vertex vertices[6];
        if (mirrorRootHorizontal != mirrorGroupHorizontal) {
            // Mirroring the completed geometry reverses its winding. Submit the
            // logical corners in the opposite order so the front face remains
            // visible with Direct3D's normal back-face culling enabled.
            vertices[0] = {topLeft.first, topLeft.second, leftU, 0};
            vertices[1] = {bottomLeft.first, bottomLeft.second, leftU, 1};
            vertices[2] = {topRight.first, topRight.second, rightU, 0};
            vertices[3] = {bottomLeft.first, bottomLeft.second, leftU, 1};
            vertices[4] = {bottomRight.first, bottomRight.second, rightU, 1};
            vertices[5] = {topRight.first, topRight.second, rightU, 0};
        } else {
            vertices[0] = {topLeft.first, topLeft.second, leftU, 0};
            vertices[1] = {topRight.first, topRight.second, rightU, 0};
            vertices[2] = {bottomLeft.first, bottomLeft.second, leftU, 1};
            vertices[3] = {bottomLeft.first, bottomLeft.second, leftU, 1};
            vertices[4] = {topRight.first, topRight.second, rightU, 0};
            vertices[5] = {bottomRight.first, bottomRight.second, rightU, 1};
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context_->Map(vertexBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        memcpy(mapped.pData, vertices, sizeof(vertices));
        context_->Unmap(vertexBuffer_.Get(), 0);
        ID3D11ShaderResourceView *view = texture.view.Get();
        context_->PSSetShaderResources(0, 1, &view);
        context_->Draw(6, 0);
    }

    void drawRect(const TextureAsset &texture, const RectF &rect)
    {
        draw(texture, rect.x, rect.y, rect.width, rect.height);
    }

    void drawOutlinedRect(const RectF &rect, const TextureAsset &fill, float borderWidth = 2.0f)
    {
        drawRect(border_, rect);
        const RectF inner{rect.x + borderWidth, rect.y + borderWidth,
                          std::max(1.0f, rect.width - borderWidth * 2.0f),
                          std::max(1.0f, rect.height - borderWidth * 2.0f)};
        drawRect(fill, inner);
    }

    void drawLabel(const TextureAsset &label, const RectF &bounds, float maxHeight)
    {
        const float scale = std::min((bounds.width - 24.0f) / label.width,
                                     maxHeight / label.height);
        const float width = label.width * scale;
        const float height = label.height * scale;
        draw(label, bounds.x + (bounds.width - width) * 0.5f,
             bounds.y + (bounds.height - height) * 0.5f, width, height);
    }

    void drawOverlay()
    {
        const UiLayout ui = calculateUiLayout(width_, height_, kOutputUiDpi);
        drawRect(presetsIcon_, ui.presets);
        drawRect(g_reactionsEnabled.load() ? reactionsOnIcon_ : reactionsOffIcon_, ui.reactions);
        drawRect(websocketIcon_, ui.websocket);
        drawRect(backgroundsIcon_, ui.backgrounds);
        drawRect(toolsIcon_, ui.tools);
        drawRect(settingsIcon_, ui.settings);
    }

    HWND window_ = nullptr;
    UINT width_ = 1;
    UINT height_ = 1;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<IDCompositionDevice> compositionDevice_;
    ComPtr<IDCompositionTarget> compositionTarget_;
    ComPtr<IDCompositionVisual> compositionVisual_;
    ComPtr<IDCompositionMatrixTransform> compositionTransform_;
    ComPtr<ID3D11RenderTargetView> target_;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    ComPtr<ID3D11Buffer> brightnessBuffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blend_;
    TextureAsset primaryAvatar_;
    TextureAsset reactionAvatar_;
    TextureAsset primaryBlinkAvatar_;
    TextureAsset reactionBlinkAvatar_;
    bool reactionAvatarLoaded_ = false;
    bool primaryBlinkAvatarLoaded_ = false;
    bool reactionBlinkAvatarLoaded_ = false;
    bool blinking_ = false;
    ULONGLONG nextBlinkAt_ = 0;
    ULONGLONG blinkEndsAt_ = 0;
    unsigned blinkRandomState_ = static_cast<unsigned>(GetTickCount());
    float currentBreathAmount_ = 0.0f;
    ULONGLONG lastBreathUpdateAt_ = 0;
    float currentShakeMix_ = 0.0f;
    ULONGLONG lastShakeUpdateAt_ = 0;
    float currentBrightness_ = 1.0f;
    float brightnessStart_ = 1.0f;
    float brightnessTarget_ = 1.0f;
    ULONGLONG brightnessTransitionStartedAt_ = 0;
    float currentFloatHeight_ = 0.0f;
    float currentFloatDrift_ = 0.0f;
    unsigned currentFloatDirection_ = 0;
    float currentFloatCorrectionX_ = 0.0f;
    float currentFloatCorrectionY_ = 0.0f;
    float floatCorrectionStartX_ = 0.0f;
    float floatCorrectionStartY_ = 0.0f;
    ULONGLONG floatCorrectionStartedAt_ = 0;
    float currentTilt_ = 0.0f;
    float tiltStart_ = 0.0f;
    float tiltTarget_ = 0.0f;
    ULONGLONG tiltTransitionStartedAt_ = 0;
    TextureAsset control_;
    TextureAsset border_;
    TextureAsset accent_;
    TextureAsset disabledOverlay_;
    TextureAsset presetsIcon_;
    TextureAsset reactionsOnIcon_;
    TextureAsset reactionsOffIcon_;
    TextureAsset websocketIcon_;
    TextureAsset backgroundsIcon_;
    TextureAsset toolsIcon_;
    TextureAsset settingsIcon_;
    TextureAsset backgroundImage_;
    struct LayerTexture {
        std::wstring id;
        TextureAsset texture;
        float reactionMix = 0.0f;
        ULONGLONG lastReactionMixUpdate = 0;
        float eyeActivityMix = 0.0f;
        ULONGLONG lastEyeMotionUpdate = 0;
    };
    std::vector<LayerTexture> layerImages_;
    std::unordered_map<std::wstring, std::pair<float, ULONGLONG>> localEffectActivity_;
    std::unordered_map<std::wstring, std::pair<float, ULONGLONG>> localEffectReaction_;
    std::unordered_map<std::wstring, std::pair<bool, ULONGLONG>> localEffectTriggers_;
    struct SpringRuntime {
        float lastDriverX=0.0f,lastDriverY=0.0f,offsetX=0.0f,offsetY=0.0f,velocityX=0.0f,velocityY=0.0f;
        ULONGLONG lastUpdate=0;
    };
    std::unordered_map<std::wstring, SpringRuntime> localEffectSprings_;
    bool backgroundImageLoaded_ = false;
    spoutDX spoutSender_;
    bool spoutDeviceOpen_ = false;
};

void renderThreadMain()
{
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        Renderer renderer;
        renderer.initialize(g_mainWindow);
        while (g_running.load()) {
            if (g_transformPending.exchange(false)) {
                const UINT latestWidth = g_clientWidth.load();
                const UINT latestHeight = g_clientHeight.load();
                if (latestWidth > 0 && latestHeight > 0)
                    renderer.updateLocalTransform(latestWidth, latestHeight);
            }
            renderer.render();
        }
    } catch (HRESULT error) {
        logMessage(L"Render thread failed with HRESULT 0x" + std::to_wstring(static_cast<unsigned>(error)));
        PostMessageW(g_mainWindow, kRenderFailureMessage, static_cast<WPARAM>(error), 0);
    } catch (...) {
        PostMessageW(g_mainWindow, kRenderFailureMessage, static_cast<WPARAM>(E_FAIL), 0);
    }
    if (SUCCEEDED(com))
        CoUninitialize();
}

void handlePointerRelease(HWND window, float x, float y)
{
    if (!g_applicationActive.load() || g_dialogOpen.load())
        return;
    RECT client{};
    GetClientRect(window, &client);
    const float clientWidth = static_cast<float>(client.right);
    const float clientHeight = static_cast<float>(client.bottom);
    if (clientWidth <= 0.0f || clientHeight <= 0.0f)
        return;
    const float scale = std::min(clientWidth / kOutputWidth, clientHeight / kOutputHeight);
    const float offsetX = (clientWidth - kOutputWidth * scale) * 0.5f;
    const float offsetY = (clientHeight - kOutputHeight * scale) * 0.5f;
    const float outputX = (x - offsetX) / scale;
    const float outputY = (y - offsetY) / scale;
    if (outputX < 0.0f || outputY < 0.0f || outputX >= kOutputWidth || outputY >= kOutputHeight)
        return;
    const UiLayout ui = calculateUiLayout(kOutputWidth, kOutputHeight, kOutputUiDpi);
    if (ui.reactions.contains(outputX, outputY)) {
        const bool enabled = !g_reactionsEnabled.load();
        g_reactionsEnabled.store(enabled);
        saveSetting(L"ReactionsEnabled", enabled ? L"1" : L"0");
        if (!enabled)
            setMicrophoneReaction(false);
        return;
    }
    if (ui.presets.contains(outputX, outputY)) {
        showAvatarSettingsWindow(window, L"presets");
        return;
    }
    if (ui.websocket.contains(outputX, outputY)) {
        showAvatarSettingsWindow(window, L"websocket");
        return;
    }
    if (ui.backgrounds.contains(outputX, outputY)) {
        showAvatarSettingsWindow(window, L"backgrounds");
        return;
    }
    if (ui.tools.contains(outputX, outputY)) {
        showAvatarSettingsWindow(window, L"feedback");
        return;
    }
    if (ui.settings.contains(outputX, outputY))
        showAvatarSettingsWindow(window);
}

bool pointIsOnRail(HWND window, float x, float y)
{
    RECT client{};
    GetClientRect(window, &client);
    const float clientWidth = static_cast<float>(client.right);
    const float clientHeight = static_cast<float>(client.bottom);
    if (clientWidth <= 0.0f || clientHeight <= 0.0f)
        return false;

    const float scale = std::min(clientWidth / kOutputWidth, clientHeight / kOutputHeight);
    const float offsetX = (clientWidth - kOutputWidth * scale) * 0.5f;
    const float offsetY = (clientHeight - kOutputHeight * scale) * 0.5f;
    const float outputX = (x - offsetX) / scale;
    const float outputY = (y - offsetY) / scale;
    const UiLayout ui = calculateUiLayout(kOutputWidth, kOutputHeight, kOutputUiDpi);
    return ui.presets.contains(outputX, outputY) ||
           ui.reactions.contains(outputX, outputY) ||
           ui.websocket.contains(outputX, outputY) ||
           ui.backgrounds.contains(outputX, outputY) ||
           ui.tools.contains(outputX, outputY) ||
           ui.settings.contains(outputX, outputY);
}

void paintWindowBackground(HWND window, HDC target)
{
    RECT client{};
    GetClientRect(window, &client);
    const int targetWidth = std::max(1L, client.right - client.left);
    const int targetHeight = std::max(1L, client.bottom - client.top);
    const unsigned mode = activeBackgroundMode();
    if (mode == 1 || mode == 2) {
        const unsigned colour = mode == 2 ? g_backgroundChromaColour.load()
                                           : g_backgroundSolidColour.load();
        HBRUSH brush = CreateSolidBrush(RGB((colour >> 16) & 0xff, (colour >> 8) & 0xff,
                                            colour & 0xff));
        FillRect(target, &client, brush);
        DeleteObject(brush);
        return;
    }

    HBRUSH fallback = static_cast<HBRUSH>(GetStockObject(mode == 3 ? BLACK_BRUSH : WHITE_BRUSH));
    FillRect(target, &client, fallback);
    if (mode != 3)
        return;

    std::lock_guard<std::mutex> lock(g_windowBackgroundMutex);
    if (g_windowBackgroundBgra.empty() || g_windowBackgroundWidth == 0 ||
        g_windowBackgroundHeight == 0)
        return;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(g_windowBackgroundWidth);
    info.bmiHeader.biHeight = -static_cast<LONG>(g_windowBackgroundHeight);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HDC source = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateDIBSection(source, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!source || !bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        if (source) DeleteDC(source);
        return;
    }
    memcpy(bits, g_windowBackgroundBgra.data(), g_windowBackgroundBgra.size());
    HGDIOBJ oldBitmap = SelectObject(source, bitmap);
    const float imageWidth = static_cast<float>(g_windowBackgroundWidth);
    const float imageHeight = static_cast<float>(g_windowBackgroundHeight);
    const unsigned fit = g_backgroundFit.load();
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    auto paint = [&](int x, int y, int width, int height) {
        AlphaBlend(target, x, y, width, height, source, 0, 0,
                   static_cast<int>(g_windowBackgroundWidth),
                   static_cast<int>(g_windowBackgroundHeight), blend);
    };
    if (fit == 2) {
        paint(0, 0, targetWidth, targetHeight);
    } else if (fit == 3) {
        for (int y = 0; y < targetHeight; y += static_cast<int>(g_windowBackgroundHeight))
            for (int x = 0; x < targetWidth; x += static_cast<int>(g_windowBackgroundWidth))
                paint(x, y, static_cast<int>(g_windowBackgroundWidth),
                      static_cast<int>(g_windowBackgroundHeight));
    } else {
        const float scaleX = static_cast<float>(targetWidth) / imageWidth;
        const float scaleY = static_cast<float>(targetHeight) / imageHeight;
        const float scale = fit == 1 ? std::max(scaleX, scaleY) : std::min(scaleX, scaleY);
        const int width = std::max(1, static_cast<int>(std::round(imageWidth * scale)));
        const int height = std::max(1, static_cast<int>(std::round(imageHeight * scale)));
        paint((targetWidth - width) / 2, (targetHeight - height) / 2, width, height);
    }
    SelectObject(source, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(source);
}

void enterBorderlessFullscreen(HWND window)
{
    if (g_borderlessFullscreen)
        return;

    g_windowedPlacement.length = sizeof(g_windowedPlacement);
    if (!GetWindowPlacement(window, &g_windowedPlacement))
        return;

    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
        return;

    g_windowedStyle = GetWindowLongPtrW(window, GWL_STYLE);
    g_borderlessFullscreen = true;
    SetWindowLongPtrW(window, GWL_STYLE,
                      (g_windowedStyle & WS_VISIBLE) | WS_POPUP);
    const RECT &bounds = monitorInfo.rcMonitor;
    SetWindowPos(window, nullptr, bounds.left, bounds.top,
                 bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_FRAMECHANGED);
}

void exitBorderlessFullscreen(HWND window)
{
    if (!g_borderlessFullscreen)
        return;

    g_borderlessFullscreen = false;
    SetWindowLongPtrW(window, GWL_STYLE, g_windowedStyle);
    SetWindowPlacement(window, &g_windowedPlacement);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                     SWP_NOZORDER | SWP_FRAMECHANGED);
}

void constrainWindowToOutputAspect(HWND window, WPARAM edge, RECT &bounds)
{
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    const DWORD extendedStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    RECT frame{0, 0, static_cast<LONG>(kOutputWidth), static_cast<LONG>(kOutputHeight)};
    AdjustWindowRectExForDpi(&frame, style, FALSE, extendedStyle, GetDpiForWindow(window));
    const LONG frameWidth = (frame.right - frame.left) - static_cast<LONG>(kOutputWidth);
    const LONG frameHeight = (frame.bottom - frame.top) - static_cast<LONG>(kOutputHeight);

    LONG clientWidth = std::max<LONG>(1, bounds.right - bounds.left - frameWidth);
    LONG clientHeight = std::max<LONG>(1, bounds.bottom - bounds.top - frameHeight);
    const bool heightDriven = edge == WMSZ_TOP || edge == WMSZ_BOTTOM;
    if (heightDriven)
        clientWidth = std::max<LONG>(1, MulDiv(clientHeight, kOutputWidth, kOutputHeight));
    else
        clientHeight = std::max<LONG>(1, MulDiv(clientWidth, kOutputHeight, kOutputWidth));

    const LONG outerWidth = clientWidth + frameWidth;
    const LONG outerHeight = clientHeight + frameHeight;
    if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
        bounds.left = bounds.right - outerWidth;
    else
        bounds.right = bounds.left + outerWidth;
    if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
        bounds.top = bounds.bottom - outerHeight;
    else
        bounds.bottom = bounds.top + outerHeight;
}

const char *localEffectTypeId(layer_model::LocalEffectType type)
{
    switch (type) {
    case layer_model::LocalEffectType::Sway: return "sway";
    case layer_model::LocalEffectType::BoundedMovement: return "bounded-movement";
    case layer_model::LocalEffectType::Orbit: return "orbit";
    case layer_model::LocalEffectType::LocalPulse: return "local-pulse";
    case layer_model::LocalEffectType::LocalSpin: return "local-spin";
    case layer_model::LocalEffectType::ReactionNudge: return "reaction-nudge";
    case layer_model::LocalEffectType::StateVisibility: return "state-visibility";
    case layer_model::LocalEffectType::DangleSpring: return "dangle-spring";
    case layer_model::LocalEffectType::Flutter: return "flutter";
    case layer_model::LocalEffectType::ArtworkStateChange: return "artwork-state-change";
    }
    return "effect";
}

const char *localEffectTypeName(layer_model::LocalEffectType type)
{
    switch (type) {
    case layer_model::LocalEffectType::Sway: return "Sway";
    case layer_model::LocalEffectType::BoundedMovement: return "Bounded movement";
    case layer_model::LocalEffectType::Orbit: return "Orbit";
    case layer_model::LocalEffectType::LocalPulse: return "Pulse";
    case layer_model::LocalEffectType::LocalSpin: return "Spin";
    case layer_model::LocalEffectType::ReactionNudge: return "Reaction nudge";
    case layer_model::LocalEffectType::StateVisibility: return "State visibility";
    case layer_model::LocalEffectType::DangleSpring: return "Dangle spring";
    case layer_model::LocalEffectType::Flutter: return "Flutter";
    case layer_model::LocalEffectType::ArtworkStateChange: return "Artwork state change";
    }
    return "Effect";
}

std::string automationCatalogueResponse()
{
    const auto presets = preset_store::listPresets();
    layer_model::Composition composition;
    { std::lock_guard<std::mutex> lock(g_layerStateMutex); composition = g_composition; }
    std::string json = "{\"ok\":true,\"action\":\"catalog.get\",\"protocolVersion\":1,\"catalog\":{";
    json += "\"activePreset\":{\"id\":" + jsonQuoted(preset_store::activePresetId()) +
            ",\"name\":" + jsonQuoted(preset_store::activePresetName()) + "},\"presets\":[";
    for (size_t index = 0; index < presets.size(); ++index) {
        if (index) json += ',';
        json += "{\"id\":" + jsonQuoted(presets[index].id) + ",\"name\":" + jsonQuoted(presets[index].name) +
                ",\"active\":" + (presets[index].active ? "true" : "false") + "}";
    }
    json += "],\"groups\":[";
    for (size_t index = 0; index < composition.groups.size(); ++index) {
        if (index) json += ',';
        const auto &group = composition.groups[index];
        json += "{\"id\":" + jsonQuoted(group.id) + ",\"name\":" + jsonQuoted(group.name) +
                ",\"visible\":" + (group.visible ? "true" : "false") + "}";
    }
    json += "],\"layers\":[";
    for (size_t index = 0; index < composition.layers.size(); ++index) {
        if (index) json += ',';
        const auto &layer = composition.layers[index];
        std::wstring groupId;
        for (const auto &group : composition.groups)
            if (std::find(group.layerOrder.begin(), group.layerOrder.end(), layer.id) != group.layerOrder.end()) { groupId = group.id; break; }
        json += "{\"id\":" + jsonQuoted(layer.id) + ",\"name\":" + jsonQuoted(layer.name) +
                ",\"groupId\":" + jsonQuoted(groupId) + ",\"visible\":" + (layer.visible ? "true" : "false") + "}";
    }
    json += "],\"effects\":[";
    bool firstEffect = true;
    auto addEffect = [&](const std::string &id, const std::string &name, const std::string &type,
                         const std::string &ownerType, const std::wstring &ownerId,
                         const std::wstring &ownerName, bool enabled) {
        if (!firstEffect) json += ',';
        firstEffect = false;
        json += "{\"id\":" + jsonQuoted(id) + ",\"name\":" + jsonQuoted(name) + ",\"type\":" + jsonQuoted(type) +
                ",\"ownerType\":" + jsonQuoted(ownerType) + ",\"ownerId\":" + jsonQuoted(ownerId) +
                ",\"ownerName\":" + jsonQuoted(ownerName) + ",\"enabled\":" + (enabled ? "true" : "false") + "}";
    };
    auto addLocalEffects = [&](const auto &owner, const char *ownerType) {
        for (const auto &effect : owner.effects)
            addEffect(toUtf8(effect.id), localEffectTypeName(effect.type), localEffectTypeId(effect.type),
                      ownerType, owner.id, owner.name, effect.enabled);
    };
    for (const auto &layer : composition.layers) addLocalEffects(layer, "layer");
    for (const auto &group : composition.groups) addLocalEffects(group, "group");
    auto addPrimary = [&](const char *id, const char *name, bool added, bool enabled) {
        if (added) addEffect(id, name, id, "primary", L"primary", L"Primary avatar", enabled);
    };
    addPrimary("bounce", "Bounce", g_bounceAdded.load(), g_bounceEnabled.load());
    addPrimary("breathing", "Breathing", g_breathingAdded.load(), g_breathingEnabled.load());
    addPrimary("squash", "Squash & stretch", g_squashAdded.load(), g_squashEnabled.load());
    addPrimary("shake", "Shake", g_shakeAdded.load(), g_shakeEnabled.load());
    addPrimary("brightness", "Brightness", g_brightnessAdded.load(), g_brightnessEnabled.load());
    addPrimary("float", "Float", g_floatAdded.load(), g_floatEnabled.load());
    addPrimary("tilt", "Tilt", g_tiltAdded.load(), g_tiltEnabled.load());
    json += "],\"savedActions\":[]}}";
    return json;
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case kWebSocketCommandMessage: {
        auto *request = reinterpret_cast<WebSocketCommandRequest *>(lParam);
        if (!request) return 0;
        const std::string action = jsonStringValue(request->payload, "action");
        {
            std::lock_guard<std::mutex> lock(g_webSocketStateMutex);
            g_webSocketLastAction = fromUtf8(action);
        }
        if (action == "client.hello") {
            request->response = "{\"ok\":true,\"action\":\"client.hello\",\"server\":\"RearSilver Avatar Suite\",\"protocolVersion\":1,\"capabilities\":[\"catalog.get\",\"preset.activate\",\"sequence.run\"]}";
            return 0;
        }
        if (action == "server.status") {
            request->response = "{\"ok\":true,\"action\":\"server.status\",\"version\":1,\"protocolVersion\":1}";
            return 0;
        }
        if (action == "catalog.get") {
            request->response = automationCatalogueResponse();
            return 0;
        }
        if (action == "sequence.run") {
            const auto steps = jsonObjectArray(request->payload, "steps");
            if (steps.empty() || steps.size() > 32) {
                request->response = "{\"ok\":false,\"error\":\"A sequence requires between 1 and 32 steps\"}";
                return 0;
            }
            layer_model::Composition next;
            { std::lock_guard<std::mutex> lock(g_layerStateMutex); next = g_composition; }
            std::vector<std::pair<std::string, bool>> primaryEffectChanges;
            bool primaryEffectsChanged = false;
            auto applyPending = [&] {
                { std::lock_guard<std::mutex> lock(g_layerStateMutex); g_composition = next; }
                for (const auto &[effect, enabled] : primaryEffectChanges) {
                    if (effect == "bounce") g_bounceEnabled.store(enabled);
                    else if (effect == "breathing") g_breathingEnabled.store(enabled);
                    else if (effect == "squash") g_squashEnabled.store(enabled);
                    else if (effect == "shake") g_shakeEnabled.store(enabled);
                    else if (effect == "brightness") g_brightnessEnabled.store(enabled);
                    else if (effect == "float") g_floatEnabled.store(enabled);
                    else if (effect == "tilt") g_tiltEnabled.store(enabled);
                }
                primaryEffectsChanged = primaryEffectsChanged || !primaryEffectChanges.empty();
                primaryEffectChanges.clear();
            };
            for (size_t index = 0; index < steps.size(); ++index) {
                const std::string stepAction = jsonStringValue(steps[index], "action");
                if (stepAction == "preset.activate") {
                    const std::wstring presetId = fromUtf8(jsonStringValue(steps[index], "presetId"));
                    bool exists = false;
                    for (const auto &preset : preset_store::listPresets())
                        if (preset.id == presetId) { exists = true; break; }
                    if (!exists) {
                        request->response = "{\"ok\":false,\"error\":\"Preset in step " + std::to_string(index + 1) + " was not found\"}";
                        return 0;
                    }
                    applyPending();
                    if (!preset_store::selectPreset(presetId) || !preset_store::revertActivePreset()) {
                        request->response = "{\"ok\":false,\"error\":\"Preset in step " + std::to_string(index + 1) + " could not be activated\"}";
                        return 0;
                    }
                    ++g_presetGeneration;
                    applyPresetDraftToRuntime();
                    { std::lock_guard<std::mutex> lock(g_layerStateMutex); next = g_composition; }
                    continue;
                }
                const std::wstring targetId = fromUtf8(jsonStringValue(steps[index], "targetId"));
                bool enabled = false;
                if (!jsonBooleanValue(steps[index], "enabled", enabled)) {
                    request->response = "{\"ok\":false,\"error\":\"Step " + std::to_string(index + 1) + " requires enabled true or false\"}";
                    return 0;
                }
                bool found = false;
                if (stepAction == "layer.visibility") {
                    for (auto &layer : next.layers) if (layer.id == targetId) { layer.visible = enabled; found = true; break; }
                } else if (stepAction == "group.visibility") {
                    for (auto &group : next.groups) if (group.id == targetId) { group.visible = enabled; found = true; break; }
                } else if (stepAction == "effect.enabled") {
                    const std::wstring effectId = fromUtf8(jsonStringValue(steps[index], "effectId"));
                    if (targetId == L"primary") {
                        const std::string primaryEffect = jsonStringValue(steps[index], "effectId");
                        const bool exists = (primaryEffect == "bounce" && g_bounceAdded.load()) ||
                            (primaryEffect == "breathing" && g_breathingAdded.load()) ||
                            (primaryEffect == "squash" && g_squashAdded.load()) ||
                            (primaryEffect == "shake" && g_shakeAdded.load()) ||
                            (primaryEffect == "brightness" && g_brightnessAdded.load()) ||
                            (primaryEffect == "float" && g_floatAdded.load()) ||
                            (primaryEffect == "tilt" && g_tiltAdded.load());
                        if (exists) { primaryEffectChanges.emplace_back(primaryEffect, enabled); found = true; }
                    }
                    auto apply = [&](auto &owners) {
                        for (auto &owner : owners) if (owner.id == targetId)
                            for (auto &effect : owner.effects) if (effect.id == effectId) {
                                effect.enabled = enabled; found = true; return;
                            }
                    };
                    if (!found) apply(next.layers); if (!found) apply(next.groups);
                } else {
                    request->response = "{\"ok\":false,\"error\":\"Unsupported action in step " + std::to_string(index + 1) + "\"}";
                    return 0;
                }
                if (!found) {
                    request->response = "{\"ok\":false,\"error\":\"Target in step " + std::to_string(index + 1) + " was not found\"}";
                    return 0;
                }
            }
            applyPending();
            InvalidateRect(window, nullptr, FALSE);
            sendLayerState();
            sendPresetState();
            if (primaryEffectsChanged) sendBounceSettings();
            request->response = "{\"ok\":true,\"action\":\"sequence.run\",\"steps\":" + std::to_string(steps.size()) + "}";
            return 0;
        }
        if (action != "preset.activate") {
            request->response = "{\"ok\":false,\"error\":\"Unknown action. Supported actions: client.hello, server.status, catalog.get, preset.activate, sequence.run\"}";
            return 0;
        }
        const std::wstring presetId = fromUtf8(jsonStringValue(request->payload, "presetId"));
        bool exists = false;
        for (const auto &preset : preset_store::listPresets())
            if (preset.id == presetId) { exists = true; break; }
        if (!exists || !preset_store::selectPreset(presetId) || !preset_store::revertActivePreset()) {
            request->response = "{\"ok\":false,\"error\":\"Preset ID was not found\"}";
            return 0;
        }
        ++g_presetGeneration;
        applyPresetDraftToRuntime();
        sendLayerState();
        sendPresetState();
        request->response = "{\"ok\":true,\"action\":\"preset.activate\"}";
        return 0;
    }
    case WM_ACTIVATE:
        g_applicationActive.store(LOWORD(wParam) != WA_INACTIVE);
        return 0;
    case WM_DPICHANGED: {
        const RECT *suggested = reinterpret_cast<const RECT *>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    }
    case WM_SIZE: {
        const UINT width = LOWORD(lParam);
        const UINT height = HIWORD(lParam);
        g_clientWidth.store(width);
        g_clientHeight.store(height);
        if (width > 0 && height > 0) {
            g_transformPending.store(true);
            RedrawWindow(window, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        }
        return 0;
    }
    case WM_SIZING:
        if (!g_borderlessFullscreen) {
            constrainWindowToOutputAspect(window, wParam, *reinterpret_cast<RECT *>(lParam));
            return TRUE;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_ERASEBKGND:
        paintWindowBackground(window, reinterpret_cast<HDC>(wParam));
        return 1;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_MAXIMIZE) {
            enterBorderlessFullscreen(window);
            return 0;
        }
        if (g_borderlessFullscreen && (wParam & 0xfff0) == SC_RESTORE) {
            exitBorderlessFullscreen(window);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_LBUTTONUP:
        handlePointerRelease(window, static_cast<float>(GET_X_LPARAM(lParam)),
                             static_cast<float>(GET_Y_LPARAM(lParam)));
        return 0;
    case WM_LBUTTONDBLCLK:
        if (g_borderlessFullscreen &&
            !pointIsOnRail(window, static_cast<float>(GET_X_LPARAM(lParam)),
                           static_cast<float>(GET_Y_LPARAM(lParam)))) {
            exitBorderlessFullscreen(window);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE && g_borderlessFullscreen) {
            exitBorderlessFullscreen(window);
            return 0;
        }
        if (wParam == VK_F11) {
            if (g_borderlessFullscreen)
                exitBorderlessFullscreen(window);
            else
                enterBorderlessFullscreen(window);
            return 0;
        }
        if (wParam == 'O' && (GetKeyState(VK_CONTROL) & 0x8000))
            openPngPicker();
        return 0;
    case kAvatarSettingsChoosePngMessage:
        openPngPicker(reinterpret_cast<HWND>(lParam));
        return 0;
    case kAvatarSettingsChooseReactionPngMessage:
        openPngPicker(reinterpret_cast<HWND>(lParam), ImageSlot::Reaction);
        return 0;
    case kAvatarSettingsChoosePrimaryBlinkMessage:
        openPngPicker(reinterpret_cast<HWND>(lParam), ImageSlot::PrimaryBlink);
        return 0;
    case kAvatarSettingsChooseReactionBlinkMessage:
        openPngPicker(reinterpret_cast<HWND>(lParam), ImageSlot::ReactionBlink);
        return 0;
    case kAvatarSettingsChooseBackgroundImageMessage:
        openPngPicker(reinterpret_cast<HWND>(lParam), ImageSlot::Background);
        return 0;
    case kAvatarSettingsRemoveBackgroundImageMessage: {
        {
            std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
            g_backgroundImagePath.clear();
            g_backgroundImageLoaded = false;
        }
        {
            std::lock_guard<std::mutex> lock(g_windowBackgroundMutex);
            g_windowBackgroundBgra.clear();
            g_windowBackgroundWidth = 0;
            g_windowBackgroundHeight = 0;
        }
        InvalidateRect(window, nullptr, TRUE);
        saveSetting(L"BackgroundImage", L"");
        saveSetting(L"BackgroundImageDisplayName", L"");
        PendingImage clear;
        clear.slot = ImageSlot::Background;
        clear.clearSlot = true;
        {
            std::lock_guard<std::mutex> lock(g_pendingImageMutex);
            g_pendingImages.push_back(std::move(clear));
        }
        sendBackgroundSettings();
        return 0;
    }
    case kAvatarSettingsUseDefaultPrimaryMessage:
    case kAvatarSettingsUseDefaultReactionMessage: {
        const bool reaction = message == kAvatarSettingsUseDefaultReactionMessage;
        const ImageSlot slot = reaction ? ImageSlot::Reaction : ImageSlot::Primary;
        const std::wstring &defaultPath = reaction ? g_defaultReactionImagePath : g_defaultPrimaryImagePath;
        try {
            PendingImage decoded;
            if (!decodePng(defaultPath.c_str(), decoded, 4096))
                throw E_INVALIDARG;
            decoded.slot = slot;
            decoded.persistSelection = false;
            {
                std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
                if (reaction) {
                    g_reactionImagePath.clear();
                    g_reactionImageLoaded = false;
                } else {
                    g_primaryImagePath.clear();
                    g_primaryImageLoaded = false;
                }
            }
            saveSetting(reaction ? L"ReactionImage" : L"PrimaryImage", L"");
            saveSetting(reaction ? L"ReactionImageDisplayName" : L"PrimaryImageDisplayName", L"");
            {
                std::lock_guard<std::mutex> lock(g_pendingImageMutex);
                g_pendingImages.push_back(std::move(decoded));
            }
            if (reaction)
                sendReactionImageState();
            else
                sendPrimaryImageState();
        } catch (...) {
            postAvatarSettingsMessage(reaction ? L"reaction-image-upload-error"
                                               : L"avatar-image-upload-error");
        }
        return 0;
    }
    case kAvatarSettingsRemovePrimaryBlinkMessage:
    case kAvatarSettingsRemoveReactionBlinkMessage: {
        const bool reaction = message == kAvatarSettingsRemoveReactionBlinkMessage;
        const ImageSlot slot = reaction ? ImageSlot::ReactionBlink : ImageSlot::PrimaryBlink;
        {
            std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
            std::wstring &path = reaction ? g_reactionBlinkImagePath : g_primaryBlinkImagePath;
            bool &loaded = reaction ? g_reactionBlinkImageLoaded : g_primaryBlinkImageLoaded;
            path.clear();
            loaded = false;
        }
        saveSetting(reaction ? L"ReactionBlinkImage" : L"PrimaryBlinkImage", L"");
        saveSetting(reaction ? L"ReactionBlinkImageDisplayName" : L"PrimaryBlinkImageDisplayName", L"");
        PendingImage clear;
        clear.slot = slot;
        clear.clearSlot = true;
        {
            std::lock_guard<std::mutex> lock(g_pendingImageMutex);
            g_pendingImages.push_back(std::move(clear));
        }
        sendBlinkImageState(slot);
        return 0;
    }
    case kAvatarSettingsPreviewReactionMessage:
        g_previewReaction.store(wParam != FALSE);
        postAvatarSettingsMessage(wParam != FALSE ? L"reaction-preview-on" : L"reaction-preview-off");
        return 0;
    case kAvatarSettingsReadyMessage:
        sendPresetState();
        sendPrimaryImageState();
        sendReactionImageState();
        sendBlinkImageState(ImageSlot::PrimaryBlink);
        sendBlinkImageState(ImageSlot::ReactionBlink);
        sendMicrophoneState();
        sendBlinkSettings();
        sendBounceSettings();
        sendBackgroundSettings();
        sendAvatarTransformSettings();
        sendLayerState();
        sendSpoutSettings();
        sendWebSocketState();
        postAvatarSettingsMessage(L"reaction-preview-off");
        return 0;
    case kAvatarSettingsWebSocketStateMessage:
        sendWebSocketState();
        return 0;
    case kAvatarSettingsUpdatePresetMessage:
        if (preset_store::updateActivePreset()) sendPresetState();
        return 0;
    case kAvatarSettingsRevertPresetMessage:
        if (preset_store::revertActivePreset()) {
            applyPresetDraftToRuntime();
            sendLayerState();
            sendPresetState();
        }
        return 0;
    case kAvatarSettingsAddLayerMessage:
        addLayerFromPicker(reinterpret_cast<HWND>(lParam));
        return 0;
    case kAvatarSettingsLayerCommandMessage: {
        std::unique_ptr<std::wstring> command(reinterpret_cast<std::wstring *>(lParam));
        if (command) handleLayerCommand(*command, wParam != 0);
        return 0;
    }
    case kAvatarSettingsPresetCommandMessage: {
        std::unique_ptr<std::wstring> command(reinterpret_cast<std::wstring *>(lParam));
        if (command) handlePresetCommand(*command);
        return 0;
    }
    case kAvatarSettingsBackgroundModeMessage:
        if (g_captureMethod.load() == 1) {
            const unsigned mode = std::clamp<unsigned>(static_cast<unsigned>(wParam), 1, 3);
            g_windowBackgroundMode.store(mode);
            saveSetting(L"WindowBackgroundMode", std::to_wstring(mode));
        } else {
            const unsigned mode = static_cast<unsigned>(wParam) == 3 ? 3 : 0;
            g_fixedBackgroundMode.store(mode);
            saveSetting(L"FixedBackgroundMode", std::to_wstring(mode));
        }
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    case kAvatarSettingsBackgroundSolidColourMessage:
    case kAvatarSettingsBackgroundChromaColourMessage: {
        std::unique_ptr<std::wstring> colour(reinterpret_cast<std::wstring *>(lParam));
        const bool chroma = message == kAvatarSettingsBackgroundChromaColourMessage;
        std::atomic<unsigned> &target = chroma ? g_backgroundChromaColour : g_backgroundSolidColour;
        target.store(parseColour(colour ? *colour : L"", target.load()));
        saveSetting(chroma ? L"BackgroundChromaColour" : L"BackgroundSolidColour",
                    colourText(target.load()));
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case kAvatarSettingsBackgroundFitMessage:
        g_backgroundFit.store(std::min<unsigned>(static_cast<unsigned>(wParam), 3));
        saveSetting(L"BackgroundFit", std::to_wstring(g_backgroundFit.load()));
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    case kAvatarSettingsCaptureMethodMessage:
        g_captureMethod.store(std::min<unsigned>(static_cast<unsigned>(wParam), 2));
        saveSetting(L"CaptureMethod", std::to_wstring(g_captureMethod.load()));
        g_transformPending.store(true);
        InvalidateRect(window, nullptr, TRUE);
        sendBackgroundSettings();
        sendSpoutSettings();
        return 0;
    case kAvatarSettingsScaleMessage:
        g_avatarScalePercent.store(
            std::clamp<unsigned>(static_cast<unsigned>(wParam), 25, 250));
        saveSetting(L"AvatarScalePercent", std::to_wstring(g_avatarScalePercent.load()));
        sendAvatarTransformSettings();
        return 0;
    case kAvatarSettingsFlipMessage:
        g_avatarFlipHorizontal.store(wParam != FALSE);
        saveSetting(L"AvatarFlipHorizontal", g_avatarFlipHorizontal.load() ? L"1" : L"0");
        return 0;
    case kAvatarSettingsSelectMicrophoneMessage: {
        std::unique_ptr<std::wstring> selected(reinterpret_cast<std::wstring *>(lParam));
        g_selectedMicrophoneId = selected && *selected != L"@default" ? *selected : L"";
        saveSetting(L"MicrophoneDevice", g_selectedMicrophoneId.empty() ? L"@default"
                                                                         : g_selectedMicrophoneId);
        if (!g_audioMonitor)
            g_audioMonitor = std::make_unique<AudioInputMonitor>();
        g_audioMonitorStatus.store(0);
        g_audioMonitorLevel.store(0);
        setMicrophoneReaction(false);
        postAvatarSettingsMessage(L"microphone-status\tConnecting…");
        postAvatarSettingsMessage(L"microphone-level\t0");
        g_audioMonitor->start(window, g_selectedMicrophoneId);
        return 0;
    }
    case kAvatarSettingsReactionThresholdMessage:
        g_reactionThreshold.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 1, 1000));
        g_noiseSensitivity.store(g_reactionThreshold.load() > g_noiseFloor.load()
                                     ? g_reactionThreshold.load() - g_noiseFloor.load()
                                     : 0);
        saveSetting(L"ReactionThreshold", std::to_wstring(g_reactionThreshold.load()));
        saveSetting(L"NoiseSensitivity", std::to_wstring(g_noiseSensitivity.load()));
        postAvatarSettingsMessage(L"noise-sensitivity\t" +
                                  std::to_wstring(g_noiseSensitivity.load()));
        processMicrophoneLevel(g_audioMonitorLevel.load());
        return 0;
    case kAvatarSettingsReleaseDelayMessage:
        g_releaseDelayMs.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 0, 5000));
        saveSetting(L"ReleaseDelayMs", std::to_wstring(g_releaseDelayMs.load()));
        return 0;
    case kAvatarSettingsBlinkEnabledMessage:
        g_blinkEnabled.store(wParam != FALSE);
        saveSetting(L"BlinkEnabled", wParam != FALSE ? L"1" : L"0");
        return 0;
    case kAvatarSettingsBlinkMinimumMessage: {
        const unsigned value = std::clamp<unsigned>(static_cast<unsigned>(wParam), 250, 30000);
        g_blinkMinimumMs.store(value);
        if (g_blinkMaximumMs.load() < value)
            g_blinkMaximumMs.store(value);
        saveSetting(L"BlinkMinimumMs", std::to_wstring(value));
        saveSetting(L"BlinkMaximumMs", std::to_wstring(g_blinkMaximumMs.load()));
        sendBlinkSettings();
        return 0;
    }
    case kAvatarSettingsBlinkMaximumMessage: {
        const unsigned value = std::clamp<unsigned>(static_cast<unsigned>(wParam), 250, 30000);
        g_blinkMaximumMs.store(value);
        if (g_blinkMinimumMs.load() > value)
            g_blinkMinimumMs.store(value);
        saveSetting(L"BlinkMaximumMs", std::to_wstring(value));
        saveSetting(L"BlinkMinimumMs", std::to_wstring(g_blinkMinimumMs.load()));
        sendBlinkSettings();
        return 0;
    }
    case kAvatarSettingsBlinkDurationMessage:
        g_blinkDurationMs.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 50, 1000));
        saveSetting(L"BlinkDurationMs", std::to_wstring(g_blinkDurationMs.load()));
        return 0;
    case kAvatarSettingsBounceEnabledMessage:
        g_bounceEnabled.store(wParam != FALSE);
        saveSetting(L"BounceEnabled", wParam != FALSE ? L"1" : L"0");
        return 0;
    case kAvatarSettingsEffectStackMessage: {
        std::unique_ptr<std::wstring> stack(reinterpret_cast<std::wstring *>(lParam));
        const std::wstring value = stack ? *stack : L"";
        {
            std::lock_guard<std::mutex> lock(g_effectStackMutex);
            g_effectStack = value;
        }
        const auto containsEffect = [&value](const std::wstring &name) {
            const std::wstring padded = L"," + value + L",";
            return padded.find(L"," + name + L",") != std::wstring::npos;
        };
        g_bounceAdded.store(containsEffect(L"bounce"));
        g_breathingAdded.store(containsEffect(L"breathing"));
        g_squashAdded.store(containsEffect(L"squash"));
        g_shakeAdded.store(containsEffect(L"shake"));
        g_brightnessAdded.store(containsEffect(L"brightness"));
        g_floatAdded.store(containsEffect(L"float"));
        g_tiltAdded.store(containsEffect(L"tilt"));
        saveSetting(L"EffectStack", value);
        if (!g_bounceAdded.load())
            g_bounceStartedAt.store(0);
        if (!g_squashAdded.load())
            g_squashStartedAt.store(0);
        if (!g_shakeAdded.load())
            g_shakePreviewUntil.store(0);
        if (!g_brightnessAdded.load())
            g_brightnessPreviewUntil.store(0);
        if (!g_floatAdded.load())
            g_floatPreviewUntil.store(0);
        if (!g_tiltAdded.load())
            g_tiltPreviewUntil.store(0);
        return 0;
    }
    case kAvatarSettingsBreathingEnabledMessage:
        g_breathingEnabled.store(wParam != FALSE);
        saveSetting(L"BreathingEnabled", wParam != FALSE ? L"1" : L"0");
        return 0;
    case kAvatarSettingsBreathingModeMessage:
        g_breathingMode.store(std::min<unsigned>(static_cast<unsigned>(wParam), 2));
        saveSetting(L"BreathingMode", std::to_wstring(g_breathingMode.load()));
        return 0;
    case kAvatarSettingsBreathingIdleMessage:
        g_breathingIdleAmount.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 0, 100));
        saveSetting(L"BreathingIdleAmount", std::to_wstring(g_breathingIdleAmount.load()));
        return 0;
    case kAvatarSettingsBreathingReactionMessage:
        g_breathingReactionAmount.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 0, 100));
        saveSetting(L"BreathingReactionAmount", std::to_wstring(g_breathingReactionAmount.load()));
        return 0;
    case kAvatarSettingsBreathingCycleMessage:
        g_breathingCycleMs.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 500, 10000));
        saveSetting(L"BreathingCycleMs", std::to_wstring(g_breathingCycleMs.load()));
        return 0;
    case kAvatarSettingsSquashEnabledMessage:
        g_squashEnabled.store(wParam != FALSE);
        saveSetting(L"SquashEnabled", g_squashEnabled.load() ? L"1" : L"0");
        return 0;
    case kAvatarSettingsSquashIntensityMessage:
        g_squashIntensity.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 0, 300));
        saveSetting(L"SquashIntensity", std::to_wstring(g_squashIntensity.load()));
        return 0;
    case kAvatarSettingsSquashDurationMessage:
        g_squashDurationMs.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 200, 1600));
        saveSetting(L"SquashDurationMs", std::to_wstring(g_squashDurationMs.load()));
        return 0;
    case kAvatarSettingsShakeEnabledMessage:
        g_shakeEnabled.store(wParam != FALSE);
        saveSetting(L"ShakeEnabled", g_shakeEnabled.load() ? L"1" : L"0");
        return 0;
    case kAvatarSettingsShakeIntensityMessage:
        g_shakeIntensity.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 0, 300));
        saveSetting(L"ShakeIntensity", std::to_wstring(g_shakeIntensity.load()));
        return 0;
    case kAvatarSettingsShakeSpeedMessage:
        g_shakeSpeed.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 8, 120));
        saveSetting(L"ShakeSpeed", std::to_wstring(g_shakeSpeed.load()));
        return 0;
    case kAvatarSettingsShakeDirectionMessage:
        g_shakeDirection.store(std::min<unsigned>(static_cast<unsigned>(wParam), 2));
        saveSetting(L"ShakeDirection", std::to_wstring(g_shakeDirection.load()));
        return 0;
    case kAvatarSettingsShakeWobbleMessage:
        g_shakeWobble.store(wParam != FALSE);
        saveSetting(L"ShakeWobble", g_shakeWobble.load() ? L"1" : L"0");
        return 0;
    case kAvatarSettingsBrightnessEnabledMessage:
        g_brightnessEnabled.store(wParam != FALSE);
        saveSetting(L"BrightnessEnabled", g_brightnessEnabled.load() ? L"1" : L"0");
        return 0;
    case kAvatarSettingsBrightnessIdleMessage:
        g_brightnessIdle.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 10, 100));
        saveSetting(L"BrightnessIdle", std::to_wstring(g_brightnessIdle.load()));
        return 0;
    case kAvatarSettingsBrightnessReactionMessage:
        g_brightnessReaction.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 100, 200));
        saveSetting(L"BrightnessReaction", std::to_wstring(g_brightnessReaction.load()));
        return 0;
    case kAvatarSettingsBrightnessTransitionMessage:
        g_brightnessTransitionMs.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 0, 2000));
        saveSetting(L"BrightnessTransitionMs", std::to_wstring(g_brightnessTransitionMs.load()));
        return 0;
    case kAvatarSettingsFloatEnabledMessage:
        g_floatEnabled.store(wParam != FALSE);
        saveSetting(L"FloatEnabled", g_floatEnabled.load() ? L"1" : L"0");
        return 0;
    case kAvatarSettingsFloatModeMessage:
        g_floatMode.store(std::min<unsigned>(static_cast<unsigned>(wParam), 2));
        saveSetting(L"FloatMode", std::to_wstring(g_floatMode.load()));
        return 0;
    case kAvatarSettingsFloatHeightMessage:
        g_floatHeightPixels.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 0, 160));
        saveSetting(L"FloatHeightPixels", std::to_wstring(g_floatHeightPixels.load()));
        return 0;
    case kAvatarSettingsFloatCycleMessage:
        g_floatCycleMs.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 1000, 12000));
        saveSetting(L"FloatCycleMs", std::to_wstring(g_floatCycleMs.load()));
        return 0;
    case kAvatarSettingsFloatDirectionMessage:
        g_floatDirection.store(std::min<unsigned>(static_cast<unsigned>(wParam), 2));
        saveSetting(L"FloatDirection", std::to_wstring(g_floatDirection.load()));
        return 0;
    case kAvatarSettingsFloatDriftMessage:
        g_floatDriftPixels.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 0, 120));
        saveSetting(L"FloatDriftPixels", std::to_wstring(g_floatDriftPixels.load()));
        return 0;
    case kAvatarSettingsTiltEnabledMessage:
        g_tiltEnabled.store(wParam != FALSE);
        saveSetting(L"TiltEnabled", g_tiltEnabled.load() ? L"1" : L"0");
        return 0;
    case kAvatarSettingsTiltAngleMessage:
        g_tiltAngleDegrees.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 0, 60));
        saveSetting(L"TiltAngleDegrees", std::to_wstring(g_tiltAngleDegrees.load()));
        return 0;
    case kAvatarSettingsTiltDirectionMessage:
        g_tiltDirection.store(std::min<unsigned>(static_cast<unsigned>(wParam), 2));
        if (g_tiltDirection.load() < 2)
            g_tiltActiveSign.store(g_tiltDirection.load() == 0 ? -1 : 1);
        saveSetting(L"TiltDirection", std::to_wstring(g_tiltDirection.load()));
        return 0;
    case kAvatarSettingsTiltTransitionMessage:
        g_tiltTransitionMs.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 0, 2000));
        saveSetting(L"TiltTransitionMs", std::to_wstring(g_tiltTransitionMs.load()));
        return 0;
    case kAvatarSettingsBounceHeightMessage:
        g_bounceHeightPixels.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 5, 160));
        saveSetting(L"BounceHeightPixels", std::to_wstring(g_bounceHeightPixels.load()));
        return 0;
    case kAvatarSettingsBounceDurationMessage:
        g_bounceDurationMs.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 100, 1200));
        saveSetting(L"BounceDurationMs", std::to_wstring(g_bounceDurationMs.load()));
        return 0;
    case kAvatarSettingsPreviewBounceMessage:
        if (g_bounceAdded.load() && g_bounceEnabled.load()) {
            g_bounceStartedAt.store(GetTickCount64());
            postAvatarSettingsMessage(L"bounce-triggered");
        }
        return 0;
    case kAvatarSettingsPreviewSquashMessage:
        if (g_squashAdded.load() && g_squashEnabled.load()) {
            g_squashStartedAt.store(GetTickCount64());
            postAvatarSettingsMessage(L"squash-triggered");
        }
        return 0;
    case kAvatarSettingsPreviewShakeMessage:
        if (g_shakeAdded.load() && g_shakeEnabled.load()) {
            g_shakePreviewUntil.store(GetTickCount64() + 2000);
            postAvatarSettingsMessage(L"shake-preview-triggered");
        }
        return 0;
    case kAvatarSettingsPreviewBrightnessMessage:
        if (g_brightnessAdded.load() && g_brightnessEnabled.load()) {
            g_brightnessPreviewUntil.store(GetTickCount64() + 2000);
            postAvatarSettingsMessage(L"brightness-preview-triggered");
        }
        return 0;
    case kAvatarSettingsPreviewFloatMessage:
        if (g_floatAdded.load() && g_floatEnabled.load()) {
            g_floatPreviewUntil.store(GetTickCount64() + std::max(2000u, g_floatCycleMs.load()));
            postAvatarSettingsMessage(L"float-preview-triggered");
        }
        return 0;
    case kAvatarSettingsPreviewTiltMessage:
        if (g_tiltAdded.load() && g_tiltEnabled.load()) {
            const int sign = selectTiltDirection();
            g_tiltActiveSign.store(sign);
            g_tiltPreviewUntil.store(GetTickCount64() + 2000);
            postAvatarSettingsMessage(L"tilt-preview-triggered\t" + std::to_wstring(sign));
        }
        return 0;
    case kAvatarSettingsCalibrateNoiseMessage:
        g_noiseCalibrationActive = true;
        g_noiseCalibrationStartedAt = GetTickCount64();
        g_noiseCalibrationSum = 0;
        g_noiseCalibrationSamples = 0;
        setMicrophoneReaction(false);
        postAvatarSettingsMessage(L"noise-calibration-started");
        return 0;
    case kAvatarSettingsNoiseSensitivityMessage:
        g_noiseSensitivity.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 0, 500));
        g_reactionThreshold.store(std::clamp<unsigned>(
            g_noiseFloor.load() + g_noiseSensitivity.load(), 1, 1000));
        saveSetting(L"NoiseSensitivity", std::to_wstring(g_noiseSensitivity.load()));
        saveSetting(L"ReactionThreshold", std::to_wstring(g_reactionThreshold.load()));
        postAvatarSettingsMessage(L"reaction-threshold\t" +
                                  std::to_wstring(g_reactionThreshold.load()));
        processMicrophoneLevel(g_audioMonitorLevel.load());
        return 0;
    case kAudioMonitorLevelMessage:
        g_audioMonitorLevel.store(static_cast<unsigned>(wParam));
        processMicrophoneLevel(static_cast<unsigned>(wParam));
        postAvatarSettingsMessage(L"microphone-level\t" + std::to_wstring(wParam));
        return 0;
    case kAudioMonitorStatusMessage:
        g_audioMonitorStatus.store(static_cast<int>(wParam));
        postAvatarSettingsMessage(wParam == 1 ? L"microphone-status\tListening"
                                               : L"microphone-status\tInput unavailable");
        if (wParam != 1)
            postAvatarSettingsMessage(L"microphone-level\t0");
        if (wParam != 1)
            setMicrophoneReaction(false);
        return 0;
    case kBlinkStateMessage:
        postAvatarSettingsMessage(wParam != FALSE ? L"blink-state-on" : L"blink-state-off");
        return 0;
    case kImageUploadFailureMessage:
        if (static_cast<ImageSlot>(wParam) == ImageSlot::Background) {
            sendBackgroundSettings();
            postAvatarSettingsMessage(L"background-image-upload-error");
        } else if (static_cast<ImageSlot>(wParam) == ImageSlot::Reaction) {
            sendReactionImageState();
            postAvatarSettingsMessage(L"reaction-image-upload-error");
        } else if (static_cast<ImageSlot>(wParam) == ImageSlot::PrimaryBlink ||
                   static_cast<ImageSlot>(wParam) == ImageSlot::ReactionBlink) {
            const ImageSlot slot = static_cast<ImageSlot>(wParam);
            sendBlinkImageState(slot);
            postAvatarSettingsMessage(std::wstring(imageMessagePrefix(slot)) + L"-upload-error");
        } else {
            sendPrimaryImageState();
            postAvatarSettingsMessage(L"avatar-image-upload-error");
        }
        MessageBoxW(window,
                    L"The PNG decoded successfully, but its GPU texture could not be created. The current avatar is unchanged.",
                    L"RearSilver Avatar Suite — PNG loading", MB_OK | MB_ICONERROR);
        return 0;
    case kImageUploadSuccessMessage:
        if (static_cast<ImageSlot>(wParam) == ImageSlot::Background) {
            std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
            setAvatarSettingsPreviewImage(4, g_backgroundImagePath);
        } else if (static_cast<ImageSlot>(wParam) == ImageSlot::Reaction) {
            std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
            setAvatarSettingsPreviewImage(static_cast<unsigned>(ImageSlot::Reaction), g_reactionImagePath);
        } else if (static_cast<ImageSlot>(wParam) == ImageSlot::PrimaryBlink) {
            std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
            setAvatarSettingsPreviewImage(2, g_primaryBlinkImagePath);
        } else if (static_cast<ImageSlot>(wParam) == ImageSlot::ReactionBlink) {
            std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
            setAvatarSettingsPreviewImage(3, g_reactionBlinkImagePath);
        } else {
            std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
            setAvatarSettingsPreviewImage(static_cast<unsigned>(ImageSlot::Primary), g_primaryImagePath);
        }
        postAvatarSettingsMessage(std::wstring(imageMessagePrefix(static_cast<ImageSlot>(wParam))) +
                                  L"-uploaded");
        postAvatarSettingsMessage(L"preset-dirty\t1");
        return 0;
    case kSpoutStatusChangedMessage:
        sendSpoutSettings();
        return 0;
    case kRenderFailureMessage: {
        g_running.store(false);
        wchar_t messageText[256]{};
        swprintf_s(messageText,
                   L"RearSilver Avatar Suite encountered a graphics error (0x%08X) and must close.",
                   static_cast<unsigned>(wParam));
        MessageBoxW(window, messageText, L"RearSilver Avatar Suite", MB_OK | MB_ICONERROR);
        DestroyWindow(window);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        g_running.store(false);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

LRESULT CALLBACK splashWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_TIMER) {
        KillTimer(window, 1);
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

HWND showSplashWindow(HINSTANCE instance)
{
    constexpr wchar_t splashClass[] = L"RearSilverAvatarSuiteSplashWindow";
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = splashWindowProcedure;
    windowClass.lpszClassName = splashClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&windowClass);

    constexpr UINT targetWidth = 720;
    constexpr UINT targetHeight = 558;
    const std::wstring path = executableDirectory() + L"\\splash.png";
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), targetWidth, targetHeight,
                                  WICBitmapInterpolationModeFant)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return nullptr;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = targetWidth;
    info.bmiHeader.biHeight = -static_cast<LONG>(targetHeight);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *pixels = nullptr;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!screen || !memory || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
        return nullptr;
    }
    if (FAILED(converter->CopyPixels(nullptr, targetWidth * 4, targetWidth * targetHeight * 4,
                                     static_cast<BYTE *>(pixels)))) {
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    POINT destination{workArea.left + (workArea.right - workArea.left - static_cast<LONG>(targetWidth)) / 2,
                      workArea.top + (workArea.bottom - workArea.top - static_cast<LONG>(targetHeight)) / 2};
    SIZE size{static_cast<LONG>(targetWidth), static_cast<LONG>(targetHeight)};
    POINT source{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    HWND splash = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                  splashClass, L"RearSilver Avatar Suite", WS_POPUP,
                                  destination.x, destination.y, targetWidth, targetHeight,
                                  nullptr, nullptr, instance, nullptr);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    if (splash)
        UpdateLayeredWindow(splash, screen, &destination, &size, memory, &source, 0, &blend, ULW_ALPHA);
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    if (splash) {
        ShowWindow(splash, SW_SHOWNOACTIVATE);
        SetTimer(splash, 1, 1400, nullptr);
    }
    return splash;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    HANDLE singleInstance = CreateMutexW(nullptr, FALSE, kSingleInstanceMutex);
    if (singleInstance && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kWindowClass, kWindowTitle)) {
            if (IsIconic(existing))
                ShowWindow(existing, SW_RESTORE);
            else
                ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        CloseHandle(singleInstance);
        return 0;
    }

    startLog();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) {
        if (singleInstance)
            CloseHandle(singleInstance);
        return 1;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    showSplashWindow(instance);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.lpszClassName = kWindowClass;
    windowClass.style = CS_DBLCLKS;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_REARSILVER_AVATAR_SUITE));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance,
        MAKEINTRESOURCEW(IDI_REARSILVER_AVATAR_SUITE), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    windowClass.hbrBackground = nullptr;
    if (!RegisterClassExW(&windowClass)) {
        CoUninitialize();
        if (singleInstance)
            CloseHandle(singleInstance);
        return 2;
    }

    RECT initialBounds{0, 0, 960, 540};
    AdjustWindowRectExForDpi(&initialBounds, WS_OVERLAPPEDWINDOW, FALSE, 0,
                             GetDpiForSystem());
    g_mainWindow = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                                   CW_USEDEFAULT, CW_USEDEFAULT,
                                   initialBounds.right - initialBounds.left,
                                   initialBounds.bottom - initialBounds.top, nullptr, nullptr,
                                   instance, nullptr);
    if (!g_mainWindow) {
        CoUninitialize();
        if (singleInstance)
            CloseHandle(singleInstance);
        return 3;
    }
    RECT client{};
    GetClientRect(g_mainWindow, &client);
    g_clientWidth.store(std::max<LONG>(1, client.right));
    g_clientHeight.store(std::max<LONG>(1, client.bottom));

    ShowWindow(g_mainWindow, showCommand);
    UpdateWindow(g_mainWindow);

    const std::wstring assetsDirectory = executableDirectory();
    g_defaultPrimaryImagePath = assetsDirectory + L"\\default-avatar-idle.png";
    g_defaultReactionImagePath = assetsDirectory + L"\\default-avatar-reaction.png";
    auto queueBundledDefault = [](const std::wstring &path, ImageSlot slot) {
        PendingImage decoded;
        if (!decodePng(path.c_str(), decoded, 4096))
            throw E_INVALIDARG;
        decoded.slot = slot;
        decoded.persistSelection = false;
        std::lock_guard<std::mutex> lock(g_pendingImageMutex);
        g_pendingImages.push_back(std::move(decoded));
    };

    const std::wstring savedPrimaryImage = loadSetting(L"PrimaryImage");
    bool primaryQueued = false;
    if (!savedPrimaryImage.empty()) {
        {
            std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
            g_primaryImagePath = savedPrimaryImage;
            g_primaryImageLoaded = false;
        }
        try {
            PendingImage decoded;
            if (!decodePng(savedPrimaryImage.c_str(), decoded))
                throw E_INVALIDARG;
            std::lock_guard<std::mutex> lock(g_pendingImageMutex);
            g_pendingImages.push_back(std::move(decoded));
            primaryQueued = true;
            logMessage(L"Saved primary avatar queued for startup restore: " + savedPrimaryImage);
        } catch (...) {
            logMessage(L"Saved primary avatar is unavailable; using built-in default: " + savedPrimaryImage);
        }
    }
    if (!primaryQueued) {
        try {
            queueBundledDefault(g_defaultPrimaryImagePath, ImageSlot::Primary);
            logMessage(L"Bundled primary mascot queued for startup.");
        } catch (...) {
            logMessage(L"Bundled primary mascot is unavailable; using emergency placeholder.");
        }
    }

    const std::wstring savedReactionImage = loadSetting(L"ReactionImage");
    bool reactionQueued = false;
    if (!savedReactionImage.empty()) {
        {
            std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
            g_reactionImagePath = savedReactionImage;
            g_reactionImageLoaded = false;
        }
        try {
            PendingImage decoded;
            if (!decodePng(savedReactionImage.c_str(), decoded))
                throw E_INVALIDARG;
            decoded.slot = ImageSlot::Reaction;
            std::lock_guard<std::mutex> lock(g_pendingImageMutex);
            g_pendingImages.push_back(std::move(decoded));
            reactionQueued = true;
            logMessage(L"Saved reaction avatar queued for startup restore: " + savedReactionImage);
        } catch (...) {
            logMessage(L"Saved reaction avatar is unavailable: " + savedReactionImage);
        }
    }
    if (!reactionQueued) {
        try {
            queueBundledDefault(g_defaultReactionImagePath, ImageSlot::Reaction);
            logMessage(L"Bundled reaction mascot queued for startup.");
        } catch (...) {
            logMessage(L"Bundled reaction mascot is unavailable.");
        }
    }

    auto queueSavedBlinkImage = [](const wchar_t *settingKey, ImageSlot slot,
                                   std::wstring &statePath, bool &stateLoaded) {
        const std::wstring savedPath = loadSetting(settingKey);
        if (savedPath.empty())
            return;
        {
            std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
            statePath = savedPath;
            stateLoaded = false;
        }
        try {
            PendingImage decoded;
            if (!decodePng(savedPath.c_str(), decoded))
                throw E_INVALIDARG;
            decoded.slot = slot;
            std::lock_guard<std::mutex> lock(g_pendingImageMutex);
            g_pendingImages.push_back(std::move(decoded));
            logMessage(L"Saved blink avatar queued for startup restore: " + savedPath);
        } catch (...) {
            logMessage(L"Saved blink avatar is unavailable: " + savedPath);
        }
    };
    queueSavedBlinkImage(L"PrimaryBlinkImage", ImageSlot::PrimaryBlink,
                         g_primaryBlinkImagePath, g_primaryBlinkImageLoaded);
    queueSavedBlinkImage(L"ReactionBlinkImage", ImageSlot::ReactionBlink,
                         g_reactionBlinkImagePath, g_reactionBlinkImageLoaded);

    const std::wstring savedBackgroundImage = loadSetting(L"BackgroundImage");
    if (!savedBackgroundImage.empty()) {
        {
            std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
            g_backgroundImagePath = savedBackgroundImage;
            g_backgroundImageLoaded = false;
        }
        try {
            PendingImage decoded;
            if (!decodePng(savedBackgroundImage.c_str(), decoded))
                throw E_INVALIDARG;
            decoded.slot = ImageSlot::Background;
            cacheWindowBackground(decoded);
            std::lock_guard<std::mutex> lock(g_pendingImageMutex);
            g_pendingImages.push_back(std::move(decoded));
            logMessage(L"Saved background image queued for startup restore: " + savedBackgroundImage);
        } catch (...) {
            logMessage(L"Saved background image is unavailable: " + savedBackgroundImage);
        }
    }

    const std::wstring savedCaptureMethod = loadSetting(L"CaptureMethod");
    if (!savedCaptureMethod.empty())
        g_captureMethod.store(std::min<unsigned>(wcstoul(savedCaptureMethod.c_str(), nullptr, 10), 2));
    const std::wstring savedWindowBackgroundMode = loadSetting(L"WindowBackgroundMode");
    if (!savedWindowBackgroundMode.empty())
        g_windowBackgroundMode.store(std::clamp<unsigned>(
            wcstoul(savedWindowBackgroundMode.c_str(), nullptr, 10), 1, 3));
    const std::wstring savedFixedBackgroundMode = loadSetting(L"FixedBackgroundMode");
    if (!savedFixedBackgroundMode.empty())
        g_fixedBackgroundMode.store(wcstoul(savedFixedBackgroundMode.c_str(), nullptr, 10) == 3 ? 3 : 0);
    const std::wstring savedBackgroundSolid = loadSetting(L"BackgroundSolidColour");
    if (!savedBackgroundSolid.empty())
        g_backgroundSolidColour.store(parseColour(savedBackgroundSolid, 0xffffff));
    const std::wstring savedBackgroundChroma = loadSetting(L"BackgroundChromaColour");
    if (!savedBackgroundChroma.empty())
        g_backgroundChromaColour.store(parseColour(savedBackgroundChroma, 0x00ff00));
    const std::wstring savedBackgroundFit = loadSetting(L"BackgroundFit");
    if (!savedBackgroundFit.empty())
        g_backgroundFit.store(std::min<unsigned>(wcstoul(savedBackgroundFit.c_str(), nullptr, 10), 3));
    const std::wstring savedAvatarScale = loadSetting(L"AvatarScalePercent");
    if (!savedAvatarScale.empty())
        g_avatarScalePercent.store(std::clamp<unsigned>(
            wcstoul(savedAvatarScale.c_str(), nullptr, 10), 25, 250));
    InvalidateRect(g_mainWindow, nullptr, TRUE);

    const std::wstring savedMicrophone = loadSetting(L"MicrophoneDevice");
    g_selectedMicrophoneId = savedMicrophone.empty() || savedMicrophone == L"@default"
                                 ? L""
                                 : savedMicrophone;
    g_audioMonitor = std::make_unique<AudioInputMonitor>();
    const std::wstring savedReactionsEnabled = loadSetting(L"ReactionsEnabled");
    if (!savedReactionsEnabled.empty())
        g_reactionsEnabled.store(savedReactionsEnabled != L"0");

    const std::wstring savedThreshold = loadSetting(L"ReactionThreshold");
    if (!savedThreshold.empty())
        g_reactionThreshold.store(std::clamp<unsigned>(wcstoul(savedThreshold.c_str(), nullptr, 10),
                                                       1, 1000));
    const std::wstring savedRelease = loadSetting(L"ReleaseDelayMs");
    if (!savedRelease.empty())
        g_releaseDelayMs.store(std::clamp<unsigned>(wcstoul(savedRelease.c_str(), nullptr, 10),
                                                    0, 5000));
    const std::wstring savedNoiseFloor = loadSetting(L"NoiseFloor");
    if (!savedNoiseFloor.empty())
        g_noiseFloor.store(std::clamp<unsigned>(wcstoul(savedNoiseFloor.c_str(), nullptr, 10),
                                                0, 950));
    const std::wstring savedNoiseSensitivity = loadSetting(L"NoiseSensitivity");
    if (!savedNoiseSensitivity.empty())
        g_noiseSensitivity.store(std::clamp<unsigned>(
            wcstoul(savedNoiseSensitivity.c_str(), nullptr, 10), 0, 500));
    const std::wstring savedBlinkEnabled = loadSetting(L"BlinkEnabled");
    if (!savedBlinkEnabled.empty())
        g_blinkEnabled.store(savedBlinkEnabled != L"0");
    const std::wstring savedBlinkMinimum = loadSetting(L"BlinkMinimumMs");
    if (!savedBlinkMinimum.empty())
        g_blinkMinimumMs.store(std::clamp<unsigned>(wcstoul(savedBlinkMinimum.c_str(), nullptr, 10),
                                                    250, 30000));
    const std::wstring savedBlinkMaximum = loadSetting(L"BlinkMaximumMs");
    if (!savedBlinkMaximum.empty())
        g_blinkMaximumMs.store(std::clamp<unsigned>(wcstoul(savedBlinkMaximum.c_str(), nullptr, 10),
                                                    g_blinkMinimumMs.load(), 30000));
    const std::wstring savedBlinkDuration = loadSetting(L"BlinkDurationMs");
    if (!savedBlinkDuration.empty())
        g_blinkDurationMs.store(std::clamp<unsigned>(wcstoul(savedBlinkDuration.c_str(), nullptr, 10),
                                                     50, 1000));
    const std::wstring savedBounceEnabled = loadSetting(L"BounceEnabled");
    if (!savedBounceEnabled.empty())
        g_bounceEnabled.store(savedBounceEnabled != L"0");
    const std::wstring savedBounceHeight = loadSetting(L"BounceHeightPixels");
    if (!savedBounceHeight.empty())
        g_bounceHeightPixels.store(std::clamp<unsigned>(wcstoul(savedBounceHeight.c_str(), nullptr, 10),
                                                        5, 160));
    const std::wstring savedBounceDuration = loadSetting(L"BounceDurationMs");
    if (!savedBounceDuration.empty())
        g_bounceDurationMs.store(std::clamp<unsigned>(wcstoul(savedBounceDuration.c_str(), nullptr, 10),
                                                      100, 1200));
    const std::wstring savedEffectStack = loadSetting(L"EffectStack");
    if (settingExists(L"EffectStack")) {
        std::lock_guard<std::mutex> lock(g_effectStackMutex);
        g_effectStack = savedEffectStack;
        const std::wstring padded = L"," + savedEffectStack + L",";
        g_bounceAdded.store(padded.find(L",bounce,") != std::wstring::npos);
        g_breathingAdded.store(padded.find(L",breathing,") != std::wstring::npos);
        g_squashAdded.store(padded.find(L",squash,") != std::wstring::npos);
        g_shakeAdded.store(padded.find(L",shake,") != std::wstring::npos);
        g_brightnessAdded.store(padded.find(L",brightness,") != std::wstring::npos);
        g_floatAdded.store(padded.find(L",float,") != std::wstring::npos);
        g_tiltAdded.store(padded.find(L",tilt,") != std::wstring::npos);
    }
    const std::wstring savedBreathingEnabled = loadSetting(L"BreathingEnabled");
    if (!savedBreathingEnabled.empty())
        g_breathingEnabled.store(savedBreathingEnabled != L"0");
    const std::wstring savedBreathingMode = loadSetting(L"BreathingMode");
    if (!savedBreathingMode.empty())
        g_breathingMode.store(std::min<unsigned>(wcstoul(savedBreathingMode.c_str(), nullptr, 10), 2));
    const std::wstring savedBreathingIdle = loadSetting(L"BreathingIdleAmount");
    if (!savedBreathingIdle.empty())
        g_breathingIdleAmount.store(std::clamp<unsigned>(wcstoul(savedBreathingIdle.c_str(), nullptr, 10), 0, 100));
    const std::wstring savedBreathingReaction = loadSetting(L"BreathingReactionAmount");
    if (!savedBreathingReaction.empty())
        g_breathingReactionAmount.store(std::clamp<unsigned>(wcstoul(savedBreathingReaction.c_str(), nullptr, 10), 0, 100));
    const std::wstring savedBreathingCycle = loadSetting(L"BreathingCycleMs");
    if (!savedBreathingCycle.empty())
        g_breathingCycleMs.store(std::clamp<unsigned>(wcstoul(savedBreathingCycle.c_str(), nullptr, 10), 500, 10000));
    const std::wstring savedSquashEnabled = loadSetting(L"SquashEnabled");
    if (!savedSquashEnabled.empty())
        g_squashEnabled.store(savedSquashEnabled != L"0");
    const std::wstring savedSquashIntensity = loadSetting(L"SquashIntensity");
    if (!savedSquashIntensity.empty())
        g_squashIntensity.store(std::clamp<unsigned>(wcstoul(savedSquashIntensity.c_str(), nullptr, 10), 0, 300));
    const std::wstring savedSquashDuration = loadSetting(L"SquashDurationMs");
    if (!savedSquashDuration.empty())
        g_squashDurationMs.store(std::clamp<unsigned>(wcstoul(savedSquashDuration.c_str(), nullptr, 10), 200, 1600));
    const std::wstring savedShakeEnabled = loadSetting(L"ShakeEnabled");
    if (!savedShakeEnabled.empty())
        g_shakeEnabled.store(savedShakeEnabled != L"0");
    const std::wstring savedShakeIntensity = loadSetting(L"ShakeIntensity");
    if (!savedShakeIntensity.empty())
        g_shakeIntensity.store(std::clamp<unsigned>(wcstoul(savedShakeIntensity.c_str(), nullptr, 10), 0, 300));
    const std::wstring savedShakeSpeed = loadSetting(L"ShakeSpeed");
    if (!savedShakeSpeed.empty())
        g_shakeSpeed.store(std::clamp<unsigned>(wcstoul(savedShakeSpeed.c_str(), nullptr, 10), 8, 120));
    const std::wstring savedShakeDirection = loadSetting(L"ShakeDirection");
    if (!savedShakeDirection.empty())
        g_shakeDirection.store(std::min<unsigned>(wcstoul(savedShakeDirection.c_str(), nullptr, 10), 2));
    const std::wstring savedShakeWobble = loadSetting(L"ShakeWobble");
    if (!savedShakeWobble.empty())
        g_shakeWobble.store(savedShakeWobble != L"0");
    const std::wstring savedBrightnessEnabled = loadSetting(L"BrightnessEnabled");
    if (!savedBrightnessEnabled.empty())
        g_brightnessEnabled.store(savedBrightnessEnabled != L"0");
    const std::wstring savedBrightnessIdle = loadSetting(L"BrightnessIdle");
    if (!savedBrightnessIdle.empty())
        g_brightnessIdle.store(std::clamp<unsigned>(wcstoul(savedBrightnessIdle.c_str(), nullptr, 10), 10, 100));
    const std::wstring savedBrightnessReaction = loadSetting(L"BrightnessReaction");
    if (!savedBrightnessReaction.empty())
        g_brightnessReaction.store(std::clamp<unsigned>(wcstoul(savedBrightnessReaction.c_str(), nullptr, 10), 100, 200));
    const std::wstring savedBrightnessTransition = loadSetting(L"BrightnessTransitionMs");
    if (!savedBrightnessTransition.empty())
        g_brightnessTransitionMs.store(std::clamp<unsigned>(wcstoul(savedBrightnessTransition.c_str(), nullptr, 10), 0, 2000));
    const std::wstring savedFloatEnabled = loadSetting(L"FloatEnabled");
    if (!savedFloatEnabled.empty())
        g_floatEnabled.store(savedFloatEnabled != L"0");
    const std::wstring savedFloatMode = loadSetting(L"FloatMode");
    if (!savedFloatMode.empty())
        g_floatMode.store(std::min<unsigned>(wcstoul(savedFloatMode.c_str(), nullptr, 10), 2));
    std::wstring savedFloatHeight = loadSetting(L"FloatHeightPixels");
    if (savedFloatHeight.empty())
        savedFloatHeight = loadSetting(L"FloatIdlePixels");
    if (!savedFloatHeight.empty())
        g_floatHeightPixels.store(std::clamp<unsigned>(wcstoul(savedFloatHeight.c_str(), nullptr, 10), 0, 160));
    const std::wstring savedFloatCycle = loadSetting(L"FloatCycleMs");
    if (!savedFloatCycle.empty())
        g_floatCycleMs.store(std::clamp<unsigned>(wcstoul(savedFloatCycle.c_str(), nullptr, 10), 1000, 12000));
    const std::wstring savedFloatDirection = loadSetting(L"FloatDirection");
    if (!savedFloatDirection.empty())
        g_floatDirection.store(std::min<unsigned>(wcstoul(savedFloatDirection.c_str(), nullptr, 10), 2));
    const std::wstring savedFloatDrift = loadSetting(L"FloatDriftPixels");
    if (!savedFloatDrift.empty())
        g_floatDriftPixels.store(std::clamp<unsigned>(wcstoul(savedFloatDrift.c_str(), nullptr, 10), 0, 120));
    const std::wstring savedTiltEnabled = loadSetting(L"TiltEnabled");
    if (!savedTiltEnabled.empty())
        g_tiltEnabled.store(savedTiltEnabled != L"0");
    const std::wstring savedTiltAngle = loadSetting(L"TiltAngleDegrees");
    if (!savedTiltAngle.empty())
        g_tiltAngleDegrees.store(std::clamp<unsigned>(wcstoul(savedTiltAngle.c_str(), nullptr, 10), 0, 60));
    const std::wstring savedTiltDirection = loadSetting(L"TiltDirection");
    if (!savedTiltDirection.empty())
        g_tiltDirection.store(std::min<unsigned>(wcstoul(savedTiltDirection.c_str(), nullptr, 10), 2));
    if (g_tiltDirection.load() < 2)
        g_tiltActiveSign.store(g_tiltDirection.load() == 0 ? -1 : 1);
    const std::wstring savedTiltTransition = loadSetting(L"TiltTransitionMs");
    if (!savedTiltTransition.empty())
        g_tiltTransitionMs.store(std::clamp<unsigned>(wcstoul(savedTiltTransition.c_str(), nullptr, 10), 0, 2000));
    reloadDraftLayers();
    g_audioMonitor->start(g_mainWindow, g_selectedMicrophoneId);

    g_webSocketServer = std::make_unique<WebSocketServer>();
    if (g_webSocketServer->start(17891, [](const std::string &payload) {
            WebSocketCommandRequest request{payload, {}};
            if (!g_mainWindow || !IsWindow(g_mainWindow) ||
                !SendMessageW(g_mainWindow, kWebSocketCommandMessage, 0,
                              reinterpret_cast<LPARAM>(&request))) {
                if (request.response.empty())
                    request.response = "{\"ok\":false,\"error\":\"Avatar Suite is shutting down\"}";
            }
            return request.response;
        })) {
        logMessage(L"WebSocket server could not bind to 127.0.0.1:17891.");
    } else logMessage(L"WebSocket server listening on ws://127.0.0.1:17891.");

    g_running.store(true);
    std::thread renderThread(renderThreadMain);

    if (isProcessRunning(L"RTSS.exe")) {
        g_dialogOpen.store(true);
        MessageBoxW(g_mainWindow,
                    L"RivaTuner Statistics Server is running. If OBS Game Capture is blank or frozen, open RTSS Setup and enable ‘Use Microsoft Detours API hooking’.",
                    L"RearSilver Avatar Suite — OBS compatibility", MB_OK | MB_ICONINFORMATION);
        g_dialogOpen.store(false);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    g_running.store(false);
    if (g_webSocketServer)
        g_webSocketServer->stop();
    if (g_audioMonitor)
        g_audioMonitor->stop();
    if (renderThread.joinable())
        renderThread.join();
    shutdownAvatarSettingsWindow();
    if (g_logFile != INVALID_HANDLE_VALUE)
        CloseHandle(g_logFile);
    CoUninitialize();
    if (singleInstance)
        CloseHandle(singleInstance);
    return static_cast<int>(message.wParam);
}
