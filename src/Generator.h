#pragma once

#include <array>
#include <random>
#include <set>
#include <string>
#include <utility>
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
        /*  One for a fresh roll. On VARY it is the depth, which scales both how
            likely each parameter is to move and how far it goes. */
        float amount = 1.0f;
        /*  How far a roll may wander from the complexity it was handed. One is
            the normal spread that keeps a batch from sounding uniform. Zero
            pins it, which is what a deliberate sweep across the range needs.
        */
        float complexityWobble = 1.0f;
        std::set<schema::Section> locks;
        const nlohmann::json* base = nullptr;             // VARY starts from this
        /*  On VARY, the sections whose settings may move. The LFO rates and the
            modulation depths move as well, in varyMotion, unless their own
            sections are locked. */
        std::set<schema::Section> varySections;
        unsigned int seed = 0;                            // 0 picks one
        /*  Which scale a sequence walks. Negative picks one, which is what
            generating a patch wants; naming it is for hearing a scale on its
            own with the rest of the patch held still. */
        int scale = -1;
        /*  Which shape the line takes. Negative picks one. Naming it is for
            hearing one gesture against another with everything else held. */
        int gesture = -1;
        /*  Which distortion circuit. Negative lets DIRT choose, which is what a
            roll wants; naming one is for hearing the six against each other. */
        int distortionType = -1;
        /*  Which kind of percussion, as an index into drums::all(). Negative
            draws one. A VARY ignores it and keeps the kind of the patch it
            started from. */
        int drumKind = -1;
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
        /*  How far the roll was allowed to move, 1 for a fresh one and the
            depth for a VARY. Reported so history records the depth that was
            actually used rather than whatever the slider says by the time the
            result lands. */
        float amount = 1.0f;
        /*  The scale a sequence ended up walking, as an index into
            sequenceScales(). Reported rather than assumed, because a request
            that asked for a random scale has no other way of saying which one
            it got, and history has to replay the same one. */
        int scale = -1;
        /*  The kind of percussion the patch was built as, or -1. Reported for
            the same reason as the scale: the screen judges a kick and a hi-hat
            by different rules, and history has to replay the same one. */
        int drumKind = -1;
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
        void constrainPitch (const Request& r, nlohmann::json& settings, std::mt19937& rng,
                             int& scaleUsed);
        /** Put every pitched voice on the same steps, so the riff is the patch
            rather than something happening behind a held note. */
        void carrySequenceToEveryVoice (nlohmann::json& settings,
                                        const std::string& driver, float depth);
        void scaleModulationDepth (const Request& r, nlohmann::json& settings);
        /** On VARY, move the LFO rates and the modulation depths by the depth. */
        void varyMotion (const Request& r, nlohmann::json& settings, unsigned int seed);
        /** Give oscillators a warp type, more often the higher COMPLEX is. */
        void chooseWarp (const Request& r, nlohmann::json& settings, unsigned int seed);
        /** Give oscillators a spectral morph type, so the morph amount does
            something, and keep the amount where that type is safe. */
        void chooseMorph (const Request& r, nlohmann::json& settings, unsigned int seed);
        /** The effects' own modes, which nothing else draws: the delay's
            style and timing, the chorus's voices, the filters' slope and the
            EQ's low band. */
        void chooseEffectModes (const Request& r, nlohmann::json& settings, unsigned int seed);
        /** Keep what LFOs and random sources do to the tone gentle, and a
            delay's echoes from outlasting the note by much. */
        void keepMotionAndEchoesInHand (const Request& r, nlohmann::json& settings);
        /** Hold a drum to the bands of its kind, after everything else. */
        void shapeDrum (const Request& r, nlohmann::json& settings, unsigned int seed);
        /** Switch the distortion on or off from DIRT, before the wiring. */
        void switchDistortion (const Request& r, nlohmann::json& settings);
        /** Set the distortion from DIRT and hold whatever moves the drive
            inside the band DIRT allows. */
        void shapeDistortion (const Request& r, nlohmann::json& settings, unsigned int seed);
        void keepStruckNotesStruck (const Request& r, nlohmann::json& settings);
        /** Put a patch back inside the rules every style shares. A drum kind
            is exempt from the ones its design breaks on purpose. */
        std::vector<std::string> repair (nlohmann::json& settings, int drumKind = -1);

        nlohmann::json initPreset;
    };

    /** A named set of intervals, in semitones above the note played. May be
        fractional, which is what a maqam needs. */
    struct Scale
    {
        const char* name;
        std::vector<float> degrees;
        /*  For the entries with no degrees, how far apart the steps may land,
            in semitones.

            A whole semitone is left to Vital's own quantiser, which is a twelve
            bit mask and so cannot express anything narrower. Anything smaller
            has to be placed by hand, with the quantiser switched off, the same
            way the maqamat place their quarter tones.
        */
        float randomStep = 0.0f;
    };

    /** Where the distortion drive may sit, as fractions of Vital's knob. */
    struct DriveBand { float restLow = 0.0f, restHigh = 0.0f, ceiling = 0.0f; };

    /** The drive band a DIRT setting asks for. */
    DriveBand driveBandFor (float dirt);

    /** Every scale a sequence may be built on. */
    const std::vector<Scale>& sequenceScales();

    /*  The same scales with the repeats taken out, each paired with its index
        into the table above.

        The table repeats a few entries so that a random pick leans toward the
        ones that survive a random walk. A menu offering "minor pentatonic"
        twice would just look like a mistake, so anything a person reads gets
        this list instead.
    */
    const std::vector<std::pair<std::string, int>>& sequenceScaleMenu();

    /** Bend a uniform percentile toward one end without piling up on it. */
    float skew (float p, float pull);
}
