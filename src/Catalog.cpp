// Deco AI - object catalog.

#include "Catalog.hpp"

#include <Geode/Geode.hpp>

#include <cmath>

using namespace geode::prelude;

namespace deco {

static bool goodSquare(GameObject* obj) {
    if (!obj) return false;
    if (!obj->m_isSolidColorBlock) return false;
    auto size = obj->getContentSize();
    return std::abs(size.width - size.height) < 1.f && size.width > 10.f;
}

Catalog const& catalog() {
    static Catalog c;
    if (c.scanned) return c;
    c.scanned = true;

    // 211 is the classic colourable square; check it really is one in this GD version
    if (auto obj = GameObject::createWithKey(211); goodSquare(obj)) {
        c.square = 211;
        c.squareSize = obj->getContentSize().width;
        return c;
    }
    auto toolbox = ObjectToolbox::sharedState();
    for (auto const& [id, frame] : toolbox->m_allKeys) {
        std::string name = frame;
        if (name.rfind("edit_", 0) == 0) continue; // triggers and editor-only objects
        auto obj = GameObject::createWithKey(id);
        if (goodSquare(obj)) {
            c.square = id;
            c.squareSize = obj->getContentSize().width;
            log::info("Deco AI: using object {} ({}) as the deco square", id, name);
            return c;
        }
    }
    log::warn("Deco AI: no colourable square found, falling back to 211");
    return c;
}

std::string exportCatalog() {
    std::string out = "id\tframe\ttype\twidth\theight\tsolidColor\tdecoration\n";
    auto toolbox = ObjectToolbox::sharedState();
    int count = 0;
    for (auto const& [id, frame] : toolbox->m_allKeys) {
        std::string name = frame;
        auto obj = GameObject::createWithKey(id);
        if (!obj) {
            out += fmt::format("{}\t{}\t?\t0\t0\t0\t0\n", id, name);
            continue;
        }
        auto size = obj->getContentSize();
        out += fmt::format("{}\t{}\t{}\t{:.1f}\t{:.1f}\t{}\t{}\n", id, name, int(obj->m_objectType), size.width, size.height,
                           obj->m_isSolidColorBlock ? 1 : 0, obj->m_isDecoration ? 1 : 0);
        count++;
    }
    auto path = Mod::get()->getSaveDir() / "object-catalog.txt";
    if (auto res = utils::file::writeString(path, out); !res) {
        log::warn("Deco AI: couldn't write catalog: {}", res.unwrapErr());
        return "";
    }
    log::info("Deco AI: wrote {} objects to {}", count, path.string());
    return path.string();
}

} // namespace deco
