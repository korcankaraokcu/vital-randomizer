# Roadmap

Open work, roughly in the order I would do it. Anything measured is measured, so
the numbers here come from `vrtest` and from rendering through Vital rather than
from impressions.

## 1. MOVE is the weakest axis

Agreement by axis, `vrtest --axis=<key> --trials=40`, about 315 pairs each:

| BRIGHT | SPACE | DIRT | MOVE |
|---|---|---|---|
| 99% | 94% | 80% | 73% |

MOVE by style, scored on how far the centroid wanders:

| Bass | SFX | Lead | Pad | Experiment | Keys | Percussion | Sequence |
|---|---|---|---|---|---|---|---|
| 86% | 85% | 75% | 72% | 71% | 69% | 67% | 65% |

It has had two rounds already, which are in the notes: the metric moved from the
envelope to tone motion, and MOVE started scaling how far its modulation
reaches as well as how fast. Nothing has been tried on it since, and the lowest
styles are the ones whose motion is least about tone: Sequence moves in pitch,
which the centroid barely sees, and Percussion and Keys are short enough that
there is little held note for anything to wander in. That points at the metric
before the axis, which is the order DIRT turned out to need as well.

## 2. DIRT reads 80% in the harness and 93% on saved pairs

DIRT is now scored against a clean twin of each patch, and on the same seeds the
harness reads 82% where the saved pairs read 93%. Holding the note over the same
window as the pairs changed nothing, and correcting for modulation running on
between renders came out worse, so the cause is still open. The next step is to
render one patch through both hosts and compare the audio directly. SFX is the
lowest style in both, at 65% here.
