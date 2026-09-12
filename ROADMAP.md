# Roadmap

Open work, roughly in the order I would do it. Anything measured is measured, so
the numbers here come from `vrtest` and from rendering through Vital rather than
from impressions.

## 1. A level that will not settle is the commonest rejection left

Twenty four of the forty four in a 240 roll run, spread across every style and
the largest single reason in most of them. The correction ran its passes and the
patch still was not where it was aimed.

It was worse before the deadband was widened and the reading averaged over three
renders, so some of what is left is genuinely a patch whose loudness depends on
something the correction cannot reach: a compressor or a limiter in its own
chain responding to the master volume rather than tracking it. Worth finding out
whether those patches are unusable or merely uncalibrated, because if it is the
latter they are being thrown away over bookkeeping.

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

## 3. The sample factory is undocumented

`docs/notes.md` explains why the wavetables are synthesised and says nothing
about the seven sample models, which are a larger piece of work and rest on the
same argument. The notes are how anybody else would understand the decisions,
and right now half of them are missing.
