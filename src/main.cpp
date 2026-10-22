// Deco AI — GD editor glue for the decoration engine (engine.hpp).
//
// Everything runs locally: no network, no API key. This file only gathers
// data from the editor, hands it to the engine, and turns the engine's
// placements into real editor objects.

#include "engine.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {

// ================================================================ object kinds

bool isSolidLike(GameObject* o) {
    return o->m_objectType == GameObjectType::Solid || o->m_objectType == GameObjectType::Slope;
}

bool isTargetBlock(GameObject* o) {
    if (o->m_objectType != GameObjectType::Solid) return false;
    // Thin solid pieces (outline-tab lines and similar) are not blocks to decorate.
    auto r = o->getObjectRect();
    return std::min(r.size.width, r.size.height) >= 7.f;
}

// Solid but too thin to be a block: these have collision, so they're never deco.
bool isThinSolid(GameObject* o) {
    return o->m_objectType == GameObjectType::Solid && !isTargetBlock(o);
}

// Pure decoration only: never spikes, orbs, pads, portals, coins or triggers.
bool isDeco(GameObject* o) {
    return o->m_objectType == GameObjectType::Decoration && !o->m_isTrigger;
}

bool isHazard(GameObject* o) {
    return o->m_objectType == GameObjectType::Hazard || o->m_objectType == GameObjectType::AnimatedHazard;
}

// Things the player interacts with that deco must never cover.
bool isGameplay(GameObject* o) {
    if (o->m_isTrigger) return false;
    switch (o->m_objectType) {
        case GameObjectType::Solid:
        case GameObjectType::Slope:
        case GameObjectType::Decoration:
        case GameObjectType::Hazard:
        case GameObjectType::AnimatedHazard:
        case GameObjectType::Modifier:
        case GameObjectType::Special:
        case GameObjectType::EnterEffectObject:
        case GameObjectType::CollisionObject:
            return false;
        default:
            return true;
    }
}

deco::Rect toRect(CCRect const& r) {
    return {r.getMinX(), r.getMinY(), r.getMaxX(), r.getMaxY()};
}

// ================================================================ save strings

std::vector<std::string> split(std::string const& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

std::string clean(std::string s) {
    while (!s.empty() && (s.back() == ';' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// Rewrites position and Z order, and drops keys a copy must not inherit:
// linked groups always (would tie every copy together), trigger groups unless
// the user opted in (would make copies move with the original's triggers).
std::string rewrite(std::string const& src, double x, double y, int zOrder, double rotDelta, bool keepGroups) {
    auto parts = split(src, ',');
    std::string out;
    bool hasX = false, hasY = false, hasZ = false, hasR = false;
    auto newRot = [&](std::string const& old) {
        double r = 0;
        try { r = std::stod(old); } catch (...) {}
        r = std::fmod(r + rotDelta, 360.0);
        if (r > 180.0) r -= 360.0;
        if (r <= -180.0) r += 360.0;
        if (std::abs(r) < 1e-6) r = 0;
        return fmt::format("{:.3f}", r);
    };
    auto put = [&](std::string const& k, std::string const& v) {
        if (!out.empty()) out += ',';
        out += k;
        out += ',';
        out += v;
    };
    for (size_t i = 0; i + 1 < parts.size(); i += 2) {
        auto const& k = parts[i];
        if (k == "2") { put(k, fmt::format("{:.3f}", x)); hasX = true; }
        else if (k == "3") { put(k, fmt::format("{:.3f}", y)); hasY = true; }
        else if (k == "25") { put(k, std::to_string(zOrder)); hasZ = true; }
        else if (k == "6") { put(k, newRot(parts[i + 1])); hasR = true; }
        else if (k == "108") continue;
        else if (!keepGroups && (k == "57" || k == "34")) continue;
        else put(k, parts[i + 1]);
    }
    if (!hasX) put("2", fmt::format("{:.3f}", x));
    if (!hasY) put("3", fmt::format("{:.3f}", y));
    if (!hasZ && zOrder != 0) put("25", std::to_string(zOrder));
    if (!hasR && std::abs(rotDelta) > 1e-6) put("6", newRot("0"));
    return out;
}

// ================================================================ kit storage

std::vector<deco::Item> g_items;
int g_nextSeq = 1;
int g_nextKit = 1;
bool g_loaded = false;

constexpr char const* kStoreKey = "kit-v3";

void save() {
    std::string s = fmt::format("#{}\t{}\n", g_nextSeq, g_nextKit);
    for (auto const& it : g_items) {
        s += fmt::format("{}\t{}\t{:.3f}\t{:.3f}\t{:.3f}\t{}\t{}\t{:.3f}\t{}\t{:.3f}\t{:.3f}\t{}\t{}\t{}\t{}\t{}\n",
            static_cast<int>(it.role), it.designed, it.bw, it.bh, it.rot0, it.longIsLocalX ? 1 : 0,
            it.axisAligned ? 1 : 0, it.normalOffset, it.autoOffset ? 1 : 0, it.offX, it.offY,
            it.kit, it.seq, it.objectID, it.zOrder0, it.saveString);
    }
    Mod::get()->setSavedValue<std::string>(kStoreKey, s);
}

void loadOnce() {
    if (g_loaded) return;
    g_loaded = true;
    auto data = Mod::get()->getSavedValue<std::string>(kStoreKey, "");
    for (auto const& line : split(data, '\n')) {
        if (line.empty()) continue;
        auto f = split(line, '\t');
        try {
            if (line[0] == '#') {
                g_nextSeq = std::stoi(f[0].substr(1));
                g_nextKit = std::stoi(f.at(1));
                continue;
            }
            if (f.size() < 16) continue;
            deco::Item it;
            int role = std::stoi(f[0]);
            if (role < 0 || role > 4) continue;
            it.role = static_cast<deco::Role>(role);
            it.designed = std::stoi(f[1]) & 3;
            it.bw = std::stod(f[2]);
            it.bh = std::stod(f[3]);
            it.rot0 = std::stod(f[4]);
            it.longIsLocalX = f[5] == "1";
            it.axisAligned = f[6] == "1";
            it.normalOffset = std::stod(f[7]);
            it.autoOffset = f[8] == "1";
            it.offX = std::stod(f[9]);
            it.offY = std::stod(f[10]);
            it.kit = std::stoi(f[11]);
            it.seq = std::stoi(f[12]);
            it.objectID = std::stoi(f[13]);
            it.zOrder0 = std::stoi(f[14]);
            it.saveString = f[15];
            if (it.saveString.empty() || it.bw <= 0 || it.bh <= 0) continue;
            g_items.push_back(std::move(it));
        } catch (...) {
            continue;
        }
    }
}

int latestKit() {
    int k = 0;
    for (auto const& it : g_items) k = std::max(k, it.kit);
    return k;
}

std::string kitSummary() {
    int kit = latestKit();
    int learned[5] = {}, loose[5] = {};
    for (auto const& it : g_items) {
        int r = static_cast<int>(it.role);
        if (it.kit == 0) loose[r]++;
        else if (it.kit == kit) learned[r]++;
    }
    auto line = [&](int* counts) {
        std::string s;
        for (int r = 0; r < 5; ++r) {
            if (!counts[r]) continue;
            if (!s.empty()) s += ", ";
            s += fmt::format("{} {}", counts[r], deco::roleName(static_cast<deco::Role>(r)));
        }
        return s.empty() ? std::string("nothing yet") : s;
    };
    return fmt::format(
        "<cg>Learned style:</c> {}\n<cy>Palette:</c> {}\n\n"
        "The learned style is used first. Palette pieces fill only what it doesn't cover, "
        "and only one outline style is ever used at a time.",
        line(learned), line(loose));
}

// Measures a deco object as the engine needs it.
struct Measure {
    double bw, bh, rot, cx, cy;
};

Measure measure(GameObject* o) {
    auto bb = o->boundingBox();
    auto pos = o->getPosition();
    return {std::max<double>(bb.size.width, 0.5), std::max<double>(bb.size.height, 0.5),
            static_cast<double>(o->getRotation()), pos.x, pos.y};
}

void fillCommon(deco::Item& it, GameObject* o, LevelEditorLayer* layer) {
    it.objectID = o->m_objectID;
    it.zOrder0 = o->m_zOrder;
    it.saveString = clean(std::string(o->getSaveString(layer)));
    it.seq = g_nextSeq++;
}

void notify(std::string const& text, NotificationIcon icon) {
    Notification::create(text, icon)->show();
}

} // namespace

// ================================================================ editor hook

class $modify(DecoEditorUI, EditorUI) {
    struct Fields {
        CCMenu* menu = nullptr;
    };

    bool init(LevelEditorLayer* editorLayer) {
        if (!EditorUI::init(editorLayer)) return false;
        loadOnce();

        auto win = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setID("deco-ai-menu"_spr);
        // A row just under the top bar, between the undo/zoom buttons on the left and
        // the edit buttons on the right, so it never covers the editor's own UI.
        float cx = win.width * 0.56f, cy = win.height - 44.f;
        menu->setPosition({cx, cy});

        std::vector<CCMenuItemSpriteExtra*> buttons;
        auto addButton = [&](char const* label, SEL_MenuHandler handler, char const* id) {
            auto spr = ButtonSprite::create(label, "goldFont.fnt", "GJ_button_04.png", 0.8f);
            spr->setScale(0.42f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, handler);
            btn->setID(id);
            menu->addChild(btn);
            buttons.push_back(btn);
        };
        addButton("Learn", menu_selector(DecoEditorUI::onDecoLearn), "learn-button"_spr);
        addButton("Palette", menu_selector(DecoEditorUI::onDecoPalette), "palette-button"_spr);
        addButton("Deco!", menu_selector(DecoEditorUI::onDecoApply), "deco-button"_spr);
        addButton("Kit", menu_selector(DecoEditorUI::onDecoKit), "kit-button"_spr);

        float total = 0.f, gap = 4.f;
        for (auto b : buttons) total += b->getScaledContentSize().width;
        total += gap * (buttons.size() - 1);
        float x = -total / 2.f;
        for (auto b : buttons) {
            float w = b->getScaledContentSize().width;
            b->setPosition({x + w / 2.f, 0.f});
            x += w + gap;
        }

        this->addChild(menu, 100);
        m_fields->menu = menu;
        this->schedule(schedule_selector(DecoEditorUI::updateDecoMenu), 0.1f);
        return true;
    }

    // Hide the buttons while playtesting, like the editor's own UI.
    void updateDecoMenu(float) {
        if (auto menu = m_fields->menu)
            menu->setVisible(m_editorLayer->m_playbackMode == PlaybackMode::Not);
    }

    std::vector<GameObject*> selection() {
        std::vector<GameObject*> out;
        if (auto sel = this->getSelectedObjects())
            for (auto o : CCArrayExt<GameObject*>(sel)) out.push_back(o);
        return out;
    }

    // ------------------------------------------------------------ Learn

    void onDecoLearn(CCObject*) {
        std::vector<GameObject*> blocks, decos;
        int collision = 0, gameplay = 0;
        for (auto o : selection()) {
            if (isTargetBlock(o)) blocks.push_back(o);
            else if (isDeco(o)) decos.push_back(o);
            else if (isThinSolid(o) || o->m_objectType == GameObjectType::Slope) collision++;
            else gameplay++;
        }
        if (blocks.empty() || decos.empty()) {
            std::string seen = fmt::format(
                "You selected: <cg>{} block(s)</c>, <cy>{} deco piece(s)</c>", blocks.size(), decos.size());
            if (collision) seen += fmt::format(", <cr>{} piece(s) with collision</c>", collision);
            if (gameplay) seen += fmt::format(", <cr>{} gameplay object(s)</c>", gameplay);
            std::string why;
            if (blocks.empty()) {
                why = "Include the example block itself in the selection.";
            } else if (collision) {
                why = "Pieces with collision (like the lines in the outline tab) count as part of the "
                      "level, so they can't be learned. Use pieces without collision: glow, details, "
                      "deco lines.";
            } else {
                why = "Also select the deco you placed on and around the block.";
            }
            createQuickPopup("Nothing to learn", seen + ".\n\n" + why, "OK", nullptr, [](FLAlertLayer*, bool) {});
            return;
        }

        int kit = g_nextKit++;
        std::vector<deco::Item> learned;
        int air = 0;
        for (auto d : decos) {
            auto m = measure(d);
            // Anchor: the selected block nearest to this piece.
            GameObject* best = nullptr;
            double bestDist = 1e18;
            for (auto b : blocks) {
                auto r = toRect(b->getObjectRect());
                double ddx = std::max({r.x0 - m.cx, 0.0, m.cx - r.x1});
                double ddy = std::max({r.y0 - m.cy, 0.0, m.cy - r.y1});
                double dist = ddx * ddx + ddy * ddy;
                if (dist < bestDist) { bestDist = dist; best = b; }
            }
            deco::Item it;
            if (!best || !deco::classifyLearned(toRect(best->getObjectRect()), m.bw, m.bh, m.rot, m.cx, m.cy, it)) {
                air++;
                continue;
            }
            fillCommon(it, d, m_editorLayer);
            it.kit = kit;
            learned.push_back(std::move(it));
        }
        if (learned.empty()) {
            notify("None of the deco was on or around the block", NotificationIcon::Error);
            return;
        }

        // A new example replaces the previous learned style.
        std::erase_if(g_items, [](deco::Item const& it) { return it.kit > 0; });
        for (auto& it : learned) g_items.push_back(std::move(it));
        save();

        std::string extra;
        if (air) extra += fmt::format("\n<co>{} floating piece(s) skipped</c> (too far from the block).", air);
        if (collision) extra += fmt::format("\n<co>{} piece(s) with collision skipped</c> (they'd change gameplay).", collision);
        if (gameplay) extra += fmt::format("\n<co>{} gameplay object(s) ignored</c> (spikes, orbs, pads...).", gameplay);
        createQuickPopup("Style learned", kitSummary() + extra, "OK", nullptr, [](FLAlertLayer*, bool) {});
    }

    // ------------------------------------------------------------ Palette

    void onDecoPalette(CCObject*) {
        int added = 0, ignored = 0, collision = 0;
        for (auto o : selection()) {
            if (!isDeco(o)) {
                if (o->m_objectType == GameObjectType::Solid || o->m_objectType == GameObjectType::Slope) collision++;
                else ignored++;
                continue;
            }
            auto m = measure(o);
            deco::Item it = deco::classifyLoose(m.bw, m.bh, m.rot);
            fillCommon(it, o, m_editorLayer);
            it.kit = 0;
            g_items.push_back(std::move(it));
            added++;
        }
        std::string extra;
        if (collision) extra += fmt::format("\n<co>{} piece(s) with collision skipped</c> (blocks, outline-tab lines...).", collision);
        if (ignored) extra += fmt::format("\n<co>{} gameplay object(s) skipped</c> (spikes, orbs, pads...).", ignored);
        if (!added) {
            if (extra.empty()) {
                notify("Select some deco pieces first", NotificationIcon::Error);
            } else {
                createQuickPopup("Nothing added",
                    "Only pieces without collision can go in the palette." + extra, "OK", nullptr,
                    [](FLAlertLayer*, bool) {});
            }
            return;
        }
        save();
        createQuickPopup("Added to palette", fmt::format("Added {} piece(s).\n\n", added) + kitSummary() + extra,
            "OK", nullptr, [](FLAlertLayer*, bool) {});
    }

    // ------------------------------------------------------------ Deco!

    void onDecoApply(CCObject*) {
        if (g_items.empty()) {
            notify("Teach me a style first: Learn or Palette", NotificationIcon::Error);
            return;
        }

        deco::Input in;
        for (auto o : selection())
            if (isTargetBlock(o)) in.targets.push_back(toRect(o->getObjectRect()));
        if (in.targets.empty()) {
            notify("Select the blocks you want decorated", NotificationIcon::Error);
            return;
        }

        for (auto o : CCArrayExt<GameObject*>(m_editorLayer->m_objects)) {
            if (isSolidLike(o)) {
                in.solids.push_back(toRect(o->getObjectRect()));
            } else if (isHazard(o)) {
                in.hazards.push_back(toRect(o->boundingBox()));
            } else if (isDeco(o)) {
                auto p = o->getPosition();
                in.existing.push_back({o->m_objectID, p.x, p.y});
                auto bb = o->boundingBox();
                if (std::max(bb.size.width, bb.size.height) <= 45.f) in.existingSmall.push_back(toRect(bb));
            } else if (isGameplay(o)) {
                in.gameplay.push_back(toRect(o->boundingBox()));
            }
        }

        auto mod = Mod::get();
        in.items = g_items;
        in.settings.outlines = mod->getSettingValue<bool>("outlines");
        in.settings.corners = mod->getSettingValue<bool>("corners");
        in.settings.edgeDetails = mod->getSettingValue<bool>("edge-details");
        in.settings.bottomDetails = mod->getSettingValue<bool>("bottom-details");
        in.settings.interior = mod->getSettingValue<bool>("interior");
        in.settings.large = mod->getSettingValue<bool>("large-pieces");
        in.settings.density = mod->getSettingValue<double>("density");
        in.settings.maxObjects = static_cast<int>(mod->getSettingValue<int64_t>("max-objects"));
        bool keepGroups = mod->getSettingValue<bool>("keep-groups");

        auto result = deco::generate(in);
        if (result.placements.empty()) {
            notify(result.stats.skippedDuplicate ? "Already decorated: nothing new to add"
                                                 : "Nothing fits here with the current style",
                NotificationIcon::Info);
            return;
        }

        auto created = CCArray::create();
        for (auto const& p : result.placements) {
            auto const& it = in.items[p.item];
            auto str = rewrite(it.saveString, p.x, p.y, it.zOrder0 + p.zRank, p.rotDelta, keepGroups);
            auto arr = m_editorLayer->createObjectsFromString(gd::string(str), true, true);
            if (!arr || arr->count() == 0) continue;
            auto obj = static_cast<GameObject*>(arr->objectAtIndex(0));
            if (std::abs(p.scaleLong - 1.0) > 0.005) {
                if (it.longIsLocalX) obj->updateCustomScaleX(obj->m_scaleX * static_cast<float>(p.scaleLong));
                else obj->updateCustomScaleY(obj->m_scaleY * static_cast<float>(p.scaleLong));
            }
            created->addObjectsFromArray(arr);
        }

        if (created->count() == 0) {
            notify("Could not create the objects", NotificationIcon::Error);
            return;
        }
        // One undo step for the whole run: Ctrl+Z removes it all.
        m_editorLayer->addToUndoList(UndoObject::createWithArray(created, UndoCommand::Paste), false);
        this->deselectAll();
        this->selectObjects(created, true);
        this->updateButtons();
        this->updateObjectInfoLabel();

        auto const& st = result.stats;
        std::string parts;
        auto add = [&](int n, char const* what) {
            if (!n) return;
            if (!parts.empty()) parts += ", ";
            parts += fmt::format("{} {}", n, what);
        };
        add(st.lines, "outline");
        add(st.corners, "corner");
        add(st.edgeDetails, "edge");
        add(st.large, "large");
        add(st.interior, "detail");
        notify(fmt::format("Placed {} ({}){}. Undo removes it all.", created->count(), parts,
                   result.capped ? " - hit the max objects setting" : ""),
            NotificationIcon::Success);
    }

    // ------------------------------------------------------------ Kit

    void onDecoKit(CCObject*) {
        createQuickPopup("Deco AI", kitSummary(), "Forget all", "OK", [](FLAlertLayer*, bool ok) {
            if (ok) return;
            createQuickPopup("Forget everything?", "This clears the learned style and the whole palette.",
                "Cancel", "Forget", [](FLAlertLayer*, bool confirm) {
                    if (!confirm) return;
                    g_items.clear();
                    save();
                    notify("Style and palette cleared", NotificationIcon::Success);
                });
        });
    }
};
