#pragma once
#include <string>
#include <vector>

namespace layer_model {
constexpr size_t kMaximumLayers = 16;
struct Layer {
    std::wstring id, name, imagePath, purpose = L"generic";
    bool visible = true, abovePrimary = true, inheritAvatarEffects = true, scaleLinked = true;
    int positionX = 0, positionY = 0, pivotX = 50, pivotY = 50;
    int scaleX = 100, scaleY = 100, rotationTenths = 0, opacity = 100;
    bool tailWagAdded = false, tailWagEnabled = true;
    int tailWagAngle = 20, tailWagCycleMs = 1200, tailWagReactionBoost = 50, tailWagPivot = 0;
};
std::vector<Layer> loadDraftLayers();
bool saveDraftLayers(const std::vector<Layer> &layers);
Layer makeLayer(const std::wstring &name, const std::wstring &imagePath);
}
