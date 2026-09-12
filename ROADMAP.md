# Roadmap

Open work, roughly in the order I would do it. Anything measured is measured, so
the numbers here come from `vrtest` and from rendering through Vital rather than
from impressions.

## 1. The screen measures three times and it is not free

Every check now runs on three renders averaged rather than one, because Vital
randomises unison phase at every note on and one render of one patch reads a
level across a couple of decibels and a pitch across a fraction of a semitone.
That took the commonest rejection, a level that would not settle, from 24 in a
240 roll run to 2, and the whole run from 82% usable to 92%.

It costs three times the rendering in the screen, which lands on the worker
thread and is hidden by the prefetch, so nobody waits for it. Whether it is
worth three renders or would do with two has not been measured, and two would be
the obvious thing to try before anybody adds a fourth.

## 2. DIRT is the weakest axis and neither metric measures it well

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
