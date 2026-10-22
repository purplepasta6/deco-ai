// Anchor templates: decoration learned around one object (a spike, slope,
// orb, pad, portal...) and re-applied around every matching object.
//
// Pure C++20, no GD dependency, so the geometry is tested offline.
//
// Orientation model. GD/cocos draws a node as  T = Rotate(rot) * Scale(fx, fy)
// where fx/fy are -1 when the object is flipped. Any such transform equals
// Rotate(rot + 180*flipY) * MirrorX^(flipX xor flipY), so an orientation is
// stored as (rotation, mirrored). Rotations are degrees, clockwise-positive,
// with y pointing up (the cocos convention).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace deco {

struct Pose {
    double x = 0, y = 0;
    double rot = 0;      // degrees, clockwise-positive
    bool flipX = false;
    bool flipY = false;
    double sx = 1, sy = 1; // object scale (always positive)
};

// Orientation in canonical form.
struct Orient {
    double rot = 0;
    bool mirror = false;
};

Orient canonical(double rot, bool flipX, bool flipY);

struct TPiece {
    int objectID = 0;
    double dx = 0, dy = 0;  // offset in the anchor's local, unscaled frame
    double lrot = 0;        // orientation relative to the anchor
    bool lmirror = false;
    double bw = 0, bh = 0;  // piece's on-screen bounding box when learned
    std::string saveString;
};

struct TVariant {
    int count = 1;
    std::vector<TPiece> pieces;
};

enum class AnchorClass : int {
    Block = 0, Slope = 1, Hazard = 2, Orb = 3, Pad = 4, Portal = 5, Collectible = 6, Other = 7,
};

char const* anchorClassName(AnchorClass c);

struct AnchorTemplate {
    int objectID = 0;
    AnchorClass cls = AnchorClass::Other;
    double bw = 30, bh = 30; // the example anchor's unscaled size
    std::vector<TVariant> variants;

    int totalCount() const;
};

// Express a piece (its own pose) relative to an anchor.
TPiece learnPiece(Pose const& anchor, Pose const& piece);

// Where a learned piece goes around a target anchor. `ratio` scales the local
// offsets (used when falling back to a template made for a different-sized
// object of the same class).
struct Placed {
    double x = 0, y = 0;
    double rot = 0;      // absolute rotation to write
    bool flipX = false;  // absolute flips to write (flipY is always false)
    double scale = 1;    // multiply the piece's own scale by this
};

Placed placePiece(Pose const& target, TPiece const& p, double ratioX = 1, double ratioY = 1);

// Signature used to group identical decoration sets when copying a section.
std::string variantSignature(std::vector<TPiece> const& pieces);

// Merge per-anchor piece sets into a template: identical sets are grouped
// and counted, and only the `maxVariants` most common ones are kept.
void addVariants(AnchorTemplate& tpl, std::vector<std::vector<TPiece>> const& sets, size_t maxVariants = 4);

// Deterministic, position-based choice of a variant (weighted by count), so
// running Deco! twice picks the same variant and duplicates are detected.
TVariant const* chooseVariant(AnchorTemplate const& tpl, double x, double y);

} // namespace deco
