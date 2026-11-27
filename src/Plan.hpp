// Deco AI - plans: what Claude (or a preset) decides, before anything is placed.
#pragma once

#include <Geode/Geode.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace deco {

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
};

Rgb mix(Rgb a, Rgb b, float t);
Rgb shade(Rgb a, float f);
std::optional<Rgb> parseHex(std::string const& s);
std::string toHex(Rgb c);

// Named colours a plan can set. Every key here is also a valid "col" in motifs.
//  bg, ground, ground2, line, mg, fill, outline, glow, back, accent
using Palette = std::map<std::string, Rgb>;
std::vector<std::string> const& paletteKeys();

enum class Outline { None, Thin, Thick, Double };
enum class Glow { None, Soft, Strong };
enum class Fill { Plain, Shaded, Grid, Bricks, Dots, Inset };
enum class Back { None, Pillars, Diamonds, Stars, Grid, Frames, Mixed };
enum class Particles { None, Embers, Rain, Snow, Dust, Sparks };
enum class Motion { Still, Drift, Rotate, Bounce };
enum class Pulse { None, Beat, HalfBeat, OffBeat };

// A rectangle in block units, relative to the motif's origin
struct Part {
    float x = 0, y = 0, w = 1, h = 1, rot = 0;
    std::string col = "accent";
};

// A small picture made of rectangles, repeated along the level
struct Motif {
    std::string layer = "back"; // back | front
    float every = 16.f;         // blocks between copies
    float y = 4.f;              // blocks above the ground
    float scale = 1.f;
    std::vector<Part> parts;
};

// A palette change partway through (startPct 0-100)
struct Section {
    float startPct = 0.f;
    Palette palette;
};

struct DecoPlan {
    std::string name;
    Palette pal;
    float glowOpacity = 0.45f;
    float backOpacity = 0.35f;
    Outline outline = Outline::Thin;
    Glow glow = Glow::Soft;
    Fill fill = Fill::Shaded;
    Back back = Back::Diamonds;
    Particles particles = Particles::None;
    Motion motion = Motion::Drift;
    Pulse pulse = Pulse::Beat;
    float density = 0.6f;
    float bpm = 0.f;
    std::vector<Section> sections;
    std::vector<Motif> motifs;
    std::string notes;
};

// Which parts of a plan to actually place (the toggles in the window)
struct DecoOptions {
    bool colors = true;
    bool blocks = true;
    bool outlines = true;
    bool glow = true;
    bool background = true;
    bool particles = true;
    bool pulses = true;
    bool motifs = true;
    unsigned seed = 1;
};

std::vector<std::pair<std::string, std::string>> const& presetList(); // id, label
DecoPlan presetPlan(std::string const& id);
geode::Result<DecoPlan> parsePlan(std::string const& text);

// ---------------------------------------------------------------- layout
enum class GameMode { Cube, Ship, Ball, Ufo, Wave };

struct Segment {
    GameMode mode = GameMode::Cube;
    int length = 40;     // blocks
    int difficulty = 4;  // 1-10
};

struct LayoutPlan {
    std::vector<Segment> segments;
    std::string notes;
};

geode::Result<LayoutPlan> parseLayout(std::string const& text);
LayoutPlan fallbackLayout(int lengthBlocks, int difficulty);

// Pulls the first {...} block out of a reply that may have extra text around it
std::string extractJson(std::string const& text);

} // namespace deco
