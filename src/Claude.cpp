// Deco AI - Claude API calls and the instructions Claude gets.

#include "Claude.hpp"

#include <cctype>

using namespace geode::prelude;

namespace deco {

char const* const kDecoSystem = R"(You are the art director for a top Geometry Dash level, with the eye of decorators behind levels like Slaughterhouse, Bloodbath, Acheron and Kocmoc. A Geode mod turns your plan into real editor objects. It: recolours BG/ground/line; overlays every existing gameplay block with a fill shaded by depth (edges lighter, insides darker), an optional pattern, outlines on exposed edges and glow outside them; adds low-opacity background shapes on B3/B4 with parallax drift, spin or bounce; scatters particles; pulses outlines/glow/background on the beat; changes palettes at section starts; and repeats small motifs you design from rectangles.

Reply with ONLY one JSON object, no markdown, no comments:
{
 "name": "short theme name",
 "palette": {"bg":"#rrggbb","ground":"#rrggbb","ground2":"#rrggbb","line":"#rrggbb","mg":"#rrggbb","fill":"#rrggbb","outline":"#rrggbb","glow":"#rrggbb","back":"#rrggbb","accent":"#rrggbb"},
 "glowOpacity": 0.1-1,
 "backOpacity": 0.1-0.8,
 "style": {"outline":"none|thin|thick|double","glow":"none|soft|strong","fill":"plain|shaded|grid|bricks|dots|inset","back":"none|pillars|diamonds|stars|grid|frames|mixed","particles":"none|embers|rain|snow|dust|sparks","motion":"still|drift|rotate|bounce","pulse":"none|beat|halfbeat|offbeat"},
 "density": 0-1,
 "bpm": song BPM if the user gave or named a known song, else 0,
 "sections": [{"startPct": 0-100, "palette": {only the colours that change}}],
 "motifs": [{"layer":"back|front","every": blocks between copies 4-60,"y": blocks above ground 0-12,"scale": 0.3-3,"parts":[{"x":0,"y":0,"w":1,"h":1,"rot":0,"col":"accent"}]}],
 "notes": "2-4 short sentences: what to polish by hand to make it look hand-made"
}

What makes deco look professional:
- Gameplay must read instantly: blocks and outlines contrast hard with the background; background is darker and lower-contrast.
- 2-3 hues plus near-black/white. Glow is the outline colour or a brighter version of it. Intense levels use near-black bg, fill and ground.
- Use 0-6 sections for build-ups, drops and mood shifts (e.g. calm blue intro, red drop at 40%).
- Motifs (0-3, up to 24 parts each, x/y/w/h in blocks relative to the motif centre, rot in degrees clockwise, col is a palette name) should form recognisable shapes: thorns, crosses, eyes, crystals, gears, skull outlines, chains, runes. Layer multiple rectangles for depth (dark copy behind, bright core in front). Front motifs only appear where there's no gameplay.
- Match density to the vibe: minimal levels 0.2-0.4, busy effect levels 0.7-1.)";

char const* const kLayoutSystem = R"(You design Geometry Dash layouts. A Geode mod builds them from segments of game modes. Reply with ONLY one JSON object, no markdown:
{"segments":[{"mode":"cube|ship|ball|ufo|wave","length": blocks 10-200,"difficulty": 1-10}],"notes":"1-3 short sentences on how to refine it"}
Make the total length close to what the user asked for. Pace it like a real level: an easier intro, mode switches every 20-60 blocks, the hardest part near a drop, a short calm ending. Match the vibe (e.g. horror = tight wave and ship parts, chill = long cube and ufo parts).)";

char const* const kIdeasSystem = R"(You are a creative Geometry Dash level designer and Geode mod developer. Give 5 ideas in Markdown with a ## heading each.
For level ideas: concept, gameplay hooks, a deco direction (palette as hex colours, shapes, effects) and the kind of song.
For mod ideas: what it does, how it works in Geode (which classes to hook, e.g. PlayLayer, EditorUI, GJBaseGameLayer) and a short rough C++ snippet using Geode's $modify.
Keep every idea compact and practical.)";

bool hasApiKey() { return !Mod::get()->getSettingValue<std::string>("api-key").empty(); }

void askClaude(async::TaskHolder<web::WebResponse>& holder, std::string const& system, std::string const& user, int maxTokens,
               Function<void(Result<std::string>)> cb) {
    auto key = Mod::get()->getSettingValue<std::string>("api-key");
    auto model = Mod::get()->getSettingValue<std::string>("model");
    if (model.empty()) model = "claude-sonnet-5-5";
    // trim spaces people paste by accident
    while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
    while (!key.empty() && std::isspace(static_cast<unsigned char>(key.front()))) key.erase(key.begin());
    if (key.empty()) {
        cb(Err("Add your Anthropic API key in the Deco AI settings first"));
        return;
    }

    auto messages = matjson::Value::array();
    messages.push(matjson::makeObject({{"role", "user"}, {"content", user}}));
    auto body = matjson::makeObject({
        {"model", model},
        {"max_tokens", maxTokens},
        {"system", system},
        {"messages", messages},
    });

    auto req = web::WebRequest();
    req.userAgent(fmt::format("DecoAI/{}", Mod::get()->getVersion().toVString()));
    req.timeout(std::chrono::seconds(180));
    req.header("x-api-key", key);
    req.header("anthropic-version", "2023-06-01");
    req.header("content-type", "application/json");
    req.bodyJSON(body);

    holder.spawn(req.post("https://api.anthropic.com/v1/messages"), [cb = std::move(cb)](web::WebResponse res) mutable {
        auto text = res.string().unwrapOr("");
        auto parsed = matjson::parse(text);
        if (!res.ok()) {
            if (res.code() == 0) {
                cb(Err("Couldn't reach api.anthropic.com - check your internet"));
                return;
            }
            std::string msg = "unknown error";
            if (parsed) {
                auto json = parsed.unwrap();
                if (json.contains("error") && json["error"].isObject() && json["error"].contains("message"))
                    msg = json["error"]["message"].asString().unwrapOr(msg);
            }
            if (res.code() == 401) msg = "your API key was rejected - check it in the settings";
            cb(Err(fmt::format("Claude API error {}: {}", res.code(), msg)));
            return;
        }
        if (!parsed) {
            cb(Err("Couldn't read Claude's reply"));
            return;
        }
        auto json = parsed.unwrap();
        std::string out;
        if (json.contains("content") && json["content"].isArray()) {
            auto const& content = json["content"];
            for (size_t i = 0; i < content.size(); i++) {
                auto const& block = content[i];
                if (block.contains("type") && block["type"].asString().unwrapOr("") == "text" && block.contains("text"))
                    out += block["text"].asString().unwrapOr("");
            }
        }
        if (out.empty()) {
            cb(Err("Claude sent an empty reply"));
            return;
        }
        cb(Ok(out));
    });
}

} // namespace deco
