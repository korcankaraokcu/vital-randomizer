#pragma once

#include <string>
#include <vector>

#include "Archetypes.h"

/*
    The kinds of percussion, each a target the generator aims at and the screen
    holds it to.

    Percussion used to be one generic hit: a metallic or square oscillator, a
    struck sample and short envelopes, judged on almost nothing, since any
    brightness from 0 to 12 kHz passed and there was no pitch check. A kick and a
    hi-hat are different instruments built from different parts, and a patch
    that is neither is what that produced.

    Every band here is in Vital's own units, taken from measurement rather than
    from the knob's label. The envelope decay setting and the time a hit takes to
    fall 40 dB were swept on a plain tone: 0.45 is 28 ms, 0.6 is 86 ms, 0.7 is
    162 ms, 0.8 is 270 ms, 0.9 is 446 ms, 1.0 is 676 ms, 1.1 is 0.95 s, 1.2 is
    1.36 s, 1.3 is 1.86 s and 1.45 is 2.8 s. A filter blend of 0 is low pass, 1
    band pass and 2 high pass. A pitch drop is an envelope on the oscillator's
    transpose, which moves 96 semitones per unit of depth. Cutoff is in Vital's
    note units: 60 is 262 Hz, 80 is 831 Hz, 90 is 1.5 kHz, 100 is 2.6 kHz, 110
    is 4.7 kHz and 120 is 8.4 kHz. The shared ranges stop the cutoff at 110,
    which is too low for a hi-hat, so a drum holds its own cutoff instead.
*/
namespace drums
{
    struct Band
    {
        float low = 0.0f, high = 0.0f;
    };

    struct Kind
    {
        const char* name;

        // Where the patch starts, on top of the percussion archetype.
        std::vector<archetype::Setting> settings;
        std::vector<archetype::Routing> routings;

        /*  The body. Tonal kinds carry a pitched oscillator that follows the
            keyboard, at this many semitones from the note played. The noise
            kinds have none, and are the sample layer alone. */
        bool tonal = true;
        const char* character = "fundamental";   // wavetable character
        Band body;                                // oscillator one's level, when held
        Band inharmonic;                          // how far partials leave the series
        float transpose = 0.0f;
        float secondTranspose = -12.0f;           // oscillator two, when it is on

        // The sample layer: a model name, or empty for none.
        const char* sample = "";
        Band sampleLevel;
        Band metalNoise;                          // noise mixed into a metal sample
        Band metalCorner;                         // where a metal sample's tilt turns, Hz
        int metalBanks = 1;                       // sets of six squares in it

        // The amplitude envelope, in Vital's units. Sustain is zero unless given.
        Band attack, decay, release, sustain;

        // Filter one, which shapes the whole hit.
        Band blend;
        /*  The cutoff heard at a normal strike, with the macros where they sit.
            Velocity and a macro on the cutoff both add to it for as long as
            the note lasts, and a macro rests halfway up its travel, so the
            knob itself is set below this by however much they add. */
        Band cutoff;
        // How far the second envelope opens the filter at the strike, semitones.
        Band sweep;
        float maxResonance = 0.3f;
        bool secondFilter = true;                 // whether filter two may stay

        // A pitch that falls into the note: semitones, and how fast it settles.
        Band drop;
        Band dropDecay;

        // A burst of quick hits before the body, the way a clap is made.
        bool flam = false;
        // Two strokes to a cycle in time with the song, for as long as the key
        // is held, the way a shaker is played.
        bool shake = false;
        // Reverb on whatever SPACE says, since this drum is never heard dry.
        bool room = false;

        // What the screen expects.
        Band brightness;                          // energy centroid, Hz
        float minLowRatio = 0.0f;                 // share of energy below 400 Hz
        float maxHeld = 0.6f;                     // level left late in the hold
        bool pitched = false;                     // must sound the note played
    };

    /** Every kind, in the order the menu lists them. */
    const std::vector<Kind>& all();

    /** A kind by index into all(), or nullptr. */
    const Kind* at (int index);

    /** The index of a kind by name, or -1. */
    int indexOf (const std::string& name);

    /*  The profile the screen judges a patch by: the style, and for percussion
        the kind after a slash, as in "Percussion/Kick". */
    std::string profile (const std::string& style, int kind);

    /** The kind a profile names, or nullptr for any other style. */
    const Kind* fromProfile (const std::string& profile);

    /** The style part of a profile. */
    std::string styleOf (const std::string& profile);
}
