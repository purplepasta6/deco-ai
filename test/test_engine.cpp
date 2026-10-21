// Offline tests for the decoration engine: builds simulated levels, runs the
// engine, checks hard rules, and writes the placements as JSON for rendering.
//
// Build: g++ -std=c++20 -O2 -I../src ../src/engine.cpp test_engine.cpp -o test_engine

#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace deco;

static int g_failures = 0;
#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            ++g_failures;                                   \
            std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                       \
            std::printf("\n");                              \
        }                                                   \
    } while (0)

static Rect block(double i, double j, double w = 1, double h = 1) {
    return {i * 30, j * 30, (i + w) * 30, (j + h) * 30};
}

struct Level {
    std::vector<Rect> solids, hazards, gameplay;
};

// A small but varied level: ground with a gap, stairs, a floating platform,
// a pillar, a half slab, a big solid mass, spikes and an orb.
static Level makeLevel() {
    Level L;
    for (int i = 0; i < 14; ++i) L.solids.push_back(block(i, 0));          // ground part 1
    for (int i = 17; i < 30; ++i) L.solids.push_back(block(i, 0));         // ground part 2 after a gap
    for (int s = 0; s < 3; ++s)                                            // stairs
        for (int k = 0; k <= s; ++k) L.solids.push_back(block(6 + s, 1 + k));
    for (int i = 10; i < 13; ++i) L.solids.push_back(block(i, 5));         // floating platform
    for (int j = 1; j < 5; ++j) L.solids.push_back(block(20, j));          // pillar
    L.solids.push_back({14 * 30, 0, 15 * 30, 15});                         // half slab in the gap
    for (int i = 22; i < 30; ++i)                                          // big mass
        for (int j = 1; j < 7; ++j) L.solids.push_back(block(i, j));
    L.hazards.push_back({2 * 30 + 6, 30, 3 * 30 - 6, 30 + 24});            // spike on the ground
    L.hazards.push_back({3 * 30 + 6, 30, 4 * 30 - 6, 30 + 24});            // second spike
    L.hazards.push_back({24 * 30 + 6, 7 * 30, 25 * 30 - 6, 7 * 30 + 24});  // spike on the mass
    L.gameplay.push_back(Rect::centered(16 * 30, 3 * 30, 26, 26));         // orb above the gap
    return L;
}

static std::vector<Item> loosePalette() {
    std::vector<Item> items;
    auto add = [&](double w, double h, int id) {
        Item it = classifyLoose(w, h, 0);
        it.objectID = id;
        it.seq = static_cast<int>(items.size());
        items.push_back(it);
    };
    add(30, 4, 101);  // thin outline
    add(8, 8, 102);   // small dot
    add(24, 24, 103); // medium accent
    add(60, 60, 104); // large piece
    add(12, 30, 105); // a second, vertical thin piece (should NOT be used: one outline only)
    return items;
}

static std::vector<Item> learnedKit() {
    std::vector<Item> items;
    Rect R = block(0, 0);
    struct P { double w, h, cx, cy; int id; } pieces[] = {
        {30, 3, 15, 30, 201},   // outline centred on the top edge
        {30, 10, 15, 34, 202},  // glow strip just outside the top edge
        {3, 30, 0, 15, 203},    // outline on the left edge
        {10, 10, 0, 30, 204},   // corner cap on the top-left corner
        {6, 6, 15, 22, 205},    // small detail just inside the top edge
        {8, 8, 15, 6, 206},     // detail just inside the bottom edge
    };
    for (auto const& p : pieces) {
        Item it;
        bool ok = classifyLearned(R, p.w, p.h, 0, p.cx, p.cy, it);
        CHECK(ok, "learned piece %d rejected", p.id);
        it.kit = 1;
        it.objectID = p.id;
        it.seq = static_cast<int>(items.size());
        items.push_back(it);
    }
    return items;
}

static bool coveredByTargets(Rect const& r, std::vector<Rect> const& targets) {
    // Sample a grid of points inside r; each must be inside some target.
    for (int a = 0; a <= 4; ++a)
        for (int b = 0; b <= 4; ++b) {
            double x = r.x0 + (r.x1 - r.x0) * (0.02 + 0.96 * a / 4.0);
            double y = r.y0 + (r.y1 - r.y0) * (0.02 + 0.96 * b / 4.0);
            bool in = false;
            for (auto const& t : targets)
                if (x >= t.x0 - 1e-6 && x <= t.x1 + 1e-6 && y >= t.y0 - 1e-6 && y <= t.y1 + 1e-6) { in = true; break; }
            if (!in) return false;
        }
    return true;
}

static void writeJson(std::string const& path, Level const& L, Input const& in, Result const& res) {
    std::ofstream f(path);
    auto rect = [&](Rect const& r) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "[%.2f,%.2f,%.2f,%.2f]", r.x0, r.y0, r.x1, r.y1);
        return std::string(buf);
    };
    f << "{\"solids\":[";
    for (size_t i = 0; i < L.solids.size(); ++i) f << (i ? "," : "") << rect(L.solids[i]);
    f << "],\"hazards\":[";
    for (size_t i = 0; i < L.hazards.size(); ++i) f << (i ? "," : "") << rect(L.hazards[i]);
    f << "],\"gameplay\":[";
    for (size_t i = 0; i < L.gameplay.size(); ++i) f << (i ? "," : "") << rect(L.gameplay[i]);
    f << "],\"placements\":[";
    for (size_t i = 0; i < res.placements.size(); ++i) {
        auto const& p = res.placements[i];
        auto const& it = in.items[p.item];
        f << (i ? "," : "") << "{\"role\":\"" << roleName(it.role) << "\",\"id\":" << it.objectID
          << ",\"box\":" << rect(p.bbox) << ",\"rot\":" << p.rotDelta << ",\"scale\":" << p.scaleLong
          << ",\"z\":" << p.zRank << "}";
    }
    f << "]}";
}

static Result runCase(char const* name, Level const& L, std::vector<Item> items, Settings s, std::string const& out) {
    std::printf("== %s\n", name);
    Input in;
    in.solids = L.solids;
    in.targets = L.solids;
    in.hazards = L.hazards;
    in.gameplay = L.gameplay;
    in.items = std::move(items);
    in.settings = s;
    Result res = generate(in);
    auto const& st = res.stats;
    std::printf("  placed %zu (lines %d, corners %d, edge %d, interior %d, large %d), skipped overlap %d dup %d\n",
        res.placements.size(), st.lines, st.corners, st.edgeDetails, st.interior, st.large,
        st.skippedOverlap, st.skippedDuplicate);

    for (auto const& p : res.placements) {
        auto const& it = in.items[p.item];
        for (auto const& g : L.gameplay)
            CHECK(!p.bbox.overlaps(g), "%s overlaps gameplay object", roleName(it.role));
        if (it.role != Role::Line)
            for (auto const& h : L.hazards)
                CHECK(!p.bbox.overlaps(h, -0.01), "%s at (%.1f,%.1f) overlaps a hazard", roleName(it.role), p.x, p.y);
        if (it.role == Role::Interior || it.role == Role::Large)
            CHECK(coveredByTargets(p.bbox, in.targets), "%s at (%.1f,%.1f) pokes out of the blocks", roleName(it.role), p.x, p.y);
        if (it.role == Role::EdgeDetail && it.autoOffset)
            CHECK(coveredByTargets(p.bbox, in.targets), "edge detail at (%.1f,%.1f) pokes out", p.x, p.y);
        if (it.role == Role::Line)
            CHECK(p.scaleLong > 0.34 && p.scaleLong < 1.51, "line stretch %.2f out of range", p.scaleLong);
        CHECK(std::abs(p.rotDelta - std::round(p.rotDelta / 90) * 90) < 1e-6, "rotation %.2f not a quarter turn", p.rotDelta);
    }

    // Idempotence: feeding the result back as existing deco must place nothing new.
    Input again = in;
    for (auto const& p : res.placements) {
        again.existing.push_back({in.items[p.item].objectID, p.x, p.y});
        if (in.items[p.item].role != Role::Line) again.existingSmall.push_back(p.bbox);
    }
    Result res2 = generate(again);
    CHECK(res2.placements.empty(), "second run placed %zu duplicates", res2.placements.size());

    writeJson(out, L, in, res);
    return res;
}

// Edge coverage: every exposed top edge of the ground must be covered by
// outline tiles with no gaps (checked by sampling along the edge).
static void checkTopCoverage(Level const& L, Result const& res, Input const& in, double y, double x0, double x1) {
    for (double x = x0 + 0.5; x < x1; x += 1.0) {
        bool covered = false;
        for (auto const& p : res.placements) {
            if (in.items[p.item].role != Role::Line) continue;
            if (x >= p.bbox.x0 - 1e-6 && x <= p.bbox.x1 + 1e-6 && y >= p.bbox.y0 - 1e-6 && y <= p.bbox.y1 + 1e-6) {
                covered = true;
                break;
            }
        }
        CHECK(covered, "gap in outline at x=%.1f y=%.1f", x, y);
        if (!covered) return;
    }
    (void)L;
}

int main(int argc, char** argv) {
    std::string outDir = argc > 1 ? argv[1] : ".";
    Level L = makeLevel();
    Settings s;

    {
        auto items = loosePalette();
        // Classification expectations.
        CHECK(items[0].role == Role::Line, "30x4 should be an outline");
        CHECK(items[1].role == Role::Interior, "8x8 should be a small detail");
        CHECK(items[2].role == Role::EdgeDetail, "24x24 should be an edge detail");
        CHECK(items[3].role == Role::Large, "60x60 should be a large piece");
        Input in;
        in.items = items;
        Result r = runCase("loose palette", L, items, s, outDir + "/loose.json");
        // Only ONE outline style may be used (design language).
        std::vector<int> lineIDs;
        for (auto const& p : r.placements)
            if (in.items[p.item].role == Role::Line &&
                std::find(lineIDs.begin(), lineIDs.end(), in.items[p.item].objectID) == lineIDs.end())
                lineIDs.push_back(in.items[p.item].objectID);
        CHECK(lineIDs.size() == 1, "%zu outline styles used", lineIDs.size());
        // Ground part 1 top edge, minus where stairs/spikes sit on it, is covered: check x in [0, 180).
        checkTopCoverage(L, r, in, 30.0, 0, 180);
    }

    {
        auto items = learnedKit();
        CHECK(items[0].role == Role::Line && items[0].designed == Top, "learned top outline");
        CHECK(items[1].role == Role::Line && items[1].designed == Top && std::abs(items[1].normalOffset - 4) < 1e-6, "learned glow offset");
        CHECK(items[2].role == Role::Line && items[2].designed == Left, "learned left outline");
        CHECK(items[3].role == Role::Corner && items[3].designed == TopLeft, "learned corner");
        CHECK(items[4].role == Role::EdgeDetail && items[4].designed == Top, "learned top detail");
        CHECK(items[5].role == Role::EdgeDetail && items[5].designed == Bottom, "learned bottom detail");
        Input in;
        in.items = items;
        Result r = runCase("learned kit", L, items, s, outDir + "/learned.json");
        checkTopCoverage(L, r, in, 30.0, 0, 180);
    }

    {
        // Partial selection: only the first 5 ground blocks. The border with the
        // unselected 6th block is not a real edge, so no vertical outline there.
        std::printf("== partial selection\n");
        Input in;
        in.solids = L.solids;
        for (int i = 0; i < 5; ++i) in.targets.push_back(block(i, 0));
        in.items = loosePalette();
        Result r = generate(in);
        for (auto const& p : r.placements)
            CHECK(!(p.bbox.x0 < 150 && p.bbox.x1 > 150 && p.bbox.y0 < 15 && p.bbox.y1 > 15 && p.bbox.h() > p.bbox.w()),
                  "outline drawn along the selection border");
        CHECK(r.stats.lines > 0, "partial selection got no outlines");
        std::printf("  placed %zu\n", r.placements.size());
    }

    {
        Level empty;
        Settings s2;
        auto items = loosePalette();
        Input in;
        in.items = items;
        in.settings = s2;
        Result r = generate(in);
        CHECK(r.placements.empty(), "nothing selected must place nothing");
    }

    {
        // Object cap.
        Settings s3;
        s3.maxObjects = 10;
        auto items = loosePalette();
        Input in;
        in.solids = L.solids;
        in.targets = L.solids;
        in.items = items;
        in.settings = s3;
        Result r = generate(in);
        CHECK(r.placements.size() == 10 && r.capped, "cap not respected (%zu)", r.placements.size());
    }

    std::printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL CHECKS PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
