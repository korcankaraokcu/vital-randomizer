#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

/*
    What the randomizer knows about each preset style, learned from the user's
    own installed Vital library rather than shipped with the plugin.

    Reading the user's library instead of bundling data means the randomizer
    learns from whatever packs they already own, the plugin stays small, and no
    preset data derived from commercial packs is redistributed.
*/
namespace model
{
    /*  Bump this whenever the shape of the model changes.

        The cache is written once and reused forever, so a model that gained a
        field reads back with that field missing and silently falls back to a
        default. That is worse than no cache: everything still works, nothing
        errors, and the feature the field was added for just quietly does
        nothing.
    */
    inline constexpr int kModelVersion = 8;

    inline constexpr int kQuantiles = 21;
    using Curve = std::array<float, kQuantiles>;

    struct Style
    {
        int count = 0;
        bool blended = false;
        std::vector<std::string> donors;                             // .vital paths
        std::unordered_map<std::string, Curve> continuous;
        std::unordered_map<std::string, std::vector<std::pair<float, int>>> discrete;
        std::vector<std::pair<std::string, int>> macroDests;         // most used first

        /*  How many modulation routings presets in this style actually use.
            A patch with only its macros wired is static, and static is what
            "random garbage" usually sounds like, so this is the floor the
            generator tops up to rather than a number pulled out of the air.
        */
        int medianRoutings = 9;

        /*  The handful of parameters that decide whether a patch reads as its
            style at all, with the value distribution presets in that style
            actually use.

            These drift during splicing and jitter like anything else, but
            unlike a filter cutoff they are structural: a lead that came out
            monophonic and a pad that lost its unison are not variations on the
            style, they are the wrong instrument. Sampling them from the corpus
            rather than pinning them to the median keeps the intentional spread,
            including things like a second oscillator sitting a fifth up.
        */
        std::unordered_map<std::string, std::vector<std::pair<float, int>>> identity;

        /*  Choices that have to be made before the character axes run, because
            the axes are entitled to override them.

            Which effects a style switches on is not a detail: bass runs
            distortion in three quarters of presets but reverb in under half,
            while a pad is reverb in almost all of them and chorus in most. Left
            to the donor these drifted, and a bass drenched in reverb is not a
            bass. Pushing SPACE should still be able to turn the reverb on, so
            these are applied first rather than enforced at the end.
        */
        std::unordered_map<std::string, std::vector<std::pair<float, int>>> early;

        /*  Which modulation routings this style actually uses, most common
            first, and how deep it runs them.

            This is what tells a bass from a drone. A bass is plucky because a
            decaying envelope closes its filter, which is the single most common
            routing in the style, while a sequence is LFOs sweeping pitch and
            level. Wiring both from one hardcoded list produced basses with
            seven LFOs at full depth and no filter envelope at all, which is a
            fine experimental patch and not a bass.

            Macro routings are excluded, since those are wired separately.
        */
        struct Routing { std::string source, destination; int count = 0; };
        std::vector<Routing> modRoutings;
        Curve modDepth {};              // |amount| quantiles, 0 when unknown
        bool hasModDepth = false;

        /** Value at a percentile in 0..1, interpolated along the curve. */
        float sample (const Curve& curve, float percentile) const;
        /** Where a value sits on its own curve, in 0..1. */
        static float percentileOf (const Curve& curve, float value);
    };

    class StyleModel
    {
    public:
        bool loadFromFile (const juce::File& file);
        bool saveToFile (const juce::File& file) const;

        /** Walk a Vital library and learn from it. `progress` is called with a
            0..1 fraction and may be null. Returns the number of presets read. */
        int buildFromLibrary (const juce::File& root,
                              std::function<void (float)> progress = {});

        bool isReady() const                       { return ! styles.empty(); }
        int presetCount() const                    { return totalPresets; }
        std::vector<std::string> styleNames() const;
        const Style* style (const std::string& name) const;

        /** Where the cached model lives between sessions. */
        static juce::File defaultCacheFile();
        /** Best guess at the user's Vital library root. */
        static juce::File defaultLibraryRoot();

    private:
        std::unordered_map<std::string, Style> styles;
        int totalPresets = 0;
    };
}
