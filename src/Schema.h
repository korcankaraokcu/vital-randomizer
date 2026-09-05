#pragma once

#include <string>
#include <vector>

/*
    Parameter classification for Vital's settings block.

    Sections are the unit the randomizer rolls and locks. Each one owns a set
    of numeric parameters and, for some, a structural blob that has to travel
    with it: oscillators own their wavetables and the sample, the LFO section
    owns its shapes. Splitting those apart produces patches whose oscillator
    settings point at a wavetable they were never written for.
*/
namespace schema
{
    enum class Section { osc, filter, env, lfo, fx, mod, global, excluded };

    inline constexpr int numSections = 7;

    Section sectionOf (const std::string& param);

    const char* sectionName (Section s);
    Section sectionFromName (const std::string& name);

    /** Housekeeping that must never be randomised: view state, host-facing
        settings, and anything that makes the patch unusable if it moves. */
    bool isExcluded (const std::string& param);

    /** Genuinely indexed parameters. Counting distinct values across hand-made presets
        cannot tell these apart from parameters that simply sit at their default
        in most presets, so they are matched by name instead. */
    bool isIndexed (const std::string& param);

    /** The section that owns a structural blob ("wavetables", "sample", "lfos"). */
    Section blobOwner (const std::string& blob);
    const std::vector<std::string>& blobNames();
}
