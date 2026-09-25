# Roadmap

Open work, roughly in the order I would do it. Anything measured is measured, so
the numbers here come from `vrtest` and from rendering through Vital rather than
from impressions.

## 1. MOVE is the weakest axis

Agreement by axis, `vrtest --axis=<key> --trials=40`, about 315 pairs each:

| BRIGHT | SPACE | DIRT | MOVE |
|---|---|---|---|
| 99% | 94% | 81% | 73% |

MOVE by style, scored on how far the centroid wanders:

| Bass | Percussion | Lead | Experiment | Pad | Keys | SFX | Sequence |
|---|---|---|---|---|---|---|---|
| 86% | 77% | 75% | 74% | 72% | 69% | 67% | 65% |

SFX read 85% on the run before this one with its patches unchanged, byte for
byte. Its patches are the most random there are, Vital's random generator is
shared across every render in a run, and the drum kinds now render just before
it, so it is measured at a different point in that sequence. Forty pairs a
style is not enough to hold a style that random still, and it should not be
read as a weak style on one run.

Percussion rose from 67% to 77% when it gained its kinds, since a kick, a hat and
a crash each move in their own way and no longer blur into one generic hit.

It has had two rounds already, which are in the notes: the metric moved from the
envelope to tone motion, and MOVE started scaling how far its modulation
reaches as well as how fast. Nothing has been tried on the axis since, and the
lowest styles are the ones whose motion is least about tone: Sequence moves in
pitch, which the centroid barely sees, and Keys are short enough that there is
little held note for anything to wander in. That points at the metric before the
axis, which is the order DIRT turned out to need as well.

There is a lead on the axis itself too, found while giving VARY the LFO rates.
MOVE's rate lever is each LFO's frequency, but most LFOs run synced to the tempo
and a synced LFO ignores its frequency: 350 of 384 LFOs in a batch were synced,
and 361 of the 367 not running free sat on the same note division. So for nearly
every LFO, MOVE's rate control is connected to nothing, and the rate it should
be moving is the note division.

## 2. DIRT reads 81% in the harness and 93% on saved pairs

DIRT is now scored against a clean twin of each patch, and on the same seeds the
harness reads 82% where the saved pairs read 93%. Holding the note over the same
window as the pairs changed nothing, and correcting for modulation running on
between renders came out worse, so the cause is still open. The next step is to
render one patch through both hosts and compare the audio directly. SFX is the
lowest style in both, at 65% here.
