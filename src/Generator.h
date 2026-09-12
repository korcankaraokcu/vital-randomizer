#pragma once

#include <array>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Archetypes.h"
#include "Schema.h"

/*
    Builds a Vital patch.

    Every patch starts from Vital's own init state with a designed archetype
    applied over it, so a bass begins life as a bass. The character axes then
    move the parameters they own, a jitter nudges the rest inside explicit
    ranges, the wavetables are synthesised, the modulation is wired, and the
    result is repaired into something that will make a sound.

    Everything it needs is in this repository and in Vital's own init state, so
    a fresh Vital install is enough to roll a patch.
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
        /*  How far a roll may wander from the complexity it was handed. One is
            the normal spread that keeps a batch from sounding uniform. Zero
            pins it, which is what a deliberate sweep across the range needs.
        */
        float complexityWobble = 1.0f;
        std::set<schema::Section> locks;
        const nlohmann::json* base = nullptr;             // VARY starts from this
        std::set<schema::Section> varySections;
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
        Generator() = default;

        /** Vital's own init patch, read from a fresh instance. Every roll is
            built on top of it, so the generator needs it before it can work. */
        void setInitPreset (nlohmann::json preset) { initPreset = std::move (preset); }
        bool isReady() const { return ! initPreset.is_null(); }

        Result roll (const Request& request);

        static std::vector<std::string> styles() { return archetype::styleNames(); }

    private:
        void applyArchetype (const archetype::Archetype& a, const Request& r,
                             nlohmann::json& settings);
        void applyComplexity (const Request& r, nlohmann::json& settings, std::mt19937& rng);
        void synthesiseSample (const Request& r, nlohmann::json& settings,
                               std::mt19937& rng);
        void synthesiseContent (const Request& r, nlohmann::json& settings,
                                std::mt19937& rng);
        void applyAxes (const Request& r, nlohmann::json& settings, std::mt19937& rng);
        void applyNoteShape (const Request& r, nlohmann::json& settings, std::mt19937& rng);
        void wireRouting (const archetype::Archetype& a, const Request& r,
                          nlohmann::json& settings, std::mt19937& rng);
        void wireMacros (const Request& r, nlohmann::json& doc,
                         nlohmann::json& settings, Result& result);
        void constrainPitch (const Request& r, nlohmann::json& settings, std::mt19937& rng);
        void scaleModulationDepth (const Request& r, nlohmann::json& settings);
        void tameDriveModulation (nlohmann::json& settings);
        void keepStruckNotesStruck (const Request& r, nlohmann::json& settings);
        std::vector<std::string> repair (nlohmann::json& settings);

        nlohmann::json initPreset;
    };

    /** Bend a uniform percentile toward one end without piling up on it. */
    float skew (float p, float pull);
}
