# Roadmap

Open work, roughly in the order I would do it. Anything measured is measured, so
the numbers here come from `vrtest` and from rendering through Vital rather than
from impressions.

## 1. The reject count is too noisy to judge a change by

Five runs of the same code, six patches a style, gave between 19 and 61 rejected
candidates, and bass was thrown out for droning between 0 and 18 times. One run
filled every slot without a single reject and the next needed nine goes at one
bass.

The 48 presets a run keeps are fine. It is the count of what it threw away
getting there that swings, and that range is wider than most of the changes
anybody would want to test, so the tally cannot tell a real improvement from a
lucky run. It was very nearly used that way twice today. The style summary is steadier, because it
averages over what got through rather than counting rare events, but anything
event shaped needs far more patches than a listening batch wants to be.

`--per-style=16` exists and takes about an hour. Something that fills a batch and
keeps rolling for statistics without writing 128 files would be better.

## 2. A bass that keeps going, when it happens

The cause named in the notes is closed: a cyclic source wired to an oscillator's
level pushed the note back up with the key still down, and bass and percussion
now drop those routings. Verified structurally, 0 of 6 in a batch where it used
to be 1 of 6.

Whether that fixed the rejections cannot be told from the batches, per item 1.
The accepted basses are clean, sustain at zero and no tail, so if there is
anything left it is most likely the decay sitting at the top of its 1.08 to 1.30
band and lasting long enough to trip the held note check.

## 3. DIRT is scored on a number that may not be measuring it

Agreement by axis: BRIGHT 97%, SPACE 93%, MOVE 77%, DIRT 72%.

DIRT is scored on crest, on the reasoning that distortion fills in the gap
between peak and average. That is true, and the level matching pass also changes
peak against average, so the number may be reading the loudness correction rather
than the axis. Three metrics turned out to be measuring the wrong thing in a
single day, so this one deserves the same suspicion before anybody concludes the
axis is weak. High harmonic energy or spectral flatness would settle it.
