// Deco AI — GD editor glue for the decoration engine.
//
// Everything runs locally: no network, no API key. This file gathers data
// from the editor, hands it to the pure engines (engine.hpp for block edges,
// templates.hpp for decoration around any other object), turns the results
// into real editor objects, and provides the style library UI.

#include "engine.hpp"
#include "styles.hpp"
#include "templates.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
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

// Pure decoration only: never spikes, orbs, pads, portals, coins or triggers.
bool isDeco(GameObject* o) {
    return o->m_objectType == GameObjectType::Decoration && !o->m_isTrigger;
}

bool isHazard(GameObject* o) {
    return o->m_objectType == GameObjectType::Hazard || o->m_objectType == GameObjectType::AnimatedHazard;
}

// Things the player interacts with that block deco must never cover.
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

// Anything that deco can be learned around / applied to, other than plain blocks.
bool isAnchor(GameObject* o) {
    if (o->m_isTrigger || isDeco(o) || isTargetBlock(o)) return false;
    // Thin pieces with collision (outline-tab lines) are part of block designs,
    // not objects to decorate around.
    if (o->m_objectType == GameObjectType::Solid) return false;
    switch (o->m_objectType) {
        case GameObjectType::Modifier:
        case GameObjectType::EnterEffectObject:
        case GameObjectType::CollisionObject:
            return false;
        default:
            return true;
    }
}

deco::AnchorClass anchorClass(GameObject* o) {
    using T = GameObjectType;
    switch (o->m_objectType) {
        case T::Solid: return deco::AnchorClass::Block;
        case T::Slope: return deco::AnchorClass::Slope;
        case T::Hazard:
        case T::AnimatedHazard: return deco::AnchorClass::Hazard;
        case T::YellowJumpRing:
        case T::PinkJumpRing:
        case T::GravityRing:
        case T::GreenRing:
        case T::RedJumpRing:
        case T::CustomRing:
        case T::DashRing:
        case T::GravityDashRing:
        case T::DropRing:
        case T::SpiderOrb:
        case T::TeleportOrb: return deco::AnchorClass::Orb;
        case T::YellowJumpPad:
        case T::PinkJumpPad:
        case T::GravityPad:
        case T::RedJumpPad:
        case T::SpiderPad: return deco::AnchorClass::Pad;
        case T::InverseGravityPortal:
        case T::NormalGravityPortal:
        case T::ShipPortal:
        case T::CubePortal:
        case T::InverseMirrorPortal:
        case T::NormalMirrorPortal:
        case T::BallPortal:
        case T::RegularSizePortal:
        case T::MiniSizePortal:
        case T::UfoPortal:
        case T::DualPortal:
        case T::SoloPortal:
        case T::WavePortal:
        case T::RobotPortal:
        case T::TeleportPortal:
        case T::SpiderPortal:
        case T::SwingPortal:
        case T::GravityTogglePortal: return deco::AnchorClass::Portal;
        case T::SecretCoin:
        case T::UserCoin:
        case T::Collectible: return deco::AnchorClass::Collectible;
        default: return deco::AnchorClass::Other;
    }
}

deco::Rect toRect(CCRect const& r) {
    return {r.getMinX(), r.getMinY(), r.getMaxX(), r.getMaxY()};
}

// Visual rect for an anchor: blocks use their hitbox, everything else its sprite.
deco::Rect anchorRect(GameObject* o) {
    return isTargetBlock(o) ? toRect(o->getObjectRect()) : toRect(o->boundingBox());
}

deco::Pose poseOf(GameObject* o) {
    auto p = o->getPosition();
    deco::Pose pose;
    pose.x = p.x;
    pose.y = p.y;
    pose.rot = o->getRotation();
    pose.flipX = o->m_isFlipX;
    pose.flipY = o->m_isFlipY;
    pose.sx = std::max(0.01f, std::abs(o->m_scaleX));
    pose.sy = std::max(0.01f, std::abs(o->m_scaleY));
    return pose;
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

std::string fmtRot(double r) {
    r = std::fmod(r, 360.0);
    if (r > 180.0) r -= 360.0;
    if (r <= -180.0) r += 360.0;
    if (std::abs(r) < 1e-6) r = 0;
    return fmt::format("{:.3f}", r);
}

struct Overrides {
    double x = 0, y = 0;
    int zOrder = 0;
    double rotDelta = 0;         // added to the piece's own rotation (block pieces)
    bool absolute = false;       // template pieces: write rotation and flips outright
    double rot = 0;
    bool flipX = false;
};

// Rewrites position, rotation and Z order, and drops keys a copy must not
// inherit: linked groups always (would tie every copy together), and trigger
// groups unless the user opted in (would make copies move with your triggers).
std::string rewrite(std::string const& src, Overrides const& o, bool keepGroups) {
    auto parts = split(src, ',');
    std::string out;
    bool hasX = false, hasY = false, hasZ = false, hasR = false;
    auto put = [&](std::string const& k, std::string const& v) {
        if (!out.empty()) out += ',';
        out += k;
        out += ',';
        out += v;
    };
    for (size_t i = 0; i + 1 < parts.size(); i += 2) {
        auto const& k = parts[i];
        auto const& v = parts[i + 1];
        if (k == "2") { put(k, fmt::format("{:.3f}", o.x)); hasX = true; }
        else if (k == "3") { put(k, fmt::format("{:.3f}", o.y)); hasY = true; }
        else if (k == "25") { put(k, std::to_string(o.zOrder)); hasZ = true; }
        else if (k == "6") {
            double r = 0;
            try { r = std::stod(v); } catch (...) {}
            put(k, fmtRot(o.absolute ? o.rot : r + o.rotDelta));
            hasR = true;
        }
        else if (o.absolute && (k == "4" || k == "5")) continue; // rewritten below
        else if (k == "108") continue;
        else if (!keepGroups && (k == "57" || k == "34")) continue;
        else put(k, v);
    }
    if (!hasX) put("2", fmt::format("{:.3f}", o.x));
    if (!hasY) put("3", fmt::format("{:.3f}", o.y));
    if (!hasZ && o.zOrder != 0) put("25", std::to_string(o.zOrder));
    if (!hasR) {
        double r = o.absolute ? o.rot : o.rotDelta;
        if (std::abs(r) > 1e-6) put("6", fmtRot(r));
    }
    if (o.absolute && o.flipX) put("4", "1");
    return out;
}

// Offset between where an object sits in the editor and the position written
// in its save string (GD stores Y relative to the ground). Measured from a real
// object instead of hard-coded, so it is always right.
CCPoint saveOffset(GameObject* o, LevelEditorLayer* layer) {
    auto parts = split(clean(std::string(o->getSaveString(layer))), ',');
    double sx = 0, sy = 0;
    bool hx = false, hy = false;
    for (size_t i = 0; i + 1 < parts.size(); i += 2) {
        try {
            if (parts[i] == "2") { sx = std::stod(parts[i + 1]); hx = true; }
            else if (parts[i] == "3") { sy = std::stod(parts[i + 1]); hy = true; }
        } catch (...) {}
    }
    auto p = o->getPosition();
    return {hx ? static_cast<float>(p.x - sx) : 0.f, hy ? static_cast<float>(p.y - sy) : 0.f};
}

// ================================================================ colour channels

// The level's own colour settings (what gets saved with the level) and the
// editor's live copy (what you see). Usually the same object; handled if not.
std::vector<GJEffectManager*> colorManagers(LevelEditorLayer* layer) {
    std::vector<GJEffectManager*> out;
    if (layer->m_levelSettings && layer->m_levelSettings->m_effectManager)
        out.push_back(layer->m_levelSettings->m_effectManager);
    if (layer->m_effectManager && (out.empty() || out[0] != layer->m_effectManager))
        out.push_back(layer->m_effectManager);
    return out;
}

deco::Channel readChannel(GJEffectManager* em, int id) {
    deco::Channel c;
    c.id = id;
    if (!em || !em->colorExists(id)) return c; // never set: default white
    auto a = em->getColorAction(id);
    if (!a) return c;
    c.r = a->m_fromColor.r;
    c.g = a->m_fromColor.g;
    c.b = a->m_fromColor.b;
    c.opacity = a->m_fromOpacity;
    c.blending = a->m_blending;
    c.copyID = a->m_copyID;
    c.h = a->m_copyHSV.h;
    c.s = a->m_copyHSV.s;
    c.v = a->m_copyHSV.v;
    c.absS = a->m_copyHSV.absoluteSaturation;
    c.absV = a->m_copyHSV.absoluteBrightness;
    c.copyOpacity = a->m_copyOpacity;
    c.playerColor = a->m_playerColor;
    return c;
}

bool channelInUse(deco::Channel const& c) {
    return c.r != 255 || c.g != 255 || c.b != 255 || std::abs(c.opacity - 1.0) > 1e-3 || c.copyID != 0 ||
           c.blending || c.playerColor > 0;
}

void writeChannel(LevelEditorLayer* layer, int id, deco::Channel const& c) {
    for (auto em : colorManagers(layer)) {
        auto a = em->getColorAction(id);
        if (!a) continue;
        ccColor3B col{static_cast<GLubyte>(c.r), static_cast<GLubyte>(c.g), static_cast<GLubyte>(c.b)};
        a->m_fromColor = col;
        a->m_toColor = col;
        a->m_color = col;
        a->m_fromOpacity = static_cast<float>(c.opacity);
        a->m_toOpacity = static_cast<float>(c.opacity);
        a->m_currentOpacity = static_cast<float>(c.opacity);
        a->m_blending = c.blending;
        a->m_copyID = c.copyID;
        a->m_copyHSV = cchsv(static_cast<float>(c.h), static_cast<float>(c.s), static_cast<float>(c.v), c.absS, c.absV);
        a->m_copyOpacity = c.copyOpacity;
        a->m_playerColor = c.playerColor;
        a->m_legacyHSV = false;
        em->colorActionChanged(a);
        if (a->m_colorSprite) em->updateColorAction(a);
    }
}

// Captures the settings of every colour channel the given save strings use,
// following copy-colour links so the whole chain comes along.
void learnChannels(LevelEditorLayer* layer, std::vector<std::string> const& saveStrings, deco::Style& style) {
    auto mgrs = colorManagers(layer);
    if (mgrs.empty()) return;
    std::vector<int> todo;
    for (auto const& str : saveStrings)
        for (int c : deco::channelsUsed(str)) todo.push_back(c);
    std::set<int> done;
    while (!todo.empty()) {
        int id = todo.back();
        todo.pop_back();
        if (id < 1 || id > 999 || !done.insert(id).second) continue;
        auto c = readChannel(mgrs[0], id);
        style.colors[id] = c;
        if (c.copyID >= 1 && c.copyID <= 999) todo.push_back(c.copyID);
    }
}

// ================================================================ library storage

deco::Library g_lib;
bool g_loaded = false;

std::filesystem::path savePath(std::string const& name) {
    return Mod::get()->getSaveDir() / name;
}

void saveLibrary() {
    auto path = savePath("styles.txt");
    auto tmp = savePath("styles.tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            log::error("[deco] could not write {}", tmp.string());
            return;
        }
        f << g_lib.serialize();
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::copy_file(tmp, path, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp, ec);
    }
}

// Earlier versions kept one kit in the mod's saved values; bring it over once.
void migrateOldKit() {
    auto data = Mod::get()->getSavedValue<std::string>("kit-v3", "");
    if (data.empty()) return;
    std::vector<deco::Item> items;
    for (auto const& line : split(data, '\n')) {
        if (line.empty() || line[0] == '#') continue;
        auto f = split(line, '\t');
        if (f.size() < 16) continue;
        try {
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
            it.kit = std::stoi(f[11]) > 0 ? 1 : 0;
            it.seq = std::stoi(f[12]);
            it.objectID = std::stoi(f[13]);
            it.zOrder0 = std::stoi(f[14]);
            it.saveString = f[15];
            if (!it.saveString.empty()) items.push_back(std::move(it));
        } catch (...) {}
    }
    if (!items.empty()) {
        auto& s = g_lib.create("My first style");
        s.items = std::move(items);
    }
}

void loadOnce() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream f(savePath("styles.txt"), std::ios::binary);
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        g_lib = deco::Library::deserialize(ss.str());
    } else {
        migrateOldKit();
        saveLibrary();
    }
}

deco::Style& activeStyle() {
    if (auto s = g_lib.active()) return *s;
    g_lib.create("");
    return *g_lib.active();
}

void notify(std::string const& text, NotificationIcon icon) {
    Notification::create(text, icon)->show();
}

void showInfo(char const* title, std::string const& body) {
    createQuickPopup(title, body, "OK", nullptr, 340.f, [](FLAlertLayer*, bool) {});
}

// ================================================================ thumbnails

// Renders a region of the level (layer coordinates) to a PNG in the save dir.
bool captureRegion(LevelEditorLayer* layer, CCRect region, std::filesystem::path const& file) {
    constexpr float W = 240.f, H = 135.f;
    if (region.size.width < 1 || region.size.height < 1) return false;
    float s = std::min(W / region.size.width, H / region.size.height);
    s = std::min(s, 4.f);

    auto rt = CCRenderTexture::create(static_cast<int>(W), static_cast<int>(H));
    if (!rt) return false;

    auto ol = layer->m_objectLayer;
    auto oldPos = ol->getPosition();
    auto oldScale = ol->getScale();
    bool gridVisible = layer->m_drawGridLayer && layer->m_drawGridLayer->isVisible();
    if (layer->m_drawGridLayer) layer->m_drawGridLayer->setVisible(false);

    float cx = region.getMidX(), cy = region.getMidY();
    ol->setScale(s);
    ol->setPosition({W / 2.f - cx * s, H / 2.f - cy * s});

    rt->beginWithClear(40 / 255.f, 72 / 255.f, 160 / 255.f, 1.f);
    ol->visit();
    rt->end();

    ol->setScale(oldScale);
    ol->setPosition(oldPos);
    if (layer->m_drawGridLayer) layer->m_drawGridLayer->setVisible(gridVisible);

    auto img = rt->newCCImage(true);
    if (!img) return false;
    bool ok = img->saveToFile(file.string().c_str(), true);
    img->release();
    CCTextureCache::sharedTextureCache()->removeTextureForKey(file.string().c_str());
    return ok;
}

// ================================================================ styles UI

class RenamePopup : public Popup {
protected:
    TextInput* m_input = nullptr;
    int m_styleId = 0;
    std::function<void()> m_done;

    bool init(int styleId, std::function<void()> done) {
        if (!Popup::init(260.f, 130.f)) return false;
        m_styleId = styleId;
        m_done = std::move(done);
        this->setTitle("Rename style");
        m_input = TextInput::create(200.f, "Style name");
        m_input->setCommonFilter(CommonFilter::Any);
        m_input->setMaxCharCount(32);
        if (auto s = g_lib.find(styleId)) m_input->setString(s->name);
        m_mainLayer->addChildAtPosition(m_input, Anchor::Center, {0.f, 5.f});
        auto ok = CCMenuItemSpriteExtra::create(ButtonSprite::create("OK"), this, menu_selector(RenamePopup::onOK));
        m_buttonMenu->addChildAtPosition(ok, Anchor::Bottom, {0.f, 25.f});
        return true;
    }

    void onOK(CCObject*) {
        if (auto s = g_lib.find(m_styleId)) {
            auto name = deco::cleanName(std::string(m_input->getString()));
            if (!name.empty()) s->name = name;
            saveLibrary();
        }
        auto done = m_done;
        this->onClose(nullptr);
        if (done) done();
    }

public:
    static RenamePopup* create(int styleId, std::function<void()> done) {
        auto ret = new RenamePopup();
        if (ret->init(styleId, std::move(done))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class StylesPopup : public Popup {
protected:
    ScrollLayer* m_list = nullptr;

    bool init() {
        if (!Popup::init(380.f, 260.f)) return false;
        this->setTitle("Deco styles");

        m_list = ScrollLayer::create({340.f, 170.f});
        m_list->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(4.f));
        m_mainLayer->addChildAtPosition(m_list, Anchor::Center, {-170.f, -95.f});

        auto newBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("New style", "goldFont.fnt", "GJ_button_01.png", .8f), this,
            menu_selector(StylesPopup::onNew));
        newBtn->setScale(.75f);
        newBtn->m_baseScale = .75f;
        m_buttonMenu->addChildAtPosition(newBtn, Anchor::Bottom, {0.f, 20.f});

        this->refresh();
        return true;
    }

    CCNode* makeRow(deco::Style const& s) {
        constexpr float RW = 336.f, RH = 56.f;
        auto row = CCNode::create();
        row->setContentSize({RW, RH});
        row->setAnchorPoint({0.5f, 0.5f});

        bool isActive = s.id == g_lib.activeId;
        auto bg = CCLayerColor::create(isActive ? ccc4(60, 200, 90, 90) : ccc4(0, 0, 0, 80), RW, RH);
        row->addChild(bg);

        // Thumbnail of the example the style was learned from.
        CCNode* thumb = nullptr;
        if (!s.preview.empty()) {
            auto path = savePath(s.preview);
            std::error_code ec;
            if (std::filesystem::exists(path, ec)) {
                auto spr = CCSprite::create(path.string().c_str());
                if (spr && spr->getContentSize().width > 0) {
                    float k = std::min(84.f / spr->getContentSize().width, 48.f / spr->getContentSize().height);
                    spr->setScale(k);
                    thumb = spr;
                }
            }
        }
        if (!thumb) {
            auto lbl = CCLabelBMFont::create("no picture", "chatFont.fnt");
            lbl->setScale(.5f);
            lbl->setOpacity(150);
            thumb = lbl;
        }
        thumb->setPosition({48.f, RH / 2.f});
        row->addChild(thumb);

        auto name = CCLabelBMFont::create(s.name.c_str(), "goldFont.fnt");
        name->setAnchorPoint({0.f, 0.5f});
        name->limitLabelWidth(150.f, .55f, .2f);
        name->setPosition({98.f, RH - 16.f});
        row->addChild(name);

        auto summary = CCLabelBMFont::create(s.summary().c_str(), "chatFont.fnt");
        summary->setAnchorPoint({0.f, 0.5f});
        summary->limitLabelWidth(150.f, .5f, .2f);
        summary->setPosition({98.f, 16.f});
        row->addChild(summary);

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->setContentSize({RW, RH});
        auto addBtn = [&](char const* text, char const* bgName, SEL_MenuHandler sel, float x, float y) {
            auto spr = ButtonSprite::create(text, "bigFont.fnt", bgName, .6f);
            spr->setScale(.45f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, sel);
            btn->setTag(s.id);
            btn->setPosition({x, y});
            menu->addChild(btn);
        };
        if (isActive) {
            auto lbl = CCLabelBMFont::create("In use", "bigFont.fnt");
            lbl->setScale(.35f);
            lbl->setColor({120, 255, 140});
            lbl->setPosition({RW - 62.f, RH - 15.f});
            row->addChild(lbl);
        } else {
            addBtn("Use", "GJ_button_01.png", menu_selector(StylesPopup::onUse), RW - 62.f, RH - 15.f);
        }
        addBtn("Rename", "GJ_button_04.png", menu_selector(StylesPopup::onRename), RW - 82.f, 15.f);
        addBtn("Delete", "GJ_button_06.png", menu_selector(StylesPopup::onDelete), RW - 30.f, 15.f);
        row->addChild(menu);
        return row;
    }

    void refresh() {
        auto content = m_list->m_contentLayer;
        content->removeAllChildren();
        if (g_lib.styles.empty()) {
            auto lbl = CCLabelBMFont::create(
                "No styles yet. Decorate an example, select it\nand press Learn.", "chatFont.fnt");
            lbl->setScale(.7f);
            lbl->setAlignment(kCCTextAlignmentCenter);
            content->addChild(lbl);
        }
        for (auto const& s : g_lib.styles) content->addChild(this->makeRow(s));
        content->updateLayout();
        m_list->scrollToTop();
        handleTouchPriority(this);
    }

    void onUse(CCObject* sender) {
        g_lib.activeId = static_cast<CCNode*>(sender)->getTag();
        saveLibrary();
        this->refresh();
    }

    void onRename(CCObject* sender) {
        int id = static_cast<CCNode*>(sender)->getTag();
        Ref<StylesPopup> self = this;
        if (auto p = RenamePopup::create(id, [self]() { self->refresh(); })) p->show();
    }

    void onDelete(CCObject* sender) {
        int id = static_cast<CCNode*>(sender)->getTag();
        auto s = g_lib.find(id);
        if (!s) return;
        Ref<StylesPopup> self = this;
        createQuickPopup("Delete style?", fmt::format("Delete <cy>{}</c>? This can't be undone.", s->name),
            "Cancel", "Delete", [self, id](FLAlertLayer*, bool confirm) {
                if (!confirm) return;
                if (auto st = g_lib.find(id); st && !st->preview.empty()) {
                    std::error_code ec;
                    std::filesystem::remove(savePath(st->preview), ec);
                }
                g_lib.remove(id);
                saveLibrary();
                self->refresh();
            });
    }

    void onNew(CCObject*) {
        g_lib.create("");
        saveLibrary();
        this->refresh();
        notify("New empty style created and in use. Decorate an example and press Learn.", NotificationIcon::Info);
    }

public:
    static StylesPopup* create() {
        auto ret = new StylesPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

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
        // A row just under the top bar, clear of the editor's own buttons.
        menu->setPosition({win.width * 0.56f, win.height - 44.f});

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
        addButton("Styles", menu_selector(DecoEditorUI::onDecoStyles), "styles-button"_spr);

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
        auto sel = selection();
        std::vector<GameObject*> blocks, anchors, decos;
        int triggers = 0;
        for (auto o : sel) {
            if (isTargetBlock(o)) blocks.push_back(o);
            else if (isDeco(o)) decos.push_back(o);
            else if (isAnchor(o)) anchors.push_back(o);
            else triggers++;
        }
        if (decos.empty() || (blocks.empty() && anchors.empty())) {
            std::string seen = fmt::format("You selected: <cg>{} block(s)</c>, <cl>{} other object(s)</c>, "
                                           "<cy>{} deco piece(s)</c>.",
                blocks.size(), anchors.size(), decos.size());
            std::string why = decos.empty()
                ? "Also select the deco you placed. Only pieces without collision count as deco "
                  "(lines from the outline tab have collision)."
                : "Also select the block, spike, orb, slope... that the deco belongs to.";
            showInfo("Nothing to learn", seen + "\n\n" + why);
            return;
        }

        // Assign every deco piece to the closest selected object. When two are
        // equally close (e.g. a spike sitting on a block), the smaller one wins.
        struct Owner { GameObject* obj; deco::Rect rect; double area; };
        std::vector<Owner> owners;
        for (auto b : blocks) { auto r = anchorRect(b); owners.push_back({b, r, r.w() * r.h()}); }
        for (auto a : anchors) { auto r = anchorRect(a); owners.push_back({a, r, std::max(1.0, r.w() * r.h())}); }

        // Blocks claim deco on and around them (up to 45 units away). Other
        // objects (spikes, orbs...) only claim deco that actually touches them
        // and isn't much bigger than they are, so ground and block deco next
        // to a spike is never mistaken for the spike's own decoration.
        std::map<GameObject*, std::vector<GameObject*>> assigned;
        int air = 0;
        for (auto d : decos) {
            auto p = d->getPosition();
            auto dbb = d->boundingBox();
            double darea = std::max(1.0, static_cast<double>(dbb.size.width) * dbb.size.height);
            Owner const* best = nullptr;
            double bestDist = 1e18;
            for (auto const& o : owners) {
                bool isBlock = isTargetBlock(o.obj);
                double ddx = std::max({o.rect.x0 - p.x, 0.0, p.x - o.rect.x1});
                double ddy = std::max({o.rect.y0 - p.y, 0.0, p.y - o.rect.y1});
                double dist = std::sqrt(ddx * ddx + ddy * ddy);
                if (isBlock) {
                    if (dist > 45.0) continue;
                } else {
                    // Must touch: the piece's box overlaps the object's box (2 units of slack).
                    bool touches = dbb.getMinX() <= o.rect.x1 + 2 && dbb.getMaxX() >= o.rect.x0 - 2 &&
                                   dbb.getMinY() <= o.rect.y1 + 2 && dbb.getMaxY() >= o.rect.y0 - 2;
                    if (!touches || darea > 4.0 * o.area) continue;
                }
                if (!best || dist < bestDist - 0.5 || (std::abs(dist - bestDist) <= 0.5 && o.area < best->area)) {
                    best = &o;
                    bestDist = dist;
                }
            }
            if (!best) { air++; continue; }
            assigned[best->obj].push_back(d);
        }

        auto& style = activeStyle();
        int styleId = style.id;

        // Blocks: edge-aware pieces, combined across all example blocks so the
        // most common design per side and corner wins (no stacking variants).
        std::vector<deco::Rect> levelSolids;
        for (auto o : CCArrayExt<GameObject*>(m_editorLayer->m_objects))
            if (isSolidLike(o)) levelSolids.push_back(toRect(o->getObjectRect()));
        auto covered = [&](deco::Rect const& probe, GameObject* self) {
            auto selfRect = toRect(self->getObjectRect());
            for (auto const& r : levelSolids) {
                if (std::abs(r.x0 - selfRect.x0) < 0.01 && std::abs(r.y0 - selfRect.y0) < 0.01 &&
                    std::abs(r.x1 - selfRect.x1) < 0.01 && std::abs(r.y1 - selfRect.y1) < 0.01) continue;
                if (r.overlaps(probe)) return true;
            }
            return false;
        };
        std::vector<deco::BlockExample> examples;
        for (auto b : blocks) {
            auto it = assigned.find(b);
            if (it == assigned.end()) continue;
            auto rect = toRect(b->getObjectRect());
            deco::BlockExample ex;
            double mx = rect.w() * 0.25, my = rect.h() * 0.25;
            ex.exposed[deco::Top] = !covered({rect.x0 + mx, rect.y1 + 1, rect.x1 - mx, rect.y1 + 3}, b);
            ex.exposed[deco::Bottom] = !covered({rect.x0 + mx, rect.y0 - 3, rect.x1 - mx, rect.y0 - 1}, b);
            ex.exposed[deco::Left] = !covered({rect.x0 - 3, rect.y0 + my, rect.x0 - 1, rect.y1 - my}, b);
            ex.exposed[deco::Right] = !covered({rect.x1 + 1, rect.y0 + my, rect.x1 + 3, rect.y1 - my}, b);
            for (auto d : it->second) {
                auto bb = d->boundingBox();
                auto p = d->getPosition();
                deco::Item item;
                if (!deco::classifyLearned(rect, std::max<double>(bb.size.width, .5), std::max<double>(bb.size.height, .5),
                        d->getRotation(), p.x, p.y, item)) {
                    air++;
                    continue;
                }
                item.objectID = d->m_objectID;
                item.zOrder0 = d->m_zOrder;
                item.saveString = clean(std::string(d->getSaveString(m_editorLayer)));
                item.seq = g_lib.nextSeq++;
                item.kit = 1;
                ex.items.push_back(std::move(item));
            }
            examples.push_back(std::move(ex));
        }
        int blockExamples = static_cast<int>(examples.size());
        std::vector<deco::Item> learnedItems = deco::consensusItems(examples);
        if (!learnedItems.empty()) {
            // A new block example replaces the style's previous block design.
            std::erase_if(style.items, [](deco::Item const& it) { return it.kit > 0; });
            deco::mergeLearnedItems(style.items, learnedItems);
        }

        // Everything else: templates per object ID, with variants.
        std::map<int, std::pair<deco::AnchorTemplate, std::vector<std::vector<deco::TPiece>>>> fresh;
        for (auto a : anchors) {
            auto it = assigned.find(a);
            if (it == assigned.end()) continue;
            auto pose = poseOf(a);
            auto& entry = fresh[a->m_objectID];
            entry.first.objectID = a->m_objectID;
            entry.first.cls = anchorClass(a);
            auto r = anchorRect(a);
            entry.first.bw = r.w() / pose.sx;
            entry.first.bh = r.h() / pose.sy;
            std::vector<deco::TPiece> set;
            for (auto d : it->second) {
                auto tp = deco::learnPiece(pose, poseOf(d));
                tp.objectID = d->m_objectID;
                auto bb = d->boundingBox();
                tp.bw = bb.size.width;
                tp.bh = bb.size.height;
                tp.saveString = clean(std::string(d->getSaveString(m_editorLayer)));
                set.push_back(std::move(tp));
            }
            entry.second.push_back(std::move(set));
        }
        std::vector<std::string> anchorLines;
        for (auto& [id, entry] : fresh) {
            deco::addVariants(entry.first, entry.second);
            int examples = static_cast<int>(entry.second.size());
            anchorLines.push_back(fmt::format("{} (#{}): {} example(s), {} variant(s)",
                deco::anchorClassName(entry.first.cls), id, examples, entry.first.variants.size()));
            style.anchors[id] = std::move(entry.first);
        }

        if (learnedItems.empty() && fresh.empty()) {
            showInfo("Nothing learned", "None of the deco was close enough to the selected objects.");
            return;
        }

        {
            std::vector<std::string> strs;
            for (auto const& it : learnedItems) strs.push_back(it.saveString);
            for (auto const& [id, tpl] : style.anchors)
                for (auto const& v : tpl.variants)
                    for (auto const& p : v.pieces) strs.push_back(p.saveString);
            learnChannels(m_editorLayer, strs, style);
        }
        saveLibrary();
        log::info("[deco] learned into style {} ({}): {} block pieces from {} blocks, {} anchor templates",
            styleId, style.name, learnedItems.size(), blockExamples, fresh.size());

        std::string body = fmt::format("Saved into style <cy>{}</c>.\n", style.name);
        if (!learnedItems.empty())
            body += fmt::format("<cg>Blocks</c> ({} example(s)): {}\n", blockExamples, style.summary());
        for (auto const& l : anchorLines) body += "<cl>" + l + "</c>\n";
        if (!style.colors.empty()) body += fmt::format("<cp>{} colour channel(s)</c> saved with the style.\n", style.colors.size());
        if (air) body += fmt::format("<co>{} floating piece(s) skipped</c> (too far from any object).\n", air);
        if (triggers) body += fmt::format("<co>{} trigger(s) ignored.</c>\n", triggers);

        // Thumbnail of the example. Selection tint would show in the picture,
        // so deselect, render on the next frame, then restore the selection.
        float rx0 = 1e9f, ry0 = 1e9f, rx1 = -1e9f, ry1 = -1e9f;
        for (auto o : sel) {
            auto bb = o->boundingBox();
            rx0 = std::min(rx0, bb.getMinX());
            ry0 = std::min(ry0, bb.getMinY());
            rx1 = std::max(rx1, bb.getMaxX());
            ry1 = std::max(ry1, bb.getMaxY());
        }
        CCRect region(rx0 - 12.f, ry0 - 12.f, (rx1 - rx0) + 24.f, (ry1 - ry0) + 24.f);
        auto keep = CCArray::create();
        for (auto o : sel) keep->addObject(o);
        Ref<CCArray> keepRef = keep;
        Ref<DecoEditorUI> self = this;
        this->deselectAll();
        auto file = fmt::format("style-{}.png", styleId);
        Loader::get()->queueInMainThread([self, keepRef, region, file, styleId]() {
            if (!self->getParent()) return;
            bool ok = captureRegion(self->m_editorLayer, region, savePath(file));
            if (auto st = g_lib.find(styleId); st && ok) {
                st->preview = file;
                saveLibrary();
            }
            self->selectObjects(keepRef.data(), true);
        });

        showInfo("Style learned", body);
    }

    // ------------------------------------------------------------ Palette

    void onDecoPalette(CCObject*) {
        auto& style = activeStyle();
        int added = 0, collision = 0, other = 0;
        for (auto o : selection()) {
            if (!isDeco(o)) {
                if (isSolidLike(o)) collision++;
                else other++;
                continue;
            }
            auto bb = o->boundingBox();
            deco::Item it = deco::classifyLoose(bb.size.width, bb.size.height, o->getRotation());
            it.objectID = o->m_objectID;
            it.zOrder0 = o->m_zOrder;
            it.saveString = clean(std::string(o->getSaveString(m_editorLayer)));
            it.seq = g_lib.nextSeq++;
            it.kit = 0;
            style.items.push_back(std::move(it));
            added++;
        }
        std::string extra;
        if (collision) extra += fmt::format("\n<co>{} piece(s) with collision skipped</c> (blocks, outline-tab lines...).", collision);
        if (other) extra += fmt::format("\n<co>{} gameplay object(s) skipped</c> (spikes, orbs, pads...).", other);
        if (!added) {
            if (extra.empty()) notify("Select some deco pieces first", NotificationIcon::Error);
            else showInfo("Nothing added", "Only pieces without collision can go in the palette." + extra);
            return;
        }
        {
            std::vector<std::string> strs;
            for (auto const& it : style.items)
                if (it.kit == 0) strs.push_back(it.saveString);
            learnChannels(m_editorLayer, strs, style);
        }
        saveLibrary();
        showInfo("Added to palette",
            fmt::format("Added {} loose piece(s) to <cy>{}</c>.\nNow: {}", added, style.name, style.summary()) + extra);
    }

    // ------------------------------------------------------------ Deco!

    void onDecoApply(CCObject*) {
        auto* stylePtr = g_lib.active();
        if (!stylePtr || stylePtr->empty()) {
            showInfo("No style yet", "Decorate an example (a block, spike, orb, slope...), select it with its deco "
                                 "and press <cg>Learn</c>. You can also copy a decorated section of any level.");
            return;
        }
        auto const& style = *stylePtr;

        deco::Input in;
        std::vector<GameObject*> anchors;
        GameObject* reference = nullptr;
        for (auto o : selection()) {
            if (isTargetBlock(o)) {
                in.targets.push_back(toRect(o->getObjectRect()));
                if (!reference) reference = o;
            } else if (isAnchor(o)) {
                anchors.push_back(o);
                if (!reference) reference = o;
            }
        }
        if (!reference) {
            notify("Select what you want decorated (blocks, spikes, orbs, slopes...)", NotificationIcon::Error);
            return;
        }

        // Existing deco, for duplicate checks.
        std::unordered_map<int64_t, std::vector<std::pair<int, CCPoint>>> existing;
        auto bucket = [](double x, double y) {
            return (static_cast<int64_t>(std::floor(x / 4)) << 32) ^ static_cast<uint32_t>(static_cast<int32_t>(std::floor(y / 4)));
        };
        for (auto o : CCArrayExt<GameObject*>(m_editorLayer->m_objects)) {
            if (isSolidLike(o)) {
                in.solids.push_back(toRect(o->getObjectRect()));
            } else if (isHazard(o)) {
                in.hazards.push_back(toRect(o->boundingBox()));
            } else if (isDeco(o)) {
                auto p = o->getPosition();
                in.existing.push_back({o->m_objectID, p.x, p.y});
                existing[bucket(p.x, p.y)].push_back({o->m_objectID, p});
                auto bb = o->boundingBox();
                if (std::max(bb.size.width, bb.size.height) <= 45.f) in.existingSmall.push_back(toRect(bb));
            } else if (isGameplay(o)) {
                in.gameplay.push_back(toRect(o->boundingBox()));
            }
        }
        auto isDup = [&](int id, double x, double y) {
            for (int a = -1; a <= 1; ++a)
                for (int b = -1; b <= 1; ++b) {
                    auto it = existing.find(bucket(x + a * 4, y + b * 4));
                    if (it == existing.end()) continue;
                    for (auto const& [eid, p] : it->second)
                        if (eid == id && std::abs(p.x - x) < 2.5 && std::abs(p.y - y) < 2.5) return true;
                }
            return false;
        };

        auto mod = Mod::get();
        in.items = style.items;
        in.settings.fills = mod->getSettingValue<bool>("fills");
        in.settings.outlines = mod->getSettingValue<bool>("outlines");
        in.settings.corners = mod->getSettingValue<bool>("corners");
        in.settings.edgeDetails = mod->getSettingValue<bool>("edge-details");
        in.settings.bottomDetails = mod->getSettingValue<bool>("bottom-details");
        in.settings.interior = mod->getSettingValue<bool>("interior");
        in.settings.large = mod->getSettingValue<bool>("large-pieces");
        in.settings.density = mod->getSettingValue<double>("density");
        in.settings.maxObjects = static_cast<int>(mod->getSettingValue<int64_t>("max-objects"));
        bool keepGroups = mod->getSettingValue<bool>("keep-groups");
        int cap = in.settings.maxObjects;

        // Colour channels: bring the style's colours into this level. A channel
        // that is free (or already identical) is used as is; one that your level
        // already uses for something else is moved to a free channel instead.
        std::map<int, int> chanMap;
        int colorsSet = 0, colorsMoved = 0;
        bool colorsReady = false;
        auto prepareColors = [&]() {
            if (colorsReady) return;
            colorsReady = true;
            if (style.colors.empty()) return;
            auto mgrs = colorManagers(m_editorLayer);
            if (mgrs.empty()) return;
            auto em = mgrs[0];
            std::set<int> used;
            for (auto o : CCArrayExt<GameObject*>(m_editorLayer->m_objects)) {
                if (o->m_baseColor) used.insert(o->m_baseColor->m_colorID);
                if (o->m_detailColor) used.insert(o->m_detailColor->m_colorID);
                if (o->m_isColorTrigger) used.insert(o->m_targetColor);
            }
            std::set<int> allocated;
            auto isFree = [&](int d) {
                return !used.count(d) && !allocated.count(d) && !channelInUse(readChannel(em, d));
            };
            std::function<int(int, int)> resolve = [&](int c, int depth) -> int {
                if (auto it = chanMap.find(c); it != chanMap.end()) return it->second;
                auto want = style.colors.find(c);
                if (want == style.colors.end() || depth > 8) return c;
                deco::Channel w = want->second;
                if (w.copyID >= 1 && w.copyID <= 999) w.copyID = resolve(w.copyID, depth + 1);
                int target = -1;
                auto cur = readChannel(em, c);
                if (cur.sameAs(w)) {
                    target = c;
                } else if (isFree(c)) {
                    writeChannel(m_editorLayer, c, w);
                    colorsSet++;
                    target = c;
                } else {
                    for (int d = 1; d <= 999 && target < 0; ++d) {
                        if (d == c || !em->colorExists(d)) continue;
                        if (readChannel(em, d).sameAs(w) && channelInUse(w)) target = d;
                    }
                    for (int d = 1; d <= 999 && target < 0; ++d) {
                        if (isFree(d)) {
                            writeChannel(m_editorLayer, d, w);
                            colorsSet++;
                            colorsMoved++;
                            target = d;
                        }
                    }
                }
                if (target < 0) target = c; // no free channel left: keep it as is
                allocated.insert(target);
                chanMap[c] = target;
                return target;
            };
            for (auto const& [id, ch] : style.colors) resolve(id, 0);
            if (colorsSet) m_editorLayer->updateLevelColors();
            log::info("[deco] colours: {} set, {} moved to free channels, map size {}", colorsSet, colorsMoved, chanMap.size());
        };

        auto offset = saveOffset(reference, m_editorLayer);
        auto created = CCArray::create();
        auto spawn = [&](std::string const& raw, double scale) -> GameObject* {
            prepareColors();
            auto str = deco::remapChannels(raw, chanMap);
            auto arr = m_editorLayer->createObjectsFromString(gd::string(str), true, true);
            if (!arr || arr->count() == 0) return nullptr;
            auto obj = static_cast<GameObject*>(arr->objectAtIndex(0));
            if (std::abs(scale - 1.0) > 0.005) {
                obj->updateCustomScaleX(obj->m_scaleX * static_cast<float>(scale));
                obj->updateCustomScaleY(obj->m_scaleY * static_cast<float>(scale));
            }
            created->addObjectsFromArray(arr);
            return obj;
        };

        // 1) Blocks.
        deco::Stats st;
        bool capped = false;
        if (!in.targets.empty() && !in.items.empty()) {
            auto result = deco::generate(in);
            st = result.stats;
            capped = result.capped;
            for (auto const& p : result.placements) {
                auto const& it = in.items[p.item];
                Overrides o;
                o.x = p.x - offset.x;
                o.y = p.y - offset.y;
                o.zOrder = it.zOrder0 + p.zRank;
                o.rotDelta = p.rotDelta;
                auto obj = spawn(rewrite(it.saveString, o, keepGroups), 1.0);
                if (obj && (std::abs(p.scaleX - 1.0) > 0.005 || std::abs(p.scaleY - 1.0) > 0.005)) {
                    obj->updateCustomScaleX(obj->m_scaleX * static_cast<float>(p.scaleX));
                    obj->updateCustomScaleY(obj->m_scaleY * static_cast<float>(p.scaleY));
                }
                if (obj && std::abs(p.scaleLong - 1.0) > 0.005) {
                    if (it.longIsLocalX) obj->updateCustomScaleX(obj->m_scaleX * static_cast<float>(p.scaleLong));
                    else obj->updateCustomScaleY(obj->m_scaleY * static_cast<float>(p.scaleLong));
                }
            }
        }

        // 2) Everything else, from templates.
        std::map<std::string, int> aroundCounts;
        std::set<std::string> missing;
        int anchorPieces = 0, dupSkipped = st.skippedDuplicate;
        for (auto a : anchors) {
            deco::AnchorTemplate const* tpl = nullptr;
            double ratio = 1.0;
            if (auto it = style.anchors.find(a->m_objectID); it != style.anchors.end()) {
                tpl = &it->second;
            } else {
                // Fall back to a template learned on a similar object of the same kind.
                auto cls = anchorClass(a);
                auto pose = poseOf(a);
                auto r = anchorRect(a);
                double w = r.w() / pose.sx, h = r.h() / pose.sy;
                double bestScore = 1e18;
                for (auto const& [id, t] : style.anchors) {
                    if (t.cls != cls || t.cls == deco::AnchorClass::Other) continue;
                    double rr = std::sqrt(std::max(1.0, w * h) / std::max(1.0, t.bw * t.bh));
                    double score = std::abs(std::log(rr));
                    if (score < bestScore) { bestScore = score; tpl = &t; ratio = rr; }
                }
                if (tpl && (ratio < 0.5 || ratio > 2.0)) tpl = nullptr;
            }
            if (!tpl) {
                missing.insert(deco::anchorClassName(anchorClass(a)));
                continue;
            }
            auto pose = poseOf(a);
            auto variant = deco::chooseVariant(*tpl, pose.x, pose.y);
            if (!variant) continue;
            for (auto const& piece : variant->pieces) {
                if (static_cast<int>(created->count()) >= cap) { capped = true; break; }
                auto pl = deco::placePiece(pose, piece, ratio, ratio);
                if (isDup(piece.objectID, pl.x, pl.y)) { dupSkipped++; continue; }
                Overrides o;
                o.x = pl.x - offset.x;
                o.y = pl.y - offset.y;
                o.absolute = true;
                o.rot = pl.rot;
                o.flipX = pl.flipX;
                // Keep the piece's own Z order: it was learned that way.
                int z = 0;
                {
                    auto parts = split(piece.saveString, ',');
                    for (size_t i = 0; i + 1 < parts.size(); i += 2)
                        if (parts[i] == "25") { try { z = std::stoi(parts[i + 1]); } catch (...) {} }
                }
                o.zOrder = z;
                if (spawn(rewrite(piece.saveString, o, keepGroups), pl.scale)) {
                    anchorPieces++;
                    aroundCounts[deco::anchorClassName(tpl->cls)]++;
                }
            }
        }

        if (created->count() == 0) {
            std::string why = dupSkipped ? "Already decorated: nothing new to add." : "Nothing fits here with this style.";
            if (!missing.empty()) {
                std::string m;
                for (auto const& s : missing) m += (m.empty() ? "" : ", ") + s;
                why += fmt::format("\nStyle <cy>{}</c> has no deco learned for: {}.", style.name, m);
            }
            showInfo("Nothing placed", why);
            return;
        }

        // One undo step for the whole run.
        m_editorLayer->addToUndoList(UndoObject::createWithArray(created, UndoCommand::Paste), false);
        this->deselectAll();
        this->selectObjects(created, true);
        this->updateButtons();
        this->updateObjectInfoLabel();

        std::string parts;
        auto add = [&](int n, std::string const& what) {
            if (!n) return;
            if (!parts.empty()) parts += ", ";
            parts += fmt::format("{} {}", n, what);
        };
        add(st.fills, "fill");
        add(st.lines, "outline");
        add(st.corners, "corner");
        add(st.edgeDetails, "edge");
        add(st.large, "large");
        add(st.interior, "detail");
        for (auto const& [k, n] : aroundCounts) add(n, "around " + k);
        std::string msg = fmt::format("Placed {} ({}). Undo removes it all.", created->count(), parts);
        if (colorsSet) msg += fmt::format(" Set up {} colour channel(s){}.", colorsSet,
            colorsMoved ? fmt::format(", {} moved to free channels", colorsMoved) : std::string());
        if (!missing.empty()) {
            std::string m;
            for (auto const& s : missing) m += (m.empty() ? "" : ", ") + s;
            msg += fmt::format(" No deco learned for: {}.", m);
        }
        if (capped) msg += " Hit the max objects setting.";
        notify(msg, NotificationIcon::Success);
        log::info("[deco] style {}: {} placed ({} around objects), {} duplicates skipped",
            style.name, created->count(), anchorPieces, dupSkipped);
    }

    // ------------------------------------------------------------ Styles

    void onDecoStyles(CCObject*) {
        if (auto p = StylesPopup::create()) p->show();
    }
};
