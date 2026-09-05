#pragma once

#include <regex>
#include <string>
#include <vector>

/*
    Character axes.

    Each axis acts on its parameters three ways: it biases continuous values,
    flips indexed ones, and switches on the effects it depends on. MOVE also
    adds modulation routings, because motion is a property of the patch's wiring
    rather than of any one knob. Only the first of those is something a Vital
    macro can do on its own, which is why the axes run when a patch is generated
    and the macros are wired afterwards as their playable subset.

    Weights say how hard an axis pulls a parameter, and each one moves its
    parameters within the ranges declared in Archetypes.h rather than anywhere
    the parameter technically allows.
*/
namespace axes
{
    struct Weighted
    {
        std::regex pattern;
        float weight;
    };

    struct MoveWiring
    {
        std::vector<std::string> sources, destinations;
        int minRoutings = 4, maxRoutings = 18;
    };

    struct Axis
    {
        std::string key;                    // "bright"
        std::string label;                  // "BRIGHT"
        std::vector<Weighted> params;
        std::vector<std::regex> flip;
        std::vector<std::string> enables;   // switches the axis turns on
        std::vector<std::string> macroDests;
        bool hasWiring = false;
        MoveWiring wiring;
    };

    /** The four axes, in the order macros are assigned to them. */
    const std::vector<Axis>& all();

    const Axis* find (const std::string& key);

    /*  Destinations that move the pitch of the note.

        A player pressing C expects to hear a C. Modulating an oscillator's tune
        or transpose freely does not read as character, it reads as a patch that
        cannot hold a pitch, and it was turning leads into sound effects. Small
        amounts are vibrato and welcome; large ones are only wanted where the
        wandering is the point.
    */
    bool isPitchDestination (const std::string& destination);

    /** Parameters in `names` that `axis` biases, with their weights. */
    std::vector<std::pair<std::string, float>>
        members (const Axis& axis, const std::vector<std::string>& names);

    /** Indexed parameters in `names` that `axis` may flip. */
    std::vector<std::string>
        flippable (const Axis& axis, const std::vector<std::string>& names);
}
