"""Render engine test output (JSON) to a PNG so placements can be inspected.

usage: python3 render.py in.json out.png
"""
import json
import sys

from PIL import Image, ImageDraw

SCALE = 2.0
PAD = 40
COLORS = {
    "outline": (255, 255, 255),
    "corner": (255, 200, 40),
    "edge detail": (80, 220, 255),
    "detail": (190, 120, 255),
    "large piece": (60, 200, 120),
}


def main(src, dst):
    data = json.load(open(src))
    allr = data["solids"] + data["hazards"] + data["gameplay"] + [p["box"] for p in data["placements"]]
    x0 = min(r[0] for r in allr) - PAD
    y0 = min(r[1] for r in allr) - PAD
    x1 = max(r[2] for r in allr) + PAD
    y1 = max(r[3] for r in allr) + PAD
    W, H = int((x1 - x0) * SCALE), int((y1 - y0) * SCALE)
    img = Image.new("RGB", (W, H), (22, 28, 60))
    d = ImageDraw.Draw(img, "RGBA")

    def box(r):
        return [(r[0] - x0) * SCALE, H - (r[3] - y0) * SCALE, (r[2] - x0) * SCALE, H - (r[1] - y0) * SCALE]

    # grid
    gx = (int(x0) // 30) * 30
    while gx < x1:
        X = (gx - x0) * SCALE
        d.line([X, 0, X, H], fill=(40, 48, 90))
        gx += 30
    gy = (int(y0) // 30) * 30
    while gy < y1:
        Y = H - (gy - y0) * SCALE
        d.line([0, Y, W, Y], fill=(40, 48, 90))
        gy += 30

    for r in data["solids"]:
        d.rectangle(box(r), fill=(10, 12, 25), outline=(70, 80, 140))
    order = {"large piece": 0, "detail": 1, "edge detail": 2, "outline": 3, "corner": 4}
    for p in sorted(data["placements"], key=lambda p: order.get(p["role"], 9)):
        c = COLORS.get(p["role"], (255, 0, 255))
        d.rectangle(box(p["box"]), fill=c + (170,), outline=c + (255,))
    for r in data["hazards"]:
        b = box(r)
        d.polygon([(b[0], b[3]), (b[2], b[3]), ((b[0] + b[2]) / 2, b[1])], fill=(230, 40, 40))
    for r in data["gameplay"]:
        d.ellipse(box(r), fill=(255, 220, 0), outline=(255, 255, 255))
    img.save(dst)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
