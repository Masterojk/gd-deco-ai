// Deco AI - finds the building blocks deco is made from, by looking at GD's own
// object list at runtime instead of trusting hard-coded IDs.
#pragma once

#include <string>

namespace deco {

struct Catalog {
    int square = 211;         // plain colourable square (made non-solid after placing)
    float squareSize = 30.f;  // its sprite size in editor units
    bool scanned = false;
};

Catalog const& catalog();

// Writes every object ID, its sprite name, type and size to a text file.
// Returns the file path, or an empty string on failure.
std::string exportCatalog();

} // namespace deco
