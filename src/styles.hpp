// Style library: named decoration styles with their learned pieces.
// Plain C++ (no GD dependency) so the save format is tested offline.

#pragma once

#include "engine.hpp"
#include "templates.hpp"

#include <map>
#include <string>
#include <vector>

namespace deco {

// Settings of one colour channel, as learned from the example's level.
struct Channel {
    int id = 0;
    int r = 255, g = 255, b = 255;
    double opacity = 1;
    bool blending = false;
    int copyID = 0;
    double h = 0, s = 0, v = 0;
    bool absS = false, absV = false;
    bool copyOpacity = false;
    int playerColor = 0;

    bool sameAs(Channel const& o) const; // same look (ids ignored, copy targets compared as given)
};

// Colour channels (main 21 / secondary 22) a save string uses; only 1..999.
std::vector<int> channelsUsed(std::string const& saveString);

// Rewrite keys 21 and 22 through `map` (channels not in the map are kept).
std::string remapChannels(std::string const& saveString, std::map<int, int> const& map);

struct Style {
    int id = 0;
    std::string name;
    std::string preview;                    // file name of the thumbnail, may be empty
    std::vector<Item> items;                // block pieces (kit 1 = learned, kit 0 = palette)
    std::map<int, AnchorTemplate> anchors;  // per anchor object ID
    std::map<int, Channel> colors;          // colour channels the learned pieces use

    bool empty() const { return items.empty() && anchors.empty(); }
    std::string summary() const;            // e.g. "3 outline, 1 corner, spike, orb"
};

struct Library {
    std::vector<Style> styles;
    int activeId = 0;
    int nextId = 1;
    int nextSeq = 1;

    Style* active();
    Style* find(int id);
    Style& create(std::string name);
    void remove(int id);

    std::string serialize() const;
    static Library deserialize(std::string const& data);
};

// Merge freshly learned block pieces into a style, dropping duplicates:
// the same piece in the same role and position on several example blocks
// is kept once, so copying a whole decorated section works.
void mergeLearnedItems(std::vector<Item>& into, std::vector<Item> const& learned);

std::string cleanName(std::string s);

} // namespace deco
