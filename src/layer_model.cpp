#include "layer_model.h"
#include "preset_store.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <set>
#include <sstream>

namespace layer_model { namespace {
std::vector<std::wstring> split(const std::wstring &value) {
    std::vector<std::wstring> out; std::wstringstream stream(value); std::wstring part;
    while (std::getline(stream, part, L',')) if (!part.empty()) out.push_back(part);
    return out;
}
std::wstring key(const std::wstring &id, const wchar_t *field) { return L"Layer." + id + L"." + field; }
std::wstring groupKey(const std::wstring &id, const wchar_t *field) { return L"Group." + id + L"." + field; }
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
std::wstring groupValue(const std::wstring &id, const wchar_t *field) {
    return preset_store::loadDraftValue(groupKey(id, field).c_str());
}
int groupInteger(const std::wstring &id, const wchar_t *field, int fallback, int low, int high) {
    const std::wstring value = groupValue(id, field);
    return value.empty() ? fallback : std::clamp(_wtoi(value.c_str()), low, high);
}
bool groupFlag(const std::wstring &id, const wchar_t *field, bool fallback) {
    const std::wstring value = groupValue(id, field);
    return value.empty() ? fallback : value != L"0";
}
bool writeGroup(const std::wstring &id, const wchar_t *field, const std::wstring &value) {
    return preset_store::saveDraftValue(groupKey(id, field).c_str(), value);
}
const wchar_t *effectTypeName(LocalEffectType type) {
    switch (type) {
    case LocalEffectType::Sway: return L"sway";
    case LocalEffectType::BoundedMovement: return L"bounded-movement";
    case LocalEffectType::Orbit: return L"orbit";
    case LocalEffectType::LocalPulse: return L"local-pulse";
    case LocalEffectType::LocalSpin: return L"local-spin";
    case LocalEffectType::ReactionNudge: return L"reaction-nudge";
    case LocalEffectType::StateVisibility: return L"state-visibility";
    case LocalEffectType::DangleSpring: return L"dangle-spring";
    case LocalEffectType::Flutter: return L"flutter";
    }
    return L"sway";
}
bool parseEffectType(const std::wstring &name, LocalEffectType &type) {
    if (name == L"sway") type = LocalEffectType::Sway;
    else if (name == L"bounded-movement") type = LocalEffectType::BoundedMovement;
    else if (name == L"orbit") type = LocalEffectType::Orbit;
    else if (name == L"local-pulse") type = LocalEffectType::LocalPulse;
    else if (name == L"local-spin") type = LocalEffectType::LocalSpin;
    else if (name == L"reaction-nudge") type = LocalEffectType::ReactionNudge;
    else if (name == L"state-visibility") type = LocalEffectType::StateVisibility;
    else if (name == L"dangle-spring") type = LocalEffectType::DangleSpring;
    else if (name == L"flutter") type = LocalEffectType::Flutter;
    else return false;
    return true;
}
std::wstring effectKey(const std::wstring &ownerKind, const std::wstring &ownerId,
                       const std::wstring &effectId, const wchar_t *field) {
    return ownerKind + L"." + ownerId + L".Effect." + effectId + L"." + field;
}
int effectInteger(const std::wstring &kind, const std::wstring &owner, const std::wstring &id,
                  const wchar_t *field, int fallback, int low, int high) {
    const std::wstring value = preset_store::loadDraftValue(effectKey(kind, owner, id, field).c_str());
    return value.empty() ? fallback : std::clamp(_wtoi(value.c_str()), low, high);
}
std::vector<LocalEffect> loadEffects(const std::wstring &kind, const std::wstring &owner,
                                     const std::wstring &order) {
    std::vector<LocalEffect> effects;
    std::set<LocalEffectType> seenTypes;
    for (const std::wstring &id : split(order)) {
        const std::wstring type = preset_store::loadDraftValue(effectKey(kind, owner, id, L"Type").c_str());
        LocalEffect effect; effect.id = id;
        if (!parseEffectType(type, effect.type) || !seenTypes.insert(effect.type).second) continue;
        effect.enabled = effectInteger(kind, owner, id, L"Enabled", 1, 0, 1) != 0;
        const int minimumAmount = effect.type == LocalEffectType::ReactionNudge ? -512 : 0;
        effect.amountX = effectInteger(kind, owner, id, L"AmountX", effect.type == LocalEffectType::Sway ? 8 : 40, minimumAmount, 512);
        effect.amountY = effectInteger(kind, owner, id, L"AmountY", 25, minimumAmount, 512);
        effect.cycleMs = effectInteger(kind, owner, id, L"CycleMs", effect.type == LocalEffectType::Sway ? 2400 : 3200, 200, 20000);
        effect.reactionBoost = effectInteger(kind, owner, id, L"ReactionBoost", 25, 0, 300);
        effect.pivot = effectInteger(kind, owner, id, L"Pivot", 0, 0, 9);
        effect.activeDuring = effectInteger(kind, owner, id, L"ActiveDuring", 2, 0, 2);
        effects.push_back(std::move(effect));
    }
    return effects;
}
bool saveEffects(const std::wstring &kind, const std::wstring &owner,
                 const std::vector<LocalEffect> &effects, std::wstring &order) {
    bool ok = true; std::set<std::wstring> ids;
    for (const LocalEffect &effect : effects) {
        if (effect.id.empty() || effect.id.find_first_of(L",.\\/") != std::wstring::npos || !ids.insert(effect.id).second) return false;
        if (!order.empty()) order += L','; order += effect.id;
        const auto save = [&](const wchar_t *field, const std::wstring &value) {
            return preset_store::saveDraftValue(effectKey(kind, owner, effect.id, field).c_str(), value);
        };
        ok = save(L"Type", effectTypeName(effect.type)) && ok;
        ok = save(L"Enabled", effect.enabled ? L"1" : L"0") && ok;
        ok = save(L"AmountX", std::to_wstring(effect.amountX)) && ok;
        ok = save(L"AmountY", std::to_wstring(effect.amountY)) && ok;
        ok = save(L"CycleMs", std::to_wstring(effect.cycleMs)) && ok;
        ok = save(L"ReactionBoost", std::to_wstring(effect.reactionBoost)) && ok;
        ok = save(L"Pivot", std::to_wstring(effect.pivot)) && ok;
        ok = save(L"ActiveDuring", std::to_wstring(effect.activeDuring)) && ok;
    }
    return ok;
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
        layer.flipHorizontal = flag(id, L"FlipHorizontal", false);
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
        layer.swayAdded = (L"," + effectStack + L",").find(L",sway,") != std::wstring::npos;
        layer.eyeMovementAdded = (L"," + effectStack + L",").find(L",eye-movement,") != std::wstring::npos;
        layer.tailWagEnabled = flag(id, L"TailWagEnabled", true);
        layer.tailWagAngle = integer(id, L"TailWagAngle", 20, 0, 90);
        layer.tailWagCycleMs = integer(id, L"TailWagCycleMs", 1200, 200, 10000);
        layer.tailWagReactionBoost = integer(id, L"TailWagReactionBoost", 50, 0, 300);
        layer.tailWagPivot = integer(id, L"TailWagPivot", 0, 0, 9);
        layer.swayEnabled = flag(id, L"SwayEnabled", true);
        layer.swayAngle = integer(id, L"SwayAngle", 8, 0, 45);
        layer.swayCycleMs = integer(id, L"SwayCycleMs", 2400, 400, 20000);
        layer.swayReactionBoost = integer(id, L"SwayReactionBoost", 25, 0, 300);
        layer.swayPivot = integer(id, L"SwayPivot", 0, 0, 9);
        layer.eyeMovementEnabled = flag(id, L"EyeMovementEnabled", true);
        layer.eyeRangeX = integer(id, L"EyeRangeX", 40, 0, 512);
        layer.eyeRangeY = integer(id, L"EyeRangeY", 25, 0, 512);
        layer.eyeCycleMs = integer(id, L"EyeCycleMs", 3200, 500, 20000);
        layer.eyeActiveDuring = integer(id, L"EyeActiveDuring", 0, 0, 2);
        layer.effects = loadEffects(L"Layer", id, preset_store::loadDraftValue(key(id, L"LocalEffectOrder").c_str()));
        const bool localEffectsMigrated = flag(id, L"LocalEffectsMigrated", false);
        if (layer.effects.empty() && !localEffectsMigrated) {
            if (layer.tailWagAdded) {
                auto effect = makeLocalEffect(LocalEffectType::Sway); effect.amountX = layer.tailWagAngle;
                effect.cycleMs = layer.tailWagCycleMs; effect.reactionBoost = layer.tailWagReactionBoost;
                effect.pivot = layer.tailWagPivot; effect.enabled = layer.tailWagEnabled; layer.effects.push_back(effect);
            } else if (layer.swayAdded) {
                auto effect = makeLocalEffect(LocalEffectType::Sway); effect.amountX = layer.swayAngle;
                effect.cycleMs = layer.swayCycleMs; effect.reactionBoost = layer.swayReactionBoost;
                effect.pivot = layer.swayPivot; effect.enabled = layer.swayEnabled; layer.effects.push_back(effect);
            }
            if (layer.eyeMovementAdded) {
                auto effect = makeLocalEffect(LocalEffectType::BoundedMovement); effect.amountX = layer.eyeRangeX;
                effect.amountY = layer.eyeRangeY; effect.cycleMs = layer.eyeCycleMs;
                effect.activeDuring = layer.eyeActiveDuring; effect.enabled = layer.eyeMovementEnabled; layer.effects.push_back(effect);
            }
        }
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
        ok = write(layer.id,L"FlipHorizontal",layer.flipHorizontal?L"1":L"0")&&ok;
        ok = write(layer.id,L"PositionX",std::to_wstring(layer.positionX))&&ok;
        ok = write(layer.id,L"PositionY",std::to_wstring(layer.positionY))&&ok;
        ok = write(layer.id,L"PivotX",std::to_wstring(layer.pivotX))&&ok;
        ok = write(layer.id,L"PivotY",std::to_wstring(layer.pivotY))&&ok;
        ok = write(layer.id,L"ScaleX",std::to_wstring(layer.scaleX))&&ok;
        ok = write(layer.id,L"ScaleY",std::to_wstring(layer.scaleY))&&ok;
        ok = write(layer.id,L"RotationTenths",std::to_wstring(layer.rotationTenths))&&ok;
        ok = write(layer.id,L"Opacity",std::to_wstring(layer.opacity))&&ok;
        std::wstring effectStack;
        if (layer.tailWagAdded) effectStack = L"tail-wag";
        if (layer.swayAdded) effectStack += (effectStack.empty() ? L"" : L",") + std::wstring(L"sway");
        if (layer.eyeMovementAdded) effectStack += (effectStack.empty() ? L"" : L",") + std::wstring(L"eye-movement");
        ok = write(layer.id,L"EffectStack",effectStack)&&ok;
        ok = write(layer.id,L"TailWagEnabled",layer.tailWagEnabled?L"1":L"0")&&ok;
        ok = write(layer.id,L"TailWagAngle",std::to_wstring(layer.tailWagAngle))&&ok;
        ok = write(layer.id,L"TailWagCycleMs",std::to_wstring(layer.tailWagCycleMs))&&ok;
        ok = write(layer.id,L"TailWagReactionBoost",std::to_wstring(layer.tailWagReactionBoost))&&ok;
        ok = write(layer.id,L"TailWagPivot",std::to_wstring(layer.tailWagPivot))&&ok;
        ok = write(layer.id,L"SwayEnabled",layer.swayEnabled?L"1":L"0")&&ok;
        ok = write(layer.id,L"SwayAngle",std::to_wstring(layer.swayAngle))&&ok;
        ok = write(layer.id,L"SwayCycleMs",std::to_wstring(layer.swayCycleMs))&&ok;
        ok = write(layer.id,L"SwayReactionBoost",std::to_wstring(layer.swayReactionBoost))&&ok;
        ok = write(layer.id,L"SwayPivot",std::to_wstring(layer.swayPivot))&&ok;
        ok = write(layer.id,L"EyeMovementEnabled",layer.eyeMovementEnabled?L"1":L"0")&&ok;
        ok = write(layer.id,L"EyeRangeX",std::to_wstring(layer.eyeRangeX))&&ok;
        ok = write(layer.id,L"EyeRangeY",std::to_wstring(layer.eyeRangeY))&&ok;
        ok = write(layer.id,L"EyeCycleMs",std::to_wstring(layer.eyeCycleMs))&&ok;
        ok = write(layer.id,L"EyeActiveDuring",std::to_wstring(layer.eyeActiveDuring))&&ok;
        std::wstring localEffectOrder;
        ok = saveEffects(L"Layer", layer.id, layer.effects, localEffectOrder) && ok;
        ok = write(layer.id, L"LocalEffectOrder", localEffectOrder) && ok;
        ok = write(layer.id, L"LocalEffectsMigrated", L"1") && ok;
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

Composition loadDraftComposition() {
    Composition composition;
    composition.layers = loadDraftLayers();
    for (const std::wstring &id : split(preset_store::loadDraftValue(L"GroupOrder"))) {
        Group group; group.id = id;
        group.name = groupValue(id, L"Name");
        if (group.name.empty()) group.name = L"Group";
        group.visible = groupFlag(id, L"Visible", true);
        group.inheritAvatarEffects = groupFlag(id, L"InheritAvatarEffects", true);
        group.scaleLinked = groupFlag(id, L"ScaleLinked", true);
        group.flipHorizontal = groupFlag(id, L"FlipHorizontal", false);
        group.positionX = groupInteger(id, L"PositionX", 0, -4096, 4096);
        group.positionY = groupInteger(id, L"PositionY", 0, -4096, 4096);
        group.pivotX = groupInteger(id, L"PivotX", 50, 0, 100);
        group.pivotY = groupInteger(id, L"PivotY", 50, 0, 100);
        group.scaleX = groupInteger(id, L"ScaleX", 100, 1, 1000);
        group.scaleY = groupInteger(id, L"ScaleY", 100, 1, 1000);
        group.rotationTenths = groupInteger(id, L"RotationTenths", 0, -3600, 3600);
        group.opacity = groupInteger(id, L"Opacity", 100, 0, 100);
        group.layerOrder = split(groupValue(id, L"LayerOrder"));
        group.effects = loadEffects(L"Group", id, groupValue(id, L"LocalEffectOrder"));
        composition.groups.push_back(std::move(group));
    }
    const std::wstring stored = preset_store::loadDraftValue(L"CompositionOrder");
    std::set<std::wstring> layerIds;
    for (const Layer &layer : composition.layers) layerIds.insert(layer.id);
    std::set<std::wstring> seenLayers, groupIds, seenGroups;
    for (const Group &group : composition.groups) groupIds.insert(group.id);
    bool sawPrimary = false;
    bool valid = !stored.empty();
    for (const std::wstring &token : split(stored)) {
        if (token == L"primary") {
            if (sawPrimary) { valid = false; break; }
            sawPrimary = true;
            composition.rootOrder.push_back({StackItemType::Primary, L""});
        } else if (token.rfind(L"layer:", 0) == 0) {
            const std::wstring id = token.substr(6);
            if (!layerIds.count(id) || !seenLayers.insert(id).second) { valid = false; break; }
            composition.rootOrder.push_back({StackItemType::Layer, id});
        } else if (token.rfind(L"group:", 0) == 0) {
            const std::wstring id = token.substr(6);
            if (!groupIds.count(id) || !seenGroups.insert(id).second) { valid = false; break; }
            composition.rootOrder.push_back({StackItemType::Group, id});
        } else {
            valid = false;
            break;
        }
    }
    if (valid) {
        for (const Group &group : composition.groups) {
            for (const std::wstring &id : group.layerOrder)
                if (!layerIds.count(id) || !seenLayers.insert(id).second) { valid = false; break; }
            if (!valid) break;
        }
    }
    valid = valid && sawPrimary && seenLayers.size() == composition.layers.size() &&
            seenGroups.size() == composition.groups.size();
    if (valid) return composition;

    // Legacy migration preserves the renderer's existing back-to-front result
    // while expressing it as the new front-to-back visible stack.
    composition.groups.clear();
    composition.rootOrder.clear();
    for (auto item = composition.layers.rbegin(); item != composition.layers.rend(); ++item)
        if (item->abovePrimary) composition.rootOrder.push_back({StackItemType::Layer, item->id});
    composition.rootOrder.push_back({StackItemType::Primary, L""});
    for (auto item = composition.layers.rbegin(); item != composition.layers.rend(); ++item)
        if (!item->abovePrimary) composition.rootOrder.push_back({StackItemType::Layer, item->id});
    return composition;
}

bool saveDraftComposition(const Composition &composition) {
    if (composition.layers.size() > kMaximumLayers) return false;
    std::set<std::wstring> layerIds;
    for (const Layer &layer : composition.layers)
        if (layer.id.empty() || !layerIds.insert(layer.id).second) return false;
    std::set<std::wstring> groupIds;
    for (const Group &group : composition.groups)
        if (group.id.empty() || !groupIds.insert(group.id).second) return false;
    std::set<std::wstring> seenLayers, seenGroups;
    bool sawPrimary = false;
    bool abovePrimary = true;
    std::wstring encoded;
    std::vector<Layer> legacyLayers = composition.layers;
    for (const StackItem &item : composition.rootOrder) {
        std::wstring token;
        if (item.type == StackItemType::Primary) {
            if (sawPrimary) return false;
            sawPrimary = true;
            abovePrimary = false;
            token = L"primary";
        } else if (item.type == StackItemType::Layer) {
            if (!layerIds.count(item.id) || !seenLayers.insert(item.id).second) return false;
            token = L"layer:" + item.id;
            const auto found = std::find_if(legacyLayers.begin(), legacyLayers.end(),
                [&](const Layer &layer) { return layer.id == item.id; });
            if (found != legacyLayers.end()) found->abovePrimary = abovePrimary;
        } else if (item.type == StackItemType::Group) {
            if (!groupIds.count(item.id) || !seenGroups.insert(item.id).second) return false;
            token = L"group:" + item.id;
            const auto group = std::find_if(composition.groups.begin(), composition.groups.end(),
                [&](const Group &candidate) { return candidate.id == item.id; });
            if (group == composition.groups.end()) return false;
            for (const std::wstring &layerId : group->layerOrder) {
                if (!layerIds.count(layerId) || !seenLayers.insert(layerId).second) return false;
                const auto found = std::find_if(legacyLayers.begin(), legacyLayers.end(),
                    [&](const Layer &layer) { return layer.id == layerId; });
                if (found != legacyLayers.end()) found->abovePrimary = abovePrimary;
            }
        } else {
            return false;
        }
        if (!encoded.empty()) encoded += L',';
        encoded += token;
    }
    if (!sawPrimary || seenLayers.size() != composition.layers.size() ||
        seenGroups.size() != composition.groups.size()) return false;
    std::wstring groupOrder;
    bool ok = true;
    for (const Group &group : composition.groups) {
        if (!groupOrder.empty()) groupOrder += L',';
        groupOrder += group.id;
        std::wstring children;
        for (const std::wstring &id : group.layerOrder) {
            if (!children.empty()) children += L',';
            children += id;
        }
        ok = writeGroup(group.id,L"Name",group.name)&&ok;
        ok = writeGroup(group.id,L"Visible",group.visible?L"1":L"0")&&ok;
        ok = writeGroup(group.id,L"InheritAvatarEffects",group.inheritAvatarEffects?L"1":L"0")&&ok;
        ok = writeGroup(group.id,L"ScaleLinked",group.scaleLinked?L"1":L"0")&&ok;
        ok = writeGroup(group.id,L"FlipHorizontal",group.flipHorizontal?L"1":L"0")&&ok;
        ok = writeGroup(group.id,L"PositionX",std::to_wstring(group.positionX))&&ok;
        ok = writeGroup(group.id,L"PositionY",std::to_wstring(group.positionY))&&ok;
        ok = writeGroup(group.id,L"PivotX",std::to_wstring(group.pivotX))&&ok;
        ok = writeGroup(group.id,L"PivotY",std::to_wstring(group.pivotY))&&ok;
        ok = writeGroup(group.id,L"ScaleX",std::to_wstring(group.scaleX))&&ok;
        ok = writeGroup(group.id,L"ScaleY",std::to_wstring(group.scaleY))&&ok;
        ok = writeGroup(group.id,L"RotationTenths",std::to_wstring(group.rotationTenths))&&ok;
        ok = writeGroup(group.id,L"Opacity",std::to_wstring(group.opacity))&&ok;
        ok = writeGroup(group.id,L"LayerOrder",children)&&ok;
        std::wstring localEffectOrder;
        ok = saveEffects(L"Group", group.id, group.effects, localEffectOrder) && ok;
        ok = writeGroup(group.id, L"LocalEffectOrder", localEffectOrder) && ok;
    }
    return saveDraftLayers(legacyLayers) && ok &&
           preset_store::saveDraftValue(L"GroupOrder", groupOrder) &&
           preset_store::saveDraftValue(L"CompositionOrder", encoded);
}

Group makeGroup(const std::wstring &name) {
    static std::atomic<unsigned long> sequence{0}; FILETIME time{}; GetSystemTimeAsFileTime(&time);
    Group group; group.id=L"group-"+std::to_wstring(time.dwHighDateTime)+L"-"+
        std::to_wstring(time.dwLowDateTime)+L"-"+std::to_wstring(++sequence);
    group.name=name.empty()?L"New group":name; return group;
}
LocalEffect makeLocalEffect(LocalEffectType type) {
    static std::atomic<unsigned long> sequence{0}; FILETIME time{}; GetSystemTimeAsFileTime(&time);
    LocalEffect effect; effect.id=L"effect-"+std::to_wstring(time.dwHighDateTime)+L"-"+
        std::to_wstring(time.dwLowDateTime)+L"-"+std::to_wstring(++sequence); effect.type=type;
    if (type == LocalEffectType::BoundedMovement) { effect.amountX=40; effect.amountY=25; effect.cycleMs=3200; effect.reactionBoost=0; }
    else if (type == LocalEffectType::Orbit) { effect.amountX=30; effect.amountY=20; effect.cycleMs=3000; effect.activeDuring=2; }
    else if (type == LocalEffectType::LocalPulse) { effect.amountX=8; effect.amountY=0; effect.cycleMs=1800; effect.activeDuring=2; }
    else if (type == LocalEffectType::LocalSpin) { effect.amountX=360; effect.amountY=0; effect.cycleMs=3000; effect.activeDuring=2; }
    else if (type == LocalEffectType::ReactionNudge) { effect.amountX=20; effect.amountY=-12; effect.cycleMs=350; effect.activeDuring=1; }
    else if (type == LocalEffectType::StateVisibility) { effect.amountX=0; effect.amountY=0; effect.cycleMs=0; effect.activeDuring=1; }
    else if (type == LocalEffectType::DangleSpring) { effect.amountX=65; effect.amountY=80; effect.cycleMs=650; effect.activeDuring=2; }
    else if (type == LocalEffectType::Flutter) { effect.amountX=8; effect.amountY=5; effect.cycleMs=900; effect.activeDuring=2; }
    return effect;
}
}
