#pragma once

#include <array>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "Schema.h"
#include "WavetableFactory.h"
#include "StyleModel.h"

/*
    Corpus-driven Vital patch generator.

    Patches are built by splicing real presets rather than by drawing 450
    numbers out of thin air, which produces silence or noise essentially every
    time. Each section is taken whole from a donor preset of the requested
    style, keeping the internal correlations that make a patch sound
    deliberate, and the character axes then re-sample their own parameters from
    the distribution that style actually shows.
*/
namespace gen
{
    inline constexpr int kModSlots = 64;
    inline constexpr int kMacros = 4;

    struct Request
    {
        std::string style;
        std::unordered_map<std::string, float> sliders;   // axis key -> 0..1
        float amount = 1.0f;
        std::set<schema::Section> locks;
        const nlohmann::json* base = nullptr;             // VARY starts from this
        std::set<schema::Section> varySections;           // which sections VARY respices
        unsigned int seed = 0;                            // 0 picks one
        std::string name;
    };

    struct Result
    {
        nlohmann::json preset;
        unsigned int seed = 0;
        std::vector<std::string> repairs;
        std::array<std::string, kMacros> macroNames {};
        std::array<std::string, kMacros> macroDests {};
        int routings = 0;
        bool ok = false;
        std::string error;
    };

    class Generator
    {
    public:
        explicit Generator (const model::StyleModel& m) : styleModel (m) {}

        Result roll (const Request& request);

        /*  Vital's own default sample, taken from a fresh instance.

            Donor presets carry whatever audio their pack author recorded, and
            names like "River" and "Jack Hammer" across the library make it
            clear that is licensed content rather than anything generic. Vital's
            own noise sample ships with the synth, so it is the one safe thing
            to fall back on.
        */
        void setDefaultSample (nlohmann::json sample) { defaultSample = std::move (sample); }

        /** Donor presets are 1 MB or so each and get reused across rolls, so
            they are kept parsed. The cache is bounded because a long session
            would otherwise hold the whole library in memory. */
        void clearDonorCache();
        size_t donorCacheSize() const { return donorCache.size(); }

    private:
        const nlohmann::json* donor (const std::string& path);
        const nlohmann::json* pickDonor (const model::Style& style, std::mt19937& rng);

        bool sampleValue (const model::Style& style, const std::string& param,
                          float percentile, std::mt19937& rng, float& out) const;

        void splice (const model::Style& style, const Request& r,
                     nlohmann::json& doc, std::mt19937& rng);
        void applyAxes (const model::Style& style, const Request& r,
                        nlohmann::json& settings, std::mt19937& rng);
        void wireMovement (const model::Style& style, const Request& r,
                           nlohmann::json& settings, std::mt19937& rng);
        void wireMacros (const model::Style& style, const Request& r,
                         nlohmann::json& doc, nlohmann::json& settings, Result& result);
        void applyChoices (
            const std::unordered_map<std::string, std::vector<std::pair<float, int>>>& choices,
            const Request& r, nlohmann::json& settings, std::mt19937& rng);
        void applyStyleEssentials (const model::Style& style, const Request& r,
                                   nlohmann::json& settings, std::mt19937& rng);
        void applyNoteShape (const model::Style& style, const Request& r,
                             nlohmann::json& settings, std::mt19937& rng);
        void constrainPitch (const Request& r, nlohmann::json& settings, std::mt19937& rng);
        void applyIdentity (const model::Style& style, const Request& r,
                            nlohmann::json& settings, std::mt19937& rng);
        std::vector<std::string> repair (const model::Style& style,
                                         nlohmann::json& settings);
        void synthesiseContent (const Request& r, nlohmann::json& settings,
                                std::mt19937& rng);

        const model::StyleModel& styleModel;
        nlohmann::json defaultSample;
        std::unordered_map<std::string, nlohmann::json> donorCache;
        std::vector<std::string> donorOrder;
    };

    /** Bend a uniform percentile toward one end without piling up on it. */
    float skew (float p, float pull);
}
