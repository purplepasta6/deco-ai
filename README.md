# Deco AI — Geode mod

An offline, example-based decoration generator for the Geometry Dash editor. No API key, no internet, no model download.

## Build it without installing anything (easiest)

1. Create a new GitHub repository and upload everything in this folder (keep the `.github` folder).
2. Open the **Actions** tab. The "Build Geode Mod" workflow runs automatically on every push (or press **Run workflow**).
3. When it finishes, download **Build Output** from the run page. Inside is `purplepasta.deco-ai.geode`.
4. Copy that file into your Geode mods folder:
   - Windows (Steam): `...\steamapps\common\Geometry Dash\geode\mods\`
   - Or in-game: Geode menu → settings icon → "Open mods folder"
5. Restart Geometry Dash.

## Build locally (if you already have the Geode toolchain)

```
geode build
```

Requires the Geode CLI and SDK (`geode sdk install`), CMake 3.21+, and a C++20 compiler (Visual Studio 2022 on Windows).

## Targets

Geode 5.x, Geometry Dash 2.2081. If you're on a different GD version, change the `gd` and `geode` fields in `mod.json`.

## Files

- `src/main.cpp` — the whole mod: learning, edge detection, placement, undo
- `mod.json` — metadata and settings (density, max objects per run)
- `about.md` — in-game description and usage guide
