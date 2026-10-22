#include "styles.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

namespace deco {

namespace {

std::vector<std::string> split(std::string const& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    return buf;
}

double toD(std::string const& s) { return std::stod(s); }
int toI(std::string const& s) { return std::stoi(s); }

long q(double v, double step) { return std::lround(v / step); }

std::string itemKey(Item const& it) {
    return std::to_string(it.objectID) + "|" + std::to_string(static_cast<int>(it.role)) + "|" +
           std::to_string(it.designed) + "|" + std::to_string(q(it.normalOffset, 0.5)) + "|" +
           std::to_string(q(it.offX, 0.5)) + "|" + std::to_string(q(it.offY, 0.5)) + "|" +
           std::to_string(q(it.rot0, 1.0)) + "|" + std::to_string(q(it.bw, 1.0)) + "|" +
           std::to_string(q(it.bh, 1.0)) + "|" + std::to_string(it.kit > 0 ? 1 : 0);
}

} // namespace

bool Channel::sameAs(Channel const& o) const {
    auto close = [](double a, double b) { return std::abs(a - b) < 1e-3; };
    if (copyID != o.copyID || blending != o.blending || playerColor != o.playerColor) return false;
    if (!close(opacity, o.opacity)) return false;
    if (copyID > 0) {
        return close(h, o.h) && close(s, o.s) && close(v, o.v) && absS == o.absS && absV == o.absV &&
               copyOpacity == o.copyOpacity;
    }
    return r == o.r && g == o.g && b == o.b;
}

std::vector<int> channelsUsed(std::string const& saveString) {
    std::vector<int> out;
    auto parts = split(saveString, ',');
    for (size_t i = 0; i + 1 < parts.size(); i += 2) {
        if (parts[i] != "21" && parts[i] != "22") continue;
        try {
            int c = std::stoi(parts[i + 1]);
            if (c >= 1 && c <= 999 && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
        } catch (...) {}
    }
    return out;
}

std::string remapChannels(std::string const& saveString, std::map<int, int> const& map) {
    if (map.empty()) return saveString;
    auto parts = split(saveString, ',');
    for (size_t i = 0; i + 1 < parts.size(); i += 2) {
        if (parts[i] != "21" && parts[i] != "22") continue;
        try {
            auto it = map.find(std::stoi(parts[i + 1]));
            if (it != map.end()) parts[i + 1] = std::to_string(it->second);
        } catch (...) {}
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += ',';
        out += parts[i];
    }
    return out;
}

std::string cleanName(std::string s) {
    for (auto& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    if (s.size() > 32) s.resize(32);
    return s;
}

std::string Style::summary() const {
    int counts[5] = {};
    for (auto const& it : items)
        if (it.kit > 0) counts[static_cast<int>(it.role)]++;
    std::string s;
    for (int r = 0; r < 5; ++r) {
        if (!counts[r]) continue;
        if (!s.empty()) s += ", ";
        s += std::to_string(counts[r]) + " " + roleName(static_cast<Role>(r));
    }
    int loose = 0;
    for (auto const& it : items)
        if (it.kit == 0) loose++;
    if (loose) {
        if (!s.empty()) s += ", ";
        s += std::to_string(loose) + " palette";
    }
    if (!colors.empty()) {
        if (!s.empty()) s += ", ";
        s += std::to_string(colors.size()) + " colour" + (colors.size() == 1 ? "" : "s");
    }
    std::set<std::string> kinds;
    for (auto const& [id, tpl] : anchors) kinds.insert(anchorClassName(tpl.cls));
    for (auto const& k : kinds) {
        if (!s.empty()) s += ", ";
        s += k;
    }
    return s.empty() ? "empty" : s;
}

Style* Library::active() { return find(activeId); }

Style* Library::find(int id) {
    for (auto& s : styles)
        if (s.id == id) return &s;
    return nullptr;
}

Style& Library::create(std::string name) {
    Style s;
    s.id = nextId++;
    s.name = cleanName(name.empty() ? "Style " + std::to_string(s.id) : name);
    styles.push_back(std::move(s));
    activeId = styles.back().id;
    return styles.back();
}

void Library::remove(int id) {
    std::erase_if(styles, [&](Style const& s) { return s.id == id; });
    if (activeId == id) activeId = styles.empty() ? 0 : styles.front().id;
}

// Format (one record per line, tab separated):
//   L  nextId nextSeq activeId
//   S  id name preview
//   I  <item fields>               (belongs to the last S)
//   A  objectID class bw bh        (anchor template, belongs to the last S)
//   V  count                       (variant, belongs to the last A)
//   P  objectID dx dy lrot lmirror bw bh saveString   (piece, belongs to the last V)
std::string Library::serialize() const {
    std::string out = "L\t" + std::to_string(nextId) + "\t" + std::to_string(nextSeq) + "\t" +
                      std::to_string(activeId) + "\n";
    for (auto const& s : styles) {
        out += "S\t" + std::to_string(s.id) + "\t" + cleanName(s.name) + "\t" + s.preview + "\n";
        for (auto const& it : s.items) {
            out += "I\t" + std::to_string(static_cast<int>(it.role)) + "\t" + std::to_string(it.designed) + "\t" +
                   num(it.bw) + "\t" + num(it.bh) + "\t" + num(it.rot0) + "\t" + (it.longIsLocalX ? "1" : "0") +
                   "\t" + (it.axisAligned ? "1" : "0") + "\t" + num(it.normalOffset) + "\t" +
                   (it.autoOffset ? "1" : "0") + "\t" + num(it.offX) + "\t" + num(it.offY) + "\t" +
                   std::to_string(it.kit) + "\t" + std::to_string(it.seq) + "\t" + std::to_string(it.objectID) +
                   "\t" + std::to_string(it.zOrder0) + "\t" + it.saveString + "\n";
        }
        for (auto const& [id, c] : s.colors) {
            out += "C\t" + std::to_string(c.id) + "\t" + std::to_string(c.r) + "\t" + std::to_string(c.g) + "\t" +
                   std::to_string(c.b) + "\t" + num(c.opacity) + "\t" + (c.blending ? "1" : "0") + "\t" +
                   std::to_string(c.copyID) + "\t" + num(c.h) + "\t" + num(c.s) + "\t" + num(c.v) + "\t" +
                   (c.absS ? "1" : "0") + "\t" + (c.absV ? "1" : "0") + "\t" + (c.copyOpacity ? "1" : "0") +
                   "\t" + std::to_string(c.playerColor) + "\n";
        }
        for (auto const& [id, tpl] : s.anchors) {
            out += "A\t" + std::to_string(tpl.objectID) + "\t" + std::to_string(static_cast<int>(tpl.cls)) + "\t" +
                   num(tpl.bw) + "\t" + num(tpl.bh) + "\n";
            for (auto const& v : tpl.variants) {
                out += "V\t" + std::to_string(v.count) + "\n";
                for (auto const& p : v.pieces) {
                    out += "P\t" + std::to_string(p.objectID) + "\t" + num(p.dx) + "\t" + num(p.dy) + "\t" +
                           num(p.lrot) + "\t" + (p.lmirror ? "1" : "0") + "\t" + num(p.bw) + "\t" + num(p.bh) +
                           "\t" + p.saveString + "\n";
                }
            }
        }
    }
    return out;
}

Library Library::deserialize(std::string const& data) {
    Library lib;
    Style* style = nullptr;
    AnchorTemplate* tpl = nullptr;
    TVariant* var = nullptr;
    for (auto const& line : split(data, '\n')) {
        if (line.empty()) continue;
        auto f = split(line, '\t');
        try {
            if (f[0] == "L" && f.size() >= 4) {
                lib.nextId = toI(f[1]);
                lib.nextSeq = toI(f[2]);
                lib.activeId = toI(f[3]);
            } else if (f[0] == "S" && f.size() >= 3) {
                Style s;
                s.id = toI(f[1]);
                s.name = cleanName(f[2]);
                s.preview = f.size() >= 4 ? f[3] : "";
                lib.styles.push_back(std::move(s));
                style = &lib.styles.back();
                tpl = nullptr;
                var = nullptr;
            } else if (f[0] == "I" && f.size() >= 17 && style) {
                Item it;
                int role = toI(f[1]);
                if (role < 0 || role > 4) continue;
                it.role = static_cast<Role>(role);
                it.designed = toI(f[2]) & 3;
                it.bw = toD(f[3]);
                it.bh = toD(f[4]);
                it.rot0 = toD(f[5]);
                it.longIsLocalX = f[6] == "1";
                it.axisAligned = f[7] == "1";
                it.normalOffset = toD(f[8]);
                it.autoOffset = f[9] == "1";
                it.offX = toD(f[10]);
                it.offY = toD(f[11]);
                it.kit = toI(f[12]);
                it.seq = toI(f[13]);
                it.objectID = toI(f[14]);
                it.zOrder0 = toI(f[15]);
                it.saveString = f[16];
                if (it.saveString.empty() || it.bw <= 0 || it.bh <= 0) continue;
                style->items.push_back(std::move(it));
            } else if (f[0] == "C" && f.size() >= 15 && style) {
                Channel c;
                c.id = toI(f[1]);
                if (c.id < 1 || c.id > 999) continue;
                c.r = std::clamp(toI(f[2]), 0, 255);
                c.g = std::clamp(toI(f[3]), 0, 255);
                c.b = std::clamp(toI(f[4]), 0, 255);
                c.opacity = std::clamp(toD(f[5]), 0.0, 1.0);
                c.blending = f[6] == "1";
                c.copyID = toI(f[7]);
                c.h = toD(f[8]);
                c.s = toD(f[9]);
                c.v = toD(f[10]);
                c.absS = f[11] == "1";
                c.absV = f[12] == "1";
                c.copyOpacity = f[13] == "1";
                c.playerColor = toI(f[14]);
                style->colors[c.id] = c;
            } else if (f[0] == "A" && f.size() >= 5 && style) {
                AnchorTemplate t;
                t.objectID = toI(f[1]);
                int cls = toI(f[2]);
                t.cls = static_cast<AnchorClass>(std::clamp(cls, 0, 7));
                t.bw = toD(f[3]);
                t.bh = toD(f[4]);
                style->anchors[t.objectID] = std::move(t);
                tpl = &style->anchors[toI(f[1])];
                var = nullptr;
            } else if (f[0] == "V" && f.size() >= 2 && tpl) {
                tpl->variants.push_back({std::max(1, toI(f[1])), {}});
                var = &tpl->variants.back();
            } else if (f[0] == "P" && f.size() >= 9 && var) {
                TPiece p;
                p.objectID = toI(f[1]);
                p.dx = toD(f[2]);
                p.dy = toD(f[3]);
                p.lrot = toD(f[4]);
                p.lmirror = f[5] == "1";
                p.bw = toD(f[6]);
                p.bh = toD(f[7]);
                p.saveString = f[8];
                if (p.saveString.empty()) continue;
                var->pieces.push_back(std::move(p));
            }
        } catch (...) {
            continue;
        }
    }
    // Drop variants that lost all their pieces, then templates with no variants.
    for (auto& s : lib.styles) {
        for (auto it = s.anchors.begin(); it != s.anchors.end();) {
            std::erase_if(it->second.variants, [](TVariant const& v) { return v.pieces.empty(); });
            if (it->second.variants.empty()) it = s.anchors.erase(it);
            else ++it;
        }
        lib.nextId = std::max(lib.nextId, s.id + 1);
    }
    if (!lib.find(lib.activeId)) lib.activeId = lib.styles.empty() ? 0 : lib.styles.front().id;
    return lib;
}

void mergeLearnedItems(std::vector<Item>& into, std::vector<Item> const& learned) {
    std::set<std::string> seen;
    for (auto const& it : into) seen.insert(itemKey(it));
    for (auto const& it : learned) {
        if (seen.insert(itemKey(it)).second) into.push_back(it);
    }
}

} // namespace deco
