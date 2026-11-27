# Deco AI (Geode mod)

A Geometry Dash editor mod: describe a vibe and Claude plans the deco, then the mod places it.

- `src/Plan.*`: deco and layout plans, presets, parsing Claude's JSON
- `src/Claude.*`: Anthropic API call and the instructions sent to Claude
- `src/Engine.*`: level scan, deco engine (fill/outline/glow overlays, background, particles, pulses, motifs), layout builder, undo
- `src/Catalog.*`: finds the colourable square object at runtime; exports the full object list
- `src/UI.cpp`: editor button and the Deco AI window

Builds with GitHub Actions (`.github/workflows/multi-platform.yml`, Windows) into a `.geode` file.
Settings: Anthropic API key, model (default `claude-sonnet-5-5`), max objects, first colour channel, editor layer.
