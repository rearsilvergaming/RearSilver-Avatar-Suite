#pragma once
#include <string>
#include <vector>

namespace layer_model {
constexpr size_t kMaximumLayers = 16;
enum class StackItemType { Primary, Layer, Group };
struct StackItem { StackItemType type = StackItemType::Layer; std::wstring id; };
struct Layer {
    std::wstring id, name, imagePath, purpose = L"generic";
    bool visible = true, abovePrimary = true, inheritAvatarEffects = true, scaleLinked = true;
    bool flipHorizontal = false;
    int positionX = 0, positionY = 0, pivotX = 50, pivotY = 50;
    int scaleX = 100, scaleY = 100, rotationTenths = 0, opacity = 100;
    bool tailWagAdded = false, tailWagEnabled = true;
    int tailWagAngle = 20, tailWagCycleMs = 1200, tailWagReactionBoost = 50, tailWagPivot = 0;
    bool swayAdded = false, swayEnabled = true;
    int swayAngle = 8, swayCycleMs = 2400, swayReactionBoost = 25, swayPivot = 0;
    bool eyeMovementAdded = false, eyeMovementEnabled = true;
    int eyeRangeX = 40, eyeRangeY = 25, eyeCycleMs = 3200, eyeActiveDuring = 0;
};
struct Group {
    std::wstring id, name;
    bool visible = true, inheritAvatarEffects = true, scaleLinked = true, flipHorizontal = false;
    int positionX = 0, positionY = 0, pivotX = 50, pivotY = 50;
    int scaleX = 100, scaleY = 100, rotationTenths = 0, opacity = 100;
    std::vector<std::wstring> layerOrder;
};
struct Composition {
    std::vector<Layer> layers;
    std::vector<Group> groups;
    // Front-to-back display and compositing order. Primary occurs exactly once.
    std::vector<StackItem> rootOrder;
};
Composition loadDraftComposition();
bool saveDraftComposition(const Composition &composition);
std::vector<Layer> loadDraftLayers();
bool saveDraftLayers(const std::vector<Layer> &layers);
Layer makeLayer(const std::wstring &name, const std::wstring &imagePath);
Group makeGroup(const std::wstring &name);
}
