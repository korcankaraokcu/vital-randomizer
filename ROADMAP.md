# Roadmap

Open work, roughly in the order I would do it. Anything measured is measured, so
the numbers here come from `vrtest` and from rendering through Vital rather than
from impressions.

## 1. The BRIGHT axis is the weakest of the four

Tested by building the same patch from the same seed twice, once with the
slider low and once high:

| axis | agreement |
|---|---|
| dirt | 90% |
| space | 80% |
| move | 80% |
| bright | **70%** |

Filter cutoff is both the obvious brightness control and the most modulated
destination, so biasing it statically gets overridden by whatever envelope is
already driving it. Adding the EQ as a lever tripled the effect size, from a
76 Hz to a 219 Hz median move, but consistency did not improve. Biasing the
modulation depths that target cutoff is the next thing to try.

## 2. Bass and keys spend a long time at the brightness ceiling

Filling a batch of 48 took 69 rolls in the last run. That is healthy in itself,
but almost none of it is spread evenly: bass and keys account for nearly all of
the rejections and both are thrown out for brightness. One run gave up on fifteen
consecutive keys candidates.

Both sit close to their ceiling by design and COMPLEX pushes them at it, so the
question is whether the ceiling is right rather than whether the patches are. It
is the one place where the screen, rather than the generator, is the thing most
likely to be wrong. Worth checking by ear before moving either number: a batch of
rejected bass candidates played back would settle it in a couple of minutes.

## 3. Sequence steps read 0.25 semitones off the grid

Against a 0.35 threshold, so they pass, and they sound right, which makes this a
question about the measurement rather than the sound. Three explanations have
been tried and none of them holds:

- **Analysis windows straddling a step.** Discarding windows whose two halves
  disagree on the pitch changed nothing, because no window was ever discarded.
- **Autocorrelation resolution.** A sequence sounds near 2 kHz, where the lag is
  about 22 samples and one sample is 0.77 of a semitone, so this looked likely.
  Fitting a parabola through the peak moved the figure not at all.
- **Modulated unison detune.** No relationship: the preset with the most detune
  and the most voices reads 0.12, the lowest of the six.

The next thing to try is a glide. The pitch LFO is a square, but if its edges are
smoothed at all then every window holds a little of the transition, and the
transition test above used a 0.4 semitone threshold that would not have caught
it. Ruling that in or out wants a look at the LFO shape rather than more
rendering.
