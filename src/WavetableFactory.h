#pragma once

#include <random>
#include <string>

#include <nlohmann/json.hpp>

/*
    Builds wavetables from scratch.

    A Vital "Wave Source" keyframe is just 2048 little-endian floats in base64,
    and every other component in the format is plain parameters, so a whole
    wavetable can be written directly.

    Doing that per roll is what gives each patch its own timbre: the harmonic
    content is fresh every time and morphs across keyframes, so two patches in a
    style still sound like different instruments. It also keeps the output clear
    of licensed content, which is what makes a generated patch yours to share.
*/
namespace wavetable
{
    struct Request
    {
        std::string style;
        float bright = 0.5f;    // the BRIGHT axis, shifts harmonic rolloff
        float dirt = 0.5f;      // the DIRT axis, adds edge and folding
        float move = 0.5f;      // the MOVE axis, decides how much the table evolves
        float complexity = 0.5f; // how many keyframes and modifiers to build
        int oscillator = 1;     // 1..3, so the three oscillators differ
    };

    /** A complete wavetable object, ready to drop into settings["wavetables"]. */
    nlohmann::json create (std::mt19937& rng, const Request& request);

    /** A plain LFO shape, generated the same way and for the same reasons. */
    nlohmann::json createLfoShape (std::mt19937& rng, float move);

    /*  A squared off LFO, for anything driving pitch.

        A smooth line on a transpose glides between notes, which reads as a siren
        rather than a sequence. Holding each value flat and jumping between them
        is what makes it sound like notes being played.
    */
    nlohmann::json createStepShape (std::mt19937& rng);
}
