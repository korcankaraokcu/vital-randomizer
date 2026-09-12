#pragma once

#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

/*
    Builds the sample layer from scratch.

    Vital's sample slot holds one second of mono sixteen bit audio in plain
    base64, which means it can be written the same way the wavetables are. Until
    now every patch got Vital's own white noise, so a layer whose job is to add
    character added the same flat hiss to all of them, and because white noise is
    flat to the top of the range it cost far more brightness than it bought: keys
    patches reading seven kilohertz turned into ordinary ones the moment it was
    muted.

    Nothing here randomises audio. Random samples are noise, which is exactly the
    thing being replaced. What is randomised is the parameters of a synthesis
    model, so every result is a different instance of a sound that exists rather
    than a different arrangement of dust. That is the same rule the wavetables
    follow and the reason they came out musical.
*/
namespace sampler
{
    struct Request
    {
        std::string style;
        float bright = 0.5f;    // where the energy of the layer sits
        float dirt = 0.5f;      // how rough and inharmonic it is allowed to be
        float complexity = 0.5f;
        /*  No looping bed, whatever else. A struck style has to stop
            while the key is still down and a bed does not stop at all. */
        bool struckOnly = false;
        /*  Name one and get that one. Empty picks from what the style allows,
            which is what generating a patch wants; naming it is for hearing a
            model on its own, with nothing else in the preset to judge it
            through.
        */
        std::string model;
    };

    struct Result
    {
        nlohmann::json sample;  // ready to drop into settings["sample"]
        bool loop = true;       // a bed repeats, a struck sound does not
        bool keytrack = false;  // only what has a pitch should follow the keyboard
        /*  How loud to run it, which only the factory can know.

            Peak normalising leaves sparse content far quieter on average than
            dense content: grit measures 13 dB below a bed at the same peak,
            because most of it is silence. Setting one level for all of them
            makes half the models inaudible and the other half overbearing.
        */
        float level = 0.15f;
    };

    /** One second of audio built from a model chosen to suit the style. */
    Result create (std::mt19937& rng, const Request& request);

    /** Every model there is, in the order they were written. */
    std::vector<std::string> modelNames();
}
