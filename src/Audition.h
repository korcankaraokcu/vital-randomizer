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

        Measured from the library with the same code that measures a candidate
        (`vrtest --corpus=40 --styles=Bass`). Hand-made presets land at a median
        of 632 Hz for bass, 1164 for keys, 1432 for pad and 2789 for lead, so a
        generated bass reading 8 kHz is not a dark bass or a bright bass, it is
        not a bass. The ceilings sit above each style's median with room for
        character, and tighter than the corpus p90 for bass because a bass is
        the one style where the whole point is the bottom end.
    */
    struct Brightness { float low, high; };
    Brightness brightnessFor (const std::string& style);

    /** Sentinel meaning a style may ring for as long as it likes. */
    inline constexpr float kNoHeldLimit = 1.0e9f;

    /** How much of a held note a style may still have late on. */
    float maxHeldRatioFor (const std::string& style);

    /*  Whether a style has to give back the note that was pressed.

        A bass, a key, a lead and a sequence are all played as melodies, so the
        pitch has to be the one on the keyboard. Effects and experiments are
        under no such obligation, and percussion is mostly not pitched at all.
    */
    struct PitchRule { bool required; float minSalience, maxErrorSemitones; };
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
    Measurement audition (juce::AudioProcessor& synth, double sampleRate,
                          int blockSize, int note = 48, bool resetFirst = true);
}
