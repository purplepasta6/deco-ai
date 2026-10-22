// Style library: named decoration styles with their learned pieces.
// Plain C++ (no GD dependency) so the save format is tested offline.

#pragma once

#include "engine.hpp"
#include "templates.hpp"

#include <map>
#include <string>
#include <vector>

namespace deco {

struct Style {
    int id = 0;
    std::string name;
    std::string preview;                    // file name of the thumbnail, may be empty
    std::vector<Item> items;                // block pieces (kit 1 = learned, kit 0 = palette)
    std::map<int, AnchorTemplate> anchors;  // per anchor object ID

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
