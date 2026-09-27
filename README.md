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

- **ROLL** makes a new patch from the style and the sliders.
- **BRIGHT / MOVE / DIRT / SPACE** set the character, and **COMPLEX** sets how
  much of the synth a patch uses.
- **VARY** nudges the current patch instead of replacing it. The bar next to it
  sets how far.
- **OSC / FILT / ENV / LFO / FX / MOD** lock a section so rolling leaves it
  alone.
- **&lt; &gt;** walk the history and **KEEP** stars a patch for the KEPT row.
- **SCALES** (Sequence) picks which scales a riff can use.
- **DRUMS** (Percussion) picks which drums a roll can build: kick, snare, clap,
  hats, crash, ride, tom, timpani, cowbell, rim and shaker. Tuned drums follow
  the keyboard.
- **EXPORT** saves the current patch as a `.vital` file.
- The options button at the top right (**...**) has a **Locate Vital.vst3**
  action that can relocate Vital if it moved. It also shows which version
  you're running.

Everything saves with the DAW project.

## CLI (vrgen)

`vrgen.exe` makes presets in batches. It uses the same generator and checks as
the plugin, so every preset is rendered through Vital and levelled to -16 LUFS.
Vital needs to be installed.

```
vrgen roll                                    6 of every style
vrgen roll --styles=Lead,Keys --count=20      20 leads and 20 keys
vrgen roll --styles=Percussion --drums=Kick,Snare --count=12
vrgen roll --styles=Sequence --scales=hijaz,dorian
vrgen roll --bright=0.8 --dirt=0.2-0.6        one slider held, one drawn per preset
vrgen roll --complexity=ramp                  low to high across each style's presets
vrgen roll --seed=1234                        the same batch again
vrgen roll --into-vital                       straight into Vital's preset browser
vrgen vary MyLead.vital --count=8 --depth=0.3 neighbours of a preset
vrgen measure "C:\Presets\Mine"              the loudness of any folder of presets
vrgen --list                                  styles, drum kinds and scales
vrgen --help                                  everything else
```

A slider takes a value, a range drawn per preset, or `ramp`, and each style's
own setting when left out. Files go to `vrgen presets` in the current folder
unless `--out=<folder>` or `--into-vital` says otherwise, named by
`--name="{style}_{n}"` with `{kind}`, `{scale}` and `{seed}` also available.

Every batch prints its seed when it starts. Run the same command with
`--seed=<that number>` to get the same batch again. This works nearly always,
but not guaranteed: every candidate is rendered to be checked, Vital's renders
vary very slightly, and one right on the edge of a rule can pass on one run and
be rolled again on the next.

## Building

Needs CMake 3.22 or newer and a C++17 compiler.

```
git clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE deps/JUCE
curl -L --create-dirs -o deps/nlohmann/json.hpp \
  https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp

cmake -S . -B build -A x64
cmake --build build --config Release --target VitalRandomizer_VST3
cmake --build build --config Release --target install_user_vst3
cmake --build build --config Release --target vrgen
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
