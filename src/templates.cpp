#include "templates.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace deco {

namespace {

constexpr double kPi = 3.14159265358979323846;

double wrap(double d) {
    d = std::fmod(d, 360.0);
    if (d < 0) d += 360.0;
    if (d >= 359.9995) d = 0;
    if (std::abs(d) < 5e-4) d = 0;
    return d;
}

// Rotate (x, y) clockwise by deg, y up.
void rotCW(double& x, double& y, double deg) {
    double r = deg * kPi / 180.0;
    double c = std::cos(r), s = std::sin(r);
    double nx = x * c + y * s;
    double ny = -x * s + y * c;
    x = std::abs(nx) < 1e-9 ? 0 : nx;
    y = std::abs(ny) < 1e-9 ? 0 : ny;
}

long q(double v, double step) { return std::lround(v / step); }

} // namespace

Orient canonical(double rot, bool flipX, bool flipY) {
    return {wrap(rot + (flipY ? 180.0 : 0.0)), flipX != flipY};
}

char const* anchorClassName(AnchorClass c) {
    switch (c) {
        case AnchorClass::Block: return "block";
        case AnchorClass::Slope: return "slope";
        case AnchorClass::Hazard: return "spike";
        case AnchorClass::Orb: return "orb";
        case AnchorClass::Pad: return "pad";
        case AnchorClass::Portal: return "portal";
        case AnchorClass::Collectible: return "collectible";
        case AnchorClass::Other: return "object";
    }
    return "object";
}

int AnchorTemplate::totalCount() const {
    int n = 0;
    for (auto const& v : variants) n += v.count;
    return n;
}

// world = Rot(A.rot) * MirrorX^A.m * Scale(sx, sy) * local
// local = Scale^-1 * MirrorX^A.m * Rot(-A.rot) * world
TPiece learnPiece(Pose const& anchor, Pose const& piece) {
    Orient A = canonical(anchor.rot, anchor.flipX, anchor.flipY);
    Orient P = canonical(piece.rot, piece.flipX, piece.flipY);

    double wx = piece.x - anchor.x, wy = piece.y - anchor.y;
    rotCW(wx, wy, -A.rot);
    if (A.mirror) wx = -wx;
    TPiece out;
    out.dx = wx / std::max(anchor.sx, 1e-6);
    out.dy = wy / std::max(anchor.sy, 1e-6);

    // T_rel = (R(A) Mx^a)^-1 * R(P) Mx^p = Mx^a R(P - A) Mx^p
    //       = R(a ? -(P - A) : (P - A)) Mx^(a xor p)
    double rel = P.rot - A.rot;
    out.lrot = wrap(A.mirror ? -rel : rel);
    out.lmirror = A.mirror != P.mirror;
    return out;
}

Placed placePiece(Pose const& target, TPiece const& p, double ratioX, double ratioY) {
    Orient A = canonical(target.rot, target.flipX, target.flipY);
    double lx = p.dx * target.sx * ratioX;
    double ly = p.dy * target.sy * ratioY;
    if (A.mirror) lx = -lx;
    rotCW(lx, ly, A.rot);

    Placed out;
    out.x = target.x + lx;
    out.y = target.y + ly;
    // T = R(A) Mx^a R(lrot) Mx^lm = R(A + (a ? -lrot : lrot)) Mx^(a xor lm)
    out.rot = wrap(A.rot + (A.mirror ? -p.lrot : p.lrot));
    out.flipX = A.mirror != p.lmirror;
    double s = std::sqrt(std::max(target.sx * ratioX, 1e-6) * std::max(target.sy * ratioY, 1e-6));
    out.scale = std::abs(s - 1.0) < 1e-6 ? 1.0 : s;
    return out;
}

std::string variantSignature(std::vector<TPiece> const& pieces) {
    std::vector<std::string> parts;
    parts.reserve(pieces.size());
    for (auto const& p : pieces) {
        parts.push_back(std::to_string(p.objectID) + ":" + std::to_string(q(p.dx, 0.5)) + ":" +
                        std::to_string(q(p.dy, 0.5)) + ":" + std::to_string(q(p.lrot, 1.0) % 360) + ":" +
                        (p.lmirror ? "m" : "n"));
    }
    std::sort(parts.begin(), parts.end());
    std::string sig;
    for (auto const& s : parts) {
        sig += s;
        sig += ';';
    }
    return sig;
}

void addVariants(AnchorTemplate& tpl, std::vector<std::vector<TPiece>> const& sets, size_t maxVariants) {
    std::map<std::string, size_t> index;
    for (size_t i = 0; i < tpl.variants.size(); ++i) index[variantSignature(tpl.variants[i].pieces)] = i;
    for (auto const& set : sets) {
        if (set.empty()) continue;
        auto sig = variantSignature(set);
        auto it = index.find(sig);
        if (it != index.end()) {
            tpl.variants[it->second].count++;
        } else {
            index[sig] = tpl.variants.size();
            tpl.variants.push_back({1, set});
        }
    }
    std::stable_sort(tpl.variants.begin(), tpl.variants.end(),
                     [](TVariant const& a, TVariant const& b) { return a.count > b.count; });
    if (tpl.variants.size() > maxVariants) tpl.variants.resize(maxVariants);
}

TVariant const* chooseVariant(AnchorTemplate const& tpl, double x, double y) {
    if (tpl.variants.empty()) return nullptr;
    int total = tpl.totalCount();
    if (total <= 0) return &tpl.variants.front();
    uint64_t z = static_cast<uint64_t>(std::llround(x * 2)) * 0x9E3779B97F4A7C15ull ^
                 (static_cast<uint64_t>(std::llround(y * 2)) + 0x632BE59BD9B4E019ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    int pick = static_cast<int>(z % static_cast<uint64_t>(total));
    for (auto const& v : tpl.variants) {
        if (pick < v.count) return &v;
        pick -= v.count;
    }
    return &tpl.variants.front();
}

} // namespace deco
