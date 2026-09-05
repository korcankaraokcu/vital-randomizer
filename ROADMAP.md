# Roadmap

Open work, roughly in the order I would do it. Anything measured is measured, so
the numbers here come from `vrtest` and from rendering through Vital rather than
from impressions.

## 1. The keeper rack has no interface

`KEEP` stars a patch and `recallKeeper()` exists in the processor, but nothing in
the editor ever lists or calls it. Starring a patch currently loses it. This is
the largest gap between what the plugin does and what it appears to offer, and it
is a small job: a row of numbered buttons in the strip.

## 2. Prepare the next candidate while the user auditions

A roll takes a few seconds because screening does real work. The last full batch
needed 73 rolls to produce 48 keepers, and each roll settles the patch, auditions
it, sometimes confirms it, and runs up to three level passes.

The original design had the queue hiding this by generating the next candidate in
the background while the current one is being listened to. It was never built. It
turns the wait into nothing without weakening any of the checks.

## 3. The BRIGHT axis is the weakest of the four

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

## 4. Smaller things

- **Template and Unlabelled** still appear in the plugin's style dropdown.
  Neither is a sound anyone sets out to make, and batches already exclude them.
- **Release packaging.** There is no release yet, so the download link in the
  README will not resolve until a build is tagged and uploaded.
- **Rejection rate.** Filling a batch of 48 took 57 rolls, so about one in six
  is thrown away. Bass accounts for most of it and sits close to its brightness
  ceiling, which COMPLEX pushes it toward.
- **Filter 2 usage** now lands at 65% of patches against 42% in a hand-made
  library. The defaults lean busy on purpose, but that is further out than
  intended and the per-style complexity defaults could come down a little.
