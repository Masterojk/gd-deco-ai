// Deco AI - the editor button and the Deco AI window (Decorate / Layout / Ideas).

#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/ui/MDPopup.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <optional>

#include "Catalog.hpp"
#include "Claude.hpp"
#include "Engine.hpp"
#include "Plan.hpp"

using namespace geode::prelude;
using namespace deco;

namespace {

ccColor3B const kRed = {255, 110, 110};
ccColor3B const kGreen = {120, 255, 140};
ccColor3B const kCyan = {110, 220, 255};
ccColor3B const kDim = {200, 200, 220};

CCLabelBMFont* label(std::string const& s, char const* font, float scale, ccColor3B col = {255, 255, 255}) {
    auto l = CCLabelBMFont::create(s.c_str(), font);
    l->setScale(scale);
    l->setColor(col);
    return l;
}

void fitWidth(CCLabelBMFont* l, float maxW) {
    float w = l->getContentSize().width * l->getScale();
    if (w > maxW && w > 0.f) l->setScale(l->getScale() * maxW / w);
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    return s;
}

class DecoPanel : public geode::Popup {
public:
    static DecoPanel* create() {
        auto ret = new DecoPanel();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

protected:
    enum class Tab { Deco, Layout, Ideas };
    static constexpr float W = 420.f;
    static constexpr float H = 290.f;

    Tab m_tab = Tab::Deco;
    CCMenu* m_menu = nullptr;
    CCNode* m_content = nullptr;
    CCLabelBMFont* m_status = nullptr;
    std::string m_statusText;
    ccColor3B m_statusColor = kDim;
    bool m_busy = false;
    async::TaskHolder<web::WebResponse> m_task;

    std::string m_vibe, m_bpm, m_layoutVibe, m_length = "120", m_difficulty = "5", m_ideaTopic;
    std::string m_preset = "claude";
    bool m_selOnly = true;
    bool m_modIdeas = false;
    DecoOptions m_opt;
    unsigned m_runs = 0;

    bool init() {
        if (!Popup::init(W, H)) return false;
        this->setTitle("Deco AI", "goldFont.fnt", 0.8f, 18.f);
        load();

        m_content = CCNode::create();
        m_content->setContentSize({W, H});
        m_mainLayer->addChild(m_content, 4);
        m_menu = CCMenu::create();
        m_menu->setPosition({0.f, 0.f});
        m_menu->setContentSize({W, H});
        m_mainLayer->addChild(m_menu, 5);

        if (!hasApiKey()) setStatus("No API key yet: presets work, or add your key in settings", kDim);
        rebuild();
        return true;
    }

    void onClose(CCObject* sender) override {
        save();
        Popup::onClose(sender);
    }

    // ------------------------------------------------------------ saved state
    void load() {
        auto m = Mod::get();
        m_vibe = m->getSavedValue<std::string>("vibe", "");
        m_bpm = m->getSavedValue<std::string>("bpm", "");
        m_layoutVibe = m->getSavedValue<std::string>("layout-vibe", "");
        m_length = m->getSavedValue<std::string>("length", "120");
        m_difficulty = m->getSavedValue<std::string>("difficulty", "5");
        m_ideaTopic = m->getSavedValue<std::string>("idea-topic", "");
        m_preset = m->getSavedValue<std::string>("preset", "claude");
        m_selOnly = m->getSavedValue<bool>("selection-only", true);
        m_opt.colors = m->getSavedValue<bool>("opt-colors", true);
        m_opt.blocks = m->getSavedValue<bool>("opt-blocks", true);
        m_opt.outlines = m->getSavedValue<bool>("opt-outlines", true);
        m_opt.glow = m->getSavedValue<bool>("opt-glow", true);
        m_opt.background = m->getSavedValue<bool>("opt-background", true);
        m_opt.particles = m->getSavedValue<bool>("opt-particles", true);
        m_opt.pulses = m->getSavedValue<bool>("opt-pulses", true);
        m_opt.motifs = m->getSavedValue<bool>("opt-motifs", true);
    }
    void save() {
        auto m = Mod::get();
        m->setSavedValue<std::string>("vibe", m_vibe);
        m->setSavedValue<std::string>("bpm", m_bpm);
        m->setSavedValue<std::string>("layout-vibe", m_layoutVibe);
        m->setSavedValue<std::string>("length", m_length);
        m->setSavedValue<std::string>("difficulty", m_difficulty);
        m->setSavedValue<std::string>("idea-topic", m_ideaTopic);
        m->setSavedValue<std::string>("preset", m_preset);
        m->setSavedValue<bool>("selection-only", m_selOnly);
        m->setSavedValue<bool>("opt-colors", m_opt.colors);
        m->setSavedValue<bool>("opt-blocks", m_opt.blocks);
        m->setSavedValue<bool>("opt-outlines", m_opt.outlines);
        m->setSavedValue<bool>("opt-glow", m_opt.glow);
        m->setSavedValue<bool>("opt-background", m_opt.background);
        m->setSavedValue<bool>("opt-particles", m_opt.particles);
        m->setSavedValue<bool>("opt-pulses", m_opt.pulses);
        m->setSavedValue<bool>("opt-motifs", m_opt.motifs);
    }

    // ------------------------------------------------------------ building blocks
    template <class T>
    T* add(T* node, CCPoint pos, CCPoint anchor = {0.5f, 0.5f}) {
        node->setAnchorPoint(anchor);
        node->setPosition(pos);
        m_content->addChild(node);
        return node;
    }

    CCMenuItemSpriteExtra* button(std::string const& text, char const* bg, float scale, CCPoint pos,
                                  geode::Function<void(CCMenuItemSpriteExtra*)> cb) {
        auto spr = ButtonSprite::create(text.c_str(), "bigFont.fnt", bg, scale);
        auto btn = CCMenuItemExt::createSpriteExtra(spr, std::move(cb));
        btn->setPosition(pos);
        m_menu->addChild(btn);
        return btn;
    }

    TextInput* input(float width, char const* placeholder, CCPoint pos, std::string* store, std::optional<CommonFilter> filter = std::nullopt,
                     size_t maxChars = 0) {
        auto in = TextInput::create(width, placeholder, "chatFont.fnt");
        if (filter) in->setCommonFilter(*filter);
        if (maxChars) in->setMaxCharCount(maxChars);
        in->setString(*store);
        in->setCallback([store](std::string const& s) { *store = s; });
        in->setPosition(pos);
        m_menu->addChild(in);
        return in;
    }

    void toggle(char const* name, CCPoint pos, bool* value) {
        auto t = CCMenuItemExt::createTogglerWithStandardSprites(0.55f, [value](CCMenuItemToggler* tog) {
            *value = !tog->isToggled(); // isToggled() is still the old state inside the callback
        });
        t->toggle(*value);
        t->setPosition(pos);
        m_menu->addChild(t);
        add(label(name, "bigFont.fnt", 0.3f), {pos.x + 14.f, pos.y}, {0.f, 0.5f});
    }

    void setStatus(std::string const& text, ccColor3B color) {
        m_statusText = text;
        m_statusColor = color;
        if (!m_status) return;
        m_status->setString(text.c_str());
        m_status->setScale(0.55f);
        m_status->setColor(color);
        fitWidth(m_status, W - 30.f);
    }

    void statusLine() {
        m_status = add(label(m_statusText, "chatFont.fnt", 0.55f, m_statusColor), {W / 2.f, 22.f});
        fitWidth(m_status, W - 30.f);
    }

    void tabs() {
        char const* names[] = {"Decorate", "Layout", "Ideas"};
        Tab ids[] = {Tab::Deco, Tab::Layout, Tab::Ideas};
        for (int i = 0; i < 3; i++) {
            Tab t = ids[i];
            button(names[i], m_tab == t ? "GJ_button_01.png" : "GJ_button_04.png", 0.5f, {W / 2.f + (i - 1) * 110.f, H - 50.f},
                   [this, t](auto) {
                       if (m_tab == t) return;
                       m_tab = t;
                       rebuild();
                   });
        }
    }

    void rebuild() {
        m_menu->removeAllChildren();
        m_content->removeAllChildren();
        m_status = nullptr;
        tabs();
        switch (m_tab) {
            case Tab::Deco: buildDeco(); break;
            case Tab::Layout: buildLayout(); break;
            case Tab::Ideas: buildIdeas(); break;
        }
        statusLine();
    }

    // ------------------------------------------------------------ Decorate tab
    void buildDeco() {
        float y = H - 84.f;
        add(label("Vibe", "bigFont.fnt", 0.38f), {18.f, y}, {0.f, 0.5f});
        input(330.f, "e.g. bloody red horror, sharp thorns, embers, pulses", {78.f + 165.f, y}, &m_vibe, CommonFilter::Any, 300);

        y -= 34.f;
        auto const& presets = presetList();
        float step = (W - 30.f) / float(presets.size());
        for (size_t i = 0; i < presets.size(); i++) {
            auto id = presets[i].first;
            button(presets[i].second, m_preset == id ? "GJ_button_01.png" : "GJ_button_04.png", 0.36f,
                   {15.f + step * (i + 0.5f), y}, [this, id](auto) {
                       m_preset = id;
                       rebuild();
                   });
        }

        y -= 32.f;
        add(label("BPM", "bigFont.fnt", 0.34f), {18.f, y}, {0.f, 0.5f});
        input(58.f, "120", {92.f, y}, &m_bpm, CommonFilter::Float, 6);
        toggle("Selection only", {140.f, y}, &m_selOnly);
        auto sum = summarizeLevel(m_selOnly);
        std::string range = sum.ok ? fmt::format("{}: {} blocks", sum.selection ? "Selection" : "Whole level", sum.lengthBlocks)
                                   : std::string("No layout yet");
        add(label(range, "chatFont.fnt", 0.5f, kDim), {W - 16.f, y}, {1.f, 0.5f});

        y -= 30.f;
        toggle("Colours", {24.f, y}, &m_opt.colors);
        toggle("Block fill", {124.f, y}, &m_opt.blocks);
        toggle("Outlines", {224.f, y}, &m_opt.outlines);
        toggle("Glow", {324.f, y}, &m_opt.glow);
        y -= 26.f;
        toggle("Background", {24.f, y}, &m_opt.background);
        toggle("Particles", {124.f, y}, &m_opt.particles);
        toggle("Pulses", {224.f, y}, &m_opt.pulses);
        toggle("Motifs", {324.f, y}, &m_opt.motifs);

        button(m_preset == "claude" ? "Decorate!" : "Apply preset", "GJ_button_01.png", 0.75f, {W / 2.f - 50.f, 56.f},
               [this](auto) { onDecorate(); });
        button("Undo", "GJ_button_06.png", 0.6f, {W / 2.f + 95.f, 56.f}, [this](auto) { onUndo(); });
    }

    std::string partsList() const {
        std::string s;
        auto addPart = [&](bool on, char const* n) {
            if (on) s += (s.empty() ? "" : ", ") + std::string(n);
        };
        addPart(m_opt.colors, "colours");
        addPart(m_opt.blocks, "block fill");
        addPart(m_opt.outlines, "outlines");
        addPart(m_opt.glow, "glow");
        addPart(m_opt.background, "background");
        addPart(m_opt.particles, "particles");
        addPart(m_opt.pulses, "pulses");
        addPart(m_opt.motifs, "motifs");
        return s.empty() ? "none" : s;
    }

    void onDecorate() {
        if (m_busy) return;
        save();
        float bpm = std::strtof(m_bpm.c_str(), nullptr);
        m_opt.seed = unsigned(std::hash<std::string>{}(m_vibe)) + m_runs++ * 7919u;

        if (m_preset != "claude") {
            auto plan = presetPlan(m_preset);
            if (bpm > 0.f) plan.bpm = bpm;
            plan.notes.clear();
            finishDeco(plan);
            return;
        }
        if (!hasApiKey()) {
            setStatus("Add your Anthropic API key in the mod settings, or pick a preset", kRed);
            return;
        }
        auto vibe = trim(m_vibe);
        if (vibe.empty()) {
            setStatus("Describe the vibe first", kRed);
            return;
        }
        auto sum = summarizeLevel(m_selOnly);
        if (!sum.ok) {
            setStatus(sum.error, kRed);
            return;
        }
        auto msg = fmt::format(
            "Vibe: {}\n\nLevel: {} blocks long (about {:.0f} seconds), {} solid block cells, {} hazards, tallest structure {} blocks, "
            "game modes: {}. Decorating {}.{}\nParts that will be placed: {}.",
            vibe, sum.lengthBlocks, sum.seconds, sum.solidCells, sum.hazards, sum.maxHeightBlocks, sum.modes,
            sum.selection ? "only the selected part" : "the whole level", bpm > 0.f ? fmt::format(" Song BPM: {:.0f}.", bpm) : std::string(),
            partsList());

        m_busy = true;
        setStatus("Asking Claude for a deco plan...", kCyan);
        askClaude(m_task, kDecoSystem, msg, 4000, [this, bpm](Result<std::string> r) {
            m_busy = false;
            if (!r) {
                setStatus(r.unwrapErr(), kRed);
                return;
            }
            auto plan = parsePlan(r.unwrap());
            if (!plan) {
                setStatus(plan.unwrapErr(), kRed);
                return;
            }
            auto p = plan.unwrap();
            if (bpm > 0.f) p.bpm = bpm;
            finishDeco(p);
        });
    }

    void finishDeco(DecoPlan const& plan) {
        setStatus("Placing deco...", kCyan);
        auto res = applyDeco(plan, m_opt, m_selOnly);
        if (!res) {
            setStatus(res.unwrapErr(), kRed);
            return;
        }
        std::string extra = (plan.bpm <= 0.f && m_opt.pulses && plan.pulse != Pulse::None) ? " - add a BPM for beat pulses" : "";
        setStatus(fmt::format("{} - {}{}", res.unwrap().summary, plan.name, extra), kGreen);
        if (!plan.notes.empty()) {
            if (auto pop = MDPopup::create("Claude's notes", fmt::format("## {}\n\n{}", plan.name, plan.notes), "OK")) pop->show();
        }
    }

    void onUndo() {
        if (m_busy) return;
        int n = undoLast();
        setStatus(n > 0 ? fmt::format("Removed {} objects from the last run", n) : std::string("Nothing to undo"), n > 0 ? kGreen : kDim);
    }

    // ------------------------------------------------------------ Layout tab
    void buildLayout() {
        float y = H - 84.f;
        add(label("Vibe", "bigFont.fnt", 0.38f), {18.f, y}, {0.f, 0.5f});
        input(330.f, "e.g. fast scary wave parts, chill cube intro", {78.f + 165.f, y}, &m_layoutVibe, CommonFilter::Any, 300);

        y -= 38.f;
        add(label("Length", "bigFont.fnt", 0.34f), {18.f, y}, {0.f, 0.5f});
        input(60.f, "120", {108.f, y}, &m_length, CommonFilter::Uint, 4);
        add(label("blocks", "chatFont.fnt", 0.5f, kDim), {142.f, y}, {0.f, 0.5f});
        add(label("Difficulty", "bigFont.fnt", 0.34f), {210.f, y}, {0.f, 0.5f});
        input(44.f, "5", {322.f, y}, &m_difficulty, CommonFilter::Uint, 2);
        add(label("/ 10", "chatFont.fnt", 0.5f, kDim), {348.f, y}, {0.f, 0.5f});

        y -= 40.f;
        auto info = add(label("Builds after the last object in your level. It's a rough layout:", "chatFont.fnt", 0.5f, kDim), {W / 2.f, y});
        fitWidth(info, W - 40.f);
        auto info2 = add(label("playtest it, fix the timings, then come back and decorate it.", "chatFont.fnt", 0.5f, kDim),
                         {W / 2.f, y - 16.f});
        fitWidth(info2, W - 40.f);

        button("Build layout", "GJ_button_01.png", 0.75f, {W / 2.f - 50.f, 56.f}, [this](auto) { onLayout(); });
        button("Undo", "GJ_button_06.png", 0.6f, {W / 2.f + 95.f, 56.f}, [this](auto) { onUndo(); });
    }

    void onLayout() {
        if (m_busy) return;
        save();
        int len = std::clamp(std::atoi(m_length.c_str()), 30, 600);
        int diff = std::clamp(std::atoi(m_difficulty.c_str()), 1, 10);
        unsigned seed = unsigned(std::hash<std::string>{}(m_layoutVibe)) + m_runs++ * 104729u;
        auto vibe = trim(m_layoutVibe);

        if (!hasApiKey() || vibe.empty()) {
            auto res = applyLayout(fallbackLayout(len, diff), seed);
            if (!res) setStatus(res.unwrapErr(), kRed);
            else setStatus(res.unwrap().summary + (hasApiKey() ? " (add a vibe to let Claude plan it)" : " (built-in, no API key)"), kGreen);
            return;
        }
        auto msg = fmt::format("Vibe: {}\nLength: about {} blocks. Difficulty around {}/10.", vibe, len, diff);
        m_busy = true;
        setStatus("Asking Claude for a layout...", kCyan);
        askClaude(m_task, kLayoutSystem, msg, 1500, [this, seed](Result<std::string> r) {
            m_busy = false;
            if (!r) {
                setStatus(r.unwrapErr(), kRed);
                return;
            }
            auto plan = parseLayout(r.unwrap());
            if (!plan) {
                setStatus(plan.unwrapErr(), kRed);
                return;
            }
            auto lp = plan.unwrap();
            auto res = applyLayout(lp, seed);
            if (!res) {
                setStatus(res.unwrapErr(), kRed);
                return;
            }
            setStatus(res.unwrap().summary, kGreen);
            if (!lp.notes.empty()) {
                if (auto pop = MDPopup::create("Claude's layout notes", lp.notes, "OK")) pop->show();
            }
        });
    }

    // ------------------------------------------------------------ Ideas tab
    void buildIdeas() {
        float y = H - 84.f;
        add(label("About", "bigFont.fnt", 0.38f), {18.f, y}, {0.f, 0.5f});
        input(330.f, "e.g. a horror level about a haunted factory", {78.f + 165.f, y}, &m_ideaTopic, CommonFilter::Any, 300);

        y -= 38.f;
        button("Level ideas", !m_modIdeas ? "GJ_button_01.png" : "GJ_button_04.png", 0.5f, {W / 2.f - 70.f, y}, [this](auto) {
            m_modIdeas = false;
            rebuild();
        });
        button("Mod ideas", m_modIdeas ? "GJ_button_01.png" : "GJ_button_04.png", 0.5f, {W / 2.f + 70.f, y}, [this](auto) {
            m_modIdeas = true;
            rebuild();
        });

        y -= 44.f;
        button("Export object list", "GJ_button_05.png", 0.4f, {W / 2.f, y}, [this](auto) { onExport(); });
        auto info = add(label("Saves every object ID and sprite name to a file (helps improve Deco AI)", "chatFont.fnt", 0.45f, kDim),
                        {W / 2.f, y - 20.f});
        fitWidth(info, W - 40.f);

        button("Get ideas", "GJ_button_01.png", 0.75f, {W / 2.f, 56.f}, [this](auto) { onIdeas(); });
    }

    void onIdeas() {
        if (m_busy) return;
        save();
        if (!hasApiKey()) {
            setStatus("Ideas need your Anthropic API key (mod settings)", kRed);
            return;
        }
        auto topic = trim(m_ideaTopic);
        if (topic.empty()) topic = "anything fresh and fun";
        auto msg = fmt::format("{} ideas. Topic: {}", m_modIdeas ? "Geode mod" : "Geometry Dash level", topic);
        bool mod = m_modIdeas;
        m_busy = true;
        setStatus("Asking Claude for ideas...", kCyan);
        askClaude(m_task, kIdeasSystem, msg, 3000, [this, mod](Result<std::string> r) {
            m_busy = false;
            if (!r) {
                setStatus(r.unwrapErr(), kRed);
                return;
            }
            setStatus("Here are your ideas", kGreen);
            if (auto pop = MDPopup::create(mod ? "Mod ideas" : "Level ideas", r.unwrap(), "OK")) pop->show();
        });
    }

    void onExport() {
        auto path = exportCatalog();
        if (path.empty()) {
            setStatus("Couldn't save the object list", kRed);
            return;
        }
        setStatus("Saved object-catalog.txt in the Deco AI save folder", kGreen);
        utils::file::openFolder(Mod::get()->getSaveDir());
    }
};

} // namespace

class $modify(DecoEditorUI, EditorUI) {
    bool init(LevelEditorLayer* editor) {
        if (!EditorUI::init(editor)) return false;
        auto lbl = CCLabelBMFont::create("AI", "bigFont.fnt");
        auto spr = CircleButtonSprite::create(lbl, CircleBaseColor::Green, CircleBaseSize::Small);
        spr->setScale(0.85f);
        auto btn = CCMenuItemExt::createSpriteExtra(spr, [](auto) {
            if (auto p = DecoPanel::create()) p->show();
        });
        btn->setID("deco-button"_spr);
        if (auto menu = this->getChildByID("undo-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            auto own = CCMenu::create();
            own->setID("menu"_spr);
            auto win = CCDirector::get()->getWinSize();
            own->setPosition({win.width / 2.f - 150.f, win.height - 25.f});
            own->addChild(btn);
            this->addChild(own, 10);
        }
        return true;
    }
};
