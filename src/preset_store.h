#pragma once

#include <string>
#include <vector>

namespace preset_store {

struct PresetSummary {
    std::wstring id;
    std::wstring name;
    bool active = false;
    bool dirty = false;
};

bool initialise(const std::wstring &legacySettingsPath);
std::wstring activePresetId();
std::wstring activePresetName();
std::wstring loadDraftValue(const wchar_t *key);
bool saveDraftValue(const wchar_t *key, const std::wstring &value);
bool updateActivePreset();
bool revertActivePreset();
bool hasUnsavedChanges();
std::vector<PresetSummary> listPresets();
bool createPreset(const std::wstring &name, const std::wstring &sourceId, std::wstring &createdId);
bool selectPreset(const std::wstring &id);
bool renamePreset(const std::wstring &id, const std::wstring &name);
bool deletePreset(const std::wstring &id);
bool exportPreset(const std::wstring &id, const std::wstring &destinationPath);
bool importPreset(const std::wstring &packagePath, std::wstring &createdId);
bool isPresetScopedKey(const wchar_t *key);
std::wstring importPngAsset(const std::wstring &sourcePath);

} // namespace preset_store
