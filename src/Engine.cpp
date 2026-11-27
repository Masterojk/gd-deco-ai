// Deco AI - the deco engine.
//
// How decorating works:
//  1. Scan the level: which 30x30 cells are solid, where hazards and speed
//     portals are, which colour channels are already used.
//  2. Recolour: colour triggers set BG/ground/line plus 11 Deco AI channels.
//  3. Blocks: every solid area gets a non-solid overlay - shaded fill by depth
//     (edge cells lighter, deep cells darker), an optional pattern, outlines on
//     the exposed edges and a glow outside them. The original blocks stay, so
//     gameplay and hitboxes don't change.
//  4. Background: big shapes on B3/B4 with low-opacity channels, moving slower
//     than the player (parallax), spinning or bouncing.
//  5. Particles, beat pulses, palette changes per section and Claude's motifs.
// Everything placed gets a fresh "marker" group so it can be undone or
// selected as one piece.

#include "Engine.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <map>
#include <numbers>
#include <random>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "Catalog.hpp"

using namespace geode::prelude;

namespace deco {

static constexpr float B = 30.f;       // one block
static constexpr float GROUND = 0.f;   // top of the ground in level units
static constexpr int GROUND_CY = 0;    // first cell row above the ground
static constexpr int MAX_RUN = 8;      // longest merged bar, in blocks (long objects get culled)

// Z layers as GD stores them
static constexpr int Z_B4 = -3, Z_B3 = -1, Z_B2 = 1, Z_B1 = 3, Z_T1 = 5;

// ---------------------------------------------------------------- helpers

static int64_t cellKey(int cx, int cy) { return (int64_t(cx) << 32) ^ int64_t(uint32_t(cy)); }

static std::string fnum(float v) {
    auto s = fmt::format("{:.2f}", v);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

// One object in GD's save format: key,value,key,value...
struct Obj {
    std::string s;
    explicit Obj(int id) { s = fmt::format("1,{}", id); }
    Obj& f(int k, float v) {
        s += fmt::format(",{},{}", k, fnum(v));
        return *this;
    }
    Obj& i(int k, int v) {
        s += fmt::format(",{},{}", k, v);
        return *this;
    }
    Obj& str(int k, std::string const& v) {
        s += fmt::format(",{},{}", k, v);
        return *this;
    }
};

// ---------------------------------------------------------------- speed / time

static float speedUnits(Speed s) {
    switch (s) {
        case Speed::Slow: return 251.16f;
        case Speed::Fast: return 387.42f;
        case Speed::Faster: return 468.f;
        case Speed::Fastest: return 576.f;
        default: return 311.58f;
    }
}

static float portalSpeed(int id) {
    switch (id) {
        case 200: return 251.16f;
        case 201: return 311.58f;
        case 202: return 387.42f;
        case 203: return 468.f;
        case 1334: return 576.f;
        default: return 0.f;
    }
}

struct SpeedMap {
    std::vector<std::pair<float, float>> segs; // x where it starts, units per second

    float timeAt(float x) const {
        float t = 0.f;
        for (size_t i = 0; i < segs.size(); i++) {
            float a = segs[i].first;
            float b = i + 1 < segs.size() ? segs[i + 1].first : 1e9f;
            if (x <= a) break;
            t += (std::min(x, b) - a) / segs[i].second;
        }
        return t;
    }
    float xAt(float t) const {
        float acc = 0.f;
        for (size_t i = 0; i < segs.size(); i++) {
            float a = segs[i].first;
            float b = i + 1 < segs.size() ? segs[i + 1].first : 1e9f;
            float dt = (b - a) / segs[i].second;
            if (t <= acc + dt) return a + (t - acc) * segs[i].second;
            acc += dt;
        }
        return segs.empty() ? t * 311.58f : segs.back().first;
    }
};

// ---------------------------------------------------------------- level scan

struct Scan {
    bool ok = false;
    bool selection = false;
    std::string error;
    float x0 = 0, x1 = 0, yTop = GROUND + B * 12;
    std::unordered_set<int64_t> solid, hazard;
    std::set<int> usedChannels;
    std::set<std::string> modes;
    SpeedMap speed;
    float maxObjX = 0.f;
    int hazardCount = 0;
    int maxCy = GROUND_CY;
};

static std::string saveKey(std::string const& save, int key) {
    // save strings are "1,899,2,15,3,105,..." - find the value for key
    size_t pos = 0;
    int idx = 0;
    std::string k;
    while (pos <= save.size()) {
        size_t next = save.find(',', pos);
        if (next == std::string::npos) next = save.size();
        auto part = save.substr(pos, next - pos);
        if (idx % 2 == 0) k = part;
        else if (k == std::to_string(key)) return part;
        idx++;
        pos = next + 1;
    }
    return "";
}

// Positions read from objects in the editor are 90 units (3 blocks) higher than
// the save format that createObjectsFromString uses, so shift them down.
static constexpr float EDITOR_Y_OFFSET = 90.f;

static void markCells(std::unordered_set<int64_t>& set, CCRect rect, int* maxCy = nullptr) {
    CCRect r = rect;
    r.origin.y -= EDITOR_Y_OFFSET;
    int cx0 = int(std::floor((r.getMinX() + 1.f) / B));
    int cx1 = int(std::floor((r.getMaxX() - 1.f) / B));
    int cy0 = int(std::floor((r.getMinY() + 1.f) / B));
    int cy1 = int(std::floor((r.getMaxY() - 1.f) / B));
    if (cx1 - cx0 > 400 || cy1 - cy0 > 200) return; // giant scaled objects: ignore
    for (int cx = cx0; cx <= cx1; cx++)
        for (int cy = cy0; cy <= cy1; cy++) {
            float mx = cx * B + B / 2.f, my = cy * B + B / 2.f;
            if (r.containsPoint({mx, my})) {
                set.insert(cellKey(cx, cy));
                if (maxCy && cy > *maxCy) *maxCy = cy;
            }
        }
}

static char const* modeOfPortal(int id) {
    switch (id) {
        case 12: return "cube";
        case 13: return "ship";
        case 47: return "ball";
        case 111: return "ufo";
        case 660: return "wave";
        case 745: return "robot";
        case 1331: return "spider";
        case 1933: return "swing";
        default: return nullptr;
    }
}

static Scan scanLevel(bool selectionOnly) {
    Scan s;
    auto ed = LevelEditorLayer::get();
    if (!ed) {
        s.error = "Open the level editor first";
        return s;
    }

    // Range: the selection if there is one (and the user wants it), else the whole level
    float minX = 1e9f, maxX = -1e9f;
    if (selectionOnly && ed->m_editorUI) {
        if (auto sel = ed->m_editorUI->getSelectedObjects(); sel && sel->count() > 0) {
            for (auto obj : CCArrayExt<GameObject*>(sel)) {
                auto const& r = obj->getObjectRect();
                minX = std::min(minX, std::min(r.getMinX(), obj->getPositionX()));
                maxX = std::max(maxX, std::max(r.getMaxX(), obj->getPositionX()));
            }
            s.selection = true;
        }
    }

    std::vector<std::pair<float, float>> portals;
    Speed start = ed->m_levelSettings ? ed->m_levelSettings->m_startSpeed : Speed::Normal;
    s.modes.insert("cube");
    for (auto obj : CCArrayExt<GameObject*>(ed->m_objects)) {
        if (!obj) continue;
        int id = obj->m_objectID;
        float x = obj->getPositionX();
        s.maxObjX = std::max(s.maxObjX, x);
        if (!s.selection) {
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
        }
        if (obj->m_baseColor) s.usedChannels.insert(obj->m_baseColor->m_colorID);
        if (obj->m_detailColor) s.usedChannels.insert(obj->m_detailColor->m_colorID);
        if (float v = portalSpeed(id); v > 0.f) portals.push_back({x, v});
        if (auto m = modeOfPortal(id)) s.modes.insert(m);
        if (id == 899 || id == 1006) { // colour / pulse trigger targets
            auto target = saveKey(std::string(obj->getSaveString(ed)), id == 899 ? 23 : 51);
            if (!target.empty()) s.usedChannels.insert(std::atoi(target.c_str()));
        }
        if (obj->m_isNoTouch) continue; // our own overlays (and other passable deco)
        if (obj->m_objectType == GameObjectType::Solid) markCells(s.solid, obj->getObjectRect(), &s.maxCy);
        else if (obj->m_objectType == GameObjectType::Hazard) {
            markCells(s.hazard, obj->getObjectRect());
            s.hazardCount++;
        }
    }

    if (minX > maxX) {
        s.error = "The level is empty - build some layout first (or use the Layout tab)";
        return s;
    }

    std::sort(portals.begin(), portals.end());
    s.speed.segs.push_back({0.f, speedUnits(start)});
    for (auto const& p : portals) s.speed.segs.push_back(p);

    s.x0 = s.selection ? std::max(0.f, minX - B * 2) : 0.f;
    s.x1 = maxX + B * (s.selection ? 2 : 12);
    s.yTop = std::clamp(std::max(GROUND + B * 12, (s.maxCy + 5) * B), GROUND + B * 12, GROUND + B * 30);
    s.ok = true;
    return s;
}

LevelSummary summarizeLevel(bool selectionOnly) {
    LevelSummary out;
    auto s = scanLevel(selectionOnly);
    out.ok = s.ok;
    out.error = s.error;
    if (!s.ok) return out;
    out.selection = s.selection;
    out.x0 = s.x0;
    out.x1 = s.x1;
    out.lengthBlocks = int((s.x1 - s.x0) / B);
    out.seconds = s.speed.timeAt(s.x1) - s.speed.timeAt(s.x0);
    int cells = 0;
    for (auto k : s.solid) {
        int cx = int(k >> 32);
        if (cx * B >= s.x0 && cx * B <= s.x1) cells++;
    }
    out.solidCells = cells;
    out.hazards = s.hazardCount;
    out.maxHeightBlocks = s.maxCy - GROUND_CY + 1;
    std::string modes;
    for (auto const& m : s.modes) modes += (modes.empty() ? "" : ", ") + m;
    out.modes = modes;
    return out;
}

// ---------------------------------------------------------------- builder

struct Channels {
    int fill1, fill2, fill3, outline, detail, glowA, glowB, back, back2, accent, accent2;
};

struct Builder {
    std::string out;
    int count = 0;
    int marker = 0;
    int editorLayer = 0;
    Catalog cat;
    float trigY = 0.f;
    int trigRow = 0;

    std::string groups(std::vector<int> const& extra) const {
        std::string g = std::to_string(marker);
        for (int e : extra)
            if (e > 0) g += "." + std::to_string(e);
        return g;
    }
    void add(Obj& o, std::vector<int> const& extra = {}) {
        o.str(57, groups(extra));
        if (editorLayer > 0) o.i(20, editorLayer);
        out += o.s;
        out += ';';
        count++;
    }
    // Non-solid colour square (rotation in degrees, clockwise like GD)
    void sq(float x, float y, float w, float h, float rot, int ch, int z, int zorder, std::vector<int> const& extra = {}) {
        if (w <= 0.1f || h <= 0.1f) return;
        Obj o(cat.square);
        o.f(2, x).f(3, y);
        if (std::abs(rot) > 0.01f) o.f(6, rot);
        o.i(21, ch).i(22, ch).i(24, z).i(25, zorder);
        o.f(128, w / cat.squareSize).f(129, h / cat.squareSize);
        o.i(96, 1); // no glow
        add(o, extra);
    }
    // A rectangle whose centre is offset (lx, ly) from (cx, cy), all rotated by `angle`
    void sqAround(float cx, float cy, float lx, float ly, float w, float h, float rot, float angle, int ch, int z, int zorder,
                  std::vector<int> const& extra = {}) {
        float a = angle * std::numbers::pi_v<float> / 180.f;
        // GD rotates clockwise
        float x = cx + lx * std::cos(a) + ly * std::sin(a);
        float y = cy - lx * std::sin(a) + ly * std::cos(a);
        sq(x, y, w, h, rot + angle, ch, z, zorder, extra);
    }

    float nextTrigY() {
        float y = trigY + float(trigRow % 6) * 15.f;
        trigRow++;
        return y;
    }
    void color(float x, int ch, Rgb c, float alpha, float dur) {
        Obj o(899);
        o.f(2, x).f(3, nextTrigY()).i(7, c.r).i(8, c.g).i(9, c.b).f(10, dur).i(23, ch).f(35, std::clamp(alpha, 0.f, 1.f));
        add(o);
    }
    void pulse(float x, int ch, Rgb c, float fin, float hold, float fout) {
        Obj o(1006);
        o.f(2, x).f(3, nextTrigY()).i(7, c.r).i(8, c.g).i(9, c.b).f(45, fin).f(46, hold).f(47, fout).i(48, 0).i(51, ch).i(52, 0);
        add(o);
    }
    void move(float x, int group, float dx, float dy, float dur, int easing) {
        Obj o(901);
        o.f(2, x).f(3, nextTrigY()).f(28, dx).f(29, dy).f(10, dur).i(51, group);
        if (easing) o.i(30, easing);
        add(o);
    }
    void rotate(float x, int group, float degrees, float dur) {
        Obj o(1346);
        o.f(2, x).f(3, nextTrigY()).i(51, group).f(68, degrees).f(10, dur);
        add(o);
    }
};

// ---------------------------------------------------------------- runs

struct Run {
    int line; // the fixed cell coordinate
    int a, b; // inclusive range along the run
};

static std::vector<Run> makeRuns(std::map<int, std::vector<int>>& lines) {
    std::vector<Run> runs;
    for (auto& [line, v] : lines) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        size_t i = 0;
        while (i < v.size()) {
            int a = v[i];
            int b = a;
            size_t j = i + 1;
            while (j < v.size() && v[j] == b + 1 && b - a + 1 < MAX_RUN) b = v[j++];
            runs.push_back({line, a, b});
            i = j;
        }
    }
    return runs;
}

// ---------------------------------------------------------------- deco

struct Ctx {
    DecoPlan const* plan;
    DecoOptions const* opt;
    Scan* scan;
    Builder* b;
    Channels ch;
    std::mt19937 rng;
    float density;
    LevelEditorLayer* ed;

    float rand(float a, float b2) { return std::uniform_real_distribution<float>(a, b2)(rng); }
    bool chance(float p) { return rand(0.f, 1.f) < p; }
};

// Fresh group IDs, never reused within one run
struct Groups {
    LevelEditorLayer* ed;
    gd::unordered_set<int> used;
    int next() {
        int g = ed->getNextFreeGroupID(used);
        used.insert(g);
        return g;
    }
};

static Rgb colorOf(Palette const& p, char const* k) {
    auto it = p.find(k);
    return it != p.end() ? it->second : Rgb{255, 255, 255};
}

// Every channel colour a palette implies
static std::vector<std::tuple<int, Rgb, float>> channelColors(Palette const& p, DecoPlan const& plan, Channels const& ch) {
    Rgb fill = colorOf(p, "fill"), outline = colorOf(p, "outline"), glow = colorOf(p, "glow");
    Rgb back = colorOf(p, "back"), accent = colorOf(p, "accent"), mg = colorOf(p, "mg");
    std::vector<std::tuple<int, Rgb, float>> out = {
        {1000, colorOf(p, "bg"), 1.f},
        {1001, colorOf(p, "ground"), 1.f},
        {1009, colorOf(p, "ground2"), 1.f},
        {1002, colorOf(p, "line"), 1.f},
        {1013, mg, 1.f},
        {1014, shade(mg, 0.75f), 1.f},
        {ch.fill1, mix(fill, outline, 0.10f), 1.f},
        {ch.fill2, fill, 1.f},
        {ch.fill3, mix(fill, Rgb{0, 0, 0}, 0.5f), 1.f},
        {ch.outline, outline, 1.f},
        {ch.detail, mix(outline, fill, 0.5f), 0.55f},
        {ch.glowA, glow, plan.glowOpacity},
        {ch.glowB, glow, plan.glowOpacity * 0.45f},
        {ch.back, back, plan.backOpacity},
        {ch.back2, back, plan.backOpacity * 0.55f},
        {ch.accent, accent, 0.85f},
        {ch.accent2, accent, 0.4f},
    };
    // Colour-trigger opacity doesn't reliably reach objects, so blend each
    // see-through layer over the background colour ourselves and keep it opaque.
    Rgb bg = colorOf(p, "bg");
    for (auto& [chn, col, alpha] : out) {
        if (alpha < 0.999f) col = mix(bg, col, alpha);
        alpha = 1.f;
    }
    return out;
}

static int channelForName(std::string const& name, Channels const& ch) {
    if (name == "bg") return 1000;
    if (name == "ground") return 1001;
    if (name == "ground2") return 1009;
    if (name == "line") return 1002;
    if (name == "mg") return 1013;
    if (name == "fill") return ch.fill2;
    if (name == "outline") return ch.outline;
    if (name == "glow") return ch.glowA;
    if (name == "back") return ch.back;
    return ch.accent;
}

static void placeColors(Ctx& c) {
    auto const& plan = *c.plan;
    float x = c.scan->x0;
    for (auto const& [chn, col, alpha] : channelColors(plan.pal, plan, c.ch)) c.b->color(x, chn, col, alpha, 0.f);

    // Palette changes partway through: colour triggers that fade over ~0.6s
    Palette current = plan.pal;
    for (auto const& sec : plan.sections) {
        Palette next = current;
        for (auto const& [k, v] : sec.palette) next[k] = v;
        float sx = c.scan->x0 + (c.scan->x1 - c.scan->x0) * sec.startPct / 100.f;
        auto before = channelColors(current, plan, c.ch);
        auto after = channelColors(next, plan, c.ch);
        for (size_t i = 0; i < after.size(); i++) {
            auto const& [chn, col, alpha] = after[i];
            auto const& old = std::get<1>(before[i]);
            if (old.r == col.r && old.g == col.g && old.b == col.b) continue;
            c.b->color(sx, chn, col, alpha, 0.6f);
        }
        current = next;
    }
}

static bool inRange(Ctx& c, int cx) {
    float x = cx * B;
    return x >= c.scan->x0 - B && x <= c.scan->x1 + B;
}

static void placeBlocks(Ctx& c) {
    auto& solid = c.scan->solid;
    auto isSolid = [&](int cx, int cy) { return cy < GROUND_CY || solid.contains(cellKey(cx, cy)); };
    auto isCell = [&](int cx, int cy) { return cy >= GROUND_CY && solid.contains(cellKey(cx, cy)); };

    std::vector<std::pair<int, int>> cells;
    for (auto k : solid) {
        int cx = int(k >> 32);
        int cy = int(int32_t(uint32_t(k & 0xffffffff)));
        if (cy < GROUND_CY || !inRange(c, cx)) continue;
        cells.push_back({cx, cy});
    }
    if (cells.empty()) return;
    auto const& plan = *c.plan;
    auto const& opt = *c.opt;

    // Depth: 1 = touches air, 2 = one in, 3 = deeper
    std::unordered_map<int64_t, int> depth;
    std::vector<std::pair<int, int>> frontier;
    for (auto [cx, cy] : cells) {
        if (!isSolid(cx + 1, cy) || !isSolid(cx - 1, cy) || !isSolid(cx, cy + 1) || !isSolid(cx, cy - 1)) {
            depth[cellKey(cx, cy)] = 1;
            frontier.push_back({cx, cy});
        }
    }
    for (int d = 2; d <= 3 && !frontier.empty(); d++) {
        std::vector<std::pair<int, int>> next;
        for (auto [cx, cy] : frontier) {
            int const dx[] = {1, -1, 0, 0}, dy[] = {0, 0, 1, -1};
            for (int i = 0; i < 4; i++) {
                int nx = cx + dx[i], ny = cy + dy[i];
                if (!isCell(nx, ny) || depth.contains(cellKey(nx, ny))) continue;
                depth[cellKey(nx, ny)] = d;
                next.push_back({nx, ny});
            }
        }
        frontier = std::move(next);
    }
    auto depthOf = [&](int cx, int cy) {
        auto it = depth.find(cellKey(cx, cy));
        return it == depth.end() ? 3 : it->second;
    };

    // Fill: horizontal runs per row and depth
    if (opt.blocks) {
        std::map<int, std::vector<int>> rows[3];
        for (auto [cx, cy] : cells) {
            int d = plan.fill == Fill::Plain ? 2 : depthOf(cx, cy);
            rows[d - 1][cy].push_back(cx);
        }
        int const chans[3] = {c.ch.fill1, c.ch.fill2, c.ch.fill3};
        for (int d = 0; d < 3; d++)
            for (auto const& r : makeRuns(rows[d])) {
                float w = (r.b - r.a + 1) * B;
                c.b->sq(r.a * B + w / 2.f, r.line * B + B / 2.f, w + 0.5f, B + 0.5f, 0, chans[d], Z_B1, 4);
            }

        // Patterns on top of the fill
        if (plan.fill == Fill::Grid || plan.fill == Fill::Bricks) {
            std::map<int, std::vector<int>> hLines, vLines;
            for (auto [cx, cy] : cells) {
                if (isCell(cx, cy + 1)) hLines[cy + 1].push_back(cx);
                if (isCell(cx + 1, cy) && (plan.fill == Fill::Grid || (cx + cy) % 2 == 0)) vLines[cx + 1].push_back(cy);
            }
            for (auto const& r : makeRuns(hLines)) {
                float w = (r.b - r.a + 1) * B;
                c.b->sq(r.a * B + w / 2.f, r.line * B, w, 1.6f, 0, c.ch.detail, Z_B1, 5);
            }
            if (plan.fill == Fill::Grid) {
                for (auto const& r : makeRuns(vLines)) {
                    float h = (r.b - r.a + 1) * B;
                    c.b->sq(r.line * B, r.a * B + h / 2.f, 1.6f, h, 0, c.ch.detail, Z_B1, 5);
                }
            } else {
                for (auto& [x, ys] : vLines)
                    for (int cy : ys) c.b->sq(x * B, cy * B + B / 2.f, 1.6f, B, 0, c.ch.detail, Z_B1, 5);
            }
        } else if (plan.fill == Fill::Dots) {
            for (auto [cx, cy] : cells)
                if (depthOf(cx, cy) >= 2 && (cx + cy) % 2 == 0) c.b->sq(cx * B + B / 2.f, cy * B + B / 2.f, 7.f, 7.f, 45, c.ch.detail, Z_B1, 5);
        }
    }

    // Exposed edges
    std::map<int, std::vector<int>> top, bottom, left, right;
    for (auto [cx, cy] : cells) {
        if (!isSolid(cx, cy + 1)) top[cy].push_back(cx);
        if (!isSolid(cx, cy - 1)) bottom[cy].push_back(cx);
        if (!isSolid(cx - 1, cy)) left[cx].push_back(cy);
        if (!isSolid(cx + 1, cy)) right[cx].push_back(cy);
    }
    auto rTop = makeRuns(top), rBottom = makeRuns(bottom), rLeft = makeRuns(left), rRight = makeRuns(right);

    // inset > 0 moves the line into the block, < 0 out of it
    auto edgeBars = [&](float thick, float inset, float extend, int chn, int z, int zorder) {
        for (auto const& r : rTop) {
            float w = (r.b - r.a + 1) * B + extend * 2;
            c.b->sq(r.a * B - extend + w / 2.f, (r.line + 1) * B - inset - thick / 2.f, w, thick, 0, chn, z, zorder);
        }
        for (auto const& r : rBottom) {
            float w = (r.b - r.a + 1) * B + extend * 2;
            c.b->sq(r.a * B - extend + w / 2.f, r.line * B + inset + thick / 2.f, w, thick, 0, chn, z, zorder);
        }
        for (auto const& r : rLeft) {
            float h = (r.b - r.a + 1) * B + extend * 2;
            c.b->sq(r.line * B + inset + thick / 2.f, r.a * B - extend + h / 2.f, thick, h, 0, chn, z, zorder);
        }
        for (auto const& r : rRight) {
            float h = (r.b - r.a + 1) * B + extend * 2;
            c.b->sq((r.line + 1) * B - inset - thick / 2.f, r.a * B - extend + h / 2.f, thick, h, 0, chn, z, zorder);
        }
    };

    if (opt.outlines && plan.outline != Outline::None) {
        switch (plan.outline) {
            case Outline::Thin: edgeBars(3.f, 0.f, 0.f, c.ch.outline, Z_B1, 7); break;
            case Outline::Thick: edgeBars(6.f, 0.f, 0.f, c.ch.outline, Z_B1, 7); break;
            case Outline::Double:
                edgeBars(4.f, 0.f, 0.f, c.ch.outline, Z_B1, 7);
                edgeBars(2.f, 9.f, 0.f, c.ch.outline, Z_B1, 6);
                break;
            default: break;
        }
    }
    if (opt.blocks && plan.fill == Fill::Inset) edgeBars(2.f, 9.f, 0.f, c.ch.detail, Z_B1, 6);

    if (opt.glow && plan.glow != Glow::None) {
        if (plan.glow == Glow::Strong) {
            edgeBars(9.f, -9.f, 9.f, c.ch.glowA, Z_B2, 2);    // bright band hugging the edge
            edgeBars(18.f, -18.f, 18.f, c.ch.glowB, Z_B2, 1); // wide soft halo
        } else {
            edgeBars(12.f, -12.f, 12.f, c.ch.glowB, Z_B2, 1);
        }
    }
}

// Places objects that move with a constant velocity so that each one is at its
// "desired" spot at the moment the player reaches it. Objects are grouped by
// 4-second slices, each with its own move trigger, so nothing ends up far away.
struct MovingLayer {
    float vx = 0.f, vy = 0.f;
    float slice = 4.f;
    std::map<int, int> groupOfSlice;

    bool moving() const { return std::abs(vx) > 0.01f || std::abs(vy) > 0.01f; }

    // returns the group to use (0 if not moving) and adjusts x, y
    int place(Ctx& c, Groups& groups, float& x, float& y) {
        if (!moving()) return 0;
        float t0 = c.scan->speed.timeAt(c.scan->x0);
        float tp = c.scan->speed.timeAt(std::max(x, c.scan->x0)) - t0;
        int k = std::max(0, int(tp / slice));
        float into = tp - k * slice;
        x -= vx * into;
        y -= vy * into;
        auto it = groupOfSlice.find(k);
        if (it == groupOfSlice.end()) it = groupOfSlice.emplace(k, groups.next()).first;
        return it->second;
    }
    void triggers(Ctx& c) {
        float t0 = c.scan->speed.timeAt(c.scan->x0);
        float dur = slice + 5.f;
        for (auto const& [k, g] : groupOfSlice) {
            float tx = c.scan->speed.xAt(t0 + k * slice);
            c.b->move(tx, g, vx * dur, vy * dur, dur, 0);
        }
    }
};

static void placeBackground(Ctx& c, Groups& groups) {
    auto const& plan = *c.plan;
    auto& s = *c.scan;
    float len = (s.x1 - s.x0) / B;
    float top = s.yTop;
    float d = c.density;

    // Parallax: far shapes drift forward at 30% of the player's speed, near ones at 12%
    MovingLayer farL, nearL;
    if (plan.motion == Motion::Drift) {
        farL.vx = 0.30f * s.speed.segs.front().second;
        nearL.vx = 0.12f * s.speed.segs.front().second;
        farL.slice = nearL.slice = 6.f;
    }
    int spinGroup = plan.motion == Motion::Rotate ? groups.next() : 0;
    int spinGroup2 = plan.motion == Motion::Rotate ? groups.next() : 0;
    int bounceGroup = plan.motion == Motion::Bounce ? groups.next() : 0;

    auto layerFor = [&](bool isFar, float& x, float& y) -> std::vector<int> {
        int g = (isFar ? farL : nearL).place(c, groups, x, y);
        std::vector<int> extra;
        if (g) extra.push_back(g);
        if (bounceGroup && !isFar) extra.push_back(bounceGroup);
        return extra;
    };

    auto diamond = [&](bool isFar, bool hollow, float x, float y, float size) {
        int chn = isFar ? c.ch.back2 : c.ch.back;
        int z = isFar ? Z_B4 : Z_B3;
        auto extra = layerFor(isFar, x, y);
        if (spinGroup) extra.push_back(isFar ? spinGroup2 : spinGroup);
        if (!hollow) {
            c.b->sq(x, y, size, size, 45, chn, z, 0, extra);
            return;
        }
        float t = std::max(2.f, size * 0.09f);
        // four bars of a square, rotated 45 degrees around the centre
        c.b->sqAround(x, y, 0, size / 2.f - t / 2.f, size, t, 0, 45, chn, z, 0, extra);
        c.b->sqAround(x, y, 0, -size / 2.f + t / 2.f, size, t, 0, 45, chn, z, 0, extra);
        c.b->sqAround(x, y, size / 2.f - t / 2.f, 0, t, size, 0, 45, chn, z, 0, extra);
        c.b->sqAround(x, y, -size / 2.f + t / 2.f, 0, t, size, 0, 45, chn, z, 0, extra);
    };
    auto star = [&](bool isFar, float x, float y, float size) {
        int chn = isFar ? c.ch.back2 : c.ch.back;
        int z = isFar ? Z_B4 : Z_B3;
        auto extra = layerFor(isFar, x, y);
        if (spinGroup) extra.push_back(isFar ? spinGroup2 : spinGroup);
        c.b->sq(x, y, size, size, 0, chn, z, 0, extra);
        c.b->sq(x, y, size, size, 45, chn, z, 0, extra);
    };
    auto pillar = [&](bool isFar, float x, float w, float h) {
        int chn = isFar ? c.ch.back2 : c.ch.back;
        int z = isFar ? Z_B4 : Z_B3;
        float y = GROUND + h / 2.f;
        auto extra = layerFor(isFar, x, y);
        // split tall pillars so no single object is huge
        int pieces = std::max(1, int(std::ceil(h / (B * MAX_RUN))));
        float ph = h / pieces;
        for (int i = 0; i < pieces; i++) c.b->sq(x, GROUND + ph * (i + 0.5f) + (y - GROUND - h / 2.f), w, ph + 0.5f, 0, chn, z, 0, extra);
        if (!isFar) c.b->sq(x, y + h / 2.f, w * 1.6f, w * 1.6f, 45, c.ch.accent2, z, 1, extra);
    };
    auto frame = [&](bool isFar, float x, float y, float w, float h, float angle) {
        int chn = isFar ? c.ch.back2 : c.ch.back;
        int z = isFar ? Z_B4 : Z_B3;
        auto extra = layerFor(isFar, x, y);
        float t = isFar ? 3.f : 2.f;
        c.b->sqAround(x, y, 0, h / 2.f, w, t, 0, angle, chn, z, 0, extra);
        c.b->sqAround(x, y, 0, -h / 2.f, w, t, 0, angle, chn, z, 0, extra);
        c.b->sqAround(x, y, w / 2.f, 0, t, h, 0, angle, chn, z, 0, extra);
        c.b->sqAround(x, y, -w / 2.f, 0, t, h, 0, angle, chn, z, 0, extra);
    };
    auto randY = [&](float lo, float hi) { return GROUND + c.rand(lo, hi) * (top - GROUND); };

    Back kind = plan.back;
    if (kind == Back::Diamonds || kind == Back::Mixed) {
        int n = int(len * (0.06f + 0.20f * d) * (kind == Back::Mixed ? 0.6f : 1.f));
        for (int i = 0; i < n; i++) {
            bool isFar = c.chance(0.5f);
            float size = isFar ? c.rand(3.f, 6.5f) * B : c.rand(1.2f, 3.f) * B;
            diamond(isFar, c.chance(0.4f), c.rand(s.x0, s.x1), randY(0.1f, 0.95f), size);
        }
    }
    if (kind == Back::Pillars || kind == Back::Mixed) {
        float step = (9.f - 5.f * d) * B * (kind == Back::Mixed ? 1.6f : 1.f);
        bool isFar = false;
        for (float x = s.x0 + step / 2.f; x < s.x1; x += step * c.rand(0.7f, 1.3f)) {
            isFar = !isFar;
            float w = (isFar ? c.rand(1.2f, 2.6f) : c.rand(0.5f, 1.4f)) * B;
            pillar(isFar, x, w, c.rand(0.25f, 0.9f) * (top - GROUND));
        }
    }
    if (kind == Back::Stars || kind == Back::Mixed) {
        int n = int(len * (0.05f + 0.16f * d) * (kind == Back::Mixed ? 0.5f : 1.f));
        for (int i = 0; i < n; i++) {
            bool isFar = c.chance(0.45f);
            star(isFar, c.rand(s.x0, s.x1), randY(0.15f, 0.95f), (isFar ? c.rand(2.f, 4.f) : c.rand(0.8f, 2.f)) * B);
        }
    }
    if (kind == Back::Frames) {
        int n = int(len * (0.05f + 0.15f * d));
        for (int i = 0; i < n; i++) {
            bool isFar = c.chance(0.5f);
            float w = c.rand(2.f, 6.f) * B, h = c.rand(2.f, 6.f) * B;
            frame(isFar, c.rand(s.x0, s.x1), randY(0.15f, 0.9f), w, h, c.chance(0.3f) ? 45.f : 0.f);
        }
    }
    if (kind == Back::Grid) {
        float cell = B * 3;
        for (float x = std::ceil(s.x0 / cell) * cell; x < s.x1; x += cell)
            for (float y = GROUND; y < top; y += B * MAX_RUN) {
                float h = std::min(B * MAX_RUN, top - y);
                float px = x, py = y + h / 2.f;
                auto extra = layerFor(true, px, py);
                c.b->sq(px, py, 2.f, h, 0, c.ch.back2, Z_B4, 0, extra);
            }
        for (float y = GROUND + cell; y < top; y += cell)
            for (float x = s.x0; x < s.x1; x += B * MAX_RUN) {
                float w = std::min(B * MAX_RUN, s.x1 - x);
                float px = x + w / 2.f, py = y;
                auto extra = layerFor(true, px, py);
                c.b->sq(px, py, w, 2.f, 0, c.ch.back2, Z_B4, 0, extra);
            }
    }

    farL.triggers(c);
    nearL.triggers(c);
    float t0 = s.speed.timeAt(s.x0), t1 = s.speed.timeAt(s.x1);
    float dur = std::max(1.f, t1 - t0);
    if (spinGroup) {
        c.b->rotate(s.x0, spinGroup, 360.f * dur / 7.f, dur);
        c.b->rotate(s.x0, spinGroup2, -360.f * dur / 12.f, dur);
    }
    if (bounceGroup) {
        float beat = plan.bpm > 0 ? 60.f / plan.bpm : 0.5f;
        int n = std::min(1500, int(dur / beat));
        for (int k = 0; k < n; k++) {
            float tx = s.speed.xAt(t0 + k * beat);
            c.b->move(tx, bounceGroup, 0, 9.f, std::min(0.12f, beat * 0.3f), 3);
            c.b->move(s.speed.xAt(t0 + k * beat + std::min(0.12f, beat * 0.3f)), bounceGroup, 0, -9.f, beat * 0.55f, 1);
        }
    }

    // A soft glow sitting on the floor
    if (c.opt->glow && plan.glow == Glow::Strong) {
        for (float x = s.x0; x < s.x1; x += B * MAX_RUN) {
            float w = std::min(B * MAX_RUN, s.x1 - x);
            c.b->sq(x + w / 2.f, GROUND + 10.f, w, 20.f, 0, c.ch.glowB, Z_B2, 0);
        }
    }
}

static void placeParticles(Ctx& c, Groups& groups) {
    auto const& plan = *c.plan;
    if (plan.particles == Particles::None) return;
    auto& s = *c.scan;
    float len = (s.x1 - s.x0) / B;
    int n = std::min(2500, int(len * (0.25f + 1.1f * c.density)));

    MovingLayer layer;
    float w = 6.f, h = 6.f, rot = 45.f;
    switch (plan.particles) {
        case Particles::Embers: layer.vx = 10.f, layer.vy = 45.f; break;
        case Particles::Sparks: layer.vx = 25.f, layer.vy = 80.f, w = 3.f, h = 9.f, rot = 20.f; break;
        case Particles::Rain: layer.vx = -40.f, layer.vy = -260.f, w = 2.f, h = 16.f, rot = 10.f; break;
        case Particles::Snow: layer.vx = -15.f, layer.vy = -35.f; break;
        case Particles::Dust: layer.vx = 20.f, layer.vy = 8.f, w = h = 4.f; break;
        default: break;
    }
    layer.slice = 4.f;
    for (int i = 0; i < n; i++) {
        float x = c.rand(s.x0, s.x1);
        float y = GROUND + c.rand(0.f, 1.f) * (s.yTop - GROUND);
        // keep particles out of the blocks themselves
        int cx = int(std::floor(x / B)), cy = int(std::floor(y / B));
        if (s.solid.contains(cellKey(cx, cy))) continue;
        bool front = c.chance(0.35f);
        float sz = c.rand(0.6f, 1.4f);
        int g = layer.place(c, groups, x, y);
        std::vector<int> extra;
        if (g) extra.push_back(g);
        c.b->sq(x, y, w * sz, h * sz, rot + (plan.particles == Particles::Rain ? 0.f : c.rand(-20.f, 20.f)),
                front ? c.ch.accent : c.ch.accent2, front ? Z_B1 : Z_B3, front ? 9 : 2, extra);
    }
    layer.triggers(c);
}

static void placePulses(Ctx& c) {
    auto const& plan = *c.plan;
    if (plan.pulse == Pulse::None || plan.bpm <= 0.f) return;
    auto& s = *c.scan;
    float beat = 60.f / plan.bpm;
    float t0 = s.speed.timeAt(s.x0), t1 = s.speed.timeAt(s.x1);
    Rgb outline = colorOf(plan.pal, "outline"), glow = colorOf(plan.pal, "glow"), back = colorOf(plan.pal, "back");
    Rgb hot = mix(outline, {255, 255, 255}, 0.55f), hotGlow = mix(glow, {255, 255, 255}, 0.35f), hotBack = mix(back, glow, 0.6f);
    float fout = std::min(0.5f, beat * 0.7f);
    int k0 = int(std::ceil(t0 / beat));
    int placed = 0;
    for (int k = k0; k * beat < t1 && placed < 2500; k++) {
        if (plan.pulse == Pulse::HalfBeat && k % 2 != 0) continue;
        float t = k * beat + (plan.pulse == Pulse::OffBeat ? beat * 0.5f : 0.f);
        float x = s.speed.xAt(t);
        if (c.opt->outlines) c.b->pulse(x, c.ch.outline, hot, 0.f, 0.04f, fout);
        if (c.opt->glow) c.b->pulse(x, c.ch.glowA, hotGlow, 0.f, 0.04f, fout);
        if (c.opt->background && k % 2 == 0) c.b->pulse(x, c.ch.back, hotBack, 0.f, 0.05f, fout * 1.3f);
        placed++;
    }
}

static void placeMotifs(Ctx& c) {
    auto const& plan = *c.plan;
    auto& s = *c.scan;
    for (auto const& m : plan.motifs) {
        float step = std::max(4.f, m.every) * B;
        bool front = m.layer == "front";
        for (float x = s.x0 + step / 2.f; x < s.x1; x += step) {
            float y = GROUND + m.y * B;
            if (front) {
                // skip copies that would sit on top of gameplay
                bool clear = true;
                for (auto const& p : m.parts) {
                    float px = x + p.x * B * m.scale, py = y + p.y * B * m.scale;
                    float rad = std::max(p.w, p.h) * B * m.scale / 2.f;
                    for (float ox = -rad; ox <= rad && clear; ox += B)
                        for (float oy = -rad; oy <= rad && clear; oy += B) {
                            auto k = cellKey(int(std::floor((px + ox) / B)), int(std::floor((py + oy) / B)));
                            if (s.solid.contains(k) || s.hazard.contains(k)) clear = false;
                        }
                    if (!clear) break;
                }
                if (!clear) continue;
            }
            int zorder = 3;
            for (auto const& p : m.parts) {
                c.b->sq(x + p.x * B * m.scale, y + p.y * B * m.scale, p.w * B * m.scale, p.h * B * m.scale, p.rot,
                        channelForName(p.col, c.ch), front ? Z_B1 : Z_B3, front ? 8 + zorder % 2 : zorder);
                zorder++;
            }
        }
    }
}

// Full-size solid blocks under a deco overlay are hidden (they still collide),
// because GD draws them over the overlay otherwise. Returns the cells an object covers.
static std::vector<int64_t> cellsOf(GameObject* obj) {
    std::unordered_set<int64_t> set;
    markCells(set, obj->getObjectRect());
    return {set.begin(), set.end()};
}

static int hideCoveredBlocks(LevelEditorLayer* ed, Scan const& s) {
    int hidden = 0;
    for (auto obj : CCArrayExt<GameObject*>(ed->m_objects)) {
        if (!obj || obj->m_isNoTouch || obj->m_isHide || obj->m_objectType != GameObjectType::Solid) continue;
        auto const& r = obj->getObjectRect();
        if (r.size.width < 25.f || r.size.height < 25.f) continue; // half blocks etc. keep their own look
        float x = obj->getPositionX();
        if (x < s.x0 - B || x > s.x1 + B) continue;
        auto cells = cellsOf(obj);
        if (cells.empty()) continue;
        obj->m_isHide = true;
        hidden++;
    }
    return hidden;
}

static void unhideUnder(LevelEditorLayer* ed, std::unordered_set<int64_t> const& cells) {
    if (cells.empty()) return;
    for (auto obj : CCArrayExt<GameObject*>(ed->m_objects)) {
        if (!obj || !obj->m_isHide || obj->m_objectType != GameObjectType::Solid) continue;
        for (auto k : cellsOf(obj))
            if (cells.contains(k)) {
                obj->m_isHide = false;
                break;
            }
    }
}

// ---------------------------------------------------------------- undo

static std::vector<Ref<GameObject>> s_last;
static std::vector<int> s_markers;

static std::string levelUndoKey() {
    auto ed = LevelEditorLayer::get();
    if (!ed || !ed->m_level) return "undo-unknown";
    std::string name = ed->m_level->m_levelName;
    std::string clean;
    for (char ch : name) clean += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
    return "undo-" + clean;
}

static std::vector<int> savedMarkers() {
    std::vector<int> out;
    auto s = Mod::get()->getSavedValue<std::string>(levelUndoKey(), "");
    size_t pos = 0;
    while (pos < s.size()) {
        size_t n = s.find(',', pos);
        if (n == std::string::npos) n = s.size();
        int g = std::atoi(s.substr(pos, n - pos).c_str());
        if (g > 0) out.push_back(g);
        pos = n + 1;
    }
    return out;
}

static void saveMarkers(std::vector<int> const& v) {
    std::string s;
    for (int g : v) s += (s.empty() ? "" : ",") + std::to_string(g);
    Mod::get()->setSavedValue<std::string>(levelUndoKey(), s);
}

bool canUndo() { return !savedMarkers().empty(); }

int undoLast() {
    auto ed = LevelEditorLayer::get();
    if (!ed) return 0;
    auto markers = savedMarkers();
    if (markers.empty()) return 0;
    int marker = markers.back();
    markers.pop_back();
    saveMarkers(markers);

    if (ed->m_editorUI) ed->m_editorUI->deselectAll();
    std::vector<Ref<GameObject>> doomed;
    for (auto obj : CCArrayExt<GameObject*>(ed->m_objects)) {
        if (!obj || !obj->m_groups) continue;
        for (int i = 0; i < obj->m_groupCount && i < 10; i++)
            if ((*obj->m_groups)[i] == marker) {
                doomed.push_back(obj);
                break;
            }
    }
    // blocks that were hidden under the removed overlays become visible again
    std::unordered_set<int64_t> covered;
    int square = catalog().square;
    for (auto& obj : doomed)
        if (obj->m_objectID == square)
            for (auto k : cellsOf(obj)) covered.insert(k);
    for (auto& obj : doomed) ed->removeObject(obj, true);
    unhideUnder(ed, covered);
    s_last.clear();
    if (ed->m_editorUI) ed->m_editorUI->updateButtons();
    return int(doomed.size());
}

// ---------------------------------------------------------------- apply

static Channels pickChannels(std::set<int> const& used) {
    int first = int(std::clamp<int64_t>(Mod::get()->getSettingValue<int64_t>("first-channel"), 100, 980));
    int c = first;
    auto freeBlock = [&](int start) {
        if (start + 10 > 999) return false;
        for (int i = 0; i < 11; i++)
            if (used.contains(start + i)) return false;
        return true;
    };
    while (!freeBlock(c) && c + 10 <= 999) c++;
    if (c + 10 > 999) c = first; // everything's taken: share
    return {c, c + 1, c + 2, c + 3, c + 4, c + 5, c + 6, c + 7, c + 8, c + 9, c + 10};
}

static Result<ApplyResult> commit(LevelEditorLayer* ed, Builder& b, int marker, char const* what) {
    if (b.count == 0) return Err("Nothing to place - try turning more parts on");
    auto arr = ed->createObjectsFromString(gd::string(b.out), false, true);
    int made = 0;
    s_last.clear();
    if (arr) {
        for (auto obj : CCArrayExt<GameObject*>(arr)) {
            if (!obj) continue;
            if (obj->m_objectID == b.cat.square) {
                obj->m_isNoTouch = true; // deco must never be touchable
                obj->m_hasNoGlow = true;
            }
            s_last.push_back(obj);
            made++;
        }
    }
    auto markers = savedMarkers();
    markers.push_back(marker);
    saveMarkers(markers);
    if (ed->m_editorUI) ed->m_editorUI->updateButtons();
    ApplyResult r;
    r.objects = made;
    r.summary = fmt::format("{} {} objects (group {})", what, made, marker);
    return Ok(r);
}

Result<ApplyResult> applyDeco(DecoPlan const& plan, DecoOptions const& opt, bool selectionOnly) {
    auto ed = LevelEditorLayer::get();
    if (!ed) return Err("Open the level editor first");
    auto scan = scanLevel(selectionOnly);
    if (!scan.ok) return Err(scan.error);

    int maxObjects = int(Mod::get()->getSettingValue<int64_t>("max-objects"));
    Channels ch = pickChannels(scan.usedChannels);

    float density = std::clamp(plan.density, 0.f, 1.f);
    for (int attempt = 0; attempt < 5; attempt++) {
        Groups groups{ed, {}};
        int marker = groups.next();
        Builder b;
        b.marker = marker;
        b.editorLayer = int(Mod::get()->getSettingValue<int64_t>("first-editor-layer"));
        b.cat = catalog();
        b.trigY = scan.yTop + B * 3;

        Ctx c{&plan, &opt, &scan, &b, ch, std::mt19937(opt.seed), density, ed};
        if (opt.colors) placeColors(c);
        if (opt.blocks || opt.outlines || opt.glow) placeBlocks(c);
        if (opt.background) placeBackground(c, groups);
        if (opt.particles) placeParticles(c, groups);
        if (opt.pulses) placePulses(c);
        if (opt.motifs) placeMotifs(c);

        if (b.count <= maxObjects || attempt == 4) {
            if (b.count > maxObjects) return Err(fmt::format("Too many objects ({}). Raise 'Max objects' in settings or decorate a selection.", b.count));
            auto res = commit(ed, b, marker, "Placed");
            if (res && opt.blocks) hideCoveredBlocks(ed, scan);
            return res;
        }
        density *= 0.55f; // thin out background and particles, then try again
    }
    return Err("Couldn't fit the deco");
}

// ---------------------------------------------------------------- layout

static int portalFor(GameMode m) {
    switch (m) {
        case GameMode::Ship: return 13;
        case GameMode::Ball: return 47;
        case GameMode::Ufo: return 111;
        case GameMode::Wave: return 660;
        default: return 12;
    }
}

Result<ApplyResult> applyLayout(LayoutPlan const& plan, unsigned seed) {
    auto ed = LevelEditorLayer::get();
    if (!ed) return Err("Open the level editor first");
    float maxX = 0.f;
    bool any = false;
    for (auto obj : CCArrayExt<GameObject*>(ed->m_objects)) {
        if (!obj) continue;
        maxX = std::max(maxX, obj->getPositionX());
        any = true;
    }
    int cx = any ? int(std::ceil(maxX / B)) + 4 : 8;

    Groups groups{ed, {}};
    int marker = groups.next();
    Builder b;
    b.marker = marker;
    b.cat = catalog();
    std::mt19937 rng(seed);
    auto rnd = [&](float a, float c2) { return std::uniform_real_distribution<float>(a, c2)(rng); };
    auto chance = [&](float p) { return rnd(0.f, 1.f) < p; };

    auto block = [&](int x, int y) {
        Obj o(1);
        o.f(2, x * B + 15.f).f(3, y * B + 15.f);
        b.add(o);
    };
    auto spike = [&](int x, int y, bool flipped) {
        Obj o(8);
        o.f(2, x * B + 15.f).f(3, y * B + 15.f);
        if (flipped) o.f(6, 180.f);
        b.add(o);
    };
    auto obj = [&](int id, int x, int y) {
        Obj o(id);
        o.f(2, x * B + 15.f).f(3, y * B + 15.f);
        b.add(o);
    };

    GameMode current = GameMode::Cube;
    for (auto const& seg : plan.segments) {
        int d = seg.difficulty;
        float t = (d - 1) / 9.f;
        int end = cx + seg.length;
        if (seg.mode != current) {
            obj(portalFor(seg.mode), cx, seg.mode == GameMode::Cube ? GROUND_CY + 1 : GROUND_CY + 4);
            current = seg.mode;
            cx += 3;
        }
        if (seg.mode == GameMode::Cube) {
            int gapMin = int(std::round(6 - 3 * t));
            while (cx < end - 6) {
                float r = rnd(0.f, 1.f);
                int gap = gapMin + int(rnd(0.f, 3.f));
                if (r < 0.30f || d < 2) {
                    spike(cx, GROUND_CY, false);
                    cx += 1 + gap;
                } else if (r < 0.50f && d >= 3) {
                    int n = d >= 6 && chance(0.5f) ? 3 : 2;
                    for (int i = 0; i < n; i++) spike(cx + i, GROUND_CY, false);
                    cx += n + gap + 1;
                } else if (r < 0.75f) {
                    int l = 3 + int(rnd(0.f, 4.f));
                    for (int i = 0; i < l; i++) block(cx + i, GROUND_CY);
                    if (d >= 5 && l >= 4) spike(cx + l / 2, GROUND_CY + 1, false);
                    if (d >= 4) spike(cx + l, GROUND_CY, false);
                    cx += l + 1 + gap;
                } else {
                    for (int i = 0; i < 4; i++) spike(cx + i, GROUND_CY, false);
                    obj(36, cx + 1, GROUND_CY + 1); // yellow orb over the pit
                    cx += 4 + gap + 1;
                }
            }
            cx = std::max(cx, end);
        } else if (seg.mode == GameMode::Ball) {
            int ceilY = GROUND_CY + 6;
            bool onFloor = true;
            int step = int(std::round(8 - 4 * t));
            for (int x = cx; x < end; x++) block(x, ceilY);
            for (int x = cx + 4; x < end - 2; x += step + int(rnd(0.f, 2.f))) {
                if (onFloor) spike(x, GROUND_CY, false);
                else spike(x, ceilY - 1, true);
                if (d >= 6 && chance(0.4f)) {
                    if (onFloor) spike(x + 1, GROUND_CY, false);
                    else spike(x + 1, ceilY - 1, true);
                }
                onFloor = !onFloor;
            }
            cx = end;
        } else {
            // Ship, UFO and wave: a corridor between a floor line and a ceiling line
            int gap = seg.mode == GameMode::Wave ? int(std::round(5 - 2 * t)) : seg.mode == GameMode::Ufo ? int(std::round(8 - 3 * t))
                                                                                                         : int(std::round(7 - 3 * t));
            int changeEvery = seg.mode == GameMode::Wave ? 1 : 3;
            int floorY = GROUND_CY + 3 - gap / 2;
            floorY = std::max(GROUND_CY, floorY);
            int prevFloor = floorY, prevCeil = floorY + gap + 1;
            for (int x = cx; x < end; x++) {
                if ((x - cx) % changeEvery == 0 && x > cx + 2) {
                    int dir = int(rnd(0.f, 3.f)) - 1;
                    floorY = std::clamp(floorY + dir, GROUND_CY, GROUND_CY + 9 - gap - 1);
                }
                int ceilY = floorY + gap + 1;
                for (int y = std::min(floorY, prevFloor); y <= std::max(floorY, prevFloor); y++) block(x, y);
                for (int y = std::min(ceilY, prevCeil); y <= std::max(ceilY, prevCeil); y++) block(x, y);
                if (seg.mode != GameMode::Wave && d >= 5 && gap >= 4 && x % 7 == 0 && x > cx + 3) {
                    if (chance(0.5f)) spike(x, floorY + 1, false);
                    else spike(x, ceilY - 1, true);
                }
                prevFloor = floorY;
                prevCeil = ceilY;
            }
            cx = end;
        }
    }
    if (current != GameMode::Cube) {
        obj(12, cx, GROUND_CY + 4);
        cx += 2;
    }
    return commit(ed, b, marker, "Built a layout with");
}

} // namespace deco
