# Vital Randomizer, engineering notes

A VST3 instrument that hosts Vital, passes your MIDI straight through so the
keyboard still plays it, and generates new patches into it while you hold a
chord. No file dialogs, no clicking around in the preset browser.

## How it works

Vital's plugin state chunk turns out to be the same JSON a `.vital` file
contains. `getStateInformation` runs the same `LoadSave::stateToJson` that
writes presets to disk, so handing Vital a patch is a matter of writing JSON
into its state rather than driving its GUI. That is the only door available:
there is no "open this file" call in a plugin interface, and Vital does not
expose its presets as VST programs either.

The chunk is wrapped in three layers, all handled in `src/VitalState.cpp`:

```
VC2! + length
  -> XML <VST3PluginState><IComponent> ... </IComponent>
    -> JUCE's base64 variant
      -> a VstW / FXB block, 176 bytes of header
        -> the preset JSON, then JUCE's own private data
```

Rather than build those wrappers from scratch, the plugin takes the live
instance's own state as a template and swaps only the JSON body, so the header
and version always come from the Vital the user actually has installed.

Patches are not built by drawing 450 random numbers, which produces silence or
noise essentially every time. Each section (oscillators, filter, envelopes,
LFOs, effects, modulation matrix) is spliced whole from a donor preset in your
own installed library, keeping the correlations that make a patch sound
deliberate. The character axes then re-sample their own parameters from the
distribution that style actually shows, so a slider never produces a value no
real preset has used.

**Wavetables are synthesised, never borrowed.** A Vital wave keyframe is 2048
little-endian floats in base64 and every other component in the format is plain
parameters, so the tables are written from scratch: harmonic spectra built from
shape families that suit the style, morphing across up to eight keyframes, with
Vital's own algorithmic modifiers stacked on top. Splicing a donor's table was
easier and carried the wavetable data of whatever pack the donor came from,
which is licensed content and not ours to put in a generated patch. The same
goes for samples, where names like "River" and "Jack Hammer" across a library
make the point plainly, so a generated patch uses Vital's own noise sample.

It helped the sound as much as the licensing. A fixed pool of donor tables meant
the same handful of timbres kept turning up; synthesising per roll is where the
variety comes from.

**What each style favours is learned, not written down.** Per style the model
keeps every parameter's value distribution, which modulation routings the style
uses and how deep it runs them, which effects it switches on, which filter
circuit, and how it is played. The style's signature routing is placed first, so
a bass always gets the decaying envelope on its filter that makes it plucky
while a sequence gets LFOs on pitch and level.

One subtlety worth writing down, because it took a wrong turn first. Envelope
shape is collected only from presets that actually route that envelope. Across
all bass presets `env_2`'s sustain is bimodal, 18 at full and 16 at zero, so its
median of 0.22 describes nothing real and sampling it hands out a sustained
envelope two times in five. Among the presets that route `env_2` at a filter the
median is 0.12 and not one of them sustains, because an envelope nobody uses
just sits at its default. Since the generator always wires this envelope, the
conditional distribution is the only one that means anything.

**A note has to be the note that was pressed.** The audition finds the sounding
pitch by autocorrelation over the loudest part of the note, and Bass, Keys,
Lead, Sequence and Pad have to land within a fraction of a semitone of an octave
of what was played. An octave is the same note; a fifth is not. Two things were
breaking it: partials detuned off the harmonic series, which is what a bell is
made of and also what stops a note being a note, and patches so thin that the
fundamental vanished and the ear latched onto a high harmonic instead. Effects,
experiments and percussion are exempt, since being unplaceable is the point
there.

**Note shape is the one place a musical judgement overrides the library.** A
bass preset usually shows a full sustain because the player is the one making
the notes short, so reading the corpus literally gives a bass that drones when
you hold a key. Each style gets a lean, and the struck ones get hard bands as
well, because leaning is not enough where the distribution is top heavy: bass
sustain runs p10 0.29 and median 1.00, so everything above halfway is a full
sustain.

The length of a struck note comes from its decay, not its release. Sustain says
whether a note keeps going, decay says how long it takes to get there, and that
is what a listener hears as the body. Measured against a held note a decay of
1.00 is gone in half a second and 1.25 lasts about 1.1, while the library's
basses sit at 0.85 to 1.12 and so arrive clipped short. Release only stretches
what happens after the key comes up, which is a different thing entirely.

None of that is trusted to the parameters, though. Whether a note keeps going is
measured from the audio, comparing late energy against early energy with the key
still down, because a bass whose sustain is zero still drones if an LFO is
pushing an oscillator's level back up. One patch doing exactly that measured a
rising envelope two seconds into a held note.

**Structural identity is put back per style.** A few parameters decide what
instrument a patch is rather than how it is voiced, and they drift during
splicing like anything else. A lead that came out monophonic and a pad that lost
its unison are not variations on the style. Polyphony, unison voices and
oscillator transpose are drawn from the distribution the style actually uses,
which keeps intentional spread like a second oscillator sitting a fifth up.

Every generated patch gets its four macros wired and named. Macros are the most
used modulation source in a real preset library, ahead of the LFOs, and they are
the only part of a patch you can automate from the host.

## Using it

Drop **Vital Randomizer** on a MIDI track. On first run it finds Vital, reads
your preset library and learns from it, which takes a few seconds and is cached
afterwards. Then:

- **ROLL** makes a fresh patch from the style and the four character sliders.
- **VARY** drifts the current patch instead of replacing it, with the small
  slider beside it setting how far. This is where usable patches actually come
  from.
- **OSC / FILT / ENV / LFO / FX / MOD** lock a section so rolling leaves it be.
- **< >** walk the candidate history, **KEEP** stars one so rolling cannot lose
  it, **EXPORT** writes a `.vital` file.
- **...** locates Vital or rescans your library if either moved.

Everything saves with the DAW project, including the patch that was playing and
your starred keepers.

## Building

Needs CMake 3.22+ and a C++17 compiler. JUCE and nlohmann/json live under
`deps/` and are fetched separately:

```
git clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE deps/JUCE
curl -L -o deps/nlohmann/json.hpp \
  https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp

cmake -S . -B build -A x64
cmake --build build --config Release --target VitalRandomizer_VST3
cmake --build build --config Release --target install_user_vst3
```

`install_user_vst3` copies to the per-user VST3 folder. JUCE's own
`COPY_PLUGIN_AFTER_BUILD` targets the machine-wide folder under Program Files
and needs an elevated build, which is why it is off.

Targets:

| target | what it is |
|---|---|
| `VitalRandomizer_VST3` | the plugin |
| `VitalRandomizer_Standalone` | the same thing as an app, useful for testing |
| `vrtest` | rolls patches, renders them through real Vital, scores them |
| `vrscan` | loads a VST3 the way a DAW does and plays a note through it |

`vrtest` also takes `--save=<dir>` to write a batch out for listening,
`--corpus=N` to measure hand-made presets from your library with the same code
the plugin uses (which is where the loudness targets come from), and
`--diag=<file>` to measure one patch repeatedly.

## Where it stands

Measured, not estimated.

**Loads and plays.** `vrscan` instantiates the plugin through the same calls a
DAW makes, sends it a note, and gets audio back from hosted Vital.

```
found: Vital Randomizer  (instrument=yes, version 0.1.0)
instantiated ok
  inputs=0 outputs=2 acceptsMidi=yes hasEditor=yes
  note through the plugin: peak=0.3606 rms=0.10238   SOUND
PASS
```

**Patch quality.** `vrtest` scored 29 of 30 usable across Bass, Lead, Pad, Keys
and Sequence at neutral slider settings, with macros wired on every one.

**Timbral variety**, as mean pairwise spectral distance within a style:

| | distance |
|---|---|
| generated Bass | **1.21** |
| hand-made Bass from the library | 1.15 |

Generated patches differ from each other slightly more than hand-written ones
do, which is the point: unique without being random.

**What each style is made of**, as learned from a 299 preset library:

| | signature routing | `env_2` sustain | mod depth | poly | transpose | unison | attack |
|---|---|---|---|---|---|---|---|
| Bass | `env_2 -> filter_1_cutoff` | 0.12 | 0.38 | 1 | -12 | 1 | 0.15 |
| Lead | `env_2 -> filter_1_cutoff` | 0.00 | 0.30 | 8 | 0 | 3 | 0.15 |
| Pad | `env_2 -> filter_1_cutoff` | 0.44 | 0.26 | 8 | 0 | 6.5 | 0.57 |
| Keys | `lfo_1 -> filter_1_cutoff` | 0.00 | 0.27 | 8 | 0 | 3 | 0.15 |
| Sequence | `lfo_1 -> osc_1_level` | 1.00 | 0.39 | 8 | 0 | 2 | 0.15 |
| Percussion | `velocity -> osc_1_level` | - | 0.23 | 6 | 0 | 3 | 0.15 |

Effect usage is learned the same way, and it is not incidental: bass runs
distortion in 73% of presets but reverb in 41%, while a pad is reverb in 97% and
chorus in 79%. So is how a style is played, with 39% of basses using legato
against 2% of pads.

**Style fidelity**, corpus against generated:

| | `env_2` sustain | filter envelope wired | polyphony |
|---|---|---|---|
| Bass | 0.12 / **0.00** | 36% / **100%** | 1 / 1 |
| Lead | 0.00 / 0.00 | 26% / 100% | 8 / 8 |
| Pad | 0.44 / 0.44 | 22% / 100% | 8 / 8 |

**Loudness**, verified in a second host that does not share any of the plugin's
measurement code:

| | before | after |
|---|---|---|
| level spread across a batch | 23 dB | **6.7 dB** |
| median level | wandered | 0.128 (hand-made presets: 0.135) |
| patches peaking over 1.0 | common | 1 of 29, and that one is caught by the limiter |
| modulation routings, neutral sliders | 4 to 5 | **9 to 34, median 12** |
| channel balance | patches audible on one side only | within about 2 dB |

The routing count is the other half of why early patches sounded like noise. A
spliced donor can arrive with nothing wired but its macros, and four macros
sitting at their default position do not move at all, so the patch is static.
Movement now scales around what presets in that style actually use, which the
style model learns per style, instead of starting from zero.

**Style defaults.** Picking a style sets the four sliders to settings tuned for
it. All four at the middle is the least characterful thing the generator can
produce, because nothing is being asked of it.

**Axes**, tested by building the same patch from the same donors twice, once
with the slider low and once high, so donor variance cancels out:

| axis | agreement | |
|---|---|---|
| dirt | 90% | works |
| space | 80% | works |
| move | 80% | works |
| bright | 70% | weakest, see below |

`bright` is the hard one. Filter cutoff is both the obvious brightness control
and the most modulated destination in a real library, so biasing it statically
gets overridden by whatever envelope is already driving it. Adding the EQ as a
lever tripled the effect size, from a 76 Hz to a 219 Hz median move, but
consistency stayed at 70%. Biasing the modulation depths that target cutoff is
the next thing to try.

**Timing**, on a real instance:

| operation | cost |
|---|---|
| creating and warming the two Vital instances | ~1 s, once at load |
| a patch load | 30 to 500 ms, median ~250 ms |
| auditioning a candidate offline | ~400 ms |
| a full roll, screened and level matched | 0.6 to 4 s, median ~1.5 s |

A roll is slower than it used to be because most of it is now spent auditioning:
up to three level passes on a candidate, and up to five candidates if the first
ones are rejected. That is the trade. Before, a roll took a fifth of a second
and a good fraction of what came out was unusable.

The first state load into a fresh Vital instance is far slower than the rest
because Vital rebuilds its wavetables, so the plugin pays that once when it
starts rather than letting it surface as a stall on your first roll.

Two Vital instances cost about 500 MB of RAM between them. The second one is
optional in the sense that the plugin still rolls without it, just unscreened
and unmatched.

## Design notes

**Every key Vital reads has to be present.** Its loaders index straight into the
json rather than checking a key exists, so one missing field fails the entire
preset with "preset is corrupted" and the instance quietly keeps playing the
previous patch. That silent fallback is worse than the error, because the
generator then measures the old sound and decides it is fine. The first load of
each candidate is verified by reading the state back and looking for a marker,
so a rejected patch becomes a plain reroll.

**Stereo.** Oscillator pans are forced to centre and stereo spread is left wide,
because hand-made presets leave the pans at zero 96 to 99 percent of the time
and run spread at full without exception. A randomised pan does not read as
width, it reads as a broken channel. The audition rejects anything leaning more
than 6 dB to one side.

**The hosted Vital needs a playhead.** Every real host gives a plugin one, and
without it Vital has no tempo to sync to, so anything tempo synced runs at a
rate it never would in a DAW. The same patch measured inside the plugin and
played in Vital itself produced entirely different envelopes, one decaying away
in a third of a second and the other holding for a full one. Every level the
plugin measured was of a sound the user was never going to hear, which is how a
batch that measured two dB apart internally arrived twenty-five dB apart on
disk. A static 120 BPM playhead fixes it.

**Vital's first render after a state load is not the settled patch.** It is
rendered once and thrown away before anything is measured. On top of that, a
level is only believed when two renders agree: a delay feeding back or a filter
near self oscillation climbs each time the note is played, and measuring once
catches those at their quietest.

**Threading.** Vital is created on the message thread, because instantiating a
VST3 on Windows needs COM initialised on the calling thread and a bare worker
thread does not have it. Reading the preset library and generating patches
happen on a worker. The audio thread takes a try-lock, never a lock: a patch
load holds it for a couple of hundred milliseconds, and going quiet for a moment
beats stalling the whole DAW.

**History costs nothing.** A patch is around 856 KB and almost all of that is
wavetable and sample data, but its 451 knobs are only 22 KB. A roll is fully
determined by its recipe (style, sliders, amount, locks, seed) and the generator
splits its random streams so that recipe reproduces the same patch every time,
so history stores recipes at about a hundred bytes each. A thousand-deep history
costs less than a tenth of a megabyte. Keepers are different, since a hand edit
in Vital's own GUI cannot be regenerated from a recipe, so those hold the real
JSON. Nothing touches disk unless you export.

**Offline audition.** A second Vital instance runs silently alongside the one
you hear. Every candidate is loaded into it and rendered before it reaches the
live instance, which is what makes a roll reliably worth listening to: silent
patches, patches that are all click and no body, and patches whose crest factor
means no level works are rejected and rolled again, up to five times.

**Loudness.** The same offline render matches the level. Peak matching is not
enough on its own, since a compressed patch and a transient-heavy one at the
same peak are nowhere near the same loudness. What gets matched is the 90th
percentile of the short-term level, which is close to what the ear tracks, with
a peak ceiling on top.

Two details matter here and both were learned the hard way. The window has to be
long enough to catch a slow attack, because a pad that swells over a second and
a half measures quiet, gets boosted, and then arrives far too loud once it opens
up. And the correction is applied, re-measured and applied again, because the
volume calibration was swept on a clean patch and one with heavy distortion in
its chain does not respond to master volume the same way.

The level is written into the patch's own master volume rather than trimmed in
the plugin's output, so it travels with the preset when you export it.

## Checking it from outside

`tools/measure.py` loads Vital independently and measures the `.vital` files on
disk. That matters because the plugin measures every candidate through the Vital
it hosts, which is right for deciding what to keep and wrong for checking that
the decision was correct: a mistake in how the plugin renders is invisible to a
check that renders the same way.

It is how the playhead bug was found. The plugin's own numbers looked healthy
while the same patches, played in Vital itself, were nothing like them.

```
pip install numpy pedalboard mido
cd tools
python measure.py level      ../out
python measure.py pitch      ../out
python measure.py held       ../out/Bass_01.vital
python measure.py provenance ../out
```

## Licensing

Vital is GPLv3, and this hosts its binary through the standard plugin interface
exactly as a DAW does rather than linking or modifying it. Vital is not bundled;
the plugin finds the copy you have installed. JUCE and the VST3 SDK are both
GPLv3-or-commercial, so this is released under GPLv3.

The style model is built from your own preset library and stays on your machine.
Patches spliced from commercial packs carry those packs' wavetables, which is
fine for your own use and not something to redistribute.
