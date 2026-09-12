#pragma once

#include <string>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

/*
    Offline audition.

    A patch that looks plausible as JSON can still be silent, a click, or a wall
    of noise, and none of that is visible without hearing it. Rendering a note
    through a second Vital instance on the worker thread costs about a quarter
    of a second and lets a bad roll be rejected and replaced before the user
    ever hears it.

    It also solves loudness. Peak normalisation is not enough on its own: a
    compressed patch and a transient-heavy one at the same peak are nowhere near
    the same loudness, which is exactly the "some are far too loud, some barely
    audible" problem. Measuring RMS and matching that instead, with a peak
    ceiling to catch clipping, puts every patch where hand-made presets actually
    sit.
*/
namespace audition
{
    struct Measurement
    {
        float rms = 0.0f;           // 90th percentile short-term level
        float meanRms = 0.0f;       // plain average, for reference
        float peak = 0.0f;
        float sustainRms = 0.0f;    // energy while the note is held
        float tailRms = 0.0f;       // energy after note off
        float motion = 0.0f;        // how much the envelope wanders, 0 is static
        float crestDb = 0.0f;       // peak over rms
        float balanceDb = 0.0f;     // left against right
        float centroidHz = 0.0f;    // where the energy sits, roughly

        /*  How much of the sound is actually down low, and whether anything
            jumps out up top.

            Centroid alone was the wrong question for a bass. It is a mean, so
            quiet detail spread across thousands of high bins drags it up even
            when the low end plainly dominates, and the check was throwing away
            basses that sound like basses because they had a little sparkle on
            them. What matters is the ratio of bottom to everything else, and a
            patch is allowed high frequency detail as long as it is quiet.

            `lowRatio` is the share of the energy below 400 Hz. `highSpike` is
            the loudest moment above 2 kHz measured against the loudest moment
            overall, past the attack, since a note-on transient is broadband on
            purpose and a decayed tail that is all treble is inaudible anyway.
        */
        float lowRatio = 0.0f;
        float highSpike = 0.0f;

        /*  Whether the level climbed across the renders rather than wandering.

            Two different things move a patch's level between plays and they
            want opposite answers. Unison phase lands somewhere new at every note
            on, which is scatter, and the mean of a few renders describes it. A
            delay feeding back or a filter near self oscillation builds instead,
            and the mean understates where it ends up: measured across a batch,
            ten patches in forty eight climb, one of them from 0.121 to 0.188.

            Where it climbs the loudest reading is the honest one, because that
            is what a player hears by the third time they press the key.
        */
        bool climbing = false;

        /*  How much the tone moves, as opposed to how much the level does.

            `motion` above measures the envelope, which is the wrong question for
            a patch whose movement is a filter sweeping. Most of what MOVE wires
            goes to cutoff, so the sound changes colour while its loudness sits
            still. This is the spread of the centroid across the note against its
            mean, so a patch that sweeps reads high and a static one reads zero.
        */
        float spectralMotion = 0.0f;

        /*  Late energy against early energy, with the note still held.

            Whether a note keeps going is not something the sustain parameter
            can be trusted to answer. A bass with its sustain at zero still
            drones if an LFO is pushing an oscillator's level back up, and one
            patch doing exactly that measured a rising envelope two seconds into
            a held note. Measuring what the note does settles it whatever the
            cause happens to be.
        */
        float heldRatio = 1.0f;

        /*  What note the patch actually sounds, and how definite it is.

            Capping pitch modulation was not enough on its own. A patch can hold
            still and be in the wrong place: partials shifted off the harmonic
            series put the pitch somewhere that is not a note at all, and a patch
            with no bottom leaves the ear latching onto a high harmonic instead
            of the fundamental. Both read the same way to a player, as a key that
            does not give back the note they pressed.
        */
        float pitchHz = 0.0f;
        float pitchSalience = 0.0f;     // 1 is a clear steady note, 0 is noise
        float pitchErrorSemitones = 0.0f;   // distance from the nearest octave
        float pitchOffGridSemitones = 0.0f; // distance from the nearest semitone

        /*  The same three, measured a step at a time.

            A sequence is meant to move, so asking whether it holds one pitch
            asks the wrong question. The window above is 0.35s and a synced LFO
            steps every 0.125s at 120 BPM, so it spans about three different
            notes; autocorrelation smears them into one smeared answer with a
            low salience. Patches that sound perfectly clean measured 0.20.

            These split the note into windows short enough to hold a single step
            and report the median across them, so a clean sequence of clean notes
            reads as what it is.
        */
        float stepSalience = 0.0f;
        float stepErrorSemitones = 0.0f;
        float stepOffGridSemitones = 0.0f;
        int steps = 0;
        bool silent = false;
        bool clickOnly = false;
        bool clipping = false;
        bool tooPeaky = false;
        bool lopsided = false;

        bool usable() const
        {
            return ! silent && ! clickOnly && ! tooPeaky && ! lopsided;
        }
    };

    /*  Where a style's energy is allowed to sit, in Hz.

        Measured once across a library of hand-made presets, with the same code
        that measures a candidate. They land at a median of 632 Hz for bass,
        1164 for keys, 1432 for pad and 2789 for lead, so a generated bass
        reading 8 kHz is not a dark bass or a bright bass, it is not a bass. The
        ceilings sit above each style's median with room for character, and
        tighter than their p90 for bass, because a bass is the one style where
        the whole point is the bottom end.
    */
    struct Brightness { float low, high; };

    /*  `complexity` widens the ceiling a little.

        More oscillators, a noise layer and a second filter all add harmonics,
        so a patch asked to use more of the synth genuinely reads brighter.
        Holding it to the same ceiling as a sparse one rejected almost every
        complex patch rather than every wrong one.
    */
    Brightness brightnessFor (const std::string& style, float complexity = 0.5f);

    /*  The low end a style has to have, and the loudest it may jump up top.

        Zero and one mean the style does not care. Used instead of the centroid
        where a style is defined by its balance rather than by where its mean
        happens to land.
    */
    struct BalanceRule { float minLowRatio, maxHighSpike; };
    BalanceRule balanceFor (const std::string& style);

    /** Sentinel meaning a style may ring for as long as it likes. */
    inline constexpr float kNoHeldLimit = 1.0e9f;

    /** How much of a held note a style may still have late on. */
    float maxHeldRatioFor (const std::string& style);

    /*  Whether a style has to give back the note that was pressed.

        A bass, a key, a lead and a sequence are all played as melodies, so the
        pitch has to be the one on the keyboard. Effects and experiments are
        under no such obligation, and percussion is mostly not pitched at all.
    */
    /*  `octavesOnly` says whether the note has to be the one that was pressed.

        For a melodic style it does: an octave is the same note, a fifth is not.
        A sequence is different, because stepping between pitches is the whole
        idea. Its steps are quantised to semitones elsewhere, so measuring one
        against the root and calling a perfect fourth wrong rejected patches
        that were doing exactly what they were designed to.
    */
    /*  Whether a style's pitch should be read a step at a time.

        A sequence steps on purpose, so its note presence and its tuning are
        properties of each step rather than of the whole note.
    */
    bool isSteppedStyle (const std::string& style);

    struct PitchRule
    {
        bool required;
        float minSalience, maxErrorSemitones;
        bool octavesOnly = true;
    };
    PitchRule pitchRuleFor (const std::string& style);

    /*  Let a freshly loaded patch settle.

        Vital's first render after taking a new state is not the settled patch,
        so one has to be thrown away before anything is measured. A full
        audition to do that costs as much as the measurement itself and a roll
        already runs several of them, so this plays a short note and discards it.
    */
    void settle (juce::AudioProcessor& synth, double sampleRate, int blockSize,
                 int note = 48);

    /** Render a held note through a prepared synth and measure what came back. */
    /*  Several renders averaged, because a patch does not measure the same way
        twice.

        Vital randomises unison phase at every note on and its modulators run
        free, so one render of one patch reads a level anywhere across a couple
        of decibels and a pitch across a fraction of a semitone. Every decision
        the screen makes was being taken from a single draw of that.

        It showed worst in the loudness correction, which was measuring once,
        correcting by what it saw, measuring once again and correcting by
        something else, so a patch could bounce either side of the target
        forever and be thrown out for never settling. Averaging first is what
        makes the correction converge rather than chase.
    */
    Measurement auditionAveraged (juce::AudioProcessor& synth, double sampleRate,
                                  int blockSize, int times = 3, int note = 48);

    Measurement audition (juce::AudioProcessor& synth, double sampleRate,
                          int blockSize, int note = 48, bool resetFirst = true);
}
