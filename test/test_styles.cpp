// Round-trip and merge tests for the style library.
// Build: g++ -std=c++20 -O2 -I../src ../src/engine.cpp ../src/templates.cpp ../src/styles.cpp test_styles.cpp -o test_styles

#include "styles.hpp"

#include <cmath>
#include <cstdio>

using namespace deco;

static int failures = 0;
#define CHECK(c, msg)                         \
    do {                                      \
        if (!(c)) {                           \
            ++failures;                       \
            std::printf("FAIL: %s\n", msg);   \
        }                                     \
    } while (0)

int main() {
    Library lib;
    auto& a = lib.create("Neon\tglow\n");
    a.preview = "style-1.png";
    Item it = classifyLoose(30, 3, 0);
    it.kit = 1;
    it.objectID = 507;
    it.saveString = "1,507,2,15,3,30,21,1004";
    a.items.push_back(it);
    AnchorTemplate tpl;
    tpl.objectID = 8;
    tpl.cls = AnchorClass::Hazard;
    tpl.bw = 12;
    tpl.bh = 24;
    TPiece p{1764, 0.5, -3.25, 90, true, 30, 8, "1,1764,2,0,3,0,24,-1"};
    addVariants(tpl, {{p}, {p}});
    a.anchors[8] = tpl;
    int aId = a.id;
    int bId = lib.create("").id; // invalidates `a`
    CHECK(lib.find(bId)->name == "Style 2", "default name");
    lib.activeId = aId;

    auto text = lib.serialize();
    Library back = Library::deserialize(text);
    CHECK(back.styles.size() == 2, "style count");
    CHECK(back.activeId == aId, "active id");
    CHECK(back.nextId == lib.nextId, "next id");
    auto* s = back.find(aId);
    CHECK(s && s->name == "Neon glow", "name cleaned and kept");
    CHECK(s && s->preview == "style-1.png", "preview kept");
    CHECK(s && s->items.size() == 1 && s->items[0].saveString == it.saveString, "items kept");
    CHECK(s && s->anchors.count(8) == 1, "anchor kept");
    if (s && s->anchors.count(8)) {
        auto const& t = s->anchors.at(8);
        CHECK(t.cls == AnchorClass::Hazard && t.variants.size() == 1 && t.variants[0].count == 2, "variant kept");
        auto const& q = t.variants[0].pieces[0];
        CHECK(q.objectID == 1764 && q.lmirror && std::abs(q.dy + 3.25) < 1e-6 && q.saveString == p.saveString,
              "piece kept");
    }
    CHECK(Library::deserialize(back.serialize()).serialize() == back.serialize(), "stable round trip");

    // Garbage in, no crash, nothing bogus out.
    Library junk = Library::deserialize("S\tx\nI\t9\nP\t1\nA\t\t\nrandom\n\n");
    CHECK(junk.styles.empty() || junk.styles[0].items.empty(), "junk tolerated");

    // Merge drops duplicates (same piece on several example blocks).
    std::vector<Item> into;
    mergeLearnedItems(into, {it, it, it});
    CHECK(into.size() == 1, "duplicates merged");
    Item other = it;
    other.normalOffset = 4;
    mergeLearnedItems(into, {other});
    CHECK(into.size() == 2, "different offset kept");

    lib.remove(aId);
    CHECK(lib.styles.size() == 1 && lib.activeId == bId, "remove switches active");

    std::printf(failures ? "%d FAILURES\n" : "ALL STYLE CHECKS PASSED\n", failures);
    return failures ? 1 : 0;
}
