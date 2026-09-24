# Roadmap

Open work, roughly in the order I would do it. Anything measured is measured, so
the numbers here come from `vrtest` and from rendering through Vital rather than
from impressions.

## 1. DIRT has no metric that fits it, and two of its levers do nothing

The distortion itself is settled and written up in the notes under "What DIRT
does to the distortion": DIRT switches it, sets the drive as a band that starts
at unity, and opens the circuits in order of how hard they hit. What is left is
the rest of the axis and the number it is judged by.

Agreement by axis, `vrtest --axis=<key> --trials=40`, about 315 pairs each:

| BRIGHT | SPACE | MOVE | DIRT |
|---|---|---|---|
| 99% | 94% | 73% | 61% |

**The metric.** DIRT is scored on crest, which assumes distortion squashes
peaks. The clippers resting around unity barely do, and the crushers change the
tone without doing much to the peak, so crest is the wrong question for most of
what DIRT now produces. Ten candidates were tried against saved pairs and none
separated DIRT: the best, spectral flatness, also scored 97% on BRIGHT and 85% on
SPACE. That was measured while DIRT's distortion was still inaudible, though,
so it deserves another go now that it makes grit. `--pairs=<dir>` writes the
pairs for exactly this, and any candidate has to be shown to stay put when
BRIGHT and SPACE move, or it measures them instead.

**The levers.** Isolated one family at a time, against two takes of the same
patch as the unit, DIRT's other parameters came out as:

| family | audible change | direction |
|---|---|---|
| filter drive | 1.0x | none, inaudible where it sits |
| oscillator warp | 1.5x | random, it is sync and formant and bend, not grit |
| unison voices | 1.3x | denser, which is thickness rather than dirt |

Whether filter drive stays, or gets a range where it does something, and whether
oscillator warp belongs to DIRT at all, are open. Anything taken out falls back
to the general jitter, which no other axis claims, and all four axes need
re-running afterwards.
