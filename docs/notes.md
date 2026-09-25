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

**Wavetables and samples are synthesised.** A Vital wave keyframe is 2048
little-endian floats in base64, a sample is mono sixteen bit audio in the same
place, and every other component in the format is plain parameters. So both are
written from scratch: the tables as harmonic spectra built from shape families
that suit the style, morphing across up to eight keyframes with Vital's own
algorithmic modifiers on top, and the sample from one of seven synthesis models.

Writing them per roll is where the variety comes from, since the content is
fresh every time, and it keeps a generated patch free of licensed content and
therefore yours to share.

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
- **VARY** moves the current patch instead of replacing it: the filters, the
  envelopes, the LFO rates and the modulation depths, by the depth set on the
  bar beside it, which the button also shows.
  This is where usable patches actually come from.
- **BRIGHT / MOVE / DIRT / SPACE** set the character, and **COMPLEX** sets how
  much of the synth a patch may use.
- **OSC / FILT / ENV / LFO / FX / MOD** lock a section so rolling leaves it be.
- **< >** walk the candidate history and **KEEP** stars one so rolling cannot
  lose it. Starred patches sit in the KEPT row: click to load, right click to
  remove.
- A sequence is one held note with an LFO doing the playing, so anything that
  fires on a key press fires once and shapes the first step only. Its sample
  layer is drawn from the models that keep sounding rather than the struck and
  plucked one-shots the played styles use, and an envelope reaching a level, a
  cutoff, a resonance or the drive gets a sustain floor. Both used to read as a
  loud pluck at the start and the layer then dropping out.
- **SCALES**, on the Sequence style only, lists the scales a riff may be built
  on. Each dropdown defaults to **Random scale**, meaning the whole table,
  which is a different thing from the two entries in the table that are called
  random and pick notes rather than scales. **+** adds another and **-**
  removes the last, and each roll draws one entry off the list. **random
  steps** and **random quarter tones** are the odd ones out and keep the
  behaviour the scales replaced: no ladder, no contour, each step its own
  draw. The semitone one is snapped by Vital's own quantiser with all twelve
  bits set, and the quarter tone one is placed by hand with the quantiser off,
  because that quantiser is a twelve bit mask and cannot express a half. A
  riff has to play enough of its scale to be recognisable as that scale, so
  the scale carries a floor: three notes of a three or four note chord, four
  of a five or six note scale, five of anything larger. The step count starts
  one above that floor, and a line that comes up short has its repeats spent
  on degrees it has not played yet. Naming a scale changes only the notes the
  sequence walks, so the same seed gives the same patch with a different set
  of intervals in it.
- **DRUMS**, on the Percussion style only, lists the kinds of drum a roll may
  build: kick, snare, clap, closed hat, open hat, crash, ride, tom, timpani,
  cowbell, rim and shaker. Each dropdown defaults to **Random drum**, meaning
  any of them, and **+** and **-** work as they do for scales. The tuned ones
  follow the keyboard.
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

**Axes**, tested by building the same patch from one seed twice, once with the
slider low and once high, each scored on the number that axis is meant to move.
BRIGHT 99%, SPACE 94%, MOVE 73%, DIRT 81%, over about 315 pairs each;
see "What a slider is worth".

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
determined by its recipe (style, sliders, amount, locks, seed, and the level the
screen settled on) and the generator splits its random streams so that recipe
reproduces the same patch every time, so history stores recipes at a few
hundred bytes each, a VARY's recipe pointing at the recipe it started from.
Keepers are different, since a hand edit
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

## Seven ways to make a sample

Every patch that used the sample slot used to get Vital's own white noise. A
layer whose job is to add character added the same flat hiss to all of them, and
because white noise is flat to the top of the range it cost far more brightness
than it bought: keys patches reading seven kilohertz turned into ordinary ones
the moment it was muted. The oscillators were never the problem.

Nothing here randomises audio, which would only be noise again. What is
randomised is the parameters of a synthesis model, so each result is a different
instance of a sound that exists. That is the same rule the wavetables follow and
the reason they came out musical.

| model | what it is | how it is made |
|---|---|---|
| Bed | dense texture, air or a room | noise through two resonant bands |
| Voice | nearer a vowel than a filter | noise through three formants |
| Grit | sparse crackle over near silence | scattered filtered ticks, 27 dB crest |
| Struck | a bell, a bar, a block | inharmonic partials decaying, high ones first |
| Pluck | a string | Karplus-Strong, harmonic where struck is not |
| Swell | arrives rather than decays | a rising envelope on an opening band |
| Thump | weight under a bass | a sine falling onto its root, as an 808 is built |

They are chosen to be far apart rather than to cover a range, because two
textures that could be mistaken for each other are worth less than one: dense
against sparse, harmonic against inharmonic, rising against decaying, texture
against voice.

Which ones a style may draw from is about what the layer is for. Bass and
percussion take only what stops on its own, since a bed never does. Keys, lead
and sequence take only what has a pitch, because a bed or a crackle under a
played line is a second sound behind the note rather than part of it. Pads never
take a struck body, which inside a chord is a second instrument. SFX and
Experiment take all seven, since being unplaceable is the point.

**The factory says how loud to run what it built.** Peak normalising leaves
sparse content far quieter on average than dense content: grit measures 13 dB
below a bed at the same peak, because most of it is silence. One level for all
of them makes half the models inaudible and the other half overbearing.

**Loops are eight seconds and nearly stationary.** Vital's own Waves preset
carries ten seconds whose spectrum moves under a tenth across the whole of it,
which is two tricks at once: nothing distinctive happens, and what does happen
takes a long time to come round. The first attempt was two seconds with a wander
in it, the opposite of both, and the wander was the worse half, since a feature
the ear can learn is one it can hear returning.

The length is free because the rate is not fixed. Vital reads the `sample_rate`
field and resamples, which was worth testing rather than assuming: the same audio
declared at half the rate comes back at the same pitch, while the same bytes
declared at full rate come back an octave up. A bed has nothing above ten
kilohertz in it, so it stores at 22050 and costs half. Eight seconds is sixteen
beats at the 120 BPM the hosted playhead reports, so it sits under the patch in
time with the LFOs rather than sliding against them. Vital's sampler has no sync
of its own, so at any other host tempo it drifts, and nothing in the format can
prevent that.

**The join is crossfaded on equal power, not equal amplitude.** Fading one out
linearly while the other fades in is right for two takes of the same thing and
wrong for two pieces of noise, which add as powers rather than amplitudes: half
and half leaves 0.707 of the level, so a bed dropped four decibels into a hole
every time round and climbed back out. Weighting by the square root keeps the
powers summing to one. Anything moving inside a loop is locked to a whole number
of cycles across it as well, or the loop point is a jump in the modulation that
no crossfade hides.

`vrtest --models=<dir>` writes three bare presets for each model, with the
oscillators, filters and effects taken out, because a layer is normally heard
under two oscillators and a reverb and that is the wrong way to judge one.

The sequence side has the same pair of demos, and they are the opposite way
round: everything else is left in and one thing is pinned, because a scale
played through nothing is a test tone and says nothing about whether it works
in a patch. `vrtest --scales=<dir>` writes one sequence per scale on a single
seed, so the only difference between two files is the set of intervals.
`vrtest --gestures=<dir>` writes one per gesture and six more with the phrases
left to fall where they will, and `--gesture-scale=<name>` says which scale
they walk. It defaults to minor pentatonic, which has no wrong note in it and
so gets out of the way, but the note floor bites hardest on the seven note
scales and that is where these are worth running.

## What makes a bass a bass

Not where its mean lands. The brightness check measured the spectral centroid,
weighted by magnitude, and a magnitude weighted mean counts bins rather than
energy: a few thousand quiet high bins outvote the handful of loud low ones. A
bass with 97% of its energy below 400 Hz was reporting a centroid of 2915 Hz and
being thrown out. The same patch weighted by energy reads 176 Hz.

That one line was rejecting most of what it rejected. Bass and keys accounted for
nearly every discarded candidate in a batch, all of it for brightness, and almost
none of them were too bright. They had a little sparkle on top of a sound that
was plainly a bass.

So the centroid is weighted by energy now, every style's ceiling is recalibrated
against the library measured the same way, and bass is judged on its balance
instead:

| | hand-made p10 | hand-made median | rule |
|---|---|---|---|
| energy below 400 Hz | 54% | 97% | at least 60% |

A patch may have as much high frequency detail as it likes as long as that detail
is quiet, which is what a listener means by a bass with air on it.

**One burst is still worth catching.** A filter LFO swinging wide open mid note
is loud and high at once, and it is the one thing in this family that does read
as broken. It is measured as the loudest moment above 2 kHz against the loudest
moment overall, so a decayed tail that is nothing but treble does not count
because it is inaudible, and the first 150 ms are skipped because a note-on
transient is broadband on purpose. Every bass anybody has called good measures
between 0 and 7 percent. The patch that prompted the rule measures 62.

The rejection rate tells the rest of the story: 48 of 49 rolls usable, against 48
of 69 before.

## Finding the note

Autocorrelation, over the loudest part of the sound, and two details in it were
wrong for years without showing.

The band ran from 40 Hz to 2 kHz. A sequence stepping several octaves up sounds
above that, so every step returned the band edge rather than a pitch: the lag sat
at exactly 22 samples, 2004.5 Hz, which happens to fall 0.252 semitones off the
semitone grid. That quarter tone was read as sequences being slightly out of
tune, and it was the detector hitting its own ceiling.

Raising the ceiling only moved the failure, because the reading followed it up to
the new edge. The actual fault is that autocorrelation starts at one and falls
away, and the largest value in the band kept landing on that opening slope rather
than on a real peak. Stepping past the descent before looking fixed both ends at
once: a bass that had come back at 5880 Hz returned to its 65 Hz fundamental, and
a sequence went from a quarter tone off the grid to a fiftieth.

Salience fell with it, from about 0.85 to about 0.45 on a sequence, because the
number is now the height of a real peak instead of the height of the slope. The
patches did not change. The Sequence threshold moved from 0.40 to 0.20 to match,
which is under the 0.26 that the worst patch anybody called good measures at.

## Counting what gets thrown away

A batch stops as soon as it has its six of a style, so how much it discards on
the way depends on how lucky the first few rolls were. Five identical runs threw
away between 19 and 61 candidates, which is a wider spread than most changes
worth testing, and it was very nearly used as evidence twice.

`vrtest --renders=N` sets how many renders each measurement averages, which is
three in the screen and is what decided that three is where it stays. Given to
`--axis`, it averages there too; without it the axis test keeps its single
render, so older figures stay comparable.

`vrtest --axis=<key> --pairs=<dir>` writes the low and high patch of every pair
instead of scoring them, so a new metric can be tried against saved patches
without rerunning the test. `--rejects=<dir>` now writes level failures as well,
with the corrections asked for on each pass and the distortion settings in the
comments field.

`vrtest --stats=N` rolls the same number every time, screens each once with no
retry, and writes nothing. Two runs of it gave 55% and 58%, and the per style
counts landed within a few of each other. That is a number a change can be judged
against.

It also found the largest fault in the generator, which the batch had been hiding
for months. Seven bass rolls in ten were being rejected, and the batch never
showed it because it simply rolled again, up to eight times a slot, and reported
96% usable.

## A note that is slow is not a note that is long

Those basses were rejected for `note keeps going`, and none of them kept going.
Played and held, every one of them stopped. What they had in common was the
opposite end: they took up to 1.35 seconds to reach full level, against 6 to 92
milliseconds for a bass that sounds right.

A note that slow to rise has more of its energy late in the hold than early,
which is exactly what the held note check measures, so it flagged them and named
the wrong fault. Leaning the attack short was not enough on its own, so a struck
style now has a hard ceiling on it, the way it already had one on sustain: bass
at 0.16, percussion at 0.10, against the 0.12 to 0.13 that basses which sound
right sit at. The decay ceiling came down with it, from 1.30 to 1.20, because at
1.29 a bass took 2.4 seconds to fall to a tenth of its peak.

The held note limit could then be loosened from a fifth to a whole, since what is
left for it to catch is the real case, a note genuinely louder late than early.

| | before | after |
|---|---|---|
| bass rejected for droning, per 40 rolls | 27 | 0 |
| percussion | 12 | 0 |
| bass attack | up to 1352 ms | 5 to 322 ms |

None of it came from the numbers. The numbers said the notes would not stop, and
they were wrong. It came from playing them.

## What a slider is worth

`vrtest --axis=<key> --trials=N` builds the same patch twice from one seed, once
with the slider low and once high, and asks whether the sound moved the way the
slider says it should. One seed means only the axis differs.

Each axis needs the number that says whether it did its job, and they are not the
same number. Scoring all four on the centroid, which is what happened before this
existed, only ever measured brightness.

| axis | scored on | agreement |
|---|---|---|
| BRIGHT | energy centroid | 99% |
| SPACE | tail against the note | 94% |
| MOVE | how far the centroid wanders | 73% |
| DIRT | grit against its own clean twin | 81% |

"One seed means only the axis differs" was not true until recently, and it
mattered most for DIRT. The wavetables and the sample are built from BRIGHT and
DIRT and used to draw from the same stream as the wiring, and every parameter
shared one value stream whose draw count depended on the sliders. So two rolls of
one seed at a low and a high DIRT differed in two fifths of their routing slots,
and a seed walked across five DIRT settings changed its reverb, its envelopes and
its levels as well. The content now has streams of its own and each parameter is
keyed by its name, and within one seed DIRT and BRIGHT now change nothing else.
SPACE still re-points a few macros at the reverb and delay, and MOVE rewires,
because that is what those two axes are for.

DIRT used to be scored on crest, on the reasoning that distortion fills in the
gap between peak and average. A clipper resting at unity barely does and a bit
crusher does not at all, so crest agreed with the slider about half the time,
and tried against saved pairs ten other audio measures did no better without
simply reading BRIGHT or SPACE: spectral flatness scored 73% on DIRT but 95% on
BRIGHT, and counting spectral peaks 72% on DIRT and 96% on BRIGHT.

What works is scoring each side against itself. The patch is rendered three
times as rolled and three times with its grit taken out, the distortion off and
the filter drive at zero, and the number is how far apart the two sound,
level matched, across third octaves. On the saved pairs it agreed with DIRT 93%
of the time and sat at 38% and 35% on BRIGHT's and SPACE's pairs, below chance
rather than above it, since a brighter or wetter patch buries a little of its
own grit.

In the harness it reads 80% over 317 pairs, from 89% on bass down to 65% on SFX,
and on the same seeds the saved pairs used it reads 82% rather than 93%. What
makes that gap is not known. The pairs were rendered through a different host,
and two things that differ between the two were tried and ruled out: holding the
note and measuring over the same window as the pairs changed nothing, and taking
off the distance between two takes of one patch, on the idea that modulation
running on between renders was adding to it, came out at 77% and was dropped.

It caught BRIGHT arguing with itself. An axis switches its effects on when pushed
and off at the other end, which is right where the effect adds the quality the
axis is named for: no reverb is less space. A filter is the opposite, the thing
that takes brightness away, so switching it off at low BRIGHT made the patch
brighter. Leaving it on at both ends was worth twelve points.

MOVE took two goes. Measured on the envelope it read 63% with bass and percussion
moving the wrong way, which is what a metric error looks like: MOVE wires
modulation to cutoff, so it moves tone rather than loudness, and on tone motion
the inversions disappeared. The gap that was left was real. MOVE set the rate of
the modulators and how many were wired and never how far they reached, and four
fast LFOs at shallow depths sit nearly still, so it scales depth now as well.

Two lessons came out of the measuring rather than the fixes. Sixteen trials is
not enough to tune on: an attempt that looked like a clear win at sixteen was a
regression at forty eight. And a failure with a shape is a failure worth
chasing, since it was the -0.75 correlation between agreement and how open a
style's filter already sat that pointed at the enables.



`vrtest --axis=bright --trials=32` builds the same patch twice from one seed,
once with the slider low and once high, and asks whether the sound moved the way
the slider says it should. One seed means only the axis differs.

It caught the axis arguing with itself. An axis switches its effects on when
pushed and off at the other end, which is right where the effect adds the quality
the axis is named for: no reverb is less space. A filter is the opposite, the
thing that takes brightness away, so switching it off at low BRIGHT made the
patch brighter. Agreement went from 73% to 85% by leaving it on at both ends.

Two lessons came out of the measuring rather than the fix. Sixteen trials is not
enough to tune on: the first attempt looked like a clear win at sixteen and was a
regression at forty eight. And the failure had a shape, at a correlation of -0.75
between agreement and how open a style's filter already sits, which is what
pointed at the enables in the first place.

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

## What DIRT does to the distortion

Vital's distortion drive runs from -30 to +30 dB, confirmed by where it clamps,
and it is not the dial it looks like. Each circuit was swept fully wet against
the effect switched off, level matched, on eight patches with the render made
deterministic:

| circuit | change in tone, 30% / 50% / 60% / 70% / 80% of the knob | level at 50% |
|---|---|---|
| soft clip | 0.1 / 0.3 / 0.9 / 2.3 / 4.2 dB | -0.4 dB |
| hard clip | 0.1 / 0.0 / 0.8 / 3.2 / 4.8 dB | 0 dB |
| linear fold | 0.1 / 0.0 / 1.4 / 5.2 / 8.5 dB | 0 dB |
| sine fold | 0.1 / 0.4 / 1.7 / 4.9 / 8.1 dB | +3.4 dB |
| bit crush | 6.4 / 8.9 / 9.4 / 9.4 / 10.1 dB | +0.8 dB |
| down sample | 4.4 / 6.1 / 5.5 / 6.4 / 10.3 dB | 0 dB |

The circuit names follow the order of Vital's menu, which is the authority if
they disagree. The clippers and folders follow the drive decibel for decibel
below the middle, so there they are a volume control and nothing else, and they
bite from about 55%. The crushers never change the level and are strong from 30%.

So DIRT sets three things directly, not by a skewed draw:

- **The switch.** Off at zero, on above it, thrown before the wiring so a macro
  is never left pointing at a switched off effect.
- **The drive**, as a band on the knob that starts at unity. At a DIRT of 0.1 it
  rests between 48% and 50% and may peak at 52%; at the top it rests between 50%
  and 60% and may peak at 70%; it is a straight line in between.
- **The circuit**, in order of how hard it hits. The clippers are open from the
  bottom, the folders from 0.4 and the crushers from 0.7, and each patch draws
  evenly from whatever is open.

The band used to start at the bottom of the knob. Below the middle a clipper only
turns the patch down: one Keys patch with the drive at 29% came out 12.2 dB
quieter with the effect on, against 12.6 dB of negative drive, and styles whose
designs do not use distortion inherit Vital's default mix of 100%, so the whole
signal went through that cut. It added no grit anyone could hear, and at a DIRT
of 0.5 a third of all rolls were thrown out because the level correction could
not win the loss back with the master volume at its maximum. Starting at unity,
every DIRT setting screens at 86% to 92%.

Whatever moves the drive, an LFO, an envelope, a random source or a macro, is
held inside the band. Each gets a share of the room between where the patch
rests and the ceiling, and of the room down to 0%. A depth is a fraction of the
whole knob, so unipolar reaches depth times 60 dB and bipolar depth times 30 dB
either way, which was checked against static renders. The old flat cap of 0.35
was 21 dB of swing, and that swing, not the resting place, was what had made
the distortion hot.

One consequence is left in on purpose. Bipolar modulation dips as far as it
lifts, so a clipper or folder resting at 55% can dip into the lower half, where
it ducks the level for a moment rather than cleaning up. It happens to about one
clip or fold patch in thirteen, down to about -8 dB at worst.

The mix is left free across the whole knob. With the drive resting at unity a
fully wet distortion is a tone rather than a level loss.

**The filters' drive** is the other half of DIRT's grit, a saturation stage
inside the filter itself. Swept with everything else held still it changes the
spectrum by 0.7 dB at 3.5, 1.5 at 10 and 3.4 at 20, where the knob clamps, and
never moves the level by more than a decibel. It used to be drawn between 0 and
7, the first third of the knob, where a busy patch drowns it. DIRT now sets it
directly, resting between half and all of DIRT times 20: nothing at zero, 5 to
10 in the middle, 10 to 20 at the top. Macros that land on it are named GRIT.

**Oscillator warp is not DIRT's.** Vital files it under distortion, but its
types are sync, formant, bend, squeeze and the like, and at one depth they
reshape the tone in every direction: most lift the centroid, only one reliably
fills the spectrum in. And its first type does nothing at all, which is where 42
patches in 48 sat, so DIRT's warp lever was usually a knob wired to nothing, and
so were the Bass style's macro on it and any macro DIRT pointed there. COMPLEX
chooses warp now: each oscillator that is on gets one of the six self-contained
types with a chance of one in twenty at the bottom of COMPLEX and one in four at
the top, measured at 6% and 30%. A macro is only wired to a warp amount when a
warp type is actually set.

What DIRT still owns besides these is the distortion's mix and the number of
unison voices. Voices are thickness rather than grit, but they move the sound
consistently in one direction, so they stay.

Two demos exist for listening rather than reading. `vrtest --dirt-ladder=<dir>`
builds two seeds per style at DIRT 0, 0.25, 0.5, 0.75 and 1, level matched, and
within a seed only what DIRT sets differs. `vrtest --distortion-types=<dir>`
builds one patch per style in each of the six circuits at DIRT 0.75, differing
in the circuit alone.

## The kinds of drum

Percussion used to be one generic hit, a metallic or square oscillator over a
struck sample with short envelopes, and the screen judged it on almost nothing:
any brightness from 0 to 12 kHz passed and there was no pitch check. A kick and
a hi-hat share nothing, so that made neither. There are twelve kinds now, kick,
snare, clap, closed hat, open hat, crash, ride, tom, timpani, cowbell, rim and
shaker, each a target the generator builds toward and the screen holds it to.
They live in `Drums.cpp` as data.

**Calibrated first.** The kinds are only as good as the numbers they are written
in, and Vital's knobs are not in milliseconds or semitones, so three were swept
before any kind was written. The envelope decay setting against the time a hit
takes to fall 40 dB: 0.45 is 28 ms, 0.6 is 86 ms, 0.7 is 162 ms, 0.8 is 270 ms,
0.9 is 446 ms, 1.0 is 676 ms, 1.1 is 0.95 s, 1.2 is 1.36 s, 1.3 is 1.86 s and
1.45 is 2.8 s. Filter blend: 0 low pass, 1 band pass, 2 high pass. Depth on
transpose: 96 semitones per unit, so a three octave kick drop is 0.375.

**Built from parts already there.** A kick is a sine-like body an octave under
the key with its pitch falling two to three octaves into the note in tens of
milliseconds, over the thump. A clap is noise alone through a band pass, struck
three or four times by a one shot LFO before its tail. A cowbell is two squares
a fifth apart. The noise is a new sample model, flat and at the full rate, since
a hi-hat lives in the octave the looping models halve away. Vital sends the
sample layer past both filters by default, so every kind routes it into filter
one.

**Hats and cymbals are not oscillators.** They began as a strongly inharmonic
wavetable three octaves up over noise, and even turned right down it made them
sound wrong, with the character coming from the noise and a tone on top that did
not belong. It was measured, not only heard: every hat, crash and ride read a
pitch at 1046 Hz with a salience of 0.7 to 0.95. A wavetable is one repeating
cycle, so every partial in it is a harmonic of one note, however far the table's
spectrum is bent. Real cymbals and the drum machines that imitate them are not
like that. The 808 sums six square waves at 205.3, 304.4, 369.6, 522.7, 540 and
800 Hz, which add up to no pitch, and band passes the sum at 3440 and 7100 Hz.
That cluster is now a sample model of its own, Metal: the six moved together and
a little apart per seed, each rounded to whole hertz so the one second loop is
seamless, band limited, tilted up the way the 808's band passes tilt it, and
mixed with noise. The hats, and at first the crash and ride, ran on it with
every oscillator off, and the decays follow the 808's, a closed hat in about
50 ms, an open one between 90 and 600. Salience fell to between 0.05 and 0.5.
The tilt was needed: without it the squares' fundamentals were the loudest
thing in the sample and a ride had a fifth of its energy under 400 Hz.

The open hat needed more than that. Ringing for up to half a second it let the
six squares be picked out one by one, which is heard as a struck chord rather
than a hat. It gets a second bank of six at an unrelated ratio above the first,
a tilt turning at 5 to 7 kHz instead of 2.5, more noise and no resonance, and
its salience fell from about 0.27 to 0.06.

**The crash and ride are plates, not clusters.** On the metal they still sounded
off, so they were measured against sampled crashes and rides, level matched and
from the onset. Three things separated them. The ride was a chord: eighty
spectral lines standing 30 dB clear of the floor, where real rides show twenty
or thirty standing 11 to 19 dB clear. Neither darkened as it rang, where a real
crash's spectral centre falls from about 10 kHz at the strike to 5 by two
seconds and a ride's from 7 to under 3, since a plate's high modes die first.
And both had a rumble real ones lack, 125 Hz at -15 to -24 dB of the body where
real cymbals sit at -30 to -64.

The rumble had two causes, and neither was the sample, which measured -48 dB
there on its own. The shared ranges stop Vital's filter blend at 1, band pass,
so every kind written as a high pass had been a band pass all along. Drums are
exempt now, and the kinds that already sounded right, the hats, clap, cowbell,
rim and shaker, are written as the band pass they were. And Vital's compressor
is multiband: on a cymbal it lifted the quiet low band by 26 dB and held the
tail up twice as long as a real one rings. Crash and ride have it off.

They run on a new sample model, Cymbal: white noise rung through two pole
resonators spread from 500 Hz to 16 kHz, crowding toward the top and weighted
toward the 3 to 9 kHz a cymbal lives in, over noise high passed at 2 kHz. A
crash has two hundred broad ones and some wash, a ride ninety sharp ones and
little. Over it a low pass the second envelope throws open by 20 to 32
semitones at the strike and closes over the ring. Measured the same way, the
rides now show 6 to 27 lines standing 10 to 16 dB clear and darken from 6 kHz to
3 or 4, the crashes from 8.6 to 5, and both sit at -33 to -42 dB at 125 Hz.
The crashes are at the washy end of the real ones, peaks 4 to 7 dB clear against
6 to 11.

**A timpani is not a sine.** It was a bare fundamental under a low pass, which
is a bass, and one roll had a formant warp from COMPLEX over heavy distortion
on top of that. A kettle drum's head rings in modes the bowl and the strike
narrow to a principal, a fifth, an octave and a tenth, at 1, 1.5, 1.98 and 2.44
times in about 5 to 4 to 3 to 1, following Synth Secrets' timpani article. Those
fall within a few cents of harmonics two to five of the note an octave down, so
the body is a new wavetable character, membrane, played an octave down with the
principal on the key. The pitch reads an octave below the key, which is what a
real kettle drum does too: the four imply a fundamental there and the ear finds
it. Over it is a felt mallet, a new one shot sample that is a short low passed
thud, and a room, since a timpani is never heard dry. No tuned drum takes a
warp now.

**A shaker is played, not struck.** Filtered white noise with a short swell was
a hiss. A real one is many small collisions, beads or seeds hitting a shell,
which is how Perry Cook's PhISEM shakers in the Synthesis ToolKit build it: at
random, each a burst of noise dying in a fraction of a millisecond, rung through
the shell's resonance, 3.2 kHz for a maraca, 3 for a cabasa, 5.5 for a sekere.
That is a new looping sample, Beads. And it is played back and forth, sha-ka: a
one shot LFO would give one stroke, so the shaker sustains and a tempo synced
LFO named Sha-ka swells the beads twice a cycle, the second stroke lighter, on
sixteenths or eighths, never dropping below a quarter to two fifths of full
since the beads never stop. A held key keeps shaking, and a tap is one stroke.
Measured over a held second, the level rises eight times, 10 to 17 dB between
stroke and trough.

**Named for what it is.** A roll's name carries its kind, so a batch file reads
Percussion_04_Crash rather than Percussion_04, and the parts in Vital's panels
say what they are for: the sample Crash metal or Snare noise, the first
oscillator Tom body. A drum's LFOs are named for where they are wired, Tone,
Pitch, Level or two of them joined, and Spare when nothing listens. A sequence's
line is named for its shape because the shape is the riff, but a drum's LFOs are
ordinary movement ones, so naming them for the drum would say nothing true.

**Held to its kind last.** The axes, the jitter, COMPLEX and the note shape all
move a patch without knowing it is meant to be a hi-hat, so each kind's bands
are applied after them: envelope, filter, pitch drop, levels. A value already in
its band is kept, which is where two kicks differ, and one outside is drawn
inside, so BRIGHT moves a hat's cutoff within the hat band. Four things had to
give way for that. The shared repair capped the noise level at 0.26, pulled any
noise above 0.45 under the oscillators, and switched an oscillator back on under
a clap. Drums are exempt. COMPLEX's third oscillator could arrive an octave down
on Vital's drop twelve stack, which put a second pitch under every tuned drum.
Four unison voices and a spectral morph each smeared a tom's pitch, one by four
and a half semitones. And a macro on spectral morph rested halfway up, so drums
have their own macros now.

**The cutoff a player hears.** Velocity and a macro on the cutoff add to it for
the whole note, a normal strike being 110 of 127 and a macro resting halfway, and
on a clap those and the strike's sweep took a band pass meant for 1.5 kHz to a
centre of 13.9 kHz. So a kind's cutoff band describes the cutoff heard at a
normal strike with the macros where they sit, and the knob is set below it by
what they add. The shared ranges stop the knob at 110, 4.7 kHz, too low for a
hat's high pass, so a drum holds its own cutoff up to 130.

**Judged as a hit.** The screen was built around a held note: brightness read
from 200 ms in, a patch with nothing left in the hold called click only, the
level the 90th percentile of short windows, and a note that never stops caught
by dividing late by a window starting at 100 ms. A closed hat is over in 60 ms,
so it read as silent, as a click, at a brightness of zero, and as never
stopping. Percussion is auditioned in hit mode now: brightness from the onset,
the level as the loudest window, the early window from the onset, and no click
only rule. And each kind carries its own rules: a brightness band, how much of
it must be bass, how long it may ring and whether it must sound the note.

**Measured.** `vrtest --drums=<dir>` builds every kind from four seeds and
prints each one against its band. All 48 land in their kind's band and every
tuned drum is on its note. Screened on their own, `--styles=Percussion/Kick` and
so on for each kind, 174 of 192 rolls passed, 91%, and the timpani were half the
rejections, reading as wrong notes while their slow settle was still sounding,
and after a quicker one the tuned drums passed 31 of 32. The rest are the screen
doing its job, a hat or shaker darker than its kind. With the hats and cymbals on
the metal sample, all 192 came out usable, and their median salience fell from
0.86 to 0.27 on a closed hat, 0.94 to 0.27 on an open one, 0.94 to 0.13 on a
crash and 0.98 to 0.44 on a ride.

They helped the axes as well, which was part of the reason for them. On
percussion's own rows, against the same metrics as before, DIRT rose from 87% to
95% and MOVE from 67% to 77%, with BRIGHT at 95% and SPACE at 100%. A kick, a
hat and a crash each answer a slider in their own way, and as one generic hit
they blurred together.

The distortion mix's macro is named DISTORT now, on every style. It and the
drive both read DRIVE, and the second of two on one patch fell back to its raw
name cut short, as DISTORTION D.

## What VARY keeps, and how history brings it back

VARY was meant to keep a patch and only move the sections most responsible for
its character, and the request said so, but nothing in the generator read that
part of it. Measured with `vrtest --vary=<dir>`, two patches per style varied at
four depths and five times in a row: even at a depth of 0.02 it changed 60 to 78
values, replaced every wavetable and four to seven LFO shapes, and how far the
sound moved had no order to it at all. A VARY also wired each macro to one more
destination and renamed it after the new one while the old routing stayed, and
it scaled every modulation depth by MOVE again, so five presses took four macro
routings to thirteen and the total modulation depth to two or three times where
it started.

Now it keeps the wavetables, the LFO shapes and their wiring, a sequence's
riff and the macros, and leaves the switches and circuit choices alone. What it
moves, each by the depth: the filter and envelope sections, toward where their
axis would draw them; how fast each LFO and random source runs; and how far each
modulation reaches, scaled by a factor centred on one so a run of VARYs wanders
rather than drifting one way.

A rate is two knobs depending on the LFO. Most run synced to the tempo and
ignore their frequency: 350 of 384 LFOs in a batch were synced, and 361 of the
367 not running free sat on the same note division. So a synced one steps a
division up or down and only a free running one has its frequency nudged. One
that follows the keyboard keeps its rate. Anything driving pitch keeps both its rate and its depth, so a riff keeps
its tempo and lands on the same notes and a vibrato keeps its speed, and macro
routings are the player's own and are left alone.

At 0.02 it changes 7 to 9 values and at full depth 23 to 49, and the median
distance the sound moves, against two takes of the same patch, goes 1.0x, 1.3x,
1.4x, 1.7x across the four depths. That measure compares the average tone of a
held note, so it sees the filters and envelopes and mostly misses the rates,
which change when things happen rather than what they sound like on average.
Five VARYs in a row leave the total modulation depth at 0.98 to 1.02 of where it
started. A fresh roll is byte for byte what it was before.

History could not bring a VARY back. An entry recorded the style, sliders and
seed, which rebuilds a fresh roll and not a patch that was moved from another,
so stepping back onto a VARY rolled something new. It could not bring anything
back at its level either, since the screen matches level by moving the master
volume after the generator is done and the entry never stored it. An entry now
records that volume, and a VARY records the entry it started from, so replaying
walks back to the fresh roll and forward again. Where the starting patch has no
recipe, a recalled keeper or a patch restored with a project, which may carry
edits made by hand in Vital, the patch itself is kept, and so is every
sixty-fourth link of a long run so replaying never has to walk further than
that. Links are written once each when a project is saved.

`vrtest --history-check` makes a fresh roll, four VARYs on it and one started
from a patch, for every style, and rebuilds each from its recipe: 40 of 40 come
back exactly as they were heard, and 40 of 40 again after the history has been
saved and read back. The old replay managed 3.

New entries always go on the end. A roll or a VARY made while sitting back in
history used to delete everything after it, the way an edit after an undo does,
so going back to the first of three VARYs to try another threw the other two
away. History is the candidates being chosen between rather than an undo stack,
and nothing depends on where an entry sits, so only the limit removes anything
now. The same check covers it.

## Three renders, and why it is not two

Every check now runs on three renders averaged rather than one, because Vital
randomises unison phase at every note on and one render of one patch reads a
level across a couple of decibels and a pitch across a fraction of a semitone.
That took the commonest rejection, a level that would not settle, from 24 in a
240 roll run to 2, and the whole run from 82% usable to 92%.

It costs three times the rendering in the screen, which lands on the worker
thread and is hidden by the prefetch, so nobody waits for it.

Whether two would do the same was the open question, and `vrtest --stats=24
--renders=N` answers it. Four passes of each count, interleaved so machine load
fell on both equally, 768 rolls per count:

| renders | usable | level never settled | level spread | wall per 192 rolls |
|---|---|---|---|---|
| 1 | 81% | 14 of 192 | | 310s |
| 2 | 87.1% | 38 of 768 (4.9%) | 1.83 dB | 444s |
| 3 | 89.3% | 27 of 768 (3.5%) | 1.83 dB | 552s |
| 4 | 86% | 11 of 192 | | 677s |

One render is clearly worse and four is no better than three while costing 44%
more, so the range worth arguing about is two against three.

Three leads on both counts and in three of the four passes, but neither lead is
significant: z = 1.34 on the usable rate and z = 1.39 on the settling count,
which is p around 0.17 either way. Separating a two point difference at this
variance would take something like five thousand rolls per count, six hours of
rendering, to decide whether to save half a second per roll on a thread nobody
is waiting for. That is a worse trade than the one being measured.

The level spread is the same to two figures at 1.83 dB, so the extra render is
not buying a tighter correction. What it buys, if anything, is fewer patches
where the correction fails to converge at all.

Staying at three. Not because three is proven better, but because two is not
proven equal and the cost is already hidden. Worth revisiting only if the
screen ever moves somewhere the user waits for it.

## Licensing

Vital is GPLv3, and this hosts its binary through the standard plugin interface
exactly as a DAW does rather than linking or modifying it. Vital is not bundled;
the plugin finds the copy you have installed. JUCE and the VST3 SDK are both
GPLv3-or-commercial, so this is released under GPLv3.

A generated patch is original work: the wavetables are synthesised per roll and
the parameters come from the designed starting points in this repository, so what
it produces is yours to do what you like with.
