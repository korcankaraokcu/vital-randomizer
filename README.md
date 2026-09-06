# Vital Randomizer

A VST3 instrument that hosts Vital and generates new presets into it while you
play. Your MIDI passes straight through, so the keyboard plays the hosted synth
exactly as it would if Vital were on the track directly, and a new patch arrives
in one keystroke.

Pick a style, move the sliders, press ROLL. Each style starts from a patch
designed to be that style, the sliders move it, and the wavetables are written
from scratch every time. Every candidate is then rendered and
checked before you hear it: silent patches, ones that clip, ones sounding a
different note than the key you pressed, and basses that drone when they should
be short all get thrown away and rolled again.

## Requirements

- **Vital**, any tier. The plugin finds your installed copy and hosts it.
- A host that loads VST3.

A fresh Vital install is enough. Every patch is built from scratch: the
wavetables are synthesised per roll and the parameters come from designed
starting points, so what it makes is yours.

## Install

Grab the latest build from the [releases page](../../releases) and put
`Vital Randomizer.vst3` in your VST3 folder:

| | |
|---|---|
| Windows | `%LOCALAPPDATA%\Programs\Common\VST3` |
| macOS | `~/Library/Audio/Plug-Ins/VST3` |
| Linux | `~/.vst3` |

Then rescan plugins in your DAW.

## Using it

- **ROLL** makes a fresh patch from the style and the sliders.
- **BRIGHT / MOVE / DIRT / SPACE** set the character. **COMPLEX** sets how much
  of the synth a patch is allowed to use, from one oscillator through one filter
  to three oscillators, both filters and a wall of modulation. Each style starts
  where that style usually sits and individual rolls vary around it, so a batch
  holds both sparse patches and busy ones.
- **VARY** drifts the current patch instead of replacing it, with the small
  slider beside it setting how far.
- **OSC / FILT / ENV / LFO / FX / MOD** lock a section so rolling leaves it be.
- **&lt; &gt;** walk the candidate history and **KEEP** stars one. Starred
  patches sit in the KEPT row: click one to load it, right click to remove it.
  A star holds the patch as it stands, so anything you changed by hand in
  Vital's own GUI is kept too.
- **EXPORT** writes the current patch to a `.vital` file.
- **...** locates Vital if it moved.

Everything saves with the DAW project, including the patch that was playing.

## Building

Needs CMake 3.22 or newer and a C++17 compiler.

```
git clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE deps/JUCE
curl -L --create-dirs -o deps/nlohmann/json.hpp \
  https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp

cmake -S . -B build -A x64
cmake --build build --config Release --target VitalRandomizer_VST3
cmake --build build --config Release --target install_user_vst3
```

`vrtest` generates a batch and scores it by rendering through real Vital,
`vrscan` loads a VST3 the way a DAW does, and `tools/measure.py` checks the
output from outside the plugin. How the generator works, and what the
measurements behind it say, is in [docs/notes.md](docs/notes.md). Planned work is
in [ROADMAP.md](ROADMAP.md).

## Licence

GPLv3. Vital is GPLv3 and this hosts its binary through the standard plugin
interface, the same way a DAW does, rather than linking against or modifying it.
Vital is not bundled, the plugin finds the copy you already have.
