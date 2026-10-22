// Deco AI decoration engine.
//
// Pure C++20 with no Geode / GD dependency, so the placement logic can be
// compiled and tested on its own. The GD glue (main.cpp) feeds it rectangles
// and palette items and turns the resulting placements into editor objects.
//
// Coordinates are GD units (one block = 30), y points up, rotations are in
// degrees, clockwise-positive (the cocos2d / GD convention).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace deco {

struct Rect {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    double w() const { return x1 - x0; }
    double h() const { return y1 - y0; }
    double cx() const { return (x0 + x1) / 2; }
    double cy() const { return (y0 + y1) / 2; }
    bool overlaps(Rect const& o, double margin = 0) const {
        return x0 < o.x1 + margin && o.x0 < x1 + margin &&
               y0 < o.y1 + margin && o.y0 < y1 + margin;
    }
    static Rect centered(double cx, double cy, double w, double h) {
        return {cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2};
    }
};

// Sides are numbered clockwise starting at the top, so a side's angle is side * 90.
enum Side : int { Top = 0, Right = 1, Bottom = 2, Left = 3 };
// Corners likewise: top-left, top-right, bottom-right, bottom-left.
enum Corner : int { TopLeft = 0, TopRight = 1, BottomRight = 2, BottomLeft = 3 };

enum class Role : int {
    Line = 0,       // outline / edge strip, tiled along exposed edges
    Corner = 1,     // sits on outer corners
    EdgeDetail = 2, // placed just inside an edge, in an even rhythm
    Interior = 3,   // small, quiet detail deep inside a block mass
    Large = 4,      // big piece, only where it fits fully inside a block mass
    Fill = 5,       // covers the block itself (the block's base); goes on every block
};

char const* roleName(Role r);

struct Item {
    Role role = Role::Interior;
    int designed = Top;      // Side (Line/EdgeDetail) or Corner (Corner) it was made for
    double bw = 0, bh = 0;   // on-screen bounding box at its original rotation
    double rot0 = 0;         // original rotation
    bool longIsLocalX = true;
    bool axisAligned = true; // rotation is a multiple of 90 degrees
    double normalOffset = 0; // Line/EdgeDetail: distance from the edge line, outward positive
    bool autoOffset = true;  // loose pieces: engine picks the offset (centred line / inset detail)
    double offX = 0, offY = 0; // Corner: offset from the corner point; Fill: offset from the block centre
    double refW = 30, refH = 30; // Fill: size of the example block it was learned on
    int kit = 0;             // 0 = loose palette piece, >0 = learned example it came from
    int seq = 0;             // insertion order (newer = larger)
    int objectID = 0;
    int zOrder0 = 0;
    std::string saveString;  // full GD object string, used by the glue
};

struct Settings {
    bool fills = true;
    bool outlines = true;
    bool corners = true;
    bool edgeDetails = true;
    bool bottomDetails = true;
    bool interior = true;
    bool large = true;
    double density = 0.6;   // 0..1, how busy edge rhythm and interior get
    int maxObjects = 5000;
};

struct Existing {
    int objectID;
    double x, y;
};

struct Input {
    std::vector<Rect> solids;    // every solid block and slope in the level (for exposure)
    std::vector<Rect> targets;   // the selected blocks to decorate
    std::vector<Rect> hazards;   // spikes, saws: never covered by details
    std::vector<Rect> gameplay;  // orbs, pads, portals, coins: never covered at all
    std::vector<Existing> existing; // decoration already in the level (dedupe)
    std::vector<Rect> existingSmall; // bboxes of small existing deco: details avoid them
    std::vector<Item> items;
    Settings settings;
    uint32_t seed = 1;
};

struct Placement {
    int item = -1;           // index into Input::items
    double x = 0, y = 0;     // final centre
    double rotDelta = 0;     // rotation to add to the piece's own rotation
    double scaleLong = 1;    // stretch factor along the piece's long local axis
    double scaleX = 1, scaleY = 1; // extra scale (fills on half blocks and scaled blocks)
    int zRank = 0;           // added to the piece's own Z order
    Rect bbox;
};

struct Stats {
    int fills = 0, lines = 0, corners = 0, edgeDetails = 0, interior = 0, large = 0;
    int skippedOverlap = 0, skippedDuplicate = 0;
};

struct Result {
    std::vector<Placement> placements;
    Stats stats;
    bool capped = false;
};

Result generate(Input const& in);

// Classify a loose palette piece purely from its shape.
Item classifyLoose(double bw, double bh, double rot0);

// Classify a piece the user placed on an example block. Returns false if the
// piece is too far from the block to belong to it (air deco is not learned).
bool classifyLearned(Rect const& block, double bw, double bh, double rot0,
                     double cx, double cy, Item& out);

} // namespace deco
