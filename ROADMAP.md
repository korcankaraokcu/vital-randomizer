# Roadmap

Open work, roughly in the order I would do it. Anything measured is measured, so
the numbers here come from `vrtest` and from rendering through Vital rather than
from impressions.

## 1. A level that will not settle is now the commonest rejection

Forty four of the ninety rejections in a 240 roll run, spread across every style,
and the largest single reason in six of the eight. SFX is the worst at fifteen.

It means the level matching ran its three passes and the patch still was not
where it was aimed. That is a patch whose loudness depends on something the
correction cannot reach, most likely a compressor or a limiter in its own chain
responding to the master volume rather than tracking it. Worth finding out
whether those patches are unusable or merely uncalibrated, because if it is the
latter they are being thrown away for a bookkeeping failure.

## 2. Keys and lead are still throwing away bright candidates

Eleven each per 240 rolls, and now the only style rule that fires in any number.
The ceilings were recalibrated when the centroid started measuring energy, but
they were set from a p90 of fourteen hand-made presets a style, which is a thin
sample to draw a hard line from.

`--rejects=<dir>` writes what the screen discarded with its reading in the name,
which is how the bass ceiling turned out to be wrong. The same listen would say
whether these two are.

## 3. DIRT is the weakest axis and neither metric measures it well

Agreement by axis: BRIGHT 97%, SPACE 93%, MOVE 77%, DIRT 72%.

DIRT is scored on crest, on the reasoning that distortion fills in the gap
between peak and average. The level matching also changes peak against average,
so that number was suspect, and spectral flatness was tried instead as something
the loudness correction cannot touch. It scored 59%, worse, with percussion
inverted at 20%.

So crest stays, and the honest reading is that DIRT is genuinely the weakest of
the four rather than mismeasured. Where it fails is worth noting, because it is
not spread evenly (40 trials a style, 316 pairs):

| Percussion | Bass | Lead | Experiment | SFX | Keys | Pad | Sequence |
|---|---|---|---|---|---|---|---|
| 97% | 87% | 80% | 66% | 65% | 62% | 61% | 60% |

It works on the styles whose crest is low to begin with and fails on the ones
that are already dense, which is itself a hint that the metric is reading the
signal rather than the drive. Both attempts are recorded here so neither gets
tried again. What would settle it is a metric built from what DIRT actually
drives, the energy that distortion adds between the harmonics, rather than a
whole signal statistic that other stages also move.
