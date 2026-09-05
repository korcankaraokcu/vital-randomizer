#pragma once

#include <string>
#include <vector>

/*
    A designed starting patch for each style.

    An archetype is coherent by construction, which is the property that makes
    the whole generator work: a bass here is a bass because it was designed as
    one.

    Each is a short list of overrides on top of Vital's own init patch, because
    that is what a preset actually is. Measured across a real library, a
    hand-made preset changes about 81 parameters out of 756 and leaves the rest
    alone. The values below are design decisions informed by what the instrument
    usually does, which is why a bass sits near a cutoff of 56 and a lead near
    86.

    Variety arrives later, from the synthesised wavetables, the character axes,
    the jitter and the per-style rules applied afterwards. The archetype only has
    to be a good place to start.
*/
namespace archetype
{
    struct Setting
    {
        const char* name;
        double value;
    };

    struct Routing
    {
        const char* source;
        const char* destination;
        float amount;
        bool bipolar;
    };

    struct Archetype
    {
        std::string style;
        std::vector<Setting> settings;
        std::vector<Routing> routings;
    };

    /*  What a parameter is allowed to be.

        An explicit low and high per parameter keeps every value musical, and
        keeps the bounds where somebody can read them and argue with them.

        A parameter with no entry here stays wherever the archetype put it. That
        is the deliberate part: motion spread thinly across four hundred
        parameters blurs the design, so only the ones listed here move, and they
        move within stated bounds.
    */
    struct Range
    {
        const char* pattern;    // regex, matched against the whole name
        float low, high;
    };

    const std::vector<Range>& ranges();

    /** Low and high for a parameter, or false when it should not be varied. */
    bool rangeFor (const std::string& parameter, float& low, float& high);

    /*  Where a style's sliders start: the four character axes, then complexity.

        Dead centre is the wrong default. Everything in the middle asks nothing
        of the generator and produces the least characteristic patch it can make,
        and a bass that sounds like a pad is not a useful starting point.

        This lives here rather than in the plugin so the test harness and the
        plugin roll the same patches. They did not for a while, and a bass tested
        at a neutral brightness came out far brighter than the one a user would
        ever hear.
    */
    struct Sliders { float bright, move, dirt, space, complexity; };
    Sliders defaultSlidersFor (const std::string& style);

    /** Where this style's macros should point, most wanted first. */
    const std::vector<const char*>& macroDestinationsFor (const std::string& style);

    /** Every style the generator can produce. */
    const std::vector<Archetype>& all();

    /** Null when the style is unknown. */
    const Archetype* find (const std::string& style);

    std::vector<std::string> styleNames();
}
