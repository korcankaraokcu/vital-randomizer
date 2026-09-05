#pragma once

#include <random>
#include <string>

#include <nlohmann/json.hpp>

/*
    Builds wavetables from scratch.

    Splicing wavetables out of donor presets was the fastest way to get a patch
    that sounded like something, but it carries the wavetable data of whatever
    pack the donor came from. That is fine for listening to on your own machine
    and not fine for anything you might share, since commercial packs are
    licensed content and a generated patch would be carrying it verbatim.

    So the tables here are synthesised. A Vital "Wave Source" keyframe is just
    2048 little-endian floats in base64, and every other component in the format
    is plain parameters, which means the whole thing can be written rather than
    borrowed. Nothing in a generated patch comes from anybody's pack.

    It turns out to help the sound too. A donor's wavetable is one fixed table,
    so patches built from the same donors kept arriving with the same handful of
    timbres. Synthesising the harmonic content per roll, and morphing it across
    keyframes, is where the variety comes from.
*/
namespace wavetable
{
    struct Request
    {
        std::string style;
        float bright = 0.5f;    // the BRIGHT axis, shifts harmonic rolloff
        float dirt = 0.5f;      // the DIRT axis, adds edge and folding
        float move = 0.5f;      // the MOVE axis, decides how much the table evolves
        int oscillator = 1;     // 1..3, so the three oscillators differ
    };

    /** A complete wavetable object, ready to drop into settings["wavetables"]. */
    nlohmann::json create (std::mt19937& rng, const Request& request);

    /** A plain LFO shape, for the same reason: donor LFO shapes are authored
        content, and generating them costs almost nothing. */
    nlohmann::json createLfoShape (std::mt19937& rng, float move);

    /*  A squared off LFO, for anything driving pitch.

        A smooth line on a transpose glides between notes, which reads as a siren
        rather than a sequence. Holding each value flat and jumping between them
        is what makes it sound like notes being played.
    */
    nlohmann::json createStepShape (std::mt19937& rng);
}
