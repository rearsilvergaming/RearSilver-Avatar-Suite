#pragma once

#include <string>

namespace preset_store {

bool initialise(const std::wstring &legacySettingsPath);
std::wstring activePresetId();
std::wstring activePresetName();
std::wstring loadDraftValue(const wchar_t *key);
bool saveDraftValue(const wchar_t *key, const std::wstring &value);
bool updateActivePreset();
bool revertActivePreset();
bool hasUnsavedChanges();
bool isPresetScopedKey(const wchar_t *key);
std::wstring importPngAsset(const std::wstring &sourcePath);

} // namespace preset_store
