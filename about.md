# Deco AI

Teach it your decoration style once, then decorate whole sections of a level in one click. Everything runs on your own computer: no API key, no internet.

## Two ways to teach it

**Palette (easiest):** drop a handful of deco pieces anywhere in the editor, loose and unarranged. Select them and press **Palette**. The mod sorts each piece by its shape:

- **Long, thin pieces** become outlines. They're tiled along every exposed edge, rotated to face outward, and the mod picks whichever piece length fits each stretch of edge most evenly.
- **Medium pieces** go on outer corners. Draw them for the top-left corner and they're rotated for the other three.
- **Small pieces** are sprinkled across block faces, mostly near the surface.
- **Big pieces** become set pieces standing on longer platforms, spaced apart.

**Learn (most control):** decorate a block by hand exactly how you want it, select the block *and* its deco, and press **Learn**. It copies that arrangement onto every block.

You can use both together. Learned styles take priority, and the palette fills whatever they don't cover.

## Decorating

1. Select the plain blocks you want decorated and press **Deco!**
2. Not happy? Press **Undo** to remove the last batch. **Forget** clears everything it has learned.

## How it decides

- It checks which sides of each block are open air (using the whole level, not only your selection), so top outlines only go on surfaces you can see and corner pieces only go on real corners.
- With several samples learned, it picks a random one per edge, which keeps long sections from looking copy-pasted.
- Colours, layers, Z order, scale, rotation and groups of your example pieces are all kept.
- The **Density** setting controls how often an edge is decorated.
