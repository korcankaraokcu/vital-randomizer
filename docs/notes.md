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

A patch is built in layers, each one narrowing what the next can do. Every roll
starts from the archetype for its style, a designed patch that already sounds
like that style. The character axes then move the parameters they own inside
declared ranges, a parameter with no range stays where the archetype put it, and
the audition decides whether the result is worth hearing.

That layering is the whole design. Vital exposes 450 numbers, and the useful
region of that space is vanishingly small, so each layer's job is to hand the
next one a smaller and better region to work in.

An archetype is coherent by construction, which is what makes this work: a bass
is a bass because it was designed as one. That coherence is measurable, and it
is why the usable rate sits in the high eighties.

**Wavetables are synthesised.** A Vital wave keyframe is 2048 little-endian
floats in base64 and every other component in the format is plain parameters, so
the tables are written from scratch: harmonic spectra built from shape families
that suit the style, morphing across up to eight keyframes, with Vital's own
algorithmic modifiers stacked on top. Samples work the same way, drawing on
Vital's own noise.

Writing them per roll is where the variety comes from, since the harmonic
content is fresh every time, and it keeps a generated patch free of licensed
content and therefore yours to share.

**What each style favours is written down.** Each archetype names the settings
and routings that make that style what it is, and the style's signature routing
is placed first, so a bass always gets the decaying envelope on its filter that
makes it plucky while a sequence gets LFOs on pitch and level.

The numbers came from measuring a real library once, and one lesson from that
work is worth keeping, because it took a wrong turn first. A statistic has to be
conditioned on the parameter actually being used. Across all bass presets
`env_2`'s sustain is bimodal, 18 at full and 16 at zero, so its median of 0.22
describes nothing real and sampling it hands out a sustained envelope two times
in five. Among the presets that route `env_2` at a filter the median is 0.12 and
not one of them sustains, because an envelope nobody uses just sits at its
default. Only the conditional figure meant anything, and that is the one the
archetypes were built from.

**A note has to be the note that was pressed.** The audition finds the sounding
pitch by autocorrelation over the loudest part of the note, and Bass, Keys,
Lead, Sequence and Pad have to land within a fraction of a semitone of an octave
of what was played. An octave is the same note; a fifth is not. Two things were
breaking it: partials detuned off the harmonic series, which is what a bell is
made of and also what stops a note being a note, and patches so thin that the
fundamental vanished and the ear latched onto a high harmonic instead. Effects,
experiments and percussion are exempt, since being unplaceable is the point
there.

**Note shape is the one place a musical judgement overrides the measurements.**
A bass preset usually shows a full sustain because the player is the one making
the notes short, so following that literally gives a bass that drones when you
hold a key. Each style gets a lean, and the struck ones get hard bands as
well, because leaning is not enough where the distribution is top heavy: bass
sustain runs p10 0.29 and median 1.00, so everything above halfway is a full
sustain.

The length of a struck note comes from its decay, not its release. Sustain says
whether a note keeps going, decay says how long it takes to get there, and that
is what a listener hears as the body. Measured against a held note a decay of
1.00 is gone in half a second and 1.25 lasts about 1.1, while hand-made basses
sit at 0.85 to 1.12 and so arrive clipped short. Release only stretches
what happens after the key comes up, which is a different thing entirely.

None of that is trusted to the parameters, though. Whether a note keeps going is
measured from the audio, comparing late energy against early energy with the key
still down, because a bass whose sustain is zero still drones if an LFO is
pushing an oscillator's level back up. One patch doing exactly that measured a
rising envelope two seconds into a held note.

**Structural identity is fixed per style.** A few parameters decide what
instrument a patch is rather than how it is voiced, and left to the jitter they
drift like anything else. A lead that came out monophonic and a pad that lost
its unison are not variations on the style, so polyphony, unison voices and
oscillator transpose are set by the archetype rather than rolled.

Every generated patch gets its four macros wired and named. Macros are the most
used modulation source in hand-made presets, ahead of the LFOs, and they are the
only part of a patch you can automate from the host.

## Using it

Drop **Vital Randomizer** on a MIDI track. It finds Vital on its own, and a
fresh install is all it needs. Then:

- **ROLL** makes a fresh patch from the style and the sliders.
- **VARY** drifts the current patch instead of replacing it, with the small
  slider beside it setting how far. This is where usable patches actually come
  from.
- **BRIGHT / MOVE / DIRT / SPACE** set the character, and **COMPLEX** sets how
  much of the synth a patch may use.
- **OSC / FILT / ENV / LFO / FX / MOD** lock a section so rolling leaves it be.
- **< >** walk the candidate history and **KEEP** stars one so rolling cannot
  lose it. Starred patches sit in the KEPT row: click to load, right click to
  remove.
- **EXPORT** writes the current patch to a `.vital` file.
- **...** locates Vital if it moved.

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
`--complexity=<0..1>` to hold every roll at one setting or `--complexity=ramp`
to walk the whole range across each style's presets, and `--diag=<file>` to
measure one patch repeatedly.

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

**What each style is made of.** Measured once across 299 hand-made presets,
which is where the archetypes started rather than anything read at run time:

| | signature routing | `env_2` sustain | mod depth | poly | transpose | unison | attack |
|---|---|---|---|---|---|---|---|
| Bass | `env_2 -> filter_1_cutoff` | 0.12 | 0.38 | 1 | -12 | 1 | 0.15 |
| Lead | `env_2 -> filter_1_cutoff` | 0.00 | 0.30 | 8 | 0 | 3 | 0.15 |
| Pad | `env_2 -> filter_1_cutoff` | 0.44 | 0.26 | 8 | 0 | 6.5 | 0.57 |
| Keys | `lfo_1 -> filter_1_cutoff` | 0.00 | 0.27 | 8 | 0 | 3 | 0.15 |
| Sequence | `lfo_1 -> osc_1_level` | 1.00 | 0.39 | 8 | 0 | 2 | 0.15 |
| Percussion | `velocity -> osc_1_level` | - | 0.23 | 6 | 0 | 3 | 0.15 |

Effect usage came from the same pass, and it is not incidental: bass runs
distortion in 73% of presets but reverb in 41%, while a pad is reverb in 97% and
chorus in 79%. So is how a style is played, with 39% of basses using legato
against 2% of pads.

**Style fidelity**, hand-made against generated:

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
patch can easily end up with nothing wired but its macros, and four macros
sitting at their default position do not move at all, so the patch is static.
Each archetype now names the routings its style depends on, and MOVE and COMPLEX
add more on top, instead of starting from zero.

**Style defaults.** Picking a style sets the sliders to settings tuned for it.
Everything at the middle is the least characterful thing the generator can
produce, because nothing is being asked of it.

**Axes**, tested by building the same patch from the same seed twice, once with
the slider low and once high, so everything but the slider cancels out:

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
thread does not have it. Generating and auditioning patches happen on a worker. The audio thread takes a try-lock, never a lock: a patch
load holds it for a couple of hundred milliseconds, and going quiet for a moment
beats stalling the whole DAW.

**The next candidate is ready before it is asked for.** Screening is most of
what a roll costs: every candidate is rendered, measured, and often rendered
again to confirm its level, and filling a batch of 48 takes about 69 rolls. None
of that has to happen after the button is pressed, because the recipe for the
next roll is known the moment the last one lands. A roll now finishes by starting
the next one on the worker, so the wait falls while the user is playing rather
than after they ask.

It carries a signature of the style, sliders and locks it was built from, and is
thrown away the moment any of those move. A prepared patch that no longer matches
the sliders is worse than no prepared patch at all.

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

## How much of the synth a patch uses

The complaint was that patches sounded boring, and the guess was that they
authored too few parameters. Measuring said otherwise. Against a hand-made
library:

| | median | range |
|---|---|---|
| authored parameters, generated | 96 | 89-111 |
| authored parameters, hand-made | 75 | 5-231 |
| routings, generated | 10 | 7-14 |
| routings, hand-made | 9 | 0-64 |

The generator was authoring *more* than a person does. What it never did was
vary. Every roll used about the same amount of Vital, and a batch of patches
that are all equally busy reads as one patch heard eight times. Filter 2 made
the point: on in 42% of hand-made presets, and in 0 of 48 generated ones.

COMPLEX decides how much of the synth is in play. It switches the third
oscillator, the noise layer, the second filter and the modulation effects on and
off, and it scales both the jitter and the number of extra routings. Each style
starts where that style usually sits, from 0.40 for percussion to 0.90 for
experiment, and every roll wobbles around its setting by a squared offset so
most land near it and the occasional one goes a long way.

Two things had to be held back. Noise is flat all the way up, so even a little
of it drags a patch toward its brightness ceiling, and a phaser or flanger rings
on after the key is released. Both are excluded for bass and percussion, which
have to stop dead.

## What the third oscillator is allowed to be

It is a layer, not a harmony part. It sits at unison about 40% of the time and
earns its place through its own wavetable, voice count and detune, and otherwise
moves an octave. Bass gets unison or a sub, never the octave up.

Fixed intervals are the thing it will not roll, and the reason is musical before
it is technical. A patch that adds a fifth to every note is choosing harmony on
the player's behalf, and a major third stack is plainly wrong over half of what
somebody plays. The octave also happens to be the only exact interval: an equal
tempered fifth is 1.4983 rather than 1.5, so root and fifth never share a period
and the sound has no single pitch to hear. With a fifth allowed, sequences came
out 4.77 semitones off the key that was pressed.

Vital has its own name for the idea in the unison stack styles. The indices were
measured by rendering middle C through each one with detune off and reading the
partials back, rather than assumed from the enum:

| index | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| | unison | drop 12 | drop 24 | octave | 2x oct | power | 2x power | major | minor | harm | odd harm |

Five through eight add a real fifth or third. Pitched styles get the octave pair
and a bass gets the drop. SFX and Experiment have no pitch rule, because they are
sounds rather than notes, so there any of the eleven is fair game.

After all of it, at the per-style defaults:

```
params med 107 (84-130)   routings med 12 (7-23)   filter 2 on in 31 of 48
84% of candidates usable
```

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
```

## Licensing

Vital is GPLv3, and this hosts its binary through the standard plugin interface
exactly as a DAW does rather than linking or modifying it. Vital is not bundled;
the plugin finds the copy you have installed. JUCE and the VST3 SDK are both
GPLv3-or-commercial, so this is released under GPLv3.

A generated patch is original work: the wavetables are synthesised per roll and
the parameters come from the designed starting points in this repository, so what
it produces is yours to do what you like with.
