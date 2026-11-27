// Deco AI - plan parsing (Claude's JSON) and built-in presets.

#include "Plan.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

using namespace geode::prelude;

namespace deco {

// ---------------------------------------------------------------- colours

Rgb mix(Rgb a, Rgb b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    auto m = [t](uint8_t x, uint8_t y) { return uint8_t(std::lround(x + (y - x) * t)); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b)};
}

Rgb shade(Rgb a, float f) {
    auto s = [f](uint8_t x) { return uint8_t(std::clamp<long>(std::lround(x * f), 0, 255)); };
    return {s(a.r), s(a.g), s(a.b)};
}

std::optional<Rgb> parseHex(std::string const& in) {
    std::string s;
    for (char c : in)
        if (std::isxdigit(static_cast<unsigned char>(c))) s += c;
    if (s.size() == 3) s = {s[0], s[0], s[1], s[1], s[2], s[2]};
    if (s.size() != 6) return std::nullopt;
    auto v = std::strtoul(s.c_str(), nullptr, 16);
    return Rgb{uint8_t((v >> 16) & 0xff), uint8_t((v >> 8) & 0xff), uint8_t(v & 0xff)};
}

std::string toHex(Rgb c) { return fmt::format("#{:02x}{:02x}{:02x}", c.r, c.g, c.b); }

std::vector<std::string> const& paletteKeys() {
    static std::vector<std::string> const keys = {"bg", "ground", "ground2", "line", "mg", "fill", "outline", "glow", "back", "accent"};
    return keys;
}

// ---------------------------------------------------------------- json helpers

static std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static bool has(matjson::Value const& o, char const* k) { return o.isObject() && o.contains(k); }

static double num(matjson::Value const& o, char const* k, double def) {
    if (!has(o, k)) return def;
    auto const& v = o[k];
    if (v.isNumber()) return v.asDouble().unwrapOr(def);
    if (v.isString()) {
        auto s = v.asString().unwrapOr("");
        char* end = nullptr;
        double d = std::strtod(s.c_str(), &end);
        if (end != s.c_str()) return d;
    }
    return def;
}

static std::string str(matjson::Value const& o, char const* k, std::string const& def = "") {
    if (!has(o, k)) return def;
    auto const& v = o[k];
    if (v.isString()) return v.asString().unwrapOr(def);
    return def;
}

static Palette readPalette(matjson::Value const& o) {
    Palette p;
    if (!o.isObject()) return p;
    for (auto const& key : paletteKeys()) {
        if (!o.contains(key)) continue;
        auto const& v = o[key];
        if (!v.isString()) continue;
        if (auto c = parseHex(v.asString().unwrapOr(""))) p[key] = *c;
    }
    return p;
}

template <class E>
static E pick(std::string const& s, std::initializer_list<std::pair<char const*, E>> opts, E def) {
    auto l = lower(s);
    for (auto const& [name, val] : opts)
        if (l == name) return val;
    return def;
}

std::string extractJson(std::string const& text) {
    auto a = text.find('{');
    auto b = text.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b < a) return text;
    return text.substr(a, b - a + 1);
}

// ---------------------------------------------------------------- presets

static Palette pal(std::initializer_list<std::pair<char const*, char const*>> list) {
    Palette p;
    for (auto const& [k, v] : list)
        if (auto c = parseHex(v)) p[k] = *c;
    return p;
}

std::vector<std::pair<std::string, std::string>> const& presetList() {
    static std::vector<std::pair<std::string, std::string>> const list = {
        {"claude", "Claude"}, {"bloodbath", "Bloodbath"}, {"neon", "Neon"}, {"frost", "Frost"},
        {"toxic", "Toxic"},   {"void", "Void"},           {"sunset", "Sunset"},
    };
    return list;
}

static void fillMissing(Palette& p) {
    // Any colour the plan left out is derived from the ones it gave
    Rgb fill = p.contains("fill") ? p["fill"] : Rgb{20, 20, 30};
    Rgb outline = p.contains("outline") ? p["outline"] : Rgb{255, 255, 255};
    if (!p.contains("bg")) p["bg"] = shade(fill, 0.5f);
    if (!p.contains("fill")) p["fill"] = fill;
    if (!p.contains("outline")) p["outline"] = outline;
    if (!p.contains("glow")) p["glow"] = outline;
    if (!p.contains("accent")) p["accent"] = mix(outline, {255, 255, 255}, 0.3f);
    if (!p.contains("back")) p["back"] = mix(p["bg"], p["glow"], 0.35f);
    if (!p.contains("ground")) p["ground"] = shade(p["bg"], 0.8f);
    if (!p.contains("ground2")) p["ground2"] = p["ground"];
    if (!p.contains("line")) p["line"] = outline;
    if (!p.contains("mg")) p["mg"] = mix(p["bg"], p["back"], 0.5f);
}

DecoPlan presetPlan(std::string const& id) {
    DecoPlan p;
    p.name = id;
    if (id == "neon") {
        p.pal = pal({{"bg", "#05001a"}, {"ground", "#0a0033"}, {"ground2", "#12004d"}, {"line", "#00f0ff"}, {"mg", "#1a0a4d"},
                     {"fill", "#0b0630"}, {"outline", "#00e5ff"}, {"glow", "#ff00d4"}, {"back", "#3a0a7a"}, {"accent", "#ff4df0"}});
        p.outline = Outline::Thin, p.glow = Glow::Strong, p.fill = Fill::Grid, p.back = Back::Mixed;
        p.particles = Particles::Sparks, p.motion = Motion::Rotate, p.pulse = Pulse::Beat, p.density = 0.65f;
        p.sections.push_back({50.f, pal({{"outline", "#ff00d4"}, {"glow", "#00e5ff"}, {"accent", "#7df9ff"}})});
    } else if (id == "frost") {
        p.pal = pal({{"bg", "#031426"}, {"ground", "#062a45"}, {"ground2", "#0a3a5e"}, {"line", "#bdf4ff"}, {"mg", "#0d3350"},
                     {"fill", "#0c3a5c"}, {"outline", "#e6fbff"}, {"glow", "#5cd6ff"}, {"back", "#1c5d8a"}, {"accent", "#ffffff"}});
        p.outline = Outline::Double, p.glow = Glow::Soft, p.fill = Fill::Inset, p.back = Back::Stars;
        p.particles = Particles::Snow, p.motion = Motion::Drift, p.pulse = Pulse::HalfBeat, p.density = 0.6f;
    } else if (id == "toxic") {
        p.pal = pal({{"bg", "#020a02"}, {"ground", "#061a06"}, {"ground2", "#0a260a"}, {"line", "#9dff00"}, {"mg", "#0d260d"},
                     {"fill", "#0a1f0a"}, {"outline", "#b6ff2e"}, {"glow", "#4dff4d"}, {"back", "#1f4d12"}, {"accent", "#e1ff6b"}});
        p.outline = Outline::Thick, p.glow = Glow::Strong, p.fill = Fill::Bricks, p.back = Back::Pillars;
        p.particles = Particles::Sparks, p.motion = Motion::Bounce, p.pulse = Pulse::OffBeat, p.density = 0.6f;
    } else if (id == "void") {
        p.pal = pal({{"bg", "#000000"}, {"ground", "#000000"}, {"ground2", "#050505"}, {"line", "#ffffff"}, {"mg", "#08080f"},
                     {"fill", "#000000"}, {"outline", "#ffffff"}, {"glow", "#8a8aff"}, {"back", "#1a1a2e"}, {"accent", "#c9c9ff"}});
        p.outline = Outline::Thin, p.glow = Glow::Soft, p.fill = Fill::Plain, p.back = Back::Frames;
        p.particles = Particles::Dust, p.motion = Motion::Rotate, p.pulse = Pulse::Beat, p.density = 0.5f;
    } else if (id == "sunset") {
        p.pal = pal({{"bg", "#1a0510"}, {"ground", "#2a0a14"}, {"ground2", "#3b0f1e"}, {"line", "#ffb347"}, {"mg", "#4a1528"},
                     {"fill", "#3b0f1e"}, {"outline", "#ffcf6b"}, {"glow", "#ff6a3d"}, {"back", "#6b1f3a"}, {"accent", "#ffe29a"}});
        p.outline = Outline::Thin, p.glow = Glow::Soft, p.fill = Fill::Dots, p.back = Back::Pillars;
        p.particles = Particles::Dust, p.motion = Motion::Drift, p.pulse = Pulse::HalfBeat, p.density = 0.55f;
    } else {
        // bloodbath: black and red horror, hard glow, embers, thorns
        p.name = "bloodbath";
        p.pal = pal({{"bg", "#070000"}, {"ground", "#0a0000"}, {"ground2", "#140000"}, {"line", "#ff1a1a"}, {"mg", "#1a0000"},
                     {"fill", "#120404"}, {"outline", "#ff2a2a"}, {"glow", "#ff0000"}, {"back", "#5a0000"}, {"accent", "#ff6a3d"}});
        p.outline = Outline::Thick, p.glow = Glow::Strong, p.fill = Fill::Shaded, p.back = Back::Diamonds;
        p.particles = Particles::Embers, p.motion = Motion::Drift, p.pulse = Pulse::Beat, p.density = 0.75f;
        p.glowOpacity = 0.55f;
        p.sections.push_back({55.f, pal({{"glow", "#ff3d00"}, {"accent", "#ffd0a0"}, {"back", "#7a0a00"}, {"outline", "#ff5a3a"}})});
        Motif thorn;
        thorn.layer = "back";
        thorn.every = 14.f;
        thorn.y = 6.f;
        thorn.scale = 1.2f;
        thorn.parts = {{0, 0, 0.35f, 3.2f, 0, "glow"}, {0, 0, 0.35f, 3.2f, 90, "glow"}, {0, 0, 1.1f, 1.1f, 45, "accent"},
                       {0, 0, 0.6f, 0.6f, 45, "bg"},   {0, 0, 0.18f, 5.f, 45, "back"},   {0, 0, 0.18f, 5.f, -45, "back"}};
        p.motifs.push_back(thorn);
    }
    fillMissing(p.pal);
    return p;
}

// ---------------------------------------------------------------- Claude's plan

Result<DecoPlan> parsePlan(std::string const& text) {
    auto parsed = matjson::parse(extractJson(text));
    if (!parsed) return Err("Claude's reply wasn't valid JSON");
    auto json = parsed.unwrap();
    if (!json.isObject()) return Err("Claude's reply wasn't a JSON object");

    DecoPlan p;
    p.name = str(json, "name", "Claude deco");
    p.notes = str(json, "notes");
    if (has(json, "palette")) p.pal = readPalette(json["palette"]);
    fillMissing(p.pal);

    p.glowOpacity = float(std::clamp(num(json, "glowOpacity", 0.45), 0.05, 1.0));
    p.backOpacity = float(std::clamp(num(json, "backOpacity", 0.35), 0.05, 1.0));
    p.density = float(std::clamp(num(json, "density", 0.6), 0.0, 1.0));
    p.bpm = float(std::clamp(num(json, "bpm", 0.0), 0.0, 400.0));

    matjson::Value style = has(json, "style") ? json["style"] : json;
    p.outline = pick<Outline>(str(style, "outline", "thin"),
                              {{"none", Outline::None}, {"thin", Outline::Thin}, {"thick", Outline::Thick}, {"double", Outline::Double}},
                              Outline::Thin);
    p.glow = pick<Glow>(str(style, "glow", "soft"), {{"none", Glow::None}, {"soft", Glow::Soft}, {"strong", Glow::Strong}}, Glow::Soft);
    p.fill = pick<Fill>(str(style, "fill", "shaded"),
                        {{"plain", Fill::Plain},
                         {"solid", Fill::Plain},
                         {"shaded", Fill::Shaded},
                         {"grid", Fill::Grid},
                         {"bricks", Fill::Bricks},
                         {"dots", Fill::Dots},
                         {"inset", Fill::Inset}},
                        Fill::Shaded);
    p.back = pick<Back>(str(style, "back", "diamonds"),
                        {{"none", Back::None},
                         {"pillars", Back::Pillars},
                         {"diamonds", Back::Diamonds},
                         {"stars", Back::Stars},
                         {"grid", Back::Grid},
                         {"frames", Back::Frames},
                         {"mixed", Back::Mixed}},
                        Back::Diamonds);
    p.particles = pick<Particles>(str(style, "particles", "none"),
                                  {{"none", Particles::None},
                                   {"embers", Particles::Embers},
                                   {"rain", Particles::Rain},
                                   {"snow", Particles::Snow},
                                   {"dust", Particles::Dust},
                                   {"sparks", Particles::Sparks}},
                                  Particles::None);
    p.motion = pick<Motion>(str(style, "motion", "drift"),
                            {{"still", Motion::Still}, {"drift", Motion::Drift}, {"rotate", Motion::Rotate}, {"bounce", Motion::Bounce}},
                            Motion::Drift);
    p.pulse = pick<Pulse>(str(style, "pulse", "beat"),
                          {{"none", Pulse::None}, {"beat", Pulse::Beat}, {"halfbeat", Pulse::HalfBeat}, {"offbeat", Pulse::OffBeat}},
                          Pulse::Beat);

    if (has(json, "sections") && json["sections"].isArray()) {
        auto const& arr = json["sections"];
        for (size_t i = 0; i < arr.size() && i < 8; i++) {
            auto const& s = arr[i];
            Section sec;
            sec.startPct = float(std::clamp(num(s, "startPct", 0.0), 0.0, 100.0));
            if (has(s, "palette")) sec.palette = readPalette(s["palette"]);
            if (!sec.palette.empty() && sec.startPct > 0.5f) p.sections.push_back(sec);
        }
        std::sort(p.sections.begin(), p.sections.end(), [](auto& a, auto& b) { return a.startPct < b.startPct; });
    }

    if (has(json, "motifs") && json["motifs"].isArray()) {
        auto const& arr = json["motifs"];
        for (size_t i = 0; i < arr.size() && i < 3; i++) {
            auto const& m = arr[i];
            Motif mo;
            mo.layer = lower(str(m, "layer", "back")) == "front" ? "front" : "back";
            mo.every = float(std::clamp(num(m, "every", 16.0), 4.0, 200.0));
            mo.y = float(std::clamp(num(m, "y", 4.0), -2.0, 14.0));
            mo.scale = float(std::clamp(num(m, "scale", 1.0), 0.2, 4.0));
            if (has(m, "parts") && m["parts"].isArray()) {
                auto const& parts = m["parts"];
                for (size_t j = 0; j < parts.size() && j < 24; j++) {
                    auto const& q = parts[j];
                    Part part;
                    part.x = float(std::clamp(num(q, "x", 0.0), -8.0, 8.0));
                    part.y = float(std::clamp(num(q, "y", 0.0), -8.0, 8.0));
                    part.w = float(std::clamp(num(q, "w", 1.0), 0.05, 12.0));
                    part.h = float(std::clamp(num(q, "h", 1.0), 0.05, 12.0));
                    part.rot = float(num(q, "rot", 0.0));
                    part.col = lower(str(q, "col", "accent"));
                    if (std::find(paletteKeys().begin(), paletteKeys().end(), part.col) == paletteKeys().end()) part.col = "accent";
                    mo.parts.push_back(part);
                }
            }
            if (!mo.parts.empty()) p.motifs.push_back(mo);
        }
    }
    return Ok(p);
}

// ---------------------------------------------------------------- layout

Result<LayoutPlan> parseLayout(std::string const& text) {
    auto parsed = matjson::parse(extractJson(text));
    if (!parsed) return Err("Claude's reply wasn't valid JSON");
    auto json = parsed.unwrap();
    if (!has(json, "segments") || !json["segments"].isArray()) return Err("Claude's reply had no segments");
    LayoutPlan lp;
    lp.notes = str(json, "notes");
    auto const& arr = json["segments"];
    int total = 0;
    for (size_t i = 0; i < arr.size() && i < 24; i++) {
        auto const& s = arr[i];
        Segment seg;
        seg.mode = pick<GameMode>(str(s, "mode", "cube"),
                                  {{"cube", GameMode::Cube},
                                   {"ship", GameMode::Ship},
                                   {"ball", GameMode::Ball},
                                   {"ufo", GameMode::Ufo},
                                   {"wave", GameMode::Wave}},
                                  GameMode::Cube);
        seg.length = int(std::clamp(num(s, "length", 40.0), 10.0, 200.0));
        seg.difficulty = int(std::clamp(num(s, "difficulty", 4.0), 1.0, 10.0));
        if (total + seg.length > 600) break;
        total += seg.length;
        lp.segments.push_back(seg);
    }
    if (lp.segments.empty()) return Err("Claude's reply had no usable segments");
    return Ok(lp);
}

LayoutPlan fallbackLayout(int lengthBlocks, int difficulty) {
    LayoutPlan lp;
    int l = std::clamp(lengthBlocks, 30, 600);
    int d = std::clamp(difficulty, 1, 10);
    lp.segments = {{GameMode::Cube, l * 35 / 100, d}, {GameMode::Ship, l * 25 / 100, d}, {GameMode::Cube, l * 20 / 100, d},
                   {GameMode::Wave, l * 20 / 100, d}};
    lp.notes = "Built-in layout (no Claude): cube, ship, cube, wave.";
    return lp;
}

} // namespace deco
