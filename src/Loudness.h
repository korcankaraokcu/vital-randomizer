#pragma once

#include <nlohmann/json.hpp>

/*
    Loudness matching for generated patches.

    Random patches land across a 23 dB range, so one candidate is inaudible and
    the next one is painful. Matching peaks does not fix this: a compressed
    patch and a transient-heavy one at the same peak are nowhere near the same
    loudness. RMS is what the ear tracks, so that is what gets matched, with a
    peak ceiling on top to catch anything that would clip.

    The targets come from measuring hand-made presets out of a real library
    through the same code the plugin uses. Measuring them any other way is
    worthless: the level here is the 90th percentile of the
    short-term windows, and a target carried over from a plain average is just a
    number that happens to look reasonable. Those presets sit at a median level
    of 0.135 and a p90 peak of 0.799, so a generated patch aimed at that lands
    where one somebody wrote by hand would.

    The volume table is measured too. Vital's `volume` parameter runs 0 to
    7399.44 on a square root scale whose exact mapping to output amplitude is
    not worth reverse engineering, so it was swept against a rendered note and
    recorded as relative dB.
*/
namespace loudness
{
    inline constexpr float kTargetRms = 0.135f;    // median of hand-made presets
    inline constexpr float kPeakCeiling = 0.80f;   // p90 of hand-made presets
    inline constexpr float kMaxBoostDb = 24.0f;    // do not amplify near-silence
    inline constexpr float kMaxCutDb = -30.0f;

    inline constexpr float kVolumeMin = 1000.0f;
    inline constexpr float kVolumeMax = 7399.0f;
    inline constexpr float kVolumeDefault = 5473.04f;

    /** The volume parameter that shifts output by `gainDb` from where it is. */
    float volumeFor (float currentVolume, float gainDb);

    /** How much to move a patch measuring `rms` / `peak` to sit where hand-made
        presets do. Returns dB, clamped, and zero when it is already close. */
    float correctionDb (float rms, float peak);

    /** Apply `correctionDb` to the patch's own master volume.

        Baking it into the patch rather than trimming the plugin's output means
        the level travels with the preset when it is exported, and the level a
        listener hears does not depend on the plugin having measured it first.
        Returns the dB actually applied after the volume parameter is clamped.
    */
    float normalise (nlohmann::json& settings, float rms, float peak);
}
