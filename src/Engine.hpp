// Deco AI - turns a plan into real editor objects.
#pragma once

#include <Geode/Geode.hpp>

#include <string>

#include "Plan.hpp"

namespace deco {

struct LevelSummary {
    bool ok = false;
    bool selection = false;   // range comes from the selected objects
    float x0 = 0.f, x1 = 0.f; // editor units
    int lengthBlocks = 0;
    float seconds = 0.f;
    int solidCells = 0;
    int hazards = 0;
    int maxHeightBlocks = 0;
    std::string modes;        // game modes found from portals, e.g. "cube, ship, wave"
    std::string error;
};

LevelSummary summarizeLevel(bool selectionOnly);

struct ApplyResult {
    int objects = 0;
    std::string summary;
};

geode::Result<ApplyResult> applyDeco(DecoPlan const& plan, DecoOptions const& opt, bool selectionOnly);
geode::Result<ApplyResult> applyLayout(LayoutPlan const& plan, unsigned seed);

// Removes the objects from the last Deco AI run on this level
bool canUndo();
int undoLast();

} // namespace deco
