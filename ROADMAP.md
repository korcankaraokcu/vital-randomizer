# Roadmap

Open work, roughly in the order I would do it. Anything measured is measured, so
the numbers here come from `vrtest` and from rendering through Vital rather than
from impressions.

## 1. MOVE is the weakest axis

Agreement by axis, `vrtest --axis=<key> --trials=40`, about 315 pairs each:

| BRIGHT | SPACE | DIRT | MOVE |
|---|---|---|---|
| 97% | 94% | 81% | 75% |

BRIGHT, SPACE and MOVE were measured again after the spectral morph was given a
type, the effects their modes, and the cyclic modulation on tone controls and
the delay's feedback caps from listening. DIRT is from before that. MOVE rose
from 73% to 77% with the morph, most where a dead morph had been wired in, and
settled at 75% with the caps, which follow the slider. BRIGHT eased from 99%, Keys, Lead and
Sequence reading 95 to 97% with the second filter slope and the morph types in
play, and Percussion at 90% after a regression to 55% was found and fixed.

MOVE by style, scored on how far the centroid wanders:

| SFX | Bass | Percussion | Experiment | Lead | Pad | Keys | Sequence |
|---|---|---|---|---|---|---|---|
| 87% | 85% | 77% | 76% | 72% | 72% | 67% | 67% |

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
reaches as well as how fast. A third, the spectral morph given a type so the
LFOs wired to it do something, lifted it from 73% to 77%, and capping how far
cyclic sources may swing a tone control, a rule from listening, gave two of
that back. The lowest styles are Keys, short enough that there is little held
note for anything to wander in, and Sequence, which moves in pitch. That points at the metric before the
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
