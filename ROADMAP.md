# Roadmap

Open work, roughly in the order I would do it. Anything measured is measured, so
the numbers here come from `vrtest` and from rendering through Vital rather than
from impressions.

## 1. The sampler is one fixed sound and it is the wrong one

Every patch that uses the sample slot gets Vital's own white noise. Only its
level, routing and transpose vary, so a layer meant to add character adds the
same flat hiss to everything, and because white noise is flat to the top of the
range it costs far more brightness than it buys.

It showed up in listening. Keys patches measuring 7 kHz and up were called too
bright, and muting the noise turned them into ordinary presets: the oscillators
were never the problem. The level is capped lower for now, which treats the
symptom.

What it wants is content. Vital reads a sample as plain audio in the preset, so
the generator could synthesise one the way it already synthesises wavetables:
filtered noise bursts, struck and scraped textures, short tuned bodies. That is
the same argument the wavetables won, and the sample slot never got it.

## 2. Keys still throws away eight bright candidates in thirty

Down from seventeen once the ceiling was raised, and lead is down from twelve to
three, but keys is still the style that fails most often on its own style rule.

The ones inside 30% of the old ceiling were all played and none sounded wrong,
which is why the line moved. What has not been listened to is the other side:
keys still rejects at four times its ceiling. Somewhere between 30% over, which
is fine, and 400% over, which is presumably not, there is a real edge, and only
listening will find it. The same `--rejects=<dir>` and a set built from the
middle of that range would say where.

## 3. A sequence on the wrong note

Five of the last thirty sequence rolls, and the largest reason left for that
style. A sequence is allowed to sit on any semitone, so failing `wrong note`
means it landed between two, which is the one thing the square stepped LFO and
the transpose quantiser are supposed to make impossible.

Worth finding whether the pitch is genuinely off grid or whether the reading is,
given how many measurements turned out to be the thing at fault this week.

## 4. DIRT is the weakest axis and neither metric measures it well

Agreement by axis: BRIGHT 97%, SPACE 93%, MOVE 77%, DIRT 72% over 316 pairs.

DIRT is scored on crest, on the reasoning that distortion fills in the gap
between peak and average. The level matching also changes peak against average,
so that number was suspect, and spectral flatness was tried instead as something
the loudness correction cannot touch. It scored 59%, worse, with percussion
inverted at 20%. Crest stays.

Where it fails is not spread evenly:

| Percussion | Bass | Lead | Experiment | SFX | Keys | Pad | Sequence |
|---|---|---|---|---|---|---|---|
| 97% | 87% | 80% | 66% | 65% | 62% | 61% | 60% |

It works on the styles whose crest is low to begin with and fails on the ones
already dense, which hints the metric reads the signal rather than the drive.
What would settle it is a measure built from what DIRT actually does, the energy
distortion adds between the harmonics.
