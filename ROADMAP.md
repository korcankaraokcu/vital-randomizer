# Roadmap

Open work, roughly in the order I would do it. Anything measured is measured, so
the numbers here come from `vrtest` and from rendering through Vital rather than
from impressions.

## 1. Drop the preset library dependency

**The largest item, and the one that changes what the project is.**

The generator currently learns from the user's installed presets: it splices
parameter values out of them section by section, and builds per-style
distributions from them. That was never a requirement, it was a shortcut taken
early because the library was sitting there.

It has two costs. The plugin does not work without a decent preset library, and
a generated patch carries settings taken verbatim from whatever presets it
spliced. Measured on a sample of generated patches, counting only parameters not
at Vital's default:

```
meaningful parameters copied verbatim from library presets:  158 of 371  (43%)
```

No single preset contributes more than about 22 of them, so nothing is a copy of
anything. But the whole modulation matrix comes from one preset per patch, and
which source is wired to which destination is the most creative decision in a
preset. That is fine for personal use and questionable for anything shared,
since most commercial pack licences forbid redistributing presets or derivatives.

The replacement is smaller than it sounds. A hand-made preset changes about **81
parameters out of 756**, median 75, and only 124 parameters are touched by more
than a quarter of presets. So:

- **Per-style archetypes** replace donor splicing. One hand-designed patch per
  style, around 75 values each, roughly 600 across all eight. An archetype is
  coherent by construction, which is better than splicing six presets and hoping
  the sections agree.
- **Explicit routing tables** replace the learned ones. Around eight lines per
  style, and the content is already known: bass is `env_2 -> filter_1_cutoff`,
  keys is `lfo_1 -> filter_1_cutoff`, sequence is LFOs on level and transpose.
- **Plain ranges** replace the quantile curves the sliders sample from. Vital's
  own `synth_parameters.cpp` carries authoritative min, max and default for every
  parameter and is already a dependency.
- Then `StyleModel` and the cached model file go, and `measure.py provenance`
  goes with them, since there will be no library left to have copied from.

Most of the migration has already happened for other reasons. Wavetables, LFO
shapes and the sample are synthesised, the amp envelope shape is hand-written
because the corpus disagreed with how a bass should behave, and the transpose
rule, pitch caps, brightness bounds, held-note limits and loudness targets are
all constants now. What is left is the splice, the routing tables, the curves and
a handful of per-style facts.

Nothing in the screening layer changes. It renders each candidate and checks the
note, the decay, the level, the balance and the brightness, and it does not care
where the parameters came from.

Do it in order: archetypes and routing tables, then ranges, then delete the
corpus. `vrtest` measures quality at every step, so regressions are visible
rather than guessed at.

## 2. The keeper rack has no interface

`KEEP` stars a patch and `recallKeeper()` exists in the processor, but nothing in
the editor ever lists or calls it. Starring a patch currently loses it. This is
the largest gap between what the plugin does and what it appears to offer, and it
is a small job: a row of numbered buttons in the strip.

## 3. Prepare the next candidate while the user auditions

A roll takes a few seconds because screening does real work. The last full batch
needed 73 rolls to produce 48 keepers, and each roll settles the patch, auditions
it, sometimes confirms it, and runs up to three level passes.

The original design had the queue hiding this by generating the next candidate in
the background while the current one is being listened to. It was never built. It
turns the wait into nothing without weakening any of the checks.

## 4. Sequences waste rolls

The last batch rejected 28 sequence candidates for `wrong note`. A sequence steps
between pitches, so a measurement can land on the fifth and be rejected even
though the step is exactly on a semitone and entirely intended. The pitch rule
should allow any semitone for this style rather than requiring an octave.

## 5. The BRIGHT axis is the weakest of the four

Tested by building the same patch from the same donors twice, once with the
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

## 6. Smaller things

- **Template and Unlabelled** still appear in the plugin's style dropdown.
  Neither is a sound anyone sets out to make, and batches already exclude them.
- **Release packaging.** There is no release yet, so the download link in the
  README will not resolve until a build is tagged and uploaded.
