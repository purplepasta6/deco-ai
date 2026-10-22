// Verifies anchor-template geometry against a direct 2x2 matrix computation.
// Build: g++ -std=c++20 -O2 -I../src ../src/templates.cpp test_templates.cpp -o test_templates

#include "templates.hpp"

#include <cmath>
#include <cstdio>
#include <random>

using namespace deco;

struct M {
    double a, b, c, d; // [a b; c d]
    M operator*(M const& o) const {
        return {a * o.a + b * o.c, a * o.b + b * o.d, c * o.a + d * o.c, c * o.b + d * o.d};
    }
    M inv() const {
        double det = a * d - b * c;
        return {d / det, -b / det, -c / det, a / det};
    }
};

static M R(double deg) { // clockwise, y up
    double r = deg * 3.14159265358979323846 / 180.0;
    return {std::cos(r), std::sin(r), -std::sin(r), std::cos(r)};
}
static M F(bool fx, bool fy) { return {fx ? -1.0 : 1.0, 0, 0, fy ? -1.0 : 1.0}; }
static M S(double sx, double sy) { return {sx, 0, 0, sy}; }

static bool near(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }
static bool nearM(M const& x, M const& y) {
    return near(x.a, y.a) && near(x.b, y.b) && near(x.c, y.c) && near(x.d, y.d);
}

int main() {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> pos(-500, 500), small(-40, 40);
    std::uniform_int_distribution<int> quarter(0, 7), coin(0, 1);
    std::uniform_real_distribution<double> scale(0.5, 2.0);
    int failures = 0, cases = 0;

    for (int t = 0; t < 20000; ++t) {
        bool anyAngle = t % 3 == 0;
        auto angle = [&]() { return anyAngle ? std::uniform_real_distribution<double>(0, 360)(rng) : quarter(rng) * 45.0; };
        Pose A{pos(rng), pos(rng), angle(), coin(rng) == 1, coin(rng) == 1, scale(rng), scale(rng)};
        Pose P{A.x + small(rng), A.y + small(rng), angle(), coin(rng) == 1, coin(rng) == 1, 1, 1};
        Pose B{pos(rng), pos(rng), angle(), coin(rng) == 1, coin(rng) == 1, scale(rng), scale(rng)};

        TPiece tp = learnPiece(A, P);
        Placed out = placePiece(B, tp);

        // Expected position.
        M MA = R(A.rot) * F(A.flipX, A.flipY) * S(A.sx, A.sy);
        M MB = R(B.rot) * F(B.flipX, B.flipY) * S(B.sx, B.sy);
        M toLocal = MA.inv();
        double lx = toLocal.a * (P.x - A.x) + toLocal.b * (P.y - A.y);
        double ly = toLocal.c * (P.x - A.x) + toLocal.d * (P.y - A.y);
        double ex = B.x + MB.a * lx + MB.b * ly;
        double ey = B.y + MB.c * lx + MB.d * ly;

        // Expected orientation (scale-free).
        M OA = R(A.rot) * F(A.flipX, A.flipY);
        M OB = R(B.rot) * F(B.flipX, B.flipY);
        M OP = R(P.rot) * F(P.flipX, P.flipY);
        M expectO = OB * OA.inv() * OP;
        M gotO = R(out.rot) * F(out.flipX, false);

        ++cases;
        bool ok = near(out.x, ex, 1e-5) && near(out.y, ey, 1e-5) && nearM(expectO, gotO);
        // Learning and placing on the same anchor must return the piece itself.
        Placed same = placePiece(A, tp);
        M sameO = R(same.rot) * F(same.flipX, false);
        ok = ok && near(same.x, P.x, 1e-5) && near(same.y, P.y, 1e-5) && nearM(sameO, OP);
        if (!ok && failures++ < 5) {
            std::printf("FAIL case %d: got (%.3f,%.3f rot %.2f fx %d) want (%.3f,%.3f)\n", t, out.x, out.y, out.rot,
                        out.flipX, ex, ey);
        }
    }

    // Variant grouping.
    AnchorTemplate tpl;
    TPiece a{1, 0, 10, 0, false, 5, 5, ""}, b{2, 0, -10, 0, false, 5, 5, ""};
    addVariants(tpl, {{a}, {a}, {a, b}, {a}, {b, a}});
    bool groupOk = tpl.variants.size() == 2 && tpl.variants[0].count == 3 && tpl.variants[1].count == 2;
    if (!groupOk) { ++failures; std::printf("FAIL variant grouping: %zu variants\n", tpl.variants.size()); }
    // Deterministic choice.
    auto v1 = chooseVariant(tpl, 123.5, 45), v2 = chooseVariant(tpl, 123.5, 45);
    if (v1 != v2) { ++failures; std::printf("FAIL variant choice not deterministic\n"); }

    std::printf("%d cases, %d failures\n", cases, failures);
    return failures ? 1 : 0;
}
