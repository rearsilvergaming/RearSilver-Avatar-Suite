#include "layer_model.h"
#include "preset_store.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <sstream>

namespace layer_model { namespace {
std::vector<std::wstring> split(const std::wstring &value) {
    std::vector<std::wstring> out; std::wstringstream stream(value); std::wstring part;
    while (std::getline(stream, part, L',')) if (!part.empty()) out.push_back(part);
    return out;
}
std::wstring key(const std::wstring &id, const wchar_t *field) { return L"Layer." + id + L"." + field; }
int integer(const std::wstring &id, const wchar_t *field, int fallback, int low, int high) {
    const std::wstring value = preset_store::loadDraftValue(key(id, field).c_str());
    return value.empty() ? fallback : std::clamp(_wtoi(value.c_str()), low, high);
}
bool flag(const std::wstring &id, const wchar_t *field, bool fallback) {
    const std::wstring value = preset_store::loadDraftValue(key(id, field).c_str());
    return value.empty() ? fallback : value != L"0";
}
bool write(const std::wstring &id, const wchar_t *field, const std::wstring &value) {
    return preset_store::saveDraftValue(key(id, field).c_str(), value);
}
}

std::vector<Layer> loadDraftLayers() {
    std::vector<Layer> layers;
    for (const std::wstring &id : split(preset_store::loadDraftValue(L"LayerOrder"))) {
        if (layers.size() == kMaximumLayers) break;
        Layer layer; layer.id = id;
        layer.name = preset_store::loadDraftValue(key(id, L"Name").c_str());
        if (layer.name.empty()) layer.name = L"Layer " + std::to_wstring(layers.size() + 1);
        layer.imagePath = preset_store::loadDraftValue(key(id, L"Image").c_str());
        layer.purpose = preset_store::loadDraftValue(key(id, L"Purpose").c_str());
        if (layer.purpose.empty()) layer.purpose = L"generic";
        layer.visible = flag(id, L"Visible", true);
        layer.abovePrimary = flag(id, L"AbovePrimary", true);
        layer.inheritAvatarEffects = flag(id, L"InheritAvatarEffects", true);
        layer.scaleLinked = flag(id, L"ScaleLinked", true);
        layer.positionX = integer(id, L"PositionX", 0, -4096, 4096);
        layer.positionY = integer(id, L"PositionY", 0, -4096, 4096);
        layer.pivotX = integer(id, L"PivotX", 50, 0, 100);
        layer.pivotY = integer(id, L"PivotY", 50, 0, 100);
        layer.scaleX = integer(id, L"ScaleX", 100, 1, 1000);
        layer.scaleY = integer(id, L"ScaleY", 100, 1, 1000);
        layer.rotationTenths = integer(id, L"RotationTenths", 0, -3600, 3600);
        layer.opacity = integer(id, L"Opacity", 100, 0, 100);
        const std::wstring effectStack = preset_store::loadDraftValue(key(id, L"EffectStack").c_str());
        layer.tailWagAdded = (L"," + effectStack + L",").find(L",tail-wag,") != std::wstring::npos;
        layer.tailWagEnabled = flag(id, L"TailWagEnabled", true);
        layer.tailWagAngle = integer(id, L"TailWagAngle", 20, 0, 90);
        layer.tailWagCycleMs = integer(id, L"TailWagCycleMs", 1200, 200, 10000);
        layer.tailWagReactionBoost = integer(id, L"TailWagReactionBoost", 50, 0, 300);
        layer.tailWagPivot = integer(id, L"TailWagPivot", 0, 0, 9);
        layers.push_back(std::move(layer));
    }
    return layers;
}

bool saveDraftLayers(const std::vector<Layer> &layers) {
    if (layers.size() > kMaximumLayers) return false;
    std::wstring order; bool ok = true;
    for (const Layer &layer : layers) {
        if (layer.id.empty() || layer.id.find_first_of(L",.\\/") != std::wstring::npos) return false;
        if (!order.empty()) order += L','; order += layer.id;
        ok = write(layer.id,L"Name",layer.name)&&ok; ok = write(layer.id,L"Image",layer.imagePath)&&ok;
        ok = write(layer.id,L"Purpose",layer.purpose)&&ok; ok = write(layer.id,L"Visible",layer.visible?L"1":L"0")&&ok;
        ok = write(layer.id,L"AbovePrimary",layer.abovePrimary?L"1":L"0")&&ok;
        ok = write(layer.id,L"InheritAvatarEffects",layer.inheritAvatarEffects?L"1":L"0")&&ok;
        ok = write(layer.id,L"ScaleLinked",layer.scaleLinked?L"1":L"0")&&ok;
        ok = write(layer.id,L"PositionX",std::to_wstring(layer.positionX))&&ok;
        ok = write(layer.id,L"PositionY",std::to_wstring(layer.positionY))&&ok;
        ok = write(layer.id,L"PivotX",std::to_wstring(layer.pivotX))&&ok;
        ok = write(layer.id,L"PivotY",std::to_wstring(layer.pivotY))&&ok;
        ok = write(layer.id,L"ScaleX",std::to_wstring(layer.scaleX))&&ok;
        ok = write(layer.id,L"ScaleY",std::to_wstring(layer.scaleY))&&ok;
        ok = write(layer.id,L"RotationTenths",std::to_wstring(layer.rotationTenths))&&ok;
        ok = write(layer.id,L"Opacity",std::to_wstring(layer.opacity))&&ok;
        ok = write(layer.id,L"EffectStack",layer.tailWagAdded?L"tail-wag":L"")&&ok;
        ok = write(layer.id,L"TailWagEnabled",layer.tailWagEnabled?L"1":L"0")&&ok;
        ok = write(layer.id,L"TailWagAngle",std::to_wstring(layer.tailWagAngle))&&ok;
        ok = write(layer.id,L"TailWagCycleMs",std::to_wstring(layer.tailWagCycleMs))&&ok;
        ok = write(layer.id,L"TailWagReactionBoost",std::to_wstring(layer.tailWagReactionBoost))&&ok;
        ok = write(layer.id,L"TailWagPivot",std::to_wstring(layer.tailWagPivot))&&ok;
    }
    return preset_store::saveDraftValue(L"LayerOrder",order) &&
           preset_store::saveDraftValue(L"LayerCount",std::to_wstring(layers.size())) && ok;
}

Layer makeLayer(const std::wstring &name, const std::wstring &imagePath) {
    static std::atomic<unsigned long> sequence{0}; FILETIME time{}; GetSystemTimeAsFileTime(&time);
    Layer layer; layer.id=L"layer-"+std::to_wstring(time.dwHighDateTime)+L"-"+
        std::to_wstring(time.dwLowDateTime)+L"-"+std::to_wstring(++sequence);
    layer.name=name.empty()?L"New layer":name; layer.imagePath=imagePath; return layer;
}
}
