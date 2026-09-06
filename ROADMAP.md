# Roadmap

Open work, roughly in the order I would do it. Anything measured is measured, so
the numbers here come from `vrtest` and from rendering through Vital rather than
from impressions.

## 1. The screen has become very permissive

48 of 49 rolls now pass, where it used to be 48 of 69. Most of that is a screen
that was wrong rather than one that has gone soft: the brightness check was
counting spectral bins instead of energy and throwing away basses for having a
little air on them. But 98% means almost nothing is being caught, and the value
of the screen was always that it spends a machine's attention instead of yours.

What still fires is worth keeping (a note that keeps going, a level that will not
settle, a patch playing the wrong note), so this is not broken. It wants a listen
to a full batch to say whether anything unusable is now getting through, and if
nothing is, the thresholds that no longer fire could go rather than sit there
looking like protection.

## 2. A bass that keeps going is the commonest rejection left

Ten of the nineteen rejections in the last batch, and the largest single reason.
Down from 44 once brightness stopped dominating, so the picture is better than it
was, but it is now the thing bass fails on.

The check measures late energy against early energy with the key still held, and
the basses that get through are clean (0.16 held, 0.00 tail), so this is the
generator making basses that drone rather than the screen misjudging. A filter
envelope with somewhere left to go and the effects tail are the likely causes,
and COMPLEX turns up both.

## 3. The axes are measured on a number that just changed

`vrtest --axis=bright --trials=32` now reports 97% agreement overall, with five
styles at 100% and SFX and Experiment at 90%. It read 73% a day ago.

Part of that is a real fix, an axis that had been switching its own filter off at
its dark end. The rest is that the test scores itself on the centroid, and the
centroid was counting spectral bins rather than energy, so the yardstick was
noisy in the same way the brightness check was wrong. There is nothing left to
chase on BRIGHT.

The other three axes have never been measured this way. DIRT, SPACE and MOVE were
scored once by hand, against the old centroid, and the test now takes any axis
key. Scoring them properly is a few minutes and would say whether the 90% and 80%
they were credited with mean anything.
