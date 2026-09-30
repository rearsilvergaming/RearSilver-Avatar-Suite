#include "preset_store.h"

#include <windows.h>
#include <objbase.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string_view>
#include <vector>

namespace preset_store {
namespace {

constexpr wchar_t kDefaultPresetId[] = L"default";
constexpr wchar_t kDefaultPresetName[] = L"Default Avatar";
constexpr std::array<std::wstring_view, 55> kPresetKeys{
    L"PrimaryImage", L"ReactionImage", L"PrimaryBlinkImage", L"ReactionBlinkImage",
    L"PrimaryImageDisplayName", L"ReactionImageDisplayName", L"PrimaryBlinkImageDisplayName",
    L"ReactionBlinkImageDisplayName", L"BackgroundImageDisplayName",
    L"AvatarScalePercent", L"AvatarFlipHorizontal", L"BlinkEnabled", L"BlinkMinimumMs", L"BlinkMaximumMs",
    L"BlinkDurationMs", L"EffectStack", L"BounceEnabled", L"BounceHeightPixels",
    L"BounceDurationMs", L"BreathingEnabled", L"BreathingMode", L"BreathingIdleAmount",
    L"BreathingReactionAmount", L"BreathingCycleMs", L"SquashEnabled", L"SquashIntensity",
    L"SquashDurationMs", L"ShakeEnabled", L"ShakeIntensity", L"ShakeSpeed",
    L"ShakeDirection", L"ShakeWobble", L"BrightnessEnabled", L"BrightnessIdle",
    L"BrightnessReaction", L"BrightnessTransitionMs", L"FloatEnabled", L"FloatMode",
    L"FloatHeightPixels", L"FloatCycleMs", L"FloatDirection", L"FloatDriftPixels",
    L"TiltEnabled", L"TiltAngleDegrees", L"TiltDirection", L"TiltTransitionMs",
    L"LayerCount", L"LayerOrder", L"CompositionOrder", L"GroupOrder", L"BackgroundImage", L"BackgroundFit",
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
std::filesystem::path presetPathFor(const std::wstring &id) { return g_root / L"presets" / (id + L".ini"); }
std::filesystem::path draftPathFor(const std::wstring &id) { return g_root / L"drafts" / (id + L".ini"); }
std::filesystem::path presetPath() { return presetPathFor(g_activeId); }
std::filesystem::path draftPath() { return draftPathFor(g_activeId); }

std::vector<std::wstring> splitOrder(const std::wstring &value)
{
    std::vector<std::wstring> result;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find(L',', start);
        const std::wstring item = value.substr(start, end == std::wstring::npos ? end : end - start);
        if (!item.empty() && std::find(result.begin(), result.end(), item) == result.end()) result.push_back(item);
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return result;
}

std::wstring joinOrder(const std::vector<std::wstring> &ids)
{
    std::wstring result;
    for (const auto &id : ids) { if (!result.empty()) result += L','; result += id; }
    return result;
}

std::wstring cleanName(std::wstring name)
{
    for (wchar_t &character : name) if (character == L'\t' || character == L'\r' || character == L'\n') character = L' ';
    const auto first = name.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    const auto last = name.find_last_not_of(L" \t");
    name = name.substr(first, last - first + 1);
    if (name.size() > 80) name.resize(80);
    return name;
}

std::wstring makeId()
{
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return {};
    wchar_t value[40]{};
    swprintf_s(value, L"%08lx%04x%04x%04x%012llx", guid.Data1, guid.Data2, guid.Data3,
               (static_cast<unsigned>(guid.Data4[0]) << 8) | guid.Data4[1],
               (static_cast<unsigned long long>(guid.Data4[2]) << 40) |
               (static_cast<unsigned long long>(guid.Data4[3]) << 32) |
               (static_cast<unsigned long long>(guid.Data4[4]) << 24) |
               (static_cast<unsigned long long>(guid.Data4[5]) << 16) |
               (static_cast<unsigned long long>(guid.Data4[6]) << 8) | guid.Data4[7]);
    return value;
}

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
    std::vector<std::wstring> order = splitOrder(readValue(indexPath(), L"Presets", L"Order"));
    if (order.empty()) order.push_back(kDefaultPresetId);
    if (std::find(order.begin(), order.end(), g_activeId) == order.end()) order.push_back(g_activeId);
    writeValue(indexPath(), L"Presets", L"Order", joinOrder(order));
    if (readValue(indexPath(), L"Preset.default", L"Name").empty())
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
std::vector<PresetSummary> listPresets()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<PresetSummary> result;
    if (!g_initialised) return result;
    for (const auto &id : splitOrder(readValue(indexPath(), L"Presets", L"Order"))) {
        const std::wstring section = L"Preset." + id;
        const std::wstring name = readValue(indexPath(), section.c_str(), L"Name");
        if (name.empty()) continue;
        result.push_back({id, name, id == g_activeId,
                          !filesEqual(presetPathFor(id), draftPathFor(id))});
    }
    return result;
}
bool createPreset(const std::wstring &requestedName, const std::wstring &sourceId, std::wstring &createdId)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::wstring name = cleanName(requestedName);
    if (!g_initialised || name.empty()) return false;
    const std::wstring id = makeId();
    if (id.empty()) return false;
    const auto saved = presetPathFor(id);
    const auto draft = draftPathFor(id);
    bool ok = true;
    if (!sourceId.empty()) {
        const auto order = splitOrder(readValue(indexPath(), L"Presets", L"Order"));
        if (std::find(order.begin(), order.end(), sourceId) == order.end()) return false;
        ok = copyAtomically(presetPathFor(sourceId), saved);
    }
    else ensureUnicodeFile(saved);
    if (ok) ok = copyAtomically(saved, draft);
    if (!ok) { DeleteFileW(saved.c_str()); DeleteFileW(draft.c_str()); return false; }
    auto order = splitOrder(readValue(indexPath(), L"Presets", L"Order"));
    order.push_back(id);
    const std::wstring section = L"Preset." + id;
    if (!writeValue(indexPath(), section.c_str(), L"Name", name) ||
        !writeValue(indexPath(), L"Presets", L"Order", joinOrder(order))) {
        DeleteFileW(saved.c_str()); DeleteFileW(draft.c_str()); return false;
    }
    createdId = id;
    return true;
}
bool selectPreset(const std::wstring &id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialised) return false;
    const auto order = splitOrder(readValue(indexPath(), L"Presets", L"Order"));
    if (std::find(order.begin(), order.end(), id) == order.end()) return false;
    std::error_code error;
    if (!std::filesystem::exists(presetPathFor(id), error)) return false;
    if (!std::filesystem::exists(draftPathFor(id), error) &&
        !CopyFileW(presetPathFor(id).c_str(), draftPathFor(id).c_str(), FALSE)) return false;
    if (!writeValue(indexPath(), L"Presets", L"ActiveId", id)) return false;
    g_activeId = id;
    return true;
}
bool renamePreset(const std::wstring &id, const std::wstring &requestedName)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::wstring name = cleanName(requestedName);
    if (!g_initialised || name.empty()) return false;
    const auto order = splitOrder(readValue(indexPath(), L"Presets", L"Order"));
    if (std::find(order.begin(), order.end(), id) == order.end()) return false;
    const std::wstring section = L"Preset." + id;
    return writeValue(indexPath(), section.c_str(), L"Name", name);
}
bool deletePreset(const std::wstring &id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialised) return false;
    auto order = splitOrder(readValue(indexPath(), L"Presets", L"Order"));
    const auto found = std::find(order.begin(), order.end(), id);
    if (found == order.end() || order.size() <= 1) return false;
    const size_t removedIndex = static_cast<size_t>(found - order.begin());
    order.erase(found);
    if (id == g_activeId) {
        g_activeId = order[std::min(removedIndex, order.size() - 1)];
        if (!writeValue(indexPath(), L"Presets", L"ActiveId", g_activeId)) return false;
    }
    if (!writeValue(indexPath(), L"Presets", L"Order", joinOrder(order))) return false;
    const std::wstring section = L"Preset." + id;
    WritePrivateProfileStringW(section.c_str(), nullptr, nullptr, indexPath().c_str());
    DeleteFileW(presetPathFor(id).c_str());
    DeleteFileW(draftPathFor(id).c_str());
    return true;
}
bool exportPreset(const std::wstring &id, const std::wstring &destinationPath)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialised) return false;
    const auto order = splitOrder(readValue(indexPath(), L"Presets", L"Order"));
    if (std::find(order.begin(), order.end(), id) == order.end()) return false;
    const auto sourcePreset = presetPathFor(id);
    const auto temporaryPreset = g_root / (L"export-" + makeId() + L".ini");
    if (!CopyFileW(sourcePreset.c_str(), temporaryPreset.c_str(), FALSE)) return false;

    std::vector<std::wstring> imageKeys{L"PrimaryImage", L"ReactionImage", L"PrimaryBlinkImage",
                                        L"ReactionBlinkImage", L"BackgroundImage"};
    for (const auto &layerId : splitOrder(readValue(temporaryPreset, L"Preset", L"LayerOrder")))
        imageKeys.push_back(L"Layer." + layerId + L".Image");
    std::map<std::string, std::vector<unsigned char>> entries;
    for (const auto &key : imageKeys) {
        const std::wstring source = readValue(temporaryPreset, L"Preset", key.c_str());
        if (source.empty()) continue;
        const std::wstring digest = sha256(source);
        if (digest.empty()) { DeleteFileW(temporaryPreset.c_str()); return false; }
        std::string entryName; entryName.reserve(digest.size());
        for (const wchar_t character : digest) entryName.push_back(static_cast<char>(character));
        if (!entries.count(entryName)) {
            std::ifstream input(std::filesystem::path(source), std::ios::binary);
            if (!input) { DeleteFileW(temporaryPreset.c_str()); return false; }
            entries[entryName] = std::vector<unsigned char>(std::istreambuf_iterator<char>(input), {});
            if (entries[entryName].empty() || entries[entryName].size() > 128ull * 1024 * 1024) {
                DeleteFileW(temporaryPreset.c_str()); return false;
            }
        }
        writeValue(temporaryPreset, L"Preset", key.c_str(), L"@asset/" + digest + L".png");
    }
    {
        std::ifstream input(temporaryPreset, std::ios::binary);
        entries["preset.ini"] = std::vector<unsigned char>(std::istreambuf_iterator<char>(input), {});
    }
    DeleteFileW(temporaryPreset.c_str());
    const std::wstring section = L"Preset." + id;
    const std::wstring name = readValue(indexPath(), section.c_str(), L"Name", L"Imported preset");
    entries["name.utf16"] = std::vector<unsigned char>(reinterpret_cast<const unsigned char *>(name.data()),
        reinterpret_cast<const unsigned char *>(name.data() + name.size()));

    const std::filesystem::path temporaryPackage = destinationPath + L".tmp";
    std::ofstream output(temporaryPackage, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    const char magic[8] = {'R','A','S','P','R','E','1','\0'};
    output.write(magic, sizeof(magic));
    const uint32_t count = static_cast<uint32_t>(entries.size());
    output.write(reinterpret_cast<const char *>(&count), sizeof(count));
    for (const auto &[entryName, bytes] : entries) {
        const uint32_t nameBytes = static_cast<uint32_t>(entryName.size());
        const uint64_t dataBytes = static_cast<uint64_t>(bytes.size());
        output.write(reinterpret_cast<const char *>(&nameBytes), sizeof(nameBytes));
        output.write(reinterpret_cast<const char *>(&dataBytes), sizeof(dataBytes));
        output.write(entryName.data(), nameBytes);
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    output.close();
    if (!output || !MoveFileExW(temporaryPackage.c_str(), destinationPath.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporaryPackage.c_str()); return false;
    }
    return true;
}
bool importPreset(const std::wstring &packagePath, std::wstring &createdId)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialised) return false;
    std::ifstream input(std::filesystem::path(packagePath), std::ios::binary);
    if (!input) return false;
    char magic[8]{}; uint32_t count = 0;
    input.read(magic, sizeof(magic)); input.read(reinterpret_cast<char *>(&count), sizeof(count));
    if (memcmp(magic, "RASPRE1", 7) != 0 || count < 2 || count > 32) return false;
    std::map<std::string, std::vector<unsigned char>> entries;
    uint64_t total = 0;
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t nameBytes = 0; uint64_t dataBytes = 0;
        input.read(reinterpret_cast<char *>(&nameBytes), sizeof(nameBytes));
        input.read(reinterpret_cast<char *>(&dataBytes), sizeof(dataBytes));
        if (!input || !nameBytes || nameBytes > 100 || dataBytes > 128ull * 1024 * 1024 ||
            total + dataBytes > 512ull * 1024 * 1024) return false;
        std::string name(nameBytes, '\0'); input.read(name.data(), nameBytes);
        if (!input || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-") != std::string::npos)
            return false;
        auto &bytes = entries[name]; bytes.resize(static_cast<size_t>(dataBytes));
        input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(dataBytes));
        if (!input) return false;
        total += dataBytes;
    }
    if (!entries.count("preset.ini") || !entries.count("name.utf16")) return false;
    const auto &nameBytes = entries["name.utf16"];
    if (nameBytes.empty() || nameBytes.size() % sizeof(wchar_t) != 0 || nameBytes.size() > 160) return false;
    std::wstring name(nameBytes.size() / sizeof(wchar_t), L'\0');
    memcpy(name.data(), nameBytes.data(), nameBytes.size());
    name = cleanName(name);
    if (name.empty()) name = L"Imported preset";
    const std::wstring id = makeId();
    if (id.empty()) return false;
    const auto saved = presetPathFor(id);
    {
        std::ofstream output(saved, std::ios::binary | std::ios::trunc);
        const auto &bytes = entries["preset.ini"];
        if (bytes.empty() || bytes.size() > 2 * 1024 * 1024) return false;
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!output) { DeleteFileW(saved.c_str()); return false; }
    }
    std::vector<std::wstring> imageKeys{L"PrimaryImage", L"ReactionImage", L"PrimaryBlinkImage",
                                        L"ReactionBlinkImage", L"BackgroundImage"};
    for (const auto &layerId : splitOrder(readValue(saved, L"Preset", L"LayerOrder")))
        imageKeys.push_back(L"Layer." + layerId + L".Image");
    for (const auto &key : imageKeys) {
        const std::wstring token = readValue(saved, L"Preset", key.c_str());
        constexpr std::wstring_view prefix = L"@asset/";
        if (token.empty()) continue;
        if (token.rfind(prefix, 0) != 0 || token.size() != prefix.size() + 68 ||
            token.substr(token.size() - 4) != L".png") { DeleteFileW(saved.c_str()); return false; }
        const std::wstring digest = token.substr(prefix.size(), 64);
        if (digest.find_first_not_of(L"0123456789abcdef") != std::wstring::npos) { DeleteFileW(saved.c_str()); return false; }
        std::string entryName; entryName.reserve(digest.size());
        for (const wchar_t character : digest) entryName.push_back(static_cast<char>(character));
        const auto found = entries.find(entryName);
        if (found == entries.end()) { DeleteFileW(saved.c_str()); return false; }
        const auto destination = g_root / L"assets" / (digest + L".png");
        if (!std::filesystem::exists(destination)) {
            const auto temporary = destination.wstring() + L".tmp";
            std::ofstream asset(temporary, std::ios::binary | std::ios::trunc);
            asset.write(reinterpret_cast<const char *>(found->second.data()),
                        static_cast<std::streamsize>(found->second.size())); asset.close();
            if (!asset || sha256(temporary) != digest || !MoveFileExW(temporary.c_str(), destination.c_str(), 0)) {
                DeleteFileW(temporary.c_str()); DeleteFileW(saved.c_str()); return false;
            }
        }
        writeValue(saved, L"Preset", key.c_str(), destination.wstring());
    }
    const auto draft = draftPathFor(id);
    if (!copyAtomically(saved, draft)) { DeleteFileW(saved.c_str()); return false; }
    auto order = splitOrder(readValue(indexPath(), L"Presets", L"Order")); order.push_back(id);
    const std::wstring section = L"Preset." + id;
    if (!writeValue(indexPath(), section.c_str(), L"Name", name) ||
        !writeValue(indexPath(), L"Presets", L"Order", joinOrder(order))) {
        DeleteFileW(saved.c_str()); DeleteFileW(draft.c_str()); return false;
    }
    createdId = id;
    return true;
}
bool isPresetScopedKey(const wchar_t *key)
{
    if (!key) return false;
    return std::find(kPresetKeys.begin(), kPresetKeys.end(), std::wstring_view(key)) != kPresetKeys.end() ||
           wcsncmp(key, L"Layer.", 6) == 0 || wcsncmp(key, L"Group.", 6) == 0;
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
