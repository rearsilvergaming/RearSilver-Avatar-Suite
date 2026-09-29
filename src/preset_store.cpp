#include "preset_store.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <vector>

namespace preset_store {
namespace {

constexpr wchar_t kDefaultPresetId[] = L"default";
constexpr wchar_t kDefaultPresetName[] = L"Default Avatar";
constexpr std::array<std::wstring_view, 47> kPresetKeys{
    L"PrimaryImage", L"ReactionImage", L"PrimaryBlinkImage", L"ReactionBlinkImage",
    L"AvatarScalePercent", L"BlinkEnabled", L"BlinkMinimumMs", L"BlinkMaximumMs",
    L"BlinkDurationMs", L"EffectStack", L"BounceEnabled", L"BounceHeightPixels",
    L"BounceDurationMs", L"BreathingEnabled", L"BreathingMode", L"BreathingIdleAmount",
    L"BreathingReactionAmount", L"BreathingCycleMs", L"SquashEnabled", L"SquashIntensity",
    L"SquashDurationMs", L"ShakeEnabled", L"ShakeIntensity", L"ShakeSpeed",
    L"ShakeDirection", L"ShakeWobble", L"BrightnessEnabled", L"BrightnessIdle",
    L"BrightnessReaction", L"BrightnessTransitionMs", L"FloatEnabled", L"FloatMode",
    L"FloatHeightPixels", L"FloatCycleMs", L"FloatDirection", L"FloatDriftPixels",
    L"TiltEnabled", L"TiltAngleDegrees", L"TiltDirection", L"TiltTransitionMs",
    L"LayerCount", L"LayerOrder", L"BackgroundImage", L"BackgroundFit",
    L"FixedBackgroundMode", L"WindowBackgroundMode", L"ReactionsEnabled"
};

std::mutex g_mutex;
std::filesystem::path g_root;
std::wstring g_activeId = kDefaultPresetId;
bool g_initialised = false;

void ensureUnicodeFile(const std::filesystem::path &path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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

std::filesystem::path indexPath() { return g_root / L"presets.ini"; }
std::filesystem::path presetPath() { return g_root / L"presets" / (g_activeId + L".ini"); }
std::filesystem::path draftPath() { return g_root / L"drafts" / (g_activeId + L".ini"); }

std::wstring readValue(const std::filesystem::path &path, const wchar_t *section,
                       const wchar_t *key, const wchar_t *fallback = L"")
{
    wchar_t value[32768]{};
    GetPrivateProfileStringW(section, key, fallback, value,
                             static_cast<DWORD>(std::size(value)), path.c_str());
    return value;
}

bool writeValue(const std::filesystem::path &path, const wchar_t *section,
                const wchar_t *key, const std::wstring &value)
{
    ensureUnicodeFile(path);
    return WritePrivateProfileStringW(section, key, value.c_str(), path.c_str()) != FALSE;
}

bool filesEqual(const std::filesystem::path &left, const std::filesystem::path &right)
{
    std::error_code error;
    if (!std::filesystem::exists(left, error) || !std::filesystem::exists(right, error))
        return false;
    if (std::filesystem::file_size(left, error) != std::filesystem::file_size(right, error))
        return false;
    HANDLE a = CreateFileW(left.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE b = CreateFileW(right.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (a == INVALID_HANDLE_VALUE || b == INVALID_HANDLE_VALUE) {
        if (a != INVALID_HANDLE_VALUE) CloseHandle(a);
        if (b != INVALID_HANDLE_VALUE) CloseHandle(b);
        return false;
    }
    std::array<unsigned char, 8192> leftBytes{}, rightBytes{};
    bool equal = true;
    for (;;) {
        DWORD leftRead = 0, rightRead = 0;
        if (!ReadFile(a, leftBytes.data(), static_cast<DWORD>(leftBytes.size()), &leftRead, nullptr) ||
            !ReadFile(b, rightBytes.data(), static_cast<DWORD>(rightBytes.size()), &rightRead, nullptr) ||
            leftRead != rightRead || memcmp(leftBytes.data(), rightBytes.data(), leftRead) != 0) {
            equal = false;
            break;
        }
        if (!leftRead) break;
    }
    CloseHandle(a);
    CloseHandle(b);
    return equal;
}

bool copyAtomically(const std::filesystem::path &source, const std::filesystem::path &destination)
{
    const std::filesystem::path temporary = destination.wstring() + L".tmp";
    DeleteFileW(temporary.c_str());
    if (!CopyFileW(source.c_str(), temporary.c_str(), FALSE)) return false;
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

std::wstring sha256(const std::wstring &path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0, resultBytes = 0;
    std::vector<unsigned char> object;
    std::array<unsigned char, 32> digest{};
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
              BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
                                &resultBytes, 0) >= 0;
    if (ok) {
        object.resize(objectBytes);
        ok = BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) >= 0;
    }
    std::array<unsigned char, 65536> buffer{};
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ok = false;
            break;
        }
        if (!read) break;
        ok = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    if (!ok) return {};
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(64);
    for (unsigned char byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}

} // namespace

bool initialise(const std::wstring &legacySettingsPath)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialised) return true;
    wchar_t localAppData[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData,
                                                  static_cast<DWORD>(std::size(localAppData)));
    if (!length || length >= std::size(localAppData)) return false;
    g_root = std::filesystem::path(std::wstring(localAppData, length)) / L"RearSilver Avatar";
    std::error_code error;
    std::filesystem::create_directories(g_root / L"presets", error);
    std::filesystem::create_directories(g_root / L"drafts", error);
    std::filesystem::create_directories(g_root / L"assets", error);
    if (error) return false;

    ensureUnicodeFile(indexPath());
    g_activeId = readValue(indexPath(), L"Presets", L"ActiveId", kDefaultPresetId);
    if (g_activeId.empty()) g_activeId = kDefaultPresetId;
    writeValue(indexPath(), L"Presets", L"ActiveId", g_activeId);
    writeValue(indexPath(), L"Preset.default", L"Name", kDefaultPresetName);

    if (!std::filesystem::exists(presetPath(), error)) {
        ensureUnicodeFile(presetPath());
        for (const std::wstring_view key : kPresetKeys) {
            const std::wstring value = readValue(legacySettingsPath, L"Avatar",
                                                 std::wstring(key).c_str());
            if (!value.empty()) writeValue(presetPath(), L"Preset", std::wstring(key).c_str(), value);
        }
    }
    if (!std::filesystem::exists(draftPath(), error))
        CopyFileW(presetPath().c_str(), draftPath().c_str(), FALSE);
    g_initialised = true;
    return true;
}

std::wstring activePresetId() { std::lock_guard<std::mutex> lock(g_mutex); return g_activeId; }
std::wstring activePresetName()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::wstring section = L"Preset." + g_activeId;
    return readValue(indexPath(), section.c_str(), L"Name", kDefaultPresetName);
}
std::wstring loadDraftValue(const wchar_t *key)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_initialised ? readValue(draftPath(), L"Preset", key) : L"";
}
bool saveDraftValue(const wchar_t *key, const std::wstring &value)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_initialised && writeValue(draftPath(), L"Preset", key, value);
}
bool updateActivePreset()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_initialised && copyAtomically(draftPath(), presetPath());
}
bool revertActivePreset()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_initialised && copyAtomically(presetPath(), draftPath());
}
bool hasUnsavedChanges()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_initialised && !filesEqual(presetPath(), draftPath());
}
bool isPresetScopedKey(const wchar_t *key)
{
    if (!key) return false;
    return std::find(kPresetKeys.begin(), kPresetKeys.end(), std::wstring_view(key)) != kPresetKeys.end() ||
           wcsncmp(key, L"Layer.", 6) == 0;
}
std::wstring importPngAsset(const std::wstring &sourcePath)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialised || _wcsicmp(std::filesystem::path(sourcePath).extension().c_str(), L".png") != 0)
        return {};
    const std::wstring digest = sha256(sourcePath);
    if (digest.empty()) return {};
    const std::filesystem::path destination = g_root / L"assets" / (digest + L".png");
    std::error_code error;
    if (!std::filesystem::exists(destination, error) &&
        !CopyFileW(sourcePath.c_str(), destination.c_str(), TRUE)) return {};
    return destination.wstring();
}

} // namespace preset_store
