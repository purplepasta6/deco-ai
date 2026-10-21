#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace deco {

namespace {

// The level is rasterised onto a quarter-block grid so half blocks, slabs and
// scaled blocks all get exact edges.
constexpr double Q = 7.5;

using Key = int64_t;
Key key(int i, int j) { return (static_cast<int64_t>(i) << 32) ^ static_cast<uint32_t>(j); }

int cellLo(double v) { return static_cast<int>(std::floor(v / Q + 1e-6)); }
int cellHi(double v) { return static_cast<int>(std::ceil(v / Q - 1e-6)) - 1; }

// Cells whose centre lies inside the rect.
template <class F>
void forCells(Rect const& r, F&& f) {
    int i0 = static_cast<int>(std::ceil(r.x0 / Q - 0.5 - 1e-6));
    int i1 = static_cast<int>(std::floor(r.x1 / Q - 0.5 + 1e-6));
    int j0 = static_cast<int>(std::ceil(r.y0 / Q - 0.5 - 1e-6));
    int j1 = static_cast<int>(std::floor(r.y1 / Q - 0.5 + 1e-6));
    for (int i = i0; i <= i1; ++i)
        for (int j = j0; j <= j1; ++j) f(i, j);
}

// Cells a rect touches at all (used to check a piece stays inside the blocks).
template <class F>
bool allTouchedCells(Rect const& r, F&& pred) {
    int i0 = cellLo(r.x0), i1 = cellHi(r.x1);
    int j0 = cellLo(r.y0), j1 = cellHi(r.y1);
    for (int i = i0; i <= i1; ++i)
        for (int j = j0; j <= j1; ++j)
            if (!pred(i, j)) return false;
    return true;
}

constexpr int dx[4] = {0, 1, 0, -1}; // Top, Right, Bottom, Left
constexpr int dy[4] = {1, 0, -1, 0};

double wrapDeg(double d) {
    d = std::fmod(d, 360.0);
    if (d < 0) d += 360.0;
    if (d > 359.999) d = 0;
    return d;
}

bool oddQuarter(double deg) {
    long q = std::lround(deg / 90.0);
    return (q % 2 + 2) % 2 == 1;
}

// Rotate a vector clockwise by deg (GD convention, y up).
void rotateCW(double& x, double& y, double deg) {
    double r = deg * 3.14159265358979323846 / 180.0;
    double c = std::cos(r), s = std::sin(r);
    double nx = x * c + y * s;
    double ny = -x * s + y * c;
    x = std::abs(nx) < 1e-9 ? 0 : nx;
    y = std::abs(ny) < 1e-9 ? 0 : ny;
}

void outward(int side, double& ox, double& oy) {
    ox = dx[side];
    oy = dy[side];
}

struct Run {
    int side;
    double edge;       // fixed coordinate of the edge line
    double start, end; // along the run axis
    bool startOuter, endOuter;
};

// Spatial hash of placed non-line pieces, for overlap checks.
class RectIndex {
public:
    void add(Rect const& r) {
        size_t idx = m_rects.size();
        m_rects.push_back(r);
        forBuckets(r, [&](Key k) { m_buckets[k].push_back(idx); });
    }
    bool hits(Rect const& r, double margin) const {
        bool hit = false;
        Rect grown{r.x0 - margin, r.y0 - margin, r.x1 + margin, r.y1 + margin};
        forBuckets(grown, [&](Key k) {
            if (hit) return;
            auto it = m_buckets.find(k);
            if (it == m_buckets.end()) return;
            for (auto idx : it->second)
                if (m_rects[idx].overlaps(r, margin)) { hit = true; return; }
        });
        return hit;
    }

private:
    static constexpr double B = 60.0;
    template <class F>
    static void forBuckets(Rect const& r, F&& f) {
        int i0 = static_cast<int>(std::floor(r.x0 / B)), i1 = static_cast<int>(std::floor(r.x1 / B));
        int j0 = static_cast<int>(std::floor(r.y0 / B)), j1 = static_cast<int>(std::floor(r.y1 / B));
        for (int i = i0; i <= i1; ++i)
            for (int j = j0; j <= j1; ++j) f(key(i, j));
    }
    std::vector<Rect> m_rects;
    std::unordered_map<Key, std::vector<size_t>> m_buckets;
};

class Engine {
public:
    explicit Engine(Input const& in) : m_in(in) {}

    Result run() {
        rasterise();
        if (m_targets.empty()) return std::move(m_out);
        computeDepth();
        buildRuns();
        indexObstacles();
        chooseKit();

        auto const& s = m_in.settings;
        if (s.outlines) placeLines();
        if (s.corners && !m_out.capped) placeCorners();
        if (s.edgeDetails && !m_out.capped) placeEdgeDetails();
        if (s.large && !m_out.capped) placeLarge();
        if (s.interior && !m_out.capped) placeInterior();
        return std::move(m_out);
    }

private:
    Input const& m_in;
    Result m_out;

    std::unordered_set<Key> m_solid, m_targets;
    std::unordered_map<Key, int> m_depth; // steps from the nearest exposed cell
    std::vector<Run> m_runs;
    std::vector<std::pair<int, int>> m_corners[4]; // cell of each outer corner, per corner type

    RectIndex m_hazards, m_gameplay, m_placed;
    std::unordered_map<int, std::unordered_map<Key, std::vector<std::pair<double, double>>>> m_existing;

    // chosen kit
    std::vector<int> m_lines, m_cornerItems, m_edge, m_interior, m_large;
    bool m_edgeFromKit = false;

    // ---------------------------------------------------------------- setup

    void rasterise() {
        for (auto const& r : m_in.solids) forCells(r, [&](int i, int j) { m_solid.insert(key(i, j)); });
        for (auto const& r : m_in.targets)
            forCells(r, [&](int i, int j) {
                m_targets.insert(key(i, j));
                m_solid.insert(key(i, j));
            });
    }

    bool solid(int i, int j) const { return m_solid.contains(key(i, j)); }
    bool target(int i, int j) const { return m_targets.contains(key(i, j)); }
    bool exposed(int i, int j, int side) const { return !solid(i + dx[side], j + dy[side]); }

    static std::pair<int, int> unkey(Key k) {
        return {static_cast<int>(k >> 32), static_cast<int>(static_cast<uint32_t>(k & 0xffffffff))};
    }

    void computeDepth() {
        std::vector<std::pair<int, int>> frontier;
        for (auto k : m_targets) {
            auto [i, j] = unkey(k);
            bool any = false;
            for (int s = 0; s < 4; ++s) any |= exposed(i, j, s);
            if (any) {
                m_depth[k] = 0;
                frontier.push_back({i, j});
            }
        }
        // 8-connected BFS so depth is close to true distance from the edge.
        size_t head = 0;
        while (head < frontier.size()) {
            auto [i, j] = frontier[head++];
            int d = m_depth[key(i, j)];
            for (int a = -1; a <= 1; ++a)
                for (int b = -1; b <= 1; ++b) {
                    if (!a && !b) continue;
                    Key k = key(i + a, j + b);
                    if (!m_targets.contains(k) || m_depth.contains(k)) continue;
                    m_depth[k] = d + 1;
                    frontier.push_back({i + a, j + b});
                }
        }
    }

    int depth(int i, int j) const {
        auto it = m_depth.find(key(i, j));
        return it == m_depth.end() ? 1 << 20 : it->second;
    }

    void buildRuns() {
        for (int side = 0; side < 4; ++side) {
            bool horizontal = side == Top || side == Bottom;
            std::map<int, std::vector<int>> lanes;
            for (auto k : m_targets) {
                auto [i, j] = unkey(k);
                if (exposed(i, j, side)) lanes[horizontal ? j : i].push_back(horizontal ? i : j);
            }
            for (auto& [lane, list] : lanes) {
                std::sort(list.begin(), list.end());
                size_t a = 0;
                while (a < list.size()) {
                    size_t b = a;
                    while (b + 1 < list.size() && list[b + 1] == list[b] + 1) ++b;
                    Run r;
                    r.side = side;
                    r.start = list[a] * Q;
                    r.end = (list[b] + 1) * Q;
                    switch (side) {
                        case Top: r.edge = (lane + 1) * Q; break;
                        case Bottom: r.edge = lane * Q; break;
                        case Left: r.edge = lane * Q; break;
                        default: r.edge = (lane + 1) * Q; break;
                    }
                    // An endpoint is an outer corner when the perpendicular side is exposed there.
                    if (horizontal) {
                        r.startOuter = exposed(list[a], lane, Left);
                        r.endOuter = exposed(list[b], lane, Right);
                    } else {
                        r.startOuter = exposed(lane, list[a], Bottom);
                        r.endOuter = exposed(lane, list[b], Top);
                    }
                    m_runs.push_back(r);
                    a = b + 1;
                }
            }
        }
        std::sort(m_runs.begin(), m_runs.end(), [](Run const& a, Run const& b) {
            if (a.side != b.side) return a.side < b.side;
            if (a.edge != b.edge) return a.edge < b.edge;
            return a.start < b.start;
        });

        for (auto k : m_targets) {
            auto [i, j] = unkey(k);
            if (exposed(i, j, Top) && exposed(i, j, Left)) m_corners[TopLeft].push_back({i, j});
            if (exposed(i, j, Top) && exposed(i, j, Right)) m_corners[TopRight].push_back({i, j});
            if (exposed(i, j, Bottom) && exposed(i, j, Right)) m_corners[BottomRight].push_back({i, j});
            if (exposed(i, j, Bottom) && exposed(i, j, Left)) m_corners[BottomLeft].push_back({i, j});
        }
        for (auto& v : m_corners) std::sort(v.begin(), v.end());
    }

    static void cornerPoint(int c, int i, int j, double& x, double& y) {
        x = (c == TopLeft || c == BottomLeft) ? i * Q : (i + 1) * Q;
        y = (c == TopLeft || c == TopRight) ? (j + 1) * Q : j * Q;
    }

    void indexObstacles() {
        for (auto const& r : m_in.existingSmall) m_placed.add(r);
        for (auto const& r : m_in.hazards) m_hazards.add(r);
        for (auto const& r : m_in.gameplay) m_gameplay.add(r);
        for (auto const& e : m_in.existing) {
            Key k = key(static_cast<int>(std::floor(e.x / 4)), static_cast<int>(std::floor(e.y / 4)));
            m_existing[e.objectID][k].push_back({e.x, e.y});
        }
    }

    bool isDuplicate(int objectID, double x, double y) const {
        auto it = m_existing.find(objectID);
        if (it == m_existing.end()) return false;
        int bi = static_cast<int>(std::floor(x / 4)), bj = static_cast<int>(std::floor(y / 4));
        for (int a = -1; a <= 1; ++a)
            for (int b = -1; b <= 1; ++b) {
                auto jt = it->second.find(key(bi + a, bj + b));
                if (jt == it->second.end()) continue;
                for (auto [ex, ey] : jt->second)
                    if (std::abs(ex - x) < 2.5 && std::abs(ey - y) < 2.5) return true;
            }
        return false;
    }

    // ---------------------------------------------------------------- kit

    // Design language: the most recently learned example defines the style.
    // Loose palette pieces only fill the roles that example does not cover,
    // and only a small, fixed set of them is used so the result stays coherent.
    void chooseKit() {
        auto const& items = m_in.items;
        int latestKit = 0;
        for (auto const& it : items) latestKit = std::max(latestKit, it.kit);

        auto collect = [&](Role role, size_t looseLimit, bool* fromKit = nullptr) {
            std::vector<int> out;
            if (latestKit > 0)
                for (int i = 0; i < static_cast<int>(items.size()); ++i)
                    if (items[i].kit == latestKit && items[i].role == role) out.push_back(i);
            if (fromKit) *fromKit = !out.empty();
            if (!out.empty()) return out;
            std::vector<int> loose;
            for (int i = 0; i < static_cast<int>(items.size()); ++i)
                if (items[i].kit == 0 && items[i].role == role) loose.push_back(i);
            std::sort(loose.begin(), loose.end(), [&](int a, int b) { return items[a].seq > items[b].seq; });
            if (loose.size() > looseLimit) loose.resize(looseLimit);
            std::sort(loose.begin(), loose.end(), [&](int a, int b) { return items[a].seq < items[b].seq; });
            return loose;
        };

        m_lines = collect(Role::Line, 1);
        // Loose lines that aren't axis aligned can't be tiled.
        std::erase_if(m_lines, [&](int i) { return !items[i].axisAligned; });
        m_cornerItems = collect(Role::Corner, 1);
        std::erase_if(m_cornerItems, [&](int i) { return !items[i].axisAligned; });
        m_edge = collect(Role::EdgeDetail, 2, &m_edgeFromKit);
        m_interior = collect(Role::Interior, 2);
        m_large = collect(Role::Large, 2);
        // Small loose pieces double as edge details when nothing else fills that role.
        if (m_edge.empty() && !m_edgeFromKit) {
            for (int i : m_interior)
                if (items[i].kit == 0) m_edge.push_back(i);
        }
    }

    // Items to use on a given side (or corner). Preference keeps the design
    // symmetric: pieces made for that side, else the opposite side's pieces
    // turned around, else (when allowed) the top / any side rotated into place.
    // Returns (item, rotDelta).
    std::vector<std::pair<int, double>> forSide(std::vector<int> const& pool, int side, bool allowAny) const {
        std::vector<std::pair<int, double>> out;
        auto take = [&](int from) {
            for (int i : pool)
                if (m_in.items[i].designed == from) out.push_back({i, wrapDeg((side - from) * 90.0)});
            return !out.empty();
        };
        if (pool.empty()) return out;
        if (take(side)) return out;
        if (take((side + 2) % 4)) return out;
        if (!allowAny) return out;
        if (take(Top)) return out;
        take(m_in.items[pool.front()].designed);
        return out;
    }

    // ---------------------------------------------------------------- emit

    bool emit(int item, double x, double y, double rotDelta, double scaleLong, int zRank, Rect const& bbox) {
        if (static_cast<int>(m_out.placements.size()) >= m_in.settings.maxObjects) {
            m_out.capped = true;
            return false;
        }
        Placement p;
        p.item = item;
        p.x = x;
        p.y = y;
        p.rotDelta = wrapDeg(rotDelta);
        p.scaleLong = scaleLong;
        p.zRank = zRank;
        p.bbox = bbox;
        m_out.placements.push_back(p);
        return true;
    }

    Rect rotatedBox(Item const& it, double cx, double cy, double rotDelta) const {
        bool swap = oddQuarter(rotDelta);
        return Rect::centered(cx, cy, swap ? it.bh : it.bw, swap ? it.bw : it.bh);
    }

    bool insideTargets(Rect const& r, int minDepth) const {
        return allTouchedCells(r, [&](int i, int j) { return target(i, j) && depth(i, j) >= minDepth; });
    }

    // ---------------------------------------------------------------- lines

    double lineHalfThickness(int side) const {
        double t = 0;
        for (auto [i, rot] : forSide(m_lines, side, true)) {
            auto const& it = m_in.items[i];
            double off = it.autoOffset ? 0 : it.normalOffset;
            t = std::max(t, std::min(it.bw, it.bh) / 2 - off);
        }
        return std::max(0.0, t);
    }

    void placeLines() {
        for (auto const& run : m_runs) {
            for (auto [idx, rot] : forSide(m_lines, run.side, true)) {
                auto const& it = m_in.items[idx];
                double thick = std::min(it.bw, it.bh);
                double len0 = std::max(it.bw, it.bh);
                double off = it.autoOffset ? 0 : it.normalOffset;
                // Extend into outer corners so neighbouring edges meet without a notch.
                double ext = std::max(0.0, off + thick / 2);
                double a = run.start - (run.startOuter ? ext : 0);
                double b = run.end + (run.endOuter ? ext : 0);
                double L = b - a;
                if (L < len0 * 0.35) continue;
                int n = std::max(1, static_cast<int>(std::lround(L / len0)));
                double seg = L / n;
                double f = seg / len0;

                double ox, oy;
                outward(run.side, ox, oy);
                bool horizontal = run.side == Top || run.side == Bottom;
                for (int k = 0; k < n; ++k) {
                    double along = a + seg * (k + 0.5);
                    double x = horizontal ? along : run.edge + ox * off;
                    double y = horizontal ? run.edge + oy * off : along;
                    Rect box = horizontal ? Rect::centered(x, y, seg, thick) : Rect::centered(x, y, thick, seg);
                    if (m_gameplay.hits(box, 0)) { m_out.stats.skippedOverlap++; continue; }
                    if (isDuplicate(it.objectID, x, y)) { m_out.stats.skippedDuplicate++; continue; }
                    if (!emit(idx, x, y, rot, f, 6, box)) return;
                    m_out.stats.lines++;
                }
            }
        }
    }

    // ---------------------------------------------------------------- corners

    void placeCorners() {
        if (m_cornerItems.empty()) return;
        for (int c = 0; c < 4; ++c) {
            auto pieces = forSide(m_cornerItems, c, true);
            for (auto [i, j] : m_corners[c]) {
                double px, py;
                cornerPoint(c, i, j, px, py);
                for (auto [idx, rot] : pieces) {
                    auto const& it = m_in.items[idx];
                    double ox = it.autoOffset ? 0 : it.offX, oy = it.autoOffset ? 0 : it.offY;
                    rotateCW(ox, oy, rot);
                    double x = px + ox, y = py + oy;
                    Rect box = rotatedBox(it, x, y, rot);
                    if (m_gameplay.hits(box, 1) || m_hazards.hits(box, 1)) { m_out.stats.skippedOverlap++; continue; }
                    if (isDuplicate(it.objectID, x, y)) { m_out.stats.skippedDuplicate++; continue; }
                    if (!emit(idx, x, y, rot, 1, 8, box)) return;
                    m_placed.add(box);
                    m_out.stats.corners++;
                }
            }
        }
    }

    // ---------------------------------------------------------------- edge details

    double edgeSpacing() const {
        double d = std::clamp(m_in.settings.density, 0.0, 1.0);
        return 30.0 * (2.0 + 4.0 * (1.0 - d)); // 60 (busy) .. 180 (sparse)
    }

    void placeEdgeDetails() {
        if (m_edge.empty()) return;
        double spacing = edgeSpacing();
        for (int side = 0; side < 4; ++side) {
            if (side == Bottom && !m_in.settings.bottomDetails) continue;
            std::vector<std::pair<int, double>> pieces;
            if (m_edgeFromKit) {
                // Learned: only where the example had details, mirrored to the opposite side.
                pieces = forSide(m_edge, side, false);
            } else if (side == Top || side == Bottom) {
                // Loose: details read best along floors and ceilings, not walls.
                pieces = forSide(m_edge, side, false);
            }
            if (pieces.empty()) continue;

            double lineHalf = lineHalfThickness(side);
            int pattern = 0;
            for (auto const& run : m_runs) {
                if (run.side != side) continue;
                double m0 = 20 + (run.startOuter ? 10 : 0);
                double m1 = 20 + (run.endOuter ? 10 : 0);
                double a = run.start + m0, b = run.end - m1;
                if (b < a) {
                    // Short run: one centred piece if there is room at all.
                    if (run.end - run.start < 45) continue;
                    a = b = (run.start + run.end) / 2;
                }
                // Even rhythm: n equal slots across the usable span, one piece
                // centred in each, so gaps at both ends match the gaps between.
                double span = b - a;
                int n = std::max(1, static_cast<int>(std::lround(span / spacing)));
                double ox, oy;
                outward(side, ox, oy);
                bool horizontal = side == Top || side == Bottom;
                for (int k = 0; k < n; ++k) {
                    double along = a + span * (k + 0.5) / n;
                    along = std::round(along * 2) / 2;
                    auto [idx, rot] = pieces[pattern++ % pieces.size()];
                    auto const& it = m_in.items[idx];
                    bool swap = oddQuarter(rot);
                    double normalSize = horizontal ? (swap ? it.bw : it.bh) : (swap ? it.bh : it.bw);
                    double off = it.autoOffset ? -(lineHalf + normalSize / 2 + 3) : it.normalOffset;
                    double x = horizontal ? along : run.edge + ox * off;
                    double y = horizontal ? run.edge + oy * off : along;
                    Rect box = rotatedBox(it, x, y, rot);
                    bool inside = off + normalSize / 2 <= 0.5;
                    if (inside && !insideTargets(box, 0)) continue;
                    if (m_gameplay.hits(box, 2) || m_hazards.hits(box, 2) || m_placed.hits(box, 2)) {
                        m_out.stats.skippedOverlap++;
                        continue;
                    }
                    if (isDuplicate(it.objectID, x, y)) { m_out.stats.skippedDuplicate++; continue; }
                    if (!emit(idx, x, y, rot, 1, 4, box)) return;
                    m_placed.add(box);
                    m_out.stats.edgeDetails++;
                }
            }
        }
    }

    // ---------------------------------------------------------------- inside the mass

    Rect targetBounds() const {
        Rect b{1e18, 1e18, -1e18, -1e18};
        for (auto k : m_targets) {
            auto [i, j] = unkey(k);
            b.x0 = std::min(b.x0, i * Q);
            b.y0 = std::min(b.y0, j * Q);
            b.x1 = std::max(b.x1, (i + 1) * Q);
            b.y1 = std::max(b.y1, (j + 1) * Q);
        }
        return b;
    }

    // Deterministic per-cell randomness: the same level always gets the same
    // scatter, so pressing Deco! again finds the earlier pieces and skips them.
    static double hash01(int64_t a, int64_t b, uint64_t salt) {
        uint64_t z = static_cast<uint64_t>(a) * 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(b) + 0x632BE59BD9B4E019ull) ^ salt;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return static_cast<double>(z >> 11) / static_cast<double>(1ull << 53);
    }

    void scatter(std::vector<int> const& pool, double spacing, double chance, int minDepth,
                 double margin, int zRank, bool large) {
        if (pool.empty()) return;
        Rect b = targetBounds();
        uint64_t salt = large ? 0xA11CEull : 0xB0Bull;
        // Grid anchored to the world, not the selection, so it is stable across runs.
        int64_t gi0 = static_cast<int64_t>(std::floor(b.x0 / spacing)), gi1 = static_cast<int64_t>(std::floor(b.x1 / spacing));
        int64_t gj0 = static_cast<int64_t>(std::floor(b.y0 / spacing)), gj1 = static_cast<int64_t>(std::floor(b.y1 / spacing));
        for (int64_t gj = gj0; gj <= gj1; ++gj) {
            for (int64_t gi = gi0; gi <= gi1; ++gi) {
                if (hash01(gi, gj, salt) > chance) continue;
                double x = (gi + 0.5 + (hash01(gi, gj, salt + 1) - 0.5) * 0.5) * spacing;
                double y = (gj + 0.5 + (hash01(gi, gj, salt + 2) - 0.5) * 0.5) * spacing;
                int idx = pool[static_cast<size_t>(hash01(gi, gj, salt + 3) * pool.size()) % pool.size()];
                auto const& it = m_in.items[idx];
                double rot = 0;
                if (it.axisAligned && std::abs(it.bw - it.bh) < 3)
                    rot = 90.0 * (static_cast<int>(hash01(gi, gj, salt + 4) * 4) % 4);
                x = std::round(x * 2) / 2;
                y = std::round(y * 2) / 2;
                Rect box = rotatedBox(it, x, y, rot);
                if (!insideTargets(box, minDepth)) continue;
                if (m_gameplay.hits(box, margin) || m_hazards.hits(box, margin) || m_placed.hits(box, margin)) {
                    m_out.stats.skippedOverlap++;
                    continue;
                }
                if (isDuplicate(it.objectID, x, y)) { m_out.stats.skippedDuplicate++; continue; }
                if (!emit(idx, x, y, rot, 1, zRank, box)) return;
                m_placed.add(box);
                (large ? m_out.stats.large : m_out.stats.interior)++;
            }
        }
    }

    void placeLarge() {
        if (m_large.empty()) return;
        double size = 0;
        for (int i : m_large) size = std::max({size, m_in.items[i].bw, m_in.items[i].bh});
        double d = std::clamp(m_in.settings.density, 0.0, 1.0);
        scatter(m_large, std::max(120.0, size * 2.2), 0.35 + 0.4 * d, 2, 6, 0, true);
    }

    void placeInterior() {
        double d = std::clamp(m_in.settings.density, 0.0, 1.0);
        scatter(m_interior, 30.0 * (3.0 + 4.0 * (1.0 - d)), 0.45 + 0.35 * d, 3, 4, 2, false);
    }
};

double normAngle(double deg) {
    double r = std::fmod(deg, 90.0);
    if (r < 0) r += 90.0;
    return std::min(r, 90.0 - r);
}

} // namespace

char const* roleName(Role r) {
    switch (r) {
        case Role::Line: return "outline";
        case Role::Corner: return "corner";
        case Role::EdgeDetail: return "edge detail";
        case Role::Interior: return "detail";
        case Role::Large: return "large piece";
    }
    return "?";
}

Result generate(Input const& in) {
    Engine e(in);
    return e.run();
}

Item classifyLoose(double bw, double bh, double rot0) {
    Item it;
    it.bw = std::max(bw, 0.5);
    it.bh = std::max(bh, 0.5);
    it.rot0 = rot0;
    it.axisAligned = normAngle(rot0) < 2.0;
    bool odd = oddQuarter(rot0);
    double lw = odd ? it.bh : it.bw, lh = odd ? it.bw : it.bh;
    it.longIsLocalX = lw >= lh;
    it.autoOffset = true;

    double maxD = std::max(it.bw, it.bh), minD = std::min(it.bw, it.bh);
    double aspect = maxD / std::max(minD, 0.5);

    if (it.axisAligned && aspect >= 2.5 && minD <= 12) {
        it.role = Role::Line;
        it.designed = it.bw >= it.bh ? Top : Left;
    } else if (maxD <= 16) {
        it.role = Role::Interior;
        it.designed = Top;
    } else if (maxD <= 40 && aspect < 2.5) {
        it.role = Role::EdgeDetail;
        it.designed = Top;
    } else {
        it.role = Role::Large;
        it.designed = Top;
    }
    return it;
}

bool classifyLearned(Rect const& R, double bw, double bh, double rot0, double cx, double cy, Item& out) {
    out = classifyLoose(bw, bh, rot0);
    out.autoOffset = false;
    double maxD = std::max(out.bw, out.bh), minD = std::min(out.bw, out.bh);
    double aspect = maxD / std::max(minD, 0.5);

    double dT = cy - R.y1, dB = R.y0 - cy, dL = R.x0 - cx, dR = cx - R.x1; // outward distances
    double outMost = std::max({dT, dB, dL, dR});
    if (outMost > 22) return false; // floating in the air: not part of the block design

    // Corner: centred near a corner of the block and not elongated.
    if (aspect < 2.5 && out.axisAligned) {
        double tol = std::clamp(minD * 0.5, 6.0, 15.0);
        double px[4] = {R.x0, R.x1, R.x1, R.x0};
        double py[4] = {R.y1, R.y1, R.y0, R.y0};
        for (int c = 0; c < 4; ++c) {
            if (std::abs(cx - px[c]) <= tol && std::abs(cy - py[c]) <= tol) {
                out.role = Role::Corner;
                out.designed = c;
                out.offX = cx - px[c];
                out.offY = cy - py[c];
                return true;
            }
        }
    }

    // Line: elongated, axis aligned, running along an edge.
    if (aspect >= 2.0 && out.axisAligned) {
        bool horizontal = out.bw >= out.bh;
        double tol = std::max(10.0, minD);
        if (horizontal) {
            double t = std::abs(cy - R.y1), b = std::abs(cy - R.y0);
            if (std::min(t, b) <= tol) {
                out.role = Role::Line;
                out.designed = t <= b ? Top : Bottom;
                out.normalOffset = out.designed == Top ? dT : dB;
                return true;
            }
        } else {
            double l = std::abs(cx - R.x0), r = std::abs(cx - R.x1);
            if (std::min(l, r) <= tol) {
                out.role = Role::Line;
                out.designed = l <= r ? Left : Right;
                out.normalOffset = out.designed == Left ? dL : dR;
                return true;
            }
        }
    }

    // Otherwise: detail. Which edge is it closest to, and how far inside?
    double d[4] = {dT, dR, dB, dL};
    int nearest = 0;
    for (int s = 1; s < 4; ++s)
        if (d[s] > d[nearest]) nearest = s;
    double inset = -d[nearest]; // positive = inside the block

    if (inset <= 20) {
        out.role = Role::EdgeDetail;
        out.designed = nearest;
        out.normalOffset = d[nearest];
    } else if (maxD > 40) {
        out.role = Role::Large;
    } else {
        out.role = Role::Interior;
    }
    return true;
}

} // namespace deco
