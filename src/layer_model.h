#pragma once
#include <string>
#include <vector>

namespace layer_model {
constexpr size_t kMaximumLayers = 16;
enum class StackItemType { Primary, Layer, Group };
struct StackItem { StackItemType type = StackItemType::Layer; std::wstring id; };
enum class LocalEffectType {
    Sway, BoundedMovement, Orbit, LocalPulse, LocalSpin, ReactionNudge, StateVisibility,
    DangleSpring, Flutter, ArtworkStateChange
};
struct LocalEffect {
    std::wstring id;
    LocalEffectType type = LocalEffectType::Sway;
    bool enabled = true;
    // Sway: amountX is angle in degrees, cycleMs is duration, reactionBoost is percent.
    // Bounded movement: amountX/Y are pixel ranges and activeDuring is idle/reaction/both.
    int amountX = 8, amountY = 25, cycleMs = 2400, reactionBoost = 25;
    int pivot = 0, activeDuring = 2;
    std::wstring imagePath, imageDisplayName;
    int artworkScaleX = 100, artworkScaleY = 100;
    bool artworkScaleLinked = true;
};
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
    std::vector<LocalEffect> effects;
};
struct Group {
    std::wstring id, name;
    bool visible = true, inheritAvatarEffects = true, scaleLinked = true, flipHorizontal = false;
    int positionX = 0, positionY = 0, pivotX = 50, pivotY = 50;
    int scaleX = 100, scaleY = 100, rotationTenths = 0, opacity = 100;
    std::vector<std::wstring> layerOrder;
    std::vector<LocalEffect> effects;
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
LocalEffect makeLocalEffect(LocalEffectType type);
}
