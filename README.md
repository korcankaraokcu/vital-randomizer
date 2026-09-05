# Vital Randomizer

A VST3 instrument that hosts Vital and generates new presets into it while you
play. Your MIDI passes straight through, so the keyboard plays the hosted synth
exactly as it would if Vital were on the track directly, and rolling a new patch
does not involve a file dialog or the preset browser.

Pick a style, move the four character sliders, press ROLL. Patches are built by
splicing presets out of your own library and synthesising fresh wavetables, and
every candidate is rendered and checked before you hear it. Silent patches, ones
that clip, ones sounding a different note than the key you pressed, and basses
that drone when they should be short all get thrown away and rolled again.

## Requirements

- **Vital**, any tier. The plugin finds your installed copy and hosts it.
- **A preset library**, normally `Documents/Vital`. The generator learns what
  each style is made of by reading it, so this is not optional. Vital's own
  factory presets are a thin starting point and more presets give noticeably
  better results.
- A host that loads VST3.

Nothing from your library is redistributed. Wavetables are written from scratch
rather than copied out of presets, so a generated patch carries no data from
anybody's pack.

## Install

Grab the latest build from the [releases page](../../releases) and put
`Vital Randomizer.vst3` in your VST3 folder:

| | |
|---|---|
| Windows | `%LOCALAPPDATA%\Programs\Common\VST3` |
| macOS | `~/Library/Audio/Plug-Ins/VST3` |
| Linux | `~/.vst3` |

Rescan plugins in your DAW. On first run it reads your preset library, which
takes a few seconds and is cached afterwards.

## Using it

- **ROLL** makes a fresh patch from the style and the four sliders.
- **VARY** drifts the current patch instead of replacing it, with the small
  slider beside it setting how far.
- **OSC / FILT / ENV / LFO / FX / MOD** lock a section so rolling leaves it be.
- **&lt; &gt;** walk the candidate history, **KEEP** stars one, **EXPORT** writes
  a `.vital` file.
- **...** locates Vital or rescans your library if either moved.

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
