// Deco AI — a local decoration generator for the GD editor.
// No API key, no internet. Two ways to teach it:
//   * Learn   — show it fully decorated example blocks; it copies that exact style.
//   * Palette — hand it a pile of loose deco pieces; it works out what each piece
//               is good for (outline, corner, detail, backdrop) from its shape and
//               places them where they fit on the blocks you select.

#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {

constexpr float kCell = 30.f;
constexpr float kHalf = 15.f;
constexpr float kEdgeThreshold = 10.f;
constexpr float kMaxAttachDist = 90.f;

// ============================================================ shared helpers

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

std::string cleanSaveString(std::string str) {
    while (!str.empty() && (str.back() == ';' || str.back() == '\n')) str.pop_back();
    return str;
}

float readKey(std::string const& saveString, std::string const& key, float fallback = 0.f) {
    auto parts = split(saveString, ',');
    for (size_t i = 0; i + 1 < parts.size(); i += 2) {
        if (parts[i] == key) {
            try { return std::stof(parts[i + 1]); } catch (...) { return fallback; }
        }
    }
    return fallback;
}

// Rewrite position (keys 2, 3) and rotation (key 6) of an object save string.
std::string placed(std::string const& saveString, float x, float y, std::optional<float> rot = std::nullopt) {
    auto parts = split(saveString, ',');
    bool hasX = false, hasY = false, hasR = false;
    for (size_t i = 0; i + 1 < parts.size(); i += 2) {
        if (parts[i] == "2") { parts[i + 1] = fmt::format("{:.2f}", x); hasX = true; }
        else if (parts[i] == "3") { parts[i + 1] = fmt::format("{:.2f}", y); hasY = true; }
        else if (parts[i] == "6" && rot) { parts[i + 1] = fmt::format("{:.2f}", *rot); hasR = true; }
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += ',';
        out += parts[i];
    }
    if (!hasX) out += fmt::format(",2,{:.2f}", x);
    if (!hasY) out += fmt::format(",3,{:.2f}", y);
    if (rot && !hasR) out += fmt::format(",6,{:.2f}", *rot);
    return out;
}

float wrapDeg(float d) {
    d = std::fmod(d, 360.f);
    if (d < 0) d += 360.f;
    return d;
}

int64_t cellKey(int gx, int gy) {
    return (static_cast<int64_t>(gx) << 32) | static_cast<uint32_t>(gy);
}

int toCell(float v) { return static_cast<int>(std::floor(v / kCell)); }

bool isSolid(GameObject* obj) {
    return obj && obj->m_objectType == GameObjectType::Solid;
}

void notify(std::string const& text, NotificationIcon icon) {
    Notification::create(text, icon)->show();
}

std::mt19937 g_rng{std::random_device{}()};

double roll() { return std::uniform_real_distribution<double>(0.0, 1.0)(g_rng); }

template <class T>
T const& pickOne(std::vector<T> const& v) {
    return v[std::uniform_int_distribution<size_t>(0, v.size() - 1)(g_rng)];
}

// ============================================================ learned style

enum class Edge : int {
    Face, Top, Bottom, Left, Right, TopLeft, TopRight, BottomLeft, BottomRight, Count
};
constexpr size_t kEdgeCount = static_cast<size_t>(Edge::Count);

struct DecoPiece {
    Edge edge;
    float dx;
    float dy;
    std::string saveString;
};
using Sample = std::vector<DecoPiece>;

std::vector<Sample> g_samples;

Edge classifyEdge(float dx, float dy) {
    bool r = dx > kEdgeThreshold, l = dx < -kEdgeThreshold;
    bool u = dy > kEdgeThreshold, d = dy < -kEdgeThreshold;
    if (u && l) return Edge::TopLeft;
    if (u && r) return Edge::TopRight;
    if (d && l) return Edge::BottomLeft;
    if (d && r) return Edge::BottomRight;
    if (u) return Edge::Top;
    if (d) return Edge::Bottom;
    if (l) return Edge::Left;
    if (r) return Edge::Right;
    return Edge::Face;
}

// ============================================================ palette

enum class Role : int { Line, Corner, Detail, Backdrop };

char const* roleName(Role r) {
    switch (r) {
        case Role::Line: return "outline";
        case Role::Corner: return "corner";
        case Role::Detail: return "detail";
        case Role::Backdrop: return "backdrop";
    }
    return "?";
}

struct PaletteItem {
    Role role;
    float w;        // on-screen bounding box
    float h;
    float length;   // long side (lines)
    float horizRot; // rotation that makes a line lie flat
    std::string saveString;
};

std::vector<PaletteItem> g_palette;

PaletteItem analyzePiece(GameObject* obj, std::string str) {
    auto bb = obj->boundingBox().size;
    float w = std::max(bb.width, 1.f);
    float h = std::max(bb.height, 1.f);
    float maxD = std::max(w, h), minD = std::min(w, h);
    float aspect = maxD / std::max(minD, 1.f);
    float baseRot = readKey(str, "6");

    Role role;
    if (aspect >= 2.2f) role = Role::Line;          // long and thin: outlines, edge strips
    else if (maxD <= 14.f) role = Role::Detail;     // small: face details, sprinkles
    else if (maxD <= 48.f) role = Role::Corner;     // medium, chunky: corners
    else role = Role::Backdrop;                     // big: set pieces on top of platforms

    float horizRot = baseRot + (h > w ? 90.f : 0.f);
    return {role, w, h, maxD, horizRot, std::move(str)};
}

// ============================================================ persistence

void save() {
    std::string s;
    for (auto const& sample : g_samples) {
        s += "#\n";
        for (auto const& p : sample)
            s += fmt::format("{}\t{:.2f}\t{:.2f}\t{}\n", static_cast<int>(p.edge), p.dx, p.dy, p.saveString);
    }
    Mod::get()->setSavedValue<std::string>("samples", s);

    std::string pal;
    for (auto const& p : g_palette)
        pal += fmt::format("{}\t{:.2f}\t{:.2f}\t{:.2f}\t{:.2f}\t{}\n",
            static_cast<int>(p.role), p.w, p.h, p.length, p.horizRot, p.saveString);
    Mod::get()->setSavedValue<std::string>("palette", pal);
}

void loadOnce() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;

    for (auto const& line : split(Mod::get()->getSavedValue<std::string>("samples", ""), '\n')) {
        if (line.empty()) continue;
        if (line == "#") { g_samples.emplace_back(); continue; }
        if (g_samples.empty()) g_samples.emplace_back();
        auto f = split(line, '\t');
        if (f.size() < 4) continue;
        try {
            int e = std::stoi(f[0]);
            if (e < 0 || e >= static_cast<int>(kEdgeCount)) continue;
            g_samples.back().push_back({static_cast<Edge>(e), std::stof(f[1]), std::stof(f[2]), f[3]});
        } catch (...) {}
    }
    std::erase_if(g_samples, [](Sample const& s) { return s.empty(); });

    for (auto const& line : split(Mod::get()->getSavedValue<std::string>("palette", ""), '\n')) {
        auto f = split(line, '\t');
        if (f.size() < 6) continue;
        try {
            int r = std::stoi(f[0]);
            if (r < 0 || r > 3) continue;
            g_palette.push_back({static_cast<Role>(r), std::stof(f[1]), std::stof(f[2]),
                std::stof(f[3]), std::stof(f[4]), f[5]});
        } catch (...) {}
    }
}

// ============================================================ generation

struct Block {
    GameObject* obj;
    CCPoint pos;
    int gx, gy;
    bool top, bottom, left, right;
};

struct Emitter {
    std::string batch;
    int64_t placed = 0;
    int64_t cap = 0;
    bool capped = false;

    bool emit(std::string const& str, float x, float y, std::optional<float> rot = std::nullopt) {
        if (placed >= cap) { capped = true; return false; }
        batch += ::placed(str, x, y, rot);
        batch += ';';
        ++placed;
        return true;
    }
};

// Edge types the learned style already covers, so the palette doesn't double up.
std::array<bool, kEdgeCount> applyLearned(std::vector<Block> const& blocks, Emitter& out, double density) {
    using Variant = std::vector<DecoPiece const*>;
    std::array<std::vector<Variant>, kEdgeCount> pools;
    for (auto const& sample : g_samples) {
        std::array<Variant, kEdgeCount> bySide;
        for (auto const& p : sample) bySide[static_cast<size_t>(p.edge)].push_back(&p);
        for (size_t e = 0; e < kEdgeCount; ++e)
            if (!bySide[e].empty()) pools[e].push_back(std::move(bySide[e]));
    }

    std::array<bool, kEdgeCount> covered{};
    for (size_t e = 0; e < kEdgeCount; ++e) covered[e] = !pools[e].empty();
    if (g_samples.empty()) return covered;

    for (auto const& b : blocks) {
        std::array<bool, kEdgeCount> exposed{};
        exposed[size_t(Edge::Face)] = true;
        exposed[size_t(Edge::Top)] = b.top;
        exposed[size_t(Edge::Bottom)] = b.bottom;
        exposed[size_t(Edge::Left)] = b.left;
        exposed[size_t(Edge::Right)] = b.right;
        exposed[size_t(Edge::TopLeft)] = b.top && b.left;
        exposed[size_t(Edge::TopRight)] = b.top && b.right;
        exposed[size_t(Edge::BottomLeft)] = b.bottom && b.left;
        exposed[size_t(Edge::BottomRight)] = b.bottom && b.right;

        for (size_t e = 0; e < kEdgeCount; ++e) {
            if (!exposed[e] || pools[e].empty() || roll() > density) continue;
            for (auto piece : pickOne(pools[e]))
                if (!out.emit(piece->saveString, b.pos.x + piece->dx, b.pos.y + piece->dy)) return covered;
        }
    }
    return covered;
}

// Choose the outline piece whose length tiles this run most cleanly (with a little randomness).
PaletteItem const* bestLineFor(float runLen, std::vector<PaletteItem const*> const& lines) {
    PaletteItem const* best = nullptr;
    float bestScore = 1e9f;
    for (auto item : lines) {
        float len = std::max(item->length, 8.f);
        float n = std::max(1.f, std::round(runLen / len));
        float spacing = runLen / n;
        float score = std::abs(spacing - len) / len + static_cast<float>(roll()) * 0.15f;
        if (score < bestScore) { bestScore = score; best = item; }
    }
    return best;
}

struct Run {
    float start, end; // along the run axis
    float edge;       // fixed coordinate of the edge line
    int blocks;
};

std::vector<Run> findRuns(std::vector<Block> const& blocks, bool horizontal,
                          std::function<bool(Block const&)> exposed, float edgeOffset) {
    // horizontal runs: group by row (gy), walk along gx. vertical: group by column, walk gy.
    std::map<int, std::vector<Block const*>> lanes;
    for (auto const& b : blocks)
        if (exposed(b)) lanes[horizontal ? b.gy : b.gx].push_back(&b);

    std::vector<Run> runs;
    for (auto& [lane, list] : lanes) {
        std::sort(list.begin(), list.end(), [&](auto a, auto b) {
            return horizontal ? a->gx < b->gx : a->gy < b->gy;
        });
        size_t i = 0;
        while (i < list.size()) {
            size_t j = i;
            while (j + 1 < list.size() &&
                   (horizontal ? list[j + 1]->gx == list[j]->gx + 1 : list[j + 1]->gy == list[j]->gy + 1))
                ++j;
            auto first = list[i], last = list[j];
            Run r;
            r.start = (horizontal ? first->pos.x : first->pos.y) - kHalf;
            r.end = (horizontal ? last->pos.x : last->pos.y) + kHalf;
            r.edge = (horizontal ? first->pos.y : first->pos.x) + edgeOffset;
            r.blocks = static_cast<int>(j - i + 1);
            runs.push_back(r);
            i = j + 1;
        }
    }
    return runs;
}

void applyPalette(std::vector<Block> const& blocks, Emitter& out, double density,
                  std::array<bool, kEdgeCount> const& learnedCovers) {
    std::vector<PaletteItem const*> lines, corners, details, backdrops;
    for (auto const& p : g_palette) {
        switch (p.role) {
            case Role::Line: lines.push_back(&p); break;
            case Role::Corner: corners.push_back(&p); break;
            case Role::Detail: details.push_back(&p); break;
            case Role::Backdrop: backdrops.push_back(&p); break;
        }
    }

    // --- outlines: tile along each continuous exposed edge, one piece type per run
    struct Side { Edge edge; bool horizontal; float offset; float rotAdd; std::function<bool(Block const&)> exposed; };
    std::array<Side, 4> sides{{
        {Edge::Top, true, kHalf, 0.f, [](Block const& b) { return b.top; }},
        {Edge::Bottom, true, -kHalf, 180.f, [](Block const& b) { return b.bottom; }},
        {Edge::Left, false, -kHalf, 270.f, [](Block const& b) { return b.left; }},
        {Edge::Right, false, kHalf, 90.f, [](Block const& b) { return b.right; }},
    }};

    std::vector<Run> topRuns;
    for (auto const& side : sides) {
        auto runs = findRuns(blocks, side.horizontal, side.exposed, side.offset);
        if (side.edge == Edge::Top) topRuns = runs;
        if (lines.empty() || learnedCovers[size_t(side.edge)]) continue;

        for (auto const& run : runs) {
            float runLen = run.end - run.start;
            auto item = bestLineFor(runLen, lines);
            if (!item) continue;
            int n = std::max(1, static_cast<int>(std::round(runLen / std::max(item->length, 8.f))));
            float spacing = runLen / n;
            float rot = wrapDeg(item->horizRot + side.rotAdd);
            for (int i = 0; i < n; ++i) {
                float along = run.start + spacing * (i + 0.5f);
                float x = side.horizontal ? along : run.edge;
                float y = side.horizontal ? run.edge : along;
                if (!out.emit(item->saveString, x, y, rot)) return;
            }
        }
    }

    // --- corners: outer corners only; pieces are assumed drawn for the top-left corner
    if (!corners.empty()) {
        struct C { Edge edge; float sx, sy, rot; bool (*ok)(Block const&); };
        std::array<C, 4> cs{{
            {Edge::TopLeft, -1, 1, 0.f, [](Block const& b) { return b.top && b.left; }},
            {Edge::TopRight, 1, 1, 90.f, [](Block const& b) { return b.top && b.right; }},
            {Edge::BottomRight, 1, -1, 180.f, [](Block const& b) { return b.bottom && b.right; }},
            {Edge::BottomLeft, -1, -1, 270.f, [](Block const& b) { return b.bottom && b.left; }},
        }};
        for (auto const& b : blocks) {
            for (auto const& c : cs) {
                if (!c.ok(b) || learnedCovers[size_t(c.edge)]) continue;
                auto item = pickOne(corners);
                float rot = wrapDeg(readKey(item->saveString, "6") + c.rot);
                if (!out.emit(item->saveString, b.pos.x + c.sx * kHalf, b.pos.y + c.sy * kHalf, rot)) return;
            }
        }
    }

    // --- details: sparse sprinkles on block faces, favouring blocks near the surface
    if (!details.empty() && !learnedCovers[size_t(Edge::Face)]) {
        for (auto const& b : blocks) {
            bool nearSurface = b.top || b.bottom || b.left || b.right;
            double p = density * (nearSurface ? 0.45 : 0.2);
            if (roll() > p) continue;
            auto item = pickOne(details);
            float margin = std::max(2.f, kHalf - std::max(item->w, item->h) / 2.f - 2.f);
            std::uniform_real_distribution<float> off(-margin, margin);
            bool square = std::abs(item->w - item->h) < 3.f;
            int turns = std::uniform_int_distribution<int>(0, 3)(g_rng);
            float rot = wrapDeg(readKey(item->saveString, "6") + (square ? turns * 90.f : (turns % 2) * 180.f));
            if (!out.emit(item->saveString, b.pos.x + off(g_rng), b.pos.y + off(g_rng), rot)) return;
        }
    }

    // --- backdrops: big set pieces standing on longer platforms, well spaced
    if (!backdrops.empty()) {
        for (auto const& run : topRuns) {
            if (run.blocks < 3) continue;
            auto item = pickOne(backdrops);
            float gap = std::max(item->w * 1.6f, 120.f);
            float runLen = run.end - run.start;
            if (item->w > runLen) continue;
            float x = run.start + item->w / 2.f + static_cast<float>(roll()) * std::min(gap * 0.5f, runLen - item->w);
            while (x + item->w / 2.f <= run.end) {
                if (roll() < density * 0.6) {
                    if (!out.emit(item->saveString, x, run.edge + item->h / 2.f)) return;
                    item = pickOne(backdrops);
                }
                x += gap * (0.8f + static_cast<float>(roll()) * 0.6f);
            }
        }
    }
}

} // namespace

// ============================================================ editor hook

class $modify(DecoEditorUI, EditorUI) {
    struct Fields {
        std::vector<Ref<GameObject>> lastGenerated;
    };

    bool init(LevelEditorLayer* editorLayer) {
        if (!EditorUI::init(editorLayer)) return false;
        loadOnce();

        auto win = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setID("deco-ai-menu"_spr);
        menu->setPosition({win.width - 32.f, win.height / 2.f + 55.f});

        float y = 0.f;
        auto addButton = [&](char const* label, SEL_MenuHandler handler) {
            auto spr = ButtonSprite::create(label);
            spr->setScale(0.42f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, handler);
            btn->setPosition({0.f, y});
            menu->addChild(btn);
            y -= 20.f;
        };

        addButton("Learn", menu_selector(DecoEditorUI::onDecoLearn));
        addButton("Palette", menu_selector(DecoEditorUI::onDecoPalette));
        addButton("Deco!", menu_selector(DecoEditorUI::onDecoApply));
        addButton("Undo", menu_selector(DecoEditorUI::onDecoUndo));
        addButton("Forget", menu_selector(DecoEditorUI::onDecoForget));

        this->addChild(menu, 100);
        return true;
    }

    // Select decorated example blocks + their deco: copies your exact arrangement.
    void onDecoLearn(CCObject*) {
        auto sel = this->getSelectedObjects();
        std::vector<GameObject*> solids, decos;
        if (sel) {
            for (auto obj : CCArrayExt<GameObject*>(sel))
                (isSolid(obj) ? solids : decos).push_back(obj);
        }
        if (solids.empty() || decos.empty()) {
            notify("Select decorated blocks AND their deco (or use Palette for loose pieces)", NotificationIcon::Error);
            return;
        }

        std::vector<Sample> fresh(solids.size());
        size_t pieces = 0;
        for (auto deco : decos) {
            auto dp = deco->getPosition();
            size_t best = 0;
            float bestDist = 1e9f;
            for (size_t i = 0; i < solids.size(); ++i) {
                float d = dp.getDistance(solids[i]->getPosition());
                if (d < bestDist) { bestDist = d; best = i; }
            }
            if (bestDist > kMaxAttachDist) continue;
            auto a = solids[best]->getPosition();
            fresh[best].push_back({classifyEdge(dp.x - a.x, dp.y - a.y), dp.x - a.x, dp.y - a.y,
                cleanSaveString(deco->getSaveString(m_editorLayer))});
            ++pieces;
        }

        size_t added = 0;
        for (auto& s : fresh)
            if (!s.empty()) { g_samples.push_back(std::move(s)); ++added; }
        save();
        notify(fmt::format("Learned {} sample(s), {} pieces", added, pieces), NotificationIcon::Success);
    }

    // Select any loose deco pieces: the mod decides what each one is for.
    void onDecoPalette(CCObject*) {
        auto sel = this->getSelectedObjects();
        std::array<int, 4> counts{};
        if (sel) {
            for (auto obj : CCArrayExt<GameObject*>(sel)) {
                if (isSolid(obj)) continue;
                auto item = analyzePiece(obj, cleanSaveString(obj->getSaveString(m_editorLayer)));
                counts[static_cast<int>(item.role)]++;
                g_palette.push_back(std::move(item));
            }
        }
        int total = counts[0] + counts[1] + counts[2] + counts[3];
        if (total == 0) {
            notify("Select some deco pieces to add to the palette", NotificationIcon::Error);
            return;
        }
        save();
        notify(fmt::format("Palette +{}: {} {}, {} {}, {} {}, {} {}", total,
            counts[0], roleName(Role::Line), counts[1], roleName(Role::Corner),
            counts[2], roleName(Role::Detail), counts[3], roleName(Role::Backdrop)),
            NotificationIcon::Success);
    }

    void onDecoApply(CCObject*) {
        if (g_samples.empty() && g_palette.empty()) {
            notify("Teach me first: Learn (decorated examples) or Palette (loose pieces)", NotificationIcon::Error);
            return;
        }

        std::unordered_set<int64_t> occupied;
        for (auto obj : CCArrayExt<GameObject*>(m_editorLayer->m_objects)) {
            if (!isSolid(obj)) continue;
            auto p = obj->getPosition();
            occupied.insert(cellKey(toCell(p.x), toCell(p.y)));
        }
        auto open = [&](int x, int y) { return !occupied.contains(cellKey(x, y)); };

        std::vector<Block> blocks;
        std::unordered_set<int64_t> seen;
        if (auto sel = this->getSelectedObjects()) {
            for (auto obj : CCArrayExt<GameObject*>(sel)) {
                if (!isSolid(obj)) continue;
                auto p = obj->getPosition();
                int gx = toCell(p.x), gy = toCell(p.y);
                if (!seen.insert(cellKey(gx, gy)).second) continue;
                blocks.push_back({obj, p, gx, gy, open(gx, gy + 1), open(gx, gy - 1), open(gx - 1, gy), open(gx + 1, gy)});
            }
        }
        if (blocks.empty()) {
            notify("Select the blocks you want decorated", NotificationIcon::Error);
            return;
        }

        double density = Mod::get()->getSettingValue<double>("density");
        Emitter out;
        out.cap = Mod::get()->getSettingValue<int64_t>("max-objects");

        auto covered = applyLearned(blocks, out, density);
        if (!out.capped && !g_palette.empty()) applyPalette(blocks, out, density, covered);

        if (out.batch.empty()) {
            notify("Nothing fit here (no exposed edges matched)", NotificationIcon::Info);
            return;
        }

        auto created = m_editorLayer->createObjectsFromString(gd::string(out.batch), true, true);
        auto& last = m_fields->lastGenerated;
        last.clear();
        if (created) {
            for (auto obj : CCArrayExt<GameObject*>(created)) last.emplace_back(obj);
            this->deselectAll();
            this->selectObjects(created, true);
        }
        notify(fmt::format("Placed {} deco objects{}", out.placed,
            out.capped ? " (hit max-objects limit)" : ""), NotificationIcon::Success);
    }

    void onDecoUndo(CCObject*) {
        auto& last = m_fields->lastGenerated;
        if (last.empty()) {
            notify("Nothing to undo", NotificationIcon::Info);
            return;
        }
        this->deselectAll();
        size_t removed = 0;
        for (auto& obj : last) {
            if (obj && obj->getParent()) {
                m_editorLayer->removeObject(obj, true);
                ++removed;
            }
        }
        last.clear();
        notify(fmt::format("Removed {} generated objects", removed), NotificationIcon::Success);
    }

    void onDecoForget(CCObject*) {
        g_samples.clear();
        g_palette.clear();
        save();
        notify("Learned style and palette cleared", NotificationIcon::Success);
    }
};
