#include "Generator.h"

#include <algorithm>
#include <cmath>
#include <regex>

#include <juce_core/juce_core.h>

#include "Axes.h"
#include "Drums.h"
#include "Loudness.h"
#include "SampleFactory.h"
#include "WavetableFactory.h"

namespace gen
{
    namespace
    {
        float uniform (std::mt19937& rng)
        {
            return std::uniform_real_distribution<float> (0.0f, 1.0f) (rng);
        }

        float gaussian (std::mt19937& rng, float sigma)
        {
            return std::normal_distribution<float> (0.0f, sigma) (rng);
        }

        std::vector<std::string> numericKeys (const nlohmann::json& settings)
        {
            std::vector<std::string> keys;
            keys.reserve (settings.size());
            for (auto it = settings.begin(); it != settings.end(); ++it)
                if (it.value().is_number())
                    keys.push_back (it.key());
            return keys;
        }

        void ensureModSlots (nlohmann::json& settings)
        {
            if (! settings.contains ("modulations") || ! settings["modulations"].is_array())
                settings["modulations"] = nlohmann::json::array();
            auto& mods = settings["modulations"];
            while (static_cast<int> (mods.size()) < kModSlots)
                mods.push_back ({ { "destination", "" }, { "source", "" } });
        }

        std::string modSource (const nlohmann::json& slot)
        {
            if (slot.is_object() && slot.contains ("source") && slot["source"].is_string())
                return slot["source"].get<std::string>();
            return {};
        }

        std::string modDest (const nlohmann::json& slot)
        {
            if (slot.is_object() && slot.contains ("destination")
                && slot["destination"].is_string())
                return slot["destination"].get<std::string>();
            return {};
        }

        /** A stream for one named parameter, independent of every other draw.
            FNV-1a rather than std::hash, so a recipe replays the same patch on
            every platform and compiler. */
        std::mt19937 streamFor (unsigned int base, const std::string& key, unsigned int salt)
        {
            std::uint32_t h = 2166136261u ^ salt;
            for (const auto c : key)
            {
                h ^= (std::uint32_t) (unsigned char) c;
                h *= 16777619u;
            }
            return std::mt19937 (base ^ h);
        }

        bool modDestExists (const nlohmann::json& mods, const std::string& source,
                            const std::string& dest)
        {
            for (const auto& slot : mods)
                if (modSource (slot) == source && modDest (slot) == dest)
                    return true;
            return false;
        }

        bool setModSlot (nlohmann::json& settings, const std::string& source,
                         const std::string& dest, float amount, bool bipolar)
        {
            if (source.empty() || dest.empty() || ! settings.contains (dest))
                return false;

            auto& mods = settings["modulations"];

            /*  One slot per pair, because Vital adds them up.

                Two slots carrying the same source to the same destination are
                not a stronger connection shown twice, they are two connections
                that sum: an LFO at 0.83 and the same LFO at 0.36 on one
                oscillator's level reach past full scale between them and pin it
                there. Vital's own editor shows one line for the pair, so the
                second is invisible as well as wrong, and it costs a slot that
                could have moved something else.
            */
            if (modDestExists (mods, source, dest))
                return false;

            for (int slot = 0; slot < kModSlots; ++slot)
            {
                const auto index = static_cast<size_t> (slot);
                if (! modSource (mods[index]).empty())
                    continue;

                mods[index] = { { "destination", dest }, { "source", source } };
                const auto n = std::to_string (slot + 1);
                settings["modulation_" + n + "_amount"] = amount;
                settings["modulation_" + n + "_bypass"] = 0.0;
                settings["modulation_" + n + "_bipolar"] = bipolar ? 1.0 : 0.0;
                settings["modulation_" + n + "_power"] = 0.0;
                settings["modulation_" + n + "_stereo"] = 0.0;
                return true;
            }
            return false;
        }

        // Real preset authors name macros after the thing they move: REVERB,
        // CUTOFF and DELAY are the three most common names in a typical library.
        // Deriving the name from the destination gets most of the way there,
        // but a few read better with a shorter word than the parameter gives.
        const std::vector<std::pair<std::regex, std::string>>& nameRules()
        {
            static const std::vector<std::pair<std::regex, std::string>> rules = {
                { std::regex ("spectral_morph"), "MORPH" },
                { std::regex ("^distortion_filter_"), "TONE" },
                { std::regex ("_wave_frame$"), "WAVE" },
                { std::regex ("distortion_amount"), "WARP" },
                // How much distortion, against how hard it is driven. Both used
                // to read DRIVE, and the second of two on one patch got its raw
                // name cut short, as DISTORTION D.
                { std::regex ("^distortion_mix$"), "DISTORT" },
                { std::regex ("^distortion_"), "DRIVE" },
                { std::regex ("^filter_\\d_drive$"), "GRIT" },
                { std::regex ("^reverb_"), "REVERB" },
                { std::regex ("^delay_"), "DELAY" },
                { std::regex ("^chorus_"), "CHORUS" },
                { std::regex ("^phaser_"), "PHASER" },
                { std::regex ("^flanger_"), "FLANGER" },
                { std::regex ("^compressor_"), "SQUASH" },
                { std::regex ("_cutoff$"), "CUTOFF" },
                { std::regex ("_resonance$"), "RESO" },
                { std::regex ("_blend$"), "BLEND" },
                { std::regex ("_unison_detune$"), "DETUNE" },
                { std::regex ("_level$"), "LEVEL" },
                { std::regex ("_frequency$"), "RATE" },
            };
            return rules;
        }

        std::string rawName (const std::string& dest, int index)
        {
            if (dest.empty())
                return "MACRO " + std::to_string (index);
            auto s = juce::String (dest).replace ("_dry_wet", "").replace ("_amount", "")
                         .replace ("_", " ").trim().toUpperCase();
            if (s.isEmpty())
                return "MACRO " + std::to_string (index);
            return s.substring (0, 12).toStdString();
        }

        std::string macroName (const std::string& dest, int index)
        {
            if (dest.empty())
                return "MACRO " + std::to_string (index);
            for (const auto& rule : nameRules())
                if (std::regex_search (dest, rule.first))
                    return rule.second;
            return rawName (dest, index);
        }

        // Effects that are switched off contribute nothing, so a macro wired
        // into one is a dead knob.
        bool destinationIsLive (const std::string& dest, const nlohmann::json& settings)
        {
            static const std::vector<std::pair<std::string, std::string>> pairs = {
                { "reverb", "reverb_on" }, { "delay", "delay_on" },
                { "chorus", "chorus_on" }, { "phaser", "phaser_on" },
                { "flanger", "flanger_on" }, { "distortion", "distortion_on" },
                { "compressor", "compressor_on" }, { "eq", "eq_on" },
                { "filter_1", "filter_1_on" }, { "filter_2", "filter_2_on" },
                { "filter_fx", "filter_fx_on" }, { "osc_1", "osc_1_on" },
                { "osc_2", "osc_2_on" }, { "osc_3", "osc_3_on" },
                { "sample", "sample_on" },
            };
            /*  An oscillator's warp amount is a dead knob while its warp type
                is the first one, which does nothing, and most patches sit
                there. A macro wired to it used to be exactly that.
            */
            const std::string warp = "_distortion_amount";
            if (dest.size() > warp.size()
                && dest.compare (dest.size() - warp.size(), warp.size(), warp) == 0
                && dest.rfind ("osc_", 0) == 0
                && settings.value (dest.substr (0, dest.size() - warp.size()) + "_distortion_type", 0.0) < 0.5)
                return false;

            for (const auto& p : pairs)
                if (dest.rfind (p.first, 0) == 0)
                    return settings.value (p.second, 1.0) >= 0.5;
            return true;
        }

        /*  How long a note should ring, per style.

            A musical judgement rather than anything measured. A bass preset in
            a real library usually shows a full sustain, because the player is
            the one making the notes short, and reading that literally gives a
            bass that drones when you hold a key.

            Values run -1 for short to +1 for long. Sustain also gets a hard
            ceiling where a style is meant to be struck, since leaning alone is
            not enough, and the decay band is where the body of a struck note
            actually comes from: measured against a held note, a decay of 1.00
            is gone in half a second and 1.25 lasts about 1.1.
        */
        struct NoteShape
        {
            float attack, decay, sustain, release;
            float sustainCeiling;
            float releaseFloor, releaseCeiling;
            float decayFloor, decayCeiling;
            /*  The hard limit on how slowly a note may start.

                Leaning the attack short was not enough. A bass took up to 1.35
                seconds to reach full level, which is not a bass whatever else it
                is, and a note that slow to rise has more energy late in the hold
                than early, so the held note check flagged it as a drone and
                mislabelled the fault. Basses that pass sit at 0.12 to 0.13.
                Zero means the style has no limit.
            */
            float attackCeiling;
            /*  And the hard limit on how little of a note may be left.

                The opposite problem, and the one a held riff has. Leaning the
                sustain long only shifts the odds, and a sequence that drew 0.08
                decayed to a twelfth of itself inside a second. Every layer goes
                at once, because this is the amplitude envelope and the whole
                riff is one note being held, so what is left is an attack and
                then most of a bar of nothing. Zero means the style has no
                limit, which is right for everything that is played a note at a
                time.
            */
            float sustainFloor;
        };

        NoteShape noteShapeFor (const std::string& style)
        {
            //                        attack  decay  sustain release susCeil relLo relHi  decLo decHi atkHi susLo
            // The decay ceiling comes down with it. At 1.29 a bass took 2.4s to
            // fall to a tenth, and the ones that sound right fall in 0.6 to 0.9.
            if (style == "Bass")       return { -0.5f, +0.3f, -1.0f, -0.4f, 0.0f,  0.15f, 0.45f, 1.08f, 1.20f, 0.16f };
            if (style == "Percussion") return { -0.8f, -0.2f, -1.0f, -0.5f, 0.0f,  0.10f, 0.35f, 0.88f, 1.12f, 0.10f };
            if (style == "Keys")       return { -0.4f, +0.1f, -0.6f, -0.2f, 0.55f, 0.20f, 0.0f,  1.00f, 1.28f, 0.35f };
            if (style == "Lead")       return { -0.1f, +0.2f, +0.4f, +0.3f, 1.0f,  0.0f,  0.0f,  0.0f,  0.0f };
            if (style == "Pad")        return { +0.7f, +0.5f, +0.7f, +0.6f, 1.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f };
            if (style == "Sequence")   return { -0.3f, +0.1f, +0.5f, -0.2f, 1.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.25f, 0.70f };
            return { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        }

        /** A value inside the parameter's range, at the given percentile. */
        bool sampleInRange (const std::string& param, float percentile, float& out)
        {
            float low = 0.0f, high = 0.0f;
            if (! archetype::rangeFor (param, low, high))
                return false;
            out = low + juce::jlimit (0.0f, 1.0f, percentile) * (high - low);
            return true;
        }
    }

    float skew (float p, float pull)
    {
        /*  Shifting and clipping looks equivalent to this and collapses: at a
            full slider every draw lands on the exact maximum, so the axis stops
            producing variety right where it should be most interesting. A power
            curve keeps the whole range in play and stays monotone in the slider.
        */
        pull = std::max (-1.0f, std::min (1.0f, pull));
        if (std::abs (pull) < 1.0e-6f)
            return p;
        const auto gain = 1.0f + std::abs (pull) * 3.0f;
        return pull > 0.0f ? std::pow (p, 1.0f / gain) : std::pow (p, gain);
    }

    // ------------------------------------------------------------ archetype ---

    void Generator::applyArchetype (const archetype::Archetype& a, const Request& r,
                                    nlohmann::json& settings)
    {
        for (const auto& setting : a.settings)
        {
            const std::string name (setting.name);
            if (r.locks.count (schema::sectionOf (name)) > 0)
                continue;
            settings[name] = setting.value;
        }
    }

    void Generator::applyComplexity (const Request& r, nlohmann::json& settings,
                                     std::mt19937& rng)
    {
        /*  How much of the synth a patch is allowed to use.

            Without this every roll used about the same amount of Vital, and
            that uniformity is what reads as dull. Measured against a real
            library, hand-made presets author anywhere from 5 to 231 parameters
            and wire between 0 and 64 modulations, while the generator sat in a
            narrow band around 96 and 10. Some patches should be a single
            oscillator through one filter, and some should be everything at
            once.
        */
        const auto complexity = r.sliders.count ("complexity") > 0
                                    ? r.sliders.at ("complexity") : 0.5f;

        const auto chance = [&] (float at) { return complexity > at && uniform (rng) < complexity; };

        /*  Two of the things below fight a style that has to stop dead. Noise
            runs flat all the way up, so it drags a bass toward the brightness
            ceiling, and a phaser or flanger keeps ringing after the key is
            released. Leaving them out for those styles costs nothing audible
            and stops the screen throwing most of the batch away.
        */
        const auto mustStopDead = r.style == "Bass" || r.style == "Percussion";

        if (r.locks.count (schema::Section::osc) == 0)
        {
            // A third oscillator and a noise layer are the two cheapest ways to
            // thicken a patch, and the two most obvious when they are missing.
            if (chance (0.30f))
            {
                /*  The third oscillator is a layer, not a harmony part.

                    Most of the time it sits at unison and earns its place
                    through its own wavetable, voice count and detune, which is
                    what layering usually means on a synth. Octaves are the
                    other move that never argues with anything.

                    Fixed intervals are what to avoid, and not for a measurement
                    reason. A patch that adds a fifth to every note is choosing
                    harmony on the player's behalf, and a major third stack is
                    plainly wrong over half of what somebody plays. The octave
                    also happens to be the only exact one: an equal tempered
                    fifth is 1.4983 rather than 1.5, so root and fifth never
                    share a period and the sound has no single pitch to hear.
                    Sequences came out 4.77 semitones off the key pressed.
                */
                settings["osc_3_on"] = 1.0;
                settings["osc_3_level"] = 0.15 + 0.25 * uniform (rng);

                const auto pick = uniform (rng);
                const auto semitones = mustStopDead ? (pick < 0.5f ?   0.0 : -12.0)
                                     : pick < 0.45f ?  0.0
                                     : pick < 0.75f ? -12.0
                                                    :  12.0;
                settings["osc_3_transpose"] = semitones;

                if (semitones == 0.0)
                {
                    // A unison layer has to justify itself some other way.
                    settings["osc_3_unison_voices"] = std::floor (3.0 + uniform (rng) * 5.0);
                    settings["osc_3_unison_detune"] = 1.5 + uniform (rng) * 3.5;
                }

                /*  Vital's own unison stack styles, indices measured by
                    rendering each one rather than read off the enum:

                      0 unison        1 drop 12     2 drop 24
                      3 octave        4 2x octave   5 power chord
                      6 2x power      7 major       8 minor
                      9 harmonics    10 odd harmonics

                    Five through eight are the fixed intervals, so pitched
                    styles get the octave pair and a bass gets the sub. SFX and
                    Experiment have no pitch rule because they are sounds rather
                    than notes, so there the chords are fair game.
                */
                if (r.style == "SFX" || r.style == "Experiment")
                    settings["osc_3_stack_style"] = std::floor (uniform (rng) * 11.0);
                else if (uniform (rng) < 0.35f)
                    settings["osc_3_stack_style"] = mustStopDead ? 1.0
                                                 : uniform (rng) < 0.5f ? 3.0 : 4.0;
            }
            else if (complexity < 0.25f)
            {
                settings["osc_2_on"] = 0.0;      // stripped right back
            }

            /*  Bass is back in.

                It was barred from this when the layer was Vital's flat white
                noise, which dragged it straight into the brightness ceiling.
                What it can draw now is a thump or a pluck, both of which stop on
                their own and neither of which carries any top, so the reason for
                the ban has gone.
            */
            if (chance (r.style == "Percussion" ? 0.60f : 0.45f))
            {
                settings["sample_on"] = 1.0;
            }
        }

        /*  The second filter. Forty two percent of hand-made presets use one and
            the generator never did, which left a whole dimension of the synth
            untouched. Serial puts it after the first, parallel gives the second
            oscillator its own path.
        */
        if (r.locks.count (schema::Section::filter) == 0 && chance (0.35f))
        {
            settings["filter_2_on"] = 1.0;
            settings["filter_2_model"] = std::floor (uniform (rng) * 4.0f);
            settings["filter_2_cutoff"] = 38.0 + uniform (rng) * 42.0;
            // Resonance rings on after the key is up, which is exactly what a
            // bass or a hit must not do.
            settings["filter_2_resonance"] = 0.1 + uniform (rng) * (mustStopDead ? 0.15 : 0.4);
            settings["filter_2_mix"] = 0.5 + 0.5 * uniform (rng);

            if (uniform (rng) < 0.5f)
            {
                settings["filter_2_filter_input"] = 1.0;    // after filter one
            }
            else if (settings.value ("osc_2_on", 0.0) >= 0.5)
            {
                settings["filter_2_filter_input"] = 0.0;
                settings["osc_2_destination"] = 1.0;        // its own path
            }
        }

        // Effects are where a simple patch and an involved one differ most
        // audibly, so the sparse end really does switch things off. Turning them
        // back on is left to MOVE, which already owns them: two paths enabling
        // the same effect put a flanger on half of every batch.
        if (r.locks.count (schema::Section::fx) == 0 && complexity < 0.3f)
            for (const auto& fx : { "phaser_on", "flanger_on", "chorus_on" })
                if (settings.contains (fx))
                    settings[fx] = 0.0;
    }

    void Generator::synthesiseSample (const Request& r, nlohmann::json& settings,
                                      std::mt19937& rng)
    {
        /*  Built rather than borrowed, the same as the wavetables.

            Every patch used to get Vital's own white noise, which is the same
            flat hiss in all of them and costs more brightness than it buys. A
            bed has a voice and a struck body has a shape, and both are worth
            having at a level a fixed noise never was.

            This runs wherever the layer ends up switched on, not only where
            COMPLEX switched it on, because the archetypes turn it on for
            themselves and those patches were still getting the old noise.
        */
        if (r.locks.count (schema::Section::osc) > 0
            || settings.value ("sample_on", 0.0) < 0.5)
            return;

        sampler::Request sr;
        sr.style = r.style;
        for (const auto& key : { "bright", "dirt", "complexity" })
        {
            const auto value = r.sliders.count (key) > 0 ? r.sliders.at (key) : 0.5f;
            if (std::string (key) == "bright")      sr.bright = value;
            else if (std::string (key) == "dirt")   sr.dirt = value;
            else                                    sr.complexity = value;
        }

        // A bed loops for as long as the key is down, which is the one thing a
        // struck style must not do, so those get the struck model or nothing.
        sr.struckOnly = r.style == "Bass" || r.style == "Percussion";
        // A drum kind names its own layer, which is how a hi-hat gets noise.
        if (const auto* k = drums::at (r.drumKind))
        {
            if (*k->sample != '\0')
                sr.model = k->sample;
            if (k->metalNoise.high > 0.0f)
                sr.noise = k->metalNoise.low + uniform (rng) * (k->metalNoise.high - k->metalNoise.low);
            if (k->metalCorner.high > 0.0f)
                sr.corner = k->metalCorner.low + uniform (rng) * (k->metalCorner.high - k->metalCorner.low);
            sr.banks = k->metalBanks;
            sr.modes = k->modes;
            if (k->modeQ.high > 0.0f)
                sr.q = k->modeQ.low + uniform (rng) * (k->modeQ.high - k->modeQ.low);
        }

        auto built = sampler::create (rng, sr);
        settings["sample"] = std::move (built.sample);
        settings["sample_loop"] = built.loop ? 1.0 : 0.0;
        settings["sample_keytrack"] = built.keytrack ? 1.0 : 0.0;
        /*  A looping layer starts wherever it likes, so two notes do not put
            the same part of it in the same place and make the repeat obvious.
            Anything one shot starts at its beginning, because its beginning is
            the attack. */
        settings["sample_random_phase"] = built.loop ? 1.0 : 0.0;
        settings["sample_level"] = built.level;
    }

    void Generator::synthesiseContent (const Request& r, nlohmann::json& settings,
                                       std::mt19937& rng)
    {
        const auto sliderOr = [&r] (const char* key, float fallback)
        {
            const auto it = r.sliders.find (key);
            return it == r.sliders.end() ? fallback : it->second;
        };
        const auto bright = sliderOr ("bright", 0.5f);
        const auto dirt = sliderOr ("dirt", 0.5f);
        const auto move = sliderOr ("move", 0.5f);
        const auto complexity = sliderOr ("complexity", 0.5f);

        /*  Wavetables are written rather than borrowed. That is a licensing
            matter first, since a table lifted out of a preset belongs to
            whoever made the pack, but it is also where the variety comes from:
            a fixed set of tables means the same timbres keep turning up.
        */
        nlohmann::json tables = nlohmann::json::array();
        for (int i = 1; i <= 3; ++i)
        {
            wavetable::Request wr;
            wr.style = r.style;
            wr.bright = bright;
            wr.dirt = dirt;
            wr.move = move;
            wr.complexity = complexity;
            wr.oscillator = i;
            if (const auto* k = drums::at (r.drumKind))
            {
                wr.character = k->character;
                wr.inharmonicLow = k->inharmonic.low;
                wr.inharmonicHigh = k->inharmonic.high;
            }
            tables.push_back (wavetable::create (rng, wr));
        }
        settings["wavetables"] = std::move (tables);

        if (settings.contains ("lfos") && settings["lfos"].is_array())
        {
            auto& lfos = settings["lfos"];
            for (size_t i = 0; i < lfos.size(); ++i)
                if (i < 4 || uniform (rng) < 0.5f)
                    lfos[i] = wavetable::createLfoShape (rng, move);
        }
    }

    // ----------------------------------------------------------------- axes ---

    void Generator::applyAxes (const Request& r, nlohmann::json& settings,
                               std::mt19937& rng)
    {
        const auto keys = numericKeys (settings);
        std::set<std::string> touched;

        /*  Every parameter draws from a stream of its own.

            These used to share one stream, and how many numbers a step took
            from it depended on the sliders: an axis sitting exactly at the
            middle skipped its draws, a type flip made an extra draw only when
            it fired, and the jitter below drew or did not depending on what
            the axes had already set. So one seed at two DIRT settings came out
            with a different reverb, different envelopes and a different
            mix of levels, none of which DIRT has anything to do with.

            Keyed by the parameter's name, a value no longer depends on how
            many numbers anything else drew. The shared stream gives up exactly
            one number to seed them, so whatever draws from it afterwards stays
            aligned as well.
        */
        const auto base = (unsigned int) rng();

        /*  VARY moves a patch, it does not rebuild it.

            On a fresh roll the axes draw their parameters outright. On VARY
            they used to do the same, whatever the depth, so the smallest VARY
            redrew every filter cutoff, every effect level and every LFO rate an
            axis owns. Now a VARY moves each one toward where the axis would
            draw it by the depth and no further, leaves the switches and the
            circuit choices alone, which are the patch's structure, and only
            touches the sections it was asked to.
        */
        const auto varying = r.base != nullptr;
        const auto mayVary = [&r, varying] (const std::string& key)
        {
            return ! varying || r.varySections.empty()
                   || r.varySections.count (schema::sectionOf (key)) > 0;
        };

        for (const auto& axis : axes::all())
        {
            const auto slider = r.sliders.find (axis.key);
            if (slider == r.sliders.end() || std::abs (slider->second - 0.5f) < 1.0e-6f)
                continue;
            const auto value = slider->second;

            for (const auto& member : axes::members (axis, keys))
            {
                if (r.locks.count (schema::sectionOf (member.first)) > 0)
                    continue;
                if (! mayVary (member.first))
                    continue;
                auto own = streamFor (base, member.first, 0xA1u);
                const auto p = skew (uniform (own), (value - 0.5f) * 2.0f * member.second);
                float sampled = 0.0f;
                if (sampleInRange (member.first, p, sampled))
                {
                    if (varying && settings[member.first].is_number())
                    {
                        const auto current = settings[member.first].get<float>();
                        sampled = current + (sampled - current) * juce::jlimit (0.0f, 1.0f, r.amount);
                    }
                    settings[member.first] = sampled;
                    touched.insert (member.first);
                }
            }

            const auto push = std::abs (value - 0.5f) * 2.0f;

            // A pushed axis owns its enables. Otherwise SPACE at full raises
            // reverb_dry_wet on a patch whose reverb is off and nothing happens,
            // which reads as a broken slider.
            for (const auto& sw : axis.enables)
            {
                if (varying)
                    break;
                if (! settings.contains (sw)
                    || r.locks.count (schema::sectionOf (sw)) > 0)
                    continue;
                auto own = streamFor (base, sw, 0xE2u);
                if (uniform (own) < push)
                    settings[sw] = (value > 0.5f || ! axis.lowEndDisables) ? 1.0 : 0.0;
            }

            // Indexed parameters have no meaningful ordering, since Vital's
            // eight filter models are different circuits rather than a
            // brightness ramp, so an extreme slider flips them.
            for (const auto& param : axes::flippable (axis, keys))
            {
                if (varying)
                    break;
                if (r.locks.count (schema::sectionOf (param)) > 0)
                    continue;
                auto own = streamFor (base, param, 0xF3u);
                const auto roll = uniform (own);
                const auto pick = std::floor (uniform (own) * 6.0f);
                if (roll < push * 0.5f)
                {
                    settings[param] = pick;
                    touched.insert (param);
                }
            }
        }

        if (r.amount <= 0.0f)
            return;

        /*  Jitter everything the axes did not claim, but only within its range,
            and only where a range is defined. Parameters without one keep the
            archetype's value.

            This is narrower than it used to be on purpose. The old jitter
            wandered across roughly half of four hundred parameters, and most of
            that motion did nothing but blur a design that was already right.
        */
        const auto complexity = r.sliders.count ("complexity") > 0
                                    ? r.sliders.at ("complexity") : 0.5f;
        // How far each value moves and how many of them move at all. A sparse
        // patch should stay close to its archetype.
        const auto spread = juce::jmap (complexity, 0.0f, 1.0f, 0.07f, 0.26f) * r.amount;
        const auto breadth = juce::jmap (complexity, 0.0f, 1.0f, 0.25f, 0.85f);
        for (const auto& key : keys)
        {
            if (touched.count (key) > 0)
                continue;
            const auto section = schema::sectionOf (key);
            if (section == schema::Section::excluded || r.locks.count (section) > 0
                || ! mayVary (key))
                continue;
            auto own = streamFor (base, key, 0x17u);
            if (uniform (own) > r.amount * breadth)
                continue;

            float low = 0.0f, high = 0.0f;
            if (! archetype::rangeFor (key, low, high))
                continue;

            const auto current = settings[key].get<float>();

            /*  Nudging assumes the value it starts from was chosen. A value
                outside the range was not: it is Vital's factory setting for a
                parameter nobody has touched, so it gets a fresh draw instead.

                Nudging it would clamp it to whichever end it sits past and
                leave it there. That is what pinned every flanger to the wettest
                setting the range allows and made half a batch sound alike.
            */
            const auto at = current < low || current > high
                                ? uniform (own)
                                : juce::jlimit (0.0f, 1.0f,
                                                (current - low) / juce::jmax (1.0e-6f, high - low)
                                                    + gaussian (own, spread));
            const auto value = low + at * (high - low);

            // A stepped parameter has to land on a step. Vital reads 6.76 unison
            // voices as six, so it works either way, but half a step is not a
            // value anybody chose and it does not survive a round trip.
            settings[key] = schema::isIndexed (key) ? std::round (value) : value;
        }
    }

    void Generator::applyNoteShape (const Request& r, nlohmann::json& settings,
                                    std::mt19937& rng)
    {
        if (r.locks.count (schema::Section::env) > 0)
            return;

        const auto shape = noteShapeFor (r.style);
        const std::pair<const char*, float> stages[] = {
            { "env_1_attack",  shape.attack },
            { "env_1_decay",   shape.decay },
            { "env_1_sustain", shape.sustain },
            { "env_1_release", shape.release },
        };

        /*  On VARY the envelope is not drawn again, since VARY nudges it by
            depth along with the rest of the envelope section. Only the style's
            hard limits below still apply, and they only act on a value that
            breaks them.
        */
        for (const auto& stage : stages)
        {
            if (r.base != nullptr)
                break;
            if (std::abs (stage.second) < 1.0e-6f)
                continue;
            float sampled = 0.0f;
            if (sampleInRange (stage.first, skew (uniform (rng), stage.second), sampled))
                settings[stage.first] = sampled;
        }

        /*  A struck note starts when the key goes down, not a second later.

            The lean above only shifts the odds. What was coming out the other
            side was a bass taking over a second to reach full level, which reads
            as a swell rather than a note being played, and which the held note
            check then blamed for droning.
        */
        if (shape.attackCeiling > 0.0f && settings.contains ("env_1_attack"))
        {
            const auto attack = settings["env_1_attack"].get<float>();
            if (attack > shape.attackCeiling)
                settings["env_1_attack"] = shape.attackCeiling * (0.3f + 0.7f * uniform (rng));
        }

        // The riff is one held note, so this is the level the riff plays at.
        if (shape.sustainFloor > 0.0f && settings.contains ("env_1_sustain"))
        {
            const auto sustain = settings["env_1_sustain"].get<float>();
            if (sustain < shape.sustainFloor)
                settings["env_1_sustain"] = shape.sustainFloor
                                                + (1.0f - shape.sustainFloor) * uniform (rng);
        }

        // A struck sound gets no sustain at all, not merely a small one. Even a
        // sixth of the peak held forever is a note that never stops.
        if (shape.sustainCeiling < 1.0f && settings.contains ("env_1_sustain"))
        {
            const auto sustain = settings["env_1_sustain"].get<float>();
            if (sustain > shape.sustainCeiling)
                settings["env_1_sustain"] = shape.sustainCeiling * uniform (rng);
        }

        // The body of a struck note comes from its decay, not its release.
        if (shape.decayFloor > 0.0f && settings.contains ("env_1_decay"))
        {
            const auto decay = settings["env_1_decay"].get<float>();
            if (r.base == nullptr || decay < shape.decayFloor || decay > shape.decayCeiling)
                settings["env_1_decay"] = shape.decayFloor
                                              + uniform (rng) * (shape.decayCeiling - shape.decayFloor);
        }

        if (settings.contains ("env_1_release"))
        {
            const auto release = settings["env_1_release"].get<float>();
            if (shape.releaseFloor > 0.0f && release < shape.releaseFloor)
                settings["env_1_release"] = shape.releaseFloor
                                                + uniform (rng) * shape.releaseFloor;
            else if (shape.releaseCeiling > 0.0f && release > shape.releaseCeiling)
                settings["env_1_release"] = shape.releaseCeiling * (0.6f + 0.4f * uniform (rng));
        }

        /*  The instrument's own pitch stays where the keyboard put it. Someone
            playing a bass part is already reaching for the bottom two octaves,
            and a patch that drops another two lands under the speaker.
        */
        if (settings.contains ("osc_1_transpose") && r.base == nullptr
            && r.locks.count (schema::Section::osc) == 0)
        {
            const auto lowRegister = r.style == "Bass" || r.style == "Percussion";
            const auto roll = uniform (rng);
            if (roll < 0.86f)
                settings["osc_1_transpose"] = 0.0;
            else
                settings["osc_1_transpose"] = lowRegister ? -12.0
                                                          : (roll < 0.95f ? -12.0 : 12.0);
        }
    }

    void Generator::varyMotion (const Request& r, nlohmann::json& settings, unsigned int seed)
    {
        /*  VARY moves the motion as well as the tone.

            It keeps the LFO shapes and what they are wired to, since those are
            the patch's identity, and moves how fast they run and how far they
            reach, each by the depth. How likely any one of them is to move,
            and how far, both scale with the depth, the same way the filters
            and envelopes do.

            A rate is two different knobs depending on the LFO. Most run synced
            to the tempo, and a synced LFO ignores its frequency: 350 of 384 in a
            batch were synced, and 361 of the 367 not running free sat on the
            same note division. So a synced one steps a division up or down,
            half or double the speed, and only a free running one has its
            frequency nudged. One that follows the keyboard keeps its rate,
            since that rate is the note played.

            Left out on purpose: anything driving pitch, so a sequence keeps
            its tempo and its steps land on the same notes and a vibrato keeps
            its speed, and the macros, which are the player's own knobs.
            Depths are scaled by a factor centred on one, so a run of VARYs
            wanders rather than drifting one way, which is what scaling them by
            MOVE on every VARY used to do.
        */
        if (r.base == nullptr || ! settings.contains ("modulations") || ! settings["modulations"].is_array())
            return;
        const auto depth = juce::jlimit (0.0f, 1.0f, r.amount);
        if (depth <= 0.0f)
            return;

        const auto complexity = r.sliders.count ("complexity") > 0
                                    ? r.sliders.at ("complexity") : 0.5f;
        const auto chance = depth * juce::jmap (complexity, 0.0f, 1.0f, 0.25f, 0.85f);
        const auto base = seed ^ 0x40710000u;
        auto& mods = settings["modulations"];

        std::set<std::string> pitchDrivers;
        for (const auto& slot : mods)
            if (! modSource (slot).empty() && axes::isPitchDestination (modDest (slot)))
                pitchDrivers.insert (modSource (slot));

        if (r.locks.count (schema::Section::lfo) == 0)
        {
            std::vector<std::string> sources;
            for (int n = 1; n <= 8; ++n)
                sources.push_back ("lfo_" + std::to_string (n));
            for (int n = 1; n <= 4; ++n)
                sources.push_back ("random_" + std::to_string (n));

            for (const auto& source : sources)
            {
                if (pitchDrivers.count (source) > 0)
                    continue;
                auto own = streamFor (base, source, 0x51u);
                const auto roll = uniform (own);
                const auto up = uniform (own) < 0.5f;
                const auto nudge = gaussian (own, 0.8f * depth);
                if (roll > chance)
                    continue;

                const auto sync = settings.value (source + "_sync", 0.0);
                const auto tempoKey = source + "_tempo";
                const auto freqKey = source + "_frequency";
                if (sync >= 0.5 && sync < 3.5 && settings.contains (tempoKey))
                {
                    // Tempo, dotted or triplet: a note division either way.
                    const auto tempo = settings[tempoKey].get<double>();
                    settings[tempoKey] = juce::jlimit (3.0, 10.0, tempo + (up ? 1.0 : -1.0));
                }
                else if (sync < 0.5 && settings.contains (freqKey))
                {
                    float low = 0.0f, high = 0.0f;
                    if (archetype::rangeFor (freqKey, low, high))
                        settings[freqKey] = juce::jlimit (low, high, settings[freqKey].get<float>() + nudge);
                }
            }
        }

        if (r.locks.count (schema::Section::mod) == 0)
        {
            for (size_t i = 0; i < mods.size(); ++i)
            {
                const auto source = modSource (mods[i]);
                if (source.empty() || source.rfind ("macro_control", 0) == 0
                    || axes::isPitchDestination (modDest (mods[i])))
                    continue;

                const auto key = "modulation_" + std::to_string (i + 1) + "_amount";
                auto own = streamFor (base, key, 0x62u);
                const auto roll = uniform (own);
                const auto factor = std::exp (gaussian (own, 0.35f * depth));
                if (roll > chance || ! settings.contains (key))
                    continue;
                settings[key] = juce::jlimit (-1.0f, 1.0f, settings[key].get<float>() * factor);
            }
        }
    }

    void Generator::scaleModulationDepth (const Request& r, nlohmann::json& settings)
    {
        /*  MOVE, applied to how far each modulation reaches.

            It already sets the rate of the modulators and how many of them are
            wired, and neither is the same as how much they move anything. A
            patch with four fast LFOs at shallow depths sits nearly still, which
            is why the slider was the weakest of the four: pushed to the top it
            changed the tone of a patch less than half the time on a lead.

            Depth is scaled rather than set, so the shape the archetype designed
            survives and only its reach changes.
        */
        /*  Once, on a fresh roll. It multiplies, so running it on every VARY
            compounded: a few presses at a high MOVE pushed every depth to the
            end of its range, and at a low MOVE shrank them toward nothing.
        */
        if (r.base != nullptr || r.locks.count (schema::Section::mod) > 0
            || ! settings.contains ("modulations") || ! settings["modulations"].is_array())
            return;

        const auto move = r.sliders.count ("move") > 0 ? r.sliders.at ("move") : 0.5f;
        const auto scale = juce::jmap (move, 0.0f, 1.0f, 0.55f, 1.45f);

        auto& mods = settings["modulations"];
        for (size_t i = 0; i < mods.size(); ++i)
        {
            if (modSource (mods[i]).empty())
                continue;
            const auto key = "modulation_" + std::to_string (i + 1) + "_amount";
            const auto amount = (float) settings.value (key, 0.0);
            settings[key] = juce::jlimit (-1.0f, 1.0f, amount * scale);
        }
    }

    DriveBand driveBandFor (float dirt)
    {
        /*  How hard the distortion is pushed, set by DIRT rather than drawn.

            Vital's drive knob runs from -30 to +30 dB, and it is not the dial it
            looks like. Swept fully wet against the effect switched off, the two
            clippers and two folders follow the drive decibel for decibel up to
            the middle of the knob, which means their shaper is linear there and
            the lower half is a volume control. They start to saturate around
            55% to 60% and are heavy past 70%. The bit crusher and sample rate
            reducer are different again, and the circuit is chosen below in
            shapeDistortion, but one band serves all six: the crushers only open
            near the top of DIRT, where it already sits above unity.

            So DIRT picks a band on the knob, in knob terms because that is what
            anyone reading the patch sees, and every band starts at the middle
            where the drive is at unity. At a tenth of the slider it rests
            between 48% and 50% and may peak at 52%, which is clean. At the top
            it rests between 50% and 60% and may peak at 70%. In between it is a
            straight line through those two, and at zero the distortion is off.

            The band used to start at the bottom of the knob, on a straight line
            from 0%, and that was measured before it was dropped. Below the
            middle a clipper is a gain stage and nothing else: a patch with the
            drive at 29% came out 12.2 dB quieter with the effect on than off,
            against 12.6 dB of negative drive, and a fully wet mix put the whole
            signal through that cut. It added no grit a listener could pick out
            from Vital's own take to take scatter anywhere below the top of the
            slider, and at a DIRT of 0.5 a third of all rolls were thrown out
            because the level correction could not make the loss back up with
            the master volume at its maximum.

            Starting at unity keeps the level where it is at every setting, and
            the peaks carry the grit in: the ceiling crosses 55%, where the knob
            begins to bite, a quarter of the way up the slider.
        */
        const auto t = (dirt - 0.1f) / 0.9f;
        const auto line = [t] (float atTenth, float atTop)
        { return juce::jlimit (0.0f, 1.0f, atTenth + (atTop - atTenth) * t); };

        DriveBand band;
        band.restLow  = line (0.48f, 0.50f);
        band.restHigh = line (0.50f, 0.60f);
        band.ceiling  = line (0.52f, 0.70f);
        return band;
    }

    namespace
    {
        /*  A drum's LFOs named for what they move.

            A sequence's step line is named for its shape, since the shape is
            the riff. A drum has no riff: its LFOs are the ordinary movement
            ones, wobbling the tone or the level of the hit, and every one of
            them came out called Generated, which says nothing in Vital's LFO
            panel. So each is named for where it is wired: Tone, Pitch, Level
            and so on, two roles joined when it drives both, and Spare when
            nothing listens to it. The flam keeps its own name.
        */
        void nameLfosForWhatTheyMove (nlohmann::json& settings)
        {
            if (! settings.contains ("lfos") || ! settings["lfos"].is_array()
                || ! settings.contains ("modulations"))
                return;

            const auto roleOf = [] (const std::string& d) -> const char*
            {
                const auto has = [&d] (const char* part) { return d.find (part) != std::string::npos; };
                if (has ("cutoff") || has ("resonance") || has ("blend") || has ("formant"))
                    return "Tone";
                if (has ("transpose") || has ("tune"))
                    return "Pitch";
                if (has ("_pan"))
                    return "Pan";
                if (has ("wave_frame") || has ("spectral") || (has ("osc_") && has ("distortion")))
                    return "Timbre";
                if (has ("distortion") || has ("drive"))
                    return "Grit";
                if (has ("reverb") || has ("delay") || has ("chorus") || has ("flanger") || has ("phaser"))
                    return "Space";
                if (has ("level") || has ("volume"))
                    return "Level";
                return "Motion";
            };

            auto& lfos = settings["lfos"];
            for (size_t i = 0; i < lfos.size(); ++i)
            {
                const auto own = lfos[i].is_object() ? lfos[i].value ("name", std::string()) : std::string();
                if (! lfos[i].is_object() || own == "Flam" || own == "Sha-ka")
                    continue;

                const auto source = "lfo_" + std::to_string (i + 1);
                std::vector<std::string> roles;
                for (const auto& slot : settings["modulations"])
                    if (modSource (slot) == source)
                    {
                        const std::string role = roleOf (slot.value ("destination", std::string()));
                        if (std::find (roles.begin(), roles.end(), role) == roles.end())
                            roles.push_back (role);
                    }

                std::string name = roles.empty() ? "Spare" : roles[0];
                if (roles.size() > 1)
                    name += " + " + roles[1];
                lfos[i]["name"] = name;
            }
        }
    }

    void Generator::shapeDrum (const Request& r, nlohmann::json& settings, unsigned int seed)
    {
        /*  Hold a drum to its kind, after everything else has had its turn.

            The axes, the jitter, COMPLEX and the note shape all move a patch
            without knowing what drum it is meant to be: COMPLEX can add a
            second filter as a low pass that buries a hi-hat, and the note shape
            draws one percussion decay for everything from a closed hat to a
            crash. So the kind's bands are applied last. A value already inside
            its band is kept, which is where the variety between two kicks comes
            from, and one outside it is drawn inside, so BRIGHT moves a hat's
            cutoff within the hat band rather than out of it.

            A fresh roll also gets the kind's structure: which layers are on,
            where they sit, the pitch drop and the flam. A VARY keeps the
            structure it was given and only has its values held to the bands.
        */
        const auto* kind = drums::at (r.drumKind);
        if (r.style != "Percussion" || kind == nullptr)
            return;

        const auto fresh = r.base == nullptr;
        const auto base = seed ^ 0xD2E40000u;
        const auto draw = [base] (const std::string& key, const drums::Band& band)
        {
            auto own = streamFor (base, key, 0x71u);
            return band.low + uniform (own) * (band.high - band.low);
        };
        const auto hold = [&] (const std::string& key, const drums::Band& band)
        {
            if (! settings.contains (key) || ! settings[key].is_number())
                return;
            const auto value = settings[key].get<float>();
            if (value < band.low - 1.0e-4f || value > band.high + 1.0e-4f)
                settings[key] = draw (key, band);
        };

        const auto oscFree = r.locks.count (schema::Section::osc) == 0;
        const auto filterFree = r.locks.count (schema::Section::filter) == 0;
        const auto envFree = r.locks.count (schema::Section::env) == 0;
        const auto modFree = r.locks.count (schema::Section::mod) == 0;

        if (fresh && oscFree)
        {
            settings["osc_1_on"] = kind->tonal ? 1.0 : 0.0;
            if (! kind->tonal)
            {
                settings["osc_2_on"] = 0.0;
                settings["osc_3_on"] = 0.0;
            }
            settings["osc_1_transpose"] = kind->transpose;
            if (settings.value ("osc_2_on", 0.0) >= 0.5)
                settings["osc_2_transpose"] = kind->secondTranspose;

            /*  A drum's body is one voice with its partials where they fall.
                Four unison voices, from DIRT's thickness lever, and a spectral
                morph reshaping the partials each smear a tuned drum's pitch,
                and one tom read four and a half semitones flat with both. A
                metallic hat or cymbal keeps its unison, which it wears as
                density rather than as a pitch.
            */
            if (kind->tonal && std::string (kind->character) != "metallic")
            {
                settings["osc_1_unison_voices"] = 1.0;
                settings["osc_1_spectral_morph_amount"] = 0.0;
            }

            /*  Nor a warp on a tuned drum. COMPLEX's warps bend, fold or
                formant the wave, and on a tom or a timpani that is no longer
                the drum: one timpani came out with a formant warp at two thirds
                over heavy distortion and was not recognisable as one. */
            if (kind->pitched)
                for (const auto* osc : { "osc_1", "osc_2", "osc_3" })
                    settings[std::string (osc) + "_distortion_type"] = 0.0;

            /*  A third oscillator from COMPLEX thickens the body at its own
                pitch. For percussion it could arrive an octave down and on
                Vital's drop twelve stack, which put a second, lower pitch under
                every tuned drum and a sub under the snare, and a wide detuned
                stack smears a drum's pitch. */
            if (kind->tonal && settings.value ("osc_3_on", 0.0) >= 0.5)
            {
                settings["osc_3_transpose"] = kind->transpose;
                settings["osc_3_stack_style"] = 0.0;
                settings["osc_3_unison_voices"] = juce::jmin (2.0, settings.value ("osc_3_unison_voices", 1.0));
                settings["osc_3_unison_detune"] = juce::jmin (1.0, settings.value ("osc_3_unison_detune", 0.0));
            }
            for (const auto* dest : { "osc_1_destination", "osc_2_destination",
                                      "osc_3_destination", "sample_destination" })
                settings[dest] = 0.0;

            const auto layered = *kind->sample != '\0';
            settings["sample_on"] = layered ? 1.0 : 0.0;
            if (layered)
                settings["sample_level"] = draw ("sample_level", kind->sampleLevel);

            // Vital shows these names in its oscillator and sample panels, so
            // the parts of a drum say what they are for.
            if (layered && settings.contains ("sample") && settings["sample"].is_object())
                settings["sample"]["name"] = std::string (kind->name) + " "
                                             + juce::String (kind->sample).toLowerCase().toStdString();
            if (settings.contains ("wavetables") && settings["wavetables"].is_array())
            {
                const char* roles[] = { " body", " second", " layer" };
                for (size_t i = 0; i < 3 && i < settings["wavetables"].size(); ++i)
                    if (settings.value ("osc_" + std::to_string (i + 1) + "_on", 0.0) >= 0.5
                        && settings["wavetables"][i].is_object())
                        settings["wavetables"][i]["name"] = std::string (kind->name) + roles[i];
            }
        }
        if (oscFree && *kind->sample != '\0')
            hold ("sample_level", kind->sampleLevel);

        if (fresh && oscFree && kind->body.high > 0.0f)
            hold ("osc_1_level", kind->body);

        // How far the second envelope throws the filter open at the strike.
        auto& mods = settings["modulations"];
        if (fresh && modFree)
        {
            const auto depth = draw ("sweep", kind->sweep) / 128.0f;
            for (size_t i = 0; i < mods.size(); ++i)
                if (modSource (mods[i]) == "env_2" && modDest (mods[i]) == "filter_1_cutoff")
                {
                    settings["modulation_" + std::to_string (i + 1) + "_amount"] = depth;
                    settings["modulation_" + std::to_string (i + 1) + "_bipolar"] = 0.0;
                }
        }

        if (filterFree)
        {
            if (fresh)
            {
                settings["filter_1_on"] = 1.0;
                if (! kind->secondFilter)
                    settings["filter_2_on"] = 0.0;
            }
            hold ("filter_1_blend", kind->blend);
            if (settings.value ("filter_1_resonance", 0.0) > kind->maxResonance)
                settings["filter_1_resonance"] = kind->maxResonance;

            /*  The cutoff a player hears, not the one on the knob.

                Velocity and a macro wired to the cutoff add to it for the whole
                note: a normal strike is 110 of 127, and a macro rests where it
                sits, halfway by default. Measured on a clap, those two and the
                strike's sweep took a band pass meant for 1.5 kHz to a centre of
                13.9 kHz. So the band is held on what they add up to, and the
                knob is set below it. The cutoff's range is 128 semitones, which
                is what a unit of depth spans.
            */
            const auto standing = [&]
            {
                double offset = 0.0;
                for (size_t i = 0; i < mods.size(); ++i)
                {
                    if (modDest (mods[i]) != "filter_1_cutoff")
                        continue;
                    const auto source = modSource (mods[i]);
                    double value = -1.0;
                    if (source == "velocity")
                        value = 110.0 / 127.0;
                    else if (source.rfind ("macro_control_", 0) == 0)
                        value = settings.value (source, 0.5);
                    if (value < 0.0)
                        continue;
                    const auto n = std::to_string (i + 1);
                    const auto amount = settings.value ("modulation_" + n + "_amount", 0.0);
                    const auto bipolar = settings.value ("modulation_" + n + "_bipolar", 0.0) >= 0.5;
                    offset += bipolar ? (value - 0.5) * 2.0 * amount * 64.0 : amount * value * 128.0;
                }
                return offset;
            };

            // The ranges' floor, and a ceiling above the ranges' 110, which is
            // 4.7 kHz and too low for the high pass that makes a hi-hat.
            constexpr double kLowest = 28.0, kHighest = 130.0;
            auto offset = standing();
            const auto heard = settings.value ("filter_1_cutoff", 0.0) + offset;
            auto target = heard;
            if (heard < kind->cutoff.low || heard > kind->cutoff.high)
                target = draw ("filter_1_cutoff", kind->cutoff);

            // More than the knob can take up: the routings give way instead.
            if (target - offset < kLowest && offset > 0.0 && modFree)
            {
                const auto scale = juce::jmax (0.0, (target - kLowest) / offset);
                for (size_t i = 0; i < mods.size(); ++i)
                {
                    const auto source = modSource (mods[i]);
                    if (modDest (mods[i]) == "filter_1_cutoff"
                        && (source == "velocity" || source.rfind ("macro_control_", 0) == 0))
                    {
                        const auto key = "modulation_" + std::to_string (i + 1) + "_amount";
                        settings[key] = settings.value (key, 0.0) * scale;
                    }
                }
                offset = standing();
            }
            settings["filter_1_cutoff"] = juce::jlimit (kLowest, kHighest, target - offset);
        }

        if (envFree)
        {
            hold ("env_1_attack", kind->attack);
            hold ("env_1_decay", kind->decay);
            hold ("env_1_release", kind->release);
            if (kind->sustain.high > 0.0f)
                hold ("env_1_sustain", kind->sustain);
            else
                settings["env_1_sustain"] = 0.0;
            if (kind->sweepDecay.high > 0.0f)
            {
                hold ("env_2_decay", kind->sweepDecay);
                settings["env_2_sustain"] = 0.0;
            }
        }

        if (fresh && kind->room && r.locks.count (schema::Section::fx) == 0)
        {
            settings["reverb_on"] = 1.0;
            settings["reverb_dry_wet"] = juce::jmax (0.15, settings.value ("reverb_dry_wet", 0.0));
        }

        /*  The pitch drop: an envelope on the body's transpose, deep enough to
            start the given number of semitones above the note, and quick
            enough to land on it before the ear follows the slide. */
        if (fresh && modFree && kind->drop.high > 0.0f)
        {
            const auto depth = draw ("drop", kind->drop) / 96.0f;
            auto& mods = settings["modulations"];
            auto found = false;
            for (size_t i = 0; i < mods.size(); ++i)
                if (modSource (mods[i]) == "env_2" && modDest (mods[i]) == "osc_1_transpose")
                {
                    settings["modulation_" + std::to_string (i + 1) + "_amount"] = depth;
                    settings["modulation_" + std::to_string (i + 1) + "_bipolar"] = 0.0;
                    found = true;
                }
            if (! found)
                setModSlot (settings, "env_2", "osc_1_transpose", depth, false);
            settings["env_2_attack"] = 0.0;
            settings["env_2_sustain"] = 0.0;
            settings["env_2_decay"] = draw ("env_2_decay", kind->dropDecay);
        }

        /*  The flam. Three quick strikes a few milliseconds apart before the
            body, made with a one shot LFO that ducks the noise between them.
            The shape is drawn in the LFO's own terms, where a point's height is
            one minus the value it sends.
        */
        if (fresh && kind->flam && modFree && r.locks.count (schema::Section::lfo) == 0
            && settings.contains ("lfos") && settings["lfos"].is_array() && settings["lfos"].size() >= 8)
        {
            // Value 0 is a strike, value 1 is ducked. Pairs share an x to jump.
            const std::vector<std::pair<float, float>> gate = {
                { 0.00f, 0.00f }, { 0.10f, 0.85f }, { 0.16f, 0.85f }, { 0.16f, 0.00f },
                { 0.26f, 0.85f }, { 0.32f, 0.85f }, { 0.32f, 0.00f }, { 0.40f, 0.60f },
                { 0.44f, 0.60f }, { 0.44f, 0.00f }, { 1.00f, 0.00f } };
            nlohmann::json points = nlohmann::json::array();
            nlohmann::json powers = nlohmann::json::array();
            for (const auto& p : gate)
            {
                points.push_back (p.first);
                points.push_back (1.0f - p.second);
                powers.push_back (0.0f);
            }
            nlohmann::json shape;
            shape["name"] = "Flam";
            shape["num_points"] = (int) gate.size();
            shape["points"] = std::move (points);
            shape["powers"] = std::move (powers);
            shape["smooth"] = false;
            settings["lfos"][7] = std::move (shape);

            settings["lfo_8_sync"] = 0.0;          // seconds, not the tempo
            settings["lfo_8_sync_type"] = 2.0;     // one shot, like an envelope
            settings["lfo_8_frequency"] = 4.0;     // one pass in about 60 ms

            const auto level = settings.value ("sample_level", 0.7);
            setModSlot (settings, "lfo_8", "sample_level", (float) -level, false);
        }

        /*  The shake. Two strokes to a cycle, sha and ka, each swelling in and
            falling away, the first a little heavier, in time with the song and
            restarted by each key, so a held key keeps shaking and a tap is one
            stroke. Drawn in the same terms as the flam, a point's height being
            one minus the value it sends.
        */
        if (fresh && kind->shake && modFree && r.locks.count (schema::Section::lfo) == 0
            && settings.contains ("lfos") && settings["lfos"].is_array() && settings["lfos"].size() >= 8)
        {
            const auto second = draw ("shake_second", { 0.60f, 0.85f });  // how heavy the ka is
            const auto swell = draw ("shake_swell", { 0.08f, 0.16f });    // how long a stroke takes to come in
            const auto rest = draw ("shake_rest", { 0.25f, 0.40f });      // the beads never stop

            // Strength of the stroke at x, 1 at its peak.
            const std::vector<std::pair<float, float>> strokes = {
                { 0.0f, rest }, { swell, 1.0f }, { 0.5f, rest },
                { 0.5f + swell, second }, { 1.0f, rest } };
            nlohmann::json points = nlohmann::json::array();
            nlohmann::json powers = nlohmann::json::array();
            for (size_t i = 0; i < strokes.size(); ++i)
            {
                points.push_back (strokes[i].first);
                points.push_back (strokes[i].second);        // 1 - (1 - strength)
                // Easing in, falling away quickly then trailing off.
                powers.push_back (i % 2 == 0 ? 1.5f : -2.5f);
            }
            nlohmann::json shape;
            shape["name"] = "Sha-ka";
            shape["num_points"] = (int) strokes.size();
            shape["points"] = std::move (points);
            shape["powers"] = std::move (powers);
            shape["smooth"] = false;
            settings["lfos"][7] = std::move (shape);

            settings["lfo_8_sync"] = 1.0;            // to the tempo
            settings["lfo_8_sync_type"] = 0.0;       // restarted by each key
            // One cycle an eighth or a quarter, so the strokes fall on
            // sixteenths or eighths. A synced cycle lasts two to the power of
            // seven minus the tempo in seconds at 120 BPM: nine is an eighth.
            settings["lfo_8_tempo"] = draw ("shake_tempo", { 0.0f, 1.0f }) < 0.6f ? 9.0 : 8.0;

            const auto level = settings.value ("sample_level", 0.7);
            setModSlot (settings, "lfo_8", "sample_level", (float) -level, false);
        }
    }

    void Generator::switchDistortion (const Request& r, nlohmann::json& settings)
    {
        /*  On for any DIRT above nothing, off at nothing.

            Decided here, before anything is wired, and from the slider alone.
            The wiring passes over destinations whose module is off, so a switch
            thrown after it would leave a macro pointing at a dead effect, and a
            switch thrown by chance, as it used to be, made two rolls of one seed
            at different DIRT settings wire themselves differently: two fifths of
            their routing slots, where BRIGHT moved three hundredths.
        */
        if (r.base != nullptr || r.locks.count (schema::Section::fx) > 0
            || ! settings.contains ("distortion_on"))
            return;

        const auto dirt = r.sliders.count ("dirt") > 0 ? r.sliders.at ("dirt") : 0.5f;
        settings["distortion_on"] = dirt < 0.01f ? 0.0 : 1.0;
    }

    void Generator::chooseWarp (const Request& r, nlohmann::json& settings, unsigned int seed)
    {
        /*  Oscillator warp, chosen by how much of the synth a patch may use.

            Vital files it under distortion, but its types are sync, formant,
            bend, squeeze and the like, and they reshape the tone in every
            direction: measured at the same depth, most lift the centroid and
            only one reliably fills the spectrum in. That is character, not
            grit, so it left DIRT. And its first type does nothing at all,
            which is where 42 patches in 48 were sitting, so a DIRT lever on
            the warp amount was a knob connected to nothing most of the time.

            A patch that is allowed more of the synth is likelier to warp an
            oscillator: one chance in twenty at the bottom of COMPLEX and one
            in four at the top, per oscillator that is on. The types are the
            six that work on an oscillator by itself; the ones that borrow from
            another oscillator are left for a patch that sets them on purpose.
            Chosen before the wiring, so a macro only lands on a warp that is
            actually doing something.
        */
        if (r.base != nullptr || r.locks.count (schema::Section::osc) > 0)
            return;

        const auto complexity = r.sliders.count ("complexity") > 0
                                    ? r.sliders.at ("complexity") : 0.5f;
        const auto chance = 0.05f + 0.20f * complexity;

        for (int n = 1; n <= 3; ++n)
        {
            const auto osc = "osc_" + std::to_string (n);
            if (settings.value (osc + "_on", 0.0) < 0.5 || ! settings.contains (osc + "_distortion_type"))
                continue;

            std::mt19937 wrng (seed ^ 0x3A7F0000u ^ (unsigned int) n);
            const auto roll = uniform (wrng);
            const auto type = std::uniform_int_distribution<int> (1, 6) (wrng);
            const auto amount = 0.2f + 0.5f * uniform (wrng);

            if (roll < chance)
            {
                settings[osc + "_distortion_type"] = (double) type;
                settings[osc + "_distortion_amount"] = amount;
            }
            else
            {
                settings[osc + "_distortion_type"] = 0.0;
            }
        }
    }

    void Generator::shapeDistortion (const Request& r, nlohmann::json& settings,
                                     unsigned int seed)
    {
        /*  Drive is not a destination to swing hard or quickly.

            MOVE raises the rate of the modulators and wires them at the drive
            at the same time, and a fast LFO on a distortion drive is not
            movement, it is the patch tearing. Depth is capped, and anything
            cyclic aimed at it is slowed to something that reads as the sound
            breathing rather than stuttering.

            The depth used to be a flat 0.35, which sounds conservative and is
            not. A depth is a fraction of the whole 60 dB knob: 0.35 unipolar is
            21 dB of swing and 0.35 bipolar is 10.5 either way, and across a
            batch that was taking a ten point resting band to both ends of the
            knob. What made the distortion hot was the swing, not where it
            rested.

            So the cap is worked backwards from where the knob may reach. Each
            modulation gets a share of the room between where this patch rests
            and the ceiling DIRT allows, and of the room down to 0%, since depth
            past the bottom is the knob stopped rather than the grit getting
            quieter. Unipolar spans the whole 60 dB and bipolar half of it either
            side, verified against static renders rather than assumed.
        */
        constexpr float kKnobDb = 60.0f;     // -30 to +30, confirmed by where it clamps
        const auto toDb = [] (float knob) { return knob * 60.0f - 30.0f; };

        const auto dirt = r.sliders.count ("dirt") > 0 ? r.sliders.at ("dirt") : 0.5f;
        const auto band = driveBandFor (dirt);
        const auto locked = r.locks.count (schema::Section::fx) > 0;

        /*  A fresh patch has its resting drive and its circuit set from DIRT.

            The switch was already thrown in switchDistortion, before the wiring.
            VARY keeps the patch it was given, so it only gets the swing cap, and
            a locked effects section is left exactly as it is. Both draws have a
            stream of their own, so where the drive lands and which circuit is
            picked never move any other value in the patch.
        */
        if (r.base == nullptr && ! locked && settings.contains ("distortion_drive")
            && settings.value ("distortion_on", 0.0) >= 0.5)
        {
            std::mt19937 drng (seed ^ 0xD1A70000u);
            std::uniform_real_distribution<float> within (band.restLow, band.restHigh);
            settings["distortion_drive"] = toDb (within (drng));

            /*  And the circuit, which matters more than the drive.

                Vital's six are two families. The two clippers and two folders
                do nothing below the middle of the knob but change the level,
                and only bite from about 55%. The last two, which behave like a
                bit crusher and a sample rate reducer, leave the level alone at
                any drive and change the tone heavily from 30% of the knob up:
                one at 30% moves the spectrum further than a soft clipper does
                at 80%. The type used to be re-rolled at random at either end of
                the slider, so a DIRT of 0.1 could land on a crusher while 0.5
                never changed type at all, and nearly all the grit a batch had
                came from the few patches that happened to get one.

                So DIRT opens them in order of how hard they hit. The clippers
                from the bottom, the folders from 0.4, the crushers from 0.7,
                and each patch draws evenly from whatever is open.
            */
            if (settings.contains ("distortion_type"))
            {
                std::vector<int> open = { 0, 1 };
                if (dirt >= 0.4f) { open.push_back (2); open.push_back (3); }
                if (dirt >= 0.7f) { open.push_back (4); open.push_back (5); }

                std::mt19937 trng (seed ^ 0xD1A7F00Du);
                const auto drawn = open[(size_t) std::uniform_int_distribution<int> (
                    0, (int) open.size() - 1) (trng)];
                settings["distortion_type"] = (double) (r.distortionType >= 0 && r.distortionType < 6
                                                            ? r.distortionType : drawn);
            }
        }

        /*  The filters' drive, which saturates inside the filter itself.

            It is a separate stage from the distortion and a gentler one: swept
            with everything else held still it changes the spectrum by 0.7 dB
            at 3.5, 1.5 at 10 and 3.4 at 20, where the knob stops, and never
            moves the level by more than a decibel, so it costs the screen
            nothing. It rests between half and all of DIRT times 20, which is
            nothing at zero, 5 to 10 in the middle and 10 to 20 at the top.
        */
        if (r.base == nullptr && r.locks.count (schema::Section::filter) == 0)
        {
            for (const auto* key : { "filter_1_drive", "filter_2_drive" })
            {
                if (! settings.contains (key))
                    continue;
                std::mt19937 frng (seed ^ 0xF1D70000u ^ (unsigned int) key[7]);
                std::uniform_real_distribution<float> within (10.0f * dirt, 20.0f * dirt);
                settings[key] = within (frng);
            }
        }

        if (! settings.contains ("modulations") || ! settings["modulations"].is_array())
            return;

        auto& mods = settings["modulations"];

        // Shared out, because two modulations on one destination add up.
        auto aimed = 0;
        for (const auto& slot : mods)
            if (! modSource (slot).empty() && modDest (slot) == "distortion_drive")
                ++aimed;
        if (aimed == 0)
            return;

        const auto resting = (float) settings.value ("distortion_drive", 0.0);
        const auto up = juce::jmax (0.0f, toDb (band.ceiling) - resting) / (float) aimed;
        const auto down = juce::jmax (0.0f, resting - toDb (0.0f)) / (float) aimed;

        for (size_t i = 0; i < mods.size(); ++i)
        {
            const auto source = modSource (mods[i]);
            if (source.empty() || modDest (mods[i]) != "distortion_drive")
                continue;

            const auto n = std::to_string (i + 1);
            const auto bipolar = settings.value ("modulation_" + n + "_bipolar", 0.0) >= 0.5;
            const auto key = "modulation_" + n + "_amount";
            const auto amount = (float) settings.value (key, 0.0);

            // Bipolar goes both ways at once, so it is held to whichever side
            // has less room. Unipolar only goes the way its sign points.
            const auto reach = bipolar ? kKnobDb * 0.5f : kKnobDb;
            const auto room = bipolar ? juce::jmin (up, down) : (amount >= 0.0f ? up : down);
            const auto ceiling = juce::jmin (0.35f, room / reach);

            settings[key] = juce::jlimit (-ceiling, ceiling, amount);

            if (source.rfind ("lfo_", 0) == 0)
            {
                const auto rate = source + "_frequency";
                if (settings.contains (rate) && settings[rate].get<float>() > 0.5f)
                    settings[rate] = 0.5f;
            }
        }
    }

    void Generator::keepStruckNotesStruck (const Request& r, nlohmann::json& settings)
    {
        /*  A struck note has to stop while the key is still down.

            Setting the amp envelope's sustain to zero is not enough on its own.
            An LFO wired to an oscillator's level pushes it back up a moment
            later, and the note swells again with the key still held: one patch
            doing exactly that measured a rising envelope two seconds in. The
            screen catches it, and catching it means rolling the whole patch
            again, so it was the commonest reason a bass was thrown away.

            Envelopes are left alone, because an envelope decays and a bass with
            its filter closing is the sound. It is the cyclic sources that have
            no end.
        */
        if (r.style != "Bass" && r.style != "Percussion")
            return;
        if (r.locks.count (schema::Section::mod) > 0
            || ! settings.contains ("modulations") || ! settings["modulations"].is_array())
            return;

        static const std::regex level (R"(^(osc_\d_level|sample_level|volume)$)");

        auto& mods = settings["modulations"];
        for (auto& slot : mods)
        {
            const auto source = modSource (slot);
            if (source.empty() || ! std::regex_match (modDest (slot), level))
                continue;
            if (source.rfind ("lfo_", 0) != 0 && source.rfind ("random_", 0) != 0)
                continue;

            slot["source"] = "";
            slot["destination"] = "";
        }
    }

    // -------------------------------------------------------------- routing ---

    void Generator::wireRouting (const archetype::Archetype& a, const Request& r,
                                 nlohmann::json& settings, std::mt19937& rng)
    {
        if (r.locks.count (schema::Section::mod) > 0)
            return;

        ensureModSlots (settings);

        /*  The style's own routings first. These are what make it that style: a
            bass is plucky because a decaying envelope closes its filter, and a
            sequence moves because LFOs drive its level and pitch. Wiring both
            from one shared list produced basses with seven LFOs and no filter
            envelope at all, which is a fine experimental patch and not a bass.
        */
        const auto move = r.sliders.count ("move") > 0 ? r.sliders.at ("move") : 0.5f;
        const auto depthScale = juce::jmap (move, 0.0f, 1.0f, 0.55f, 1.5f);

        for (const auto& routing : a.routings)
        {
            const auto amount = juce::jlimit (0.02f, 1.0f,
                routing.amount * depthScale * (0.85f + 0.3f * uniform (rng)));
            setModSlot (settings, routing.source, routing.destination,
                        amount, routing.bipolar);
        }

        /*  More routings on top, from a general pool. Motion is topology:
            raising an LFO rate on a patch with nothing routed changes nothing
            anyone can hear.

            Both sliders feed this. MOVE asks for motion and COMPLEXITY asks for
            more of the synth to be involved, and a patch can want either
            without the other.
        */
        const auto complexity = r.sliders.count ("complexity") > 0
                                    ? r.sliders.at ("complexity") : 0.5f;
        if (move <= 0.5f && complexity <= 0.5f)
            return;

        static const std::vector<const char*> sources = {
            "lfo_1", "lfo_2", "lfo_3", "random_1", "env_2" };
        static const std::vector<const char*> destinations = {
            "osc_1_wave_frame", "osc_2_wave_frame", "filter_1_cutoff",
            "osc_1_spectral_morph_amount", "filter_2_cutoff", "osc_1_level",
            "distortion_drive", "osc_1_unison_detune", "filter_1_resonance" };

        const auto fromMove = juce::jmap (juce::jmax (0.0f, move - 0.5f), 0.0f, 0.5f, 0.0f, 7.0f);
        const auto fromComplexity = juce::jmap (juce::jmax (0.0f, complexity - 0.5f),
                                                0.0f, 0.5f, 0.0f, 9.0f);
        const auto extra = juce::roundToInt (fromMove + fromComplexity);
        std::uniform_int_distribution<size_t> pickSource (0, sources.size() - 1);
        std::uniform_int_distribution<size_t> pickDest (0, destinations.size() - 1);

        for (int i = 0; i < extra * 3 && i < 40; ++i)
        {
            int used = 0;
            for (const auto& slot : settings["modulations"])
                if (! modSource (slot).empty())
                    ++used;
            if (used >= (int) a.routings.size() + extra)
                break;

            setModSlot (settings, sources[pickSource (rng)], destinations[pickDest (rng)],
                        0.15f + uniform (rng) * 0.5f, uniform (rng) < 0.3f);
        }
    }

    void Generator::wireMacros (const Request& r, nlohmann::json& doc,
                                nlohmann::json& settings, Result& result)
    {
        ensureModSlots (settings);
        auto& mods = settings["modulations"];

        /*  VARY keeps the macros it was given.

            Rewiring ran on every VARY, and a macro that already had a routing
            got another one to a destination not yet taken, and was renamed for
            the new one, while the old routing stayed. After a few VARYs a knob
            named for the cutoff moved three other things as well. The names and
            destinations are read back instead, for the summary shown under the
            strip.
        */
        if (r.base != nullptr)
        {
            for (int i = 0; i < kMacros; ++i)
            {
                const auto source = "macro_control_" + std::to_string (i + 1);
                for (const auto& slot : mods)
                    if (modSource (slot) == source)
                    {
                        result.macroDests[static_cast<size_t> (i)] = modDest (slot);
                        break;
                    }
                const auto key = "macro" + std::to_string (i + 1);
                if (doc.contains (key) && doc[key].is_string())
                    result.macroNames[static_cast<size_t> (i)] = doc[key].get<std::string>();
            }
            return;
        }

        /*  Four wired, named macros on every patch. Macros are the most used
            modulation source in hand-made presets, ahead of the LFOs, and
            they are the only part a producer can automate from the host. A
            patch without them is a sound rather than an instrument.
        */
        const auto& wanted = archetype::macroDestinationsFor (r.style);
        const auto& axisList = axes::all();

        for (int i = 0; i < kMacros; ++i)
        {
            const auto source = "macro_control_" + std::to_string (i + 1);

            std::set<std::string> taken;
            for (const auto& slot : mods)
                if (modSource (slot).rfind ("macro_control", 0) == 0)
                    taken.insert (modDest (slot));

            std::vector<std::string> candidates;
            for (const auto* d : wanted)
                if (settings.contains (d) && taken.count (d) == 0)
                    candidates.push_back (d);

            // Fall back to the axis destinations if the style list runs out.
            const auto& axis = axisList[static_cast<size_t> (i) % axisList.size()];
            for (const auto& d : axis.macroDests)
                if (settings.contains (d) && taken.count (d) == 0)
                    candidates.push_back (d);

            std::vector<std::string> live;
            for (const auto& d : candidates)
                if (destinationIsLive (d, settings))
                    live.push_back (d);
            const auto& usable = live.empty() ? candidates : live;
            if (usable.empty())
                continue;

            const auto strength = r.sliders.count (axis.key) > 0 ? r.sliders.at (axis.key) : 0.5f;
            if (setModSlot (settings, source, usable.front(), 0.3f + 0.5f * strength, false))
            {
                result.macroDests[static_cast<size_t> (i)] = usable.front();
                if (! settings.contains (source))
                    settings[source] = 0.5;
            }
        }

        std::set<std::string> seen;
        for (int i = 0; i < kMacros; ++i)
        {
            const auto& dest = result.macroDests[static_cast<size_t> (i)];
            auto name = macroName (dest, i + 1);
            // Two knobs named alike are worse than one clumsy name, so the
            // second of any pair falls back to its parameter's own.
            if (seen.count (name) > 0)
                name = rawName (dest, i + 1);
            seen.insert (name);
            result.macroNames[static_cast<size_t> (i)] = name;
            doc["macro" + std::to_string (i + 1)] = name;
        }
    }

    const std::vector<Scale>& sequenceScales()
    {
        /*  The scales a sequence may walk.

            The steps used to be chosen at random and snapped to the nearest
            semitone by Vital's quantiser with every bit of its mask set. That
            is a scale of everything, which is why a sequence sounded like it
            was picking notes rather than playing them.

            Now the interval is chosen first and the step placed exactly where
            it falls, which is also what lets a maqam in: a quarter tone is not
            a semitone and no twelve bit mask can hold one.

            Weighted toward what stays musical under a random walk, since that
            is what a stepped LFO is. A pentatonic has no wrong note in it; a
            major scale has a semitone that wants arriving at rather than
            jumping to. The maqamat are the 24-TET approximations theory uses,
            and real practice varies the intonation by region and player, so
            these are a fair rendering rather than the last word.
        */
        static const std::vector<Scale> scales = {
            { "minor pentatonic",  { 0, 3, 5, 7, 10 } },
            { "minor pentatonic",  { 0, 3, 5, 7, 10 } },
            { "major pentatonic",  { 0, 2, 4, 7, 9 } },
            { "major pentatonic",  { 0, 2, 4, 7, 9 } },
            { "natural minor",     { 0, 2, 3, 5, 7, 8, 10 } },
            { "natural minor",     { 0, 2, 3, 5, 7, 8, 10 } },
            { "dorian",            { 0, 2, 3, 5, 7, 9, 10 } },
            { "phrygian",          { 0, 1, 3, 5, 7, 8, 10 } },
            { "blues",             { 0, 3, 5, 6, 7, 10 } },
            { "major",             { 0, 2, 4, 5, 7, 9, 11 } },
            { "mixolydian",        { 0, 2, 4, 5, 7, 9, 10 } },
            { "lydian",            { 0, 2, 4, 6, 7, 9, 11 } },
            { "harmonic minor",    { 0, 2, 3, 5, 7, 8, 11 } },
            { "whole tone",        { 0, 2, 4, 6, 8, 10 } },
            { "octatonic",         { 0, 1, 3, 4, 6, 7, 9, 10 } },
            { "minor triad",       { 0, 3, 7, 12 } },
            { "major triad",       { 0, 4, 7, 12 } },
            { "diminished seventh",{ 0, 3, 6, 9 } },
            { "augmented",         { 0, 4, 8 } },
            // The quarter tone ones. Hijaz, Nahawand and Kurd are twelve tone
            // maqamat and sit above with their western names.
            { "maqam rast",        { 0, 2, 3.5f, 5, 7, 9, 10.5f } },
            { "maqam bayati",      { 0, 1.5f, 3, 5, 7, 8, 10 } },
            { "maqam saba",        { 0, 1.5f, 3, 4, 7, 8, 10 } },
            { "maqam huzam",       { 0, 1.5f, 2.5f, 5, 6.5f, 8.5f, 11 } },
            { "maqam hijaz",       { 0, 1, 4, 5, 7, 8, 10 } },
            { "maqam nahawand",    { 0, 2, 3, 5, 7, 8, 11 } },
            /*  No scale at all, which is where this started.

                Every step drawn on its own and snapped to the nearest
                semitone, so the line leaps about with nothing pulling it
                anywhere. Walking a scale replaced it because it sounded like a
                sequence picking notes rather than playing them, which it does,
                but picking notes is a sound in its own right and a machine one
                that none of the scales above will give you.

                One entry among all of these, so a roll left on Random scale lands
                here about as often as it lands on any single scale.
            */
            { "random steps",      {},  1.0f },
            /*  The same again, on quarter tones.

                Between every pair of semitones there is another step, so the
                line lands on pitches the keyboard has no key for. The maqamat
                above use the same quarter tone grid and sound like music
                because they pick seven of them and stay there; this picks
                freely from all twenty four, which is the sound of something
                being tuned rather than played, and is the point of it.

                Placed rather than quantised. Vital's quantiser works in whole
                semitones and would round every one of these away.
            */
            { "random quarter tones", {}, 0.5f },
        };;
        return scales;
    }

    const std::vector<std::pair<std::string, int>>& sequenceScaleMenu()
    {
        static const std::vector<std::pair<std::string, int>> menu = []
        {
            std::vector<std::pair<std::string, int>> unique;
            const auto& all = sequenceScales();
            for (int i = 0; i < (int) all.size(); ++i)
            {
                const std::string name = all[(size_t) i].name;
                const auto seen = std::any_of (unique.begin(), unique.end(),
                                               [&] (const auto& e) { return e.first == name; });
                if (! seen)
                    unique.emplace_back (name, i);
            }
            return unique;
        }();
        return menu;
    }

    void Generator::constrainPitch (const Request& r, nlohmann::json& settings,
                                    std::mt19937& rng, int& scaleUsed)
    {
        /*  Keep the note on the note.

            A player pressing C expects to hear a C. Small depths are vibrato
            and stay. A sequence is the exception, since stepping between
            pitches is the entire idea, and there the steps are snapped to
            semitones and squared off instead of gliding.
        */
        if (! settings.contains ("modulations") || ! settings["modulations"].is_array())
            return;

        const auto sequenced = r.style == "Sequence";
        const auto freePitch = sequenced || r.style == "SFX" || r.style == "Experiment";
        const auto cap = freePitch ? 1.0f : 0.06f;

        auto& mods = settings["modulations"];
        std::set<std::string> pitchDrivers;

        for (size_t i = 0; i < mods.size(); ++i)
        {
            const auto source = modSource (mods[i]);
            const auto dest = modDest (mods[i]);
            if (source.empty() || ! axes::isPitchDestination (dest))
                continue;

            pitchDrivers.insert (source);

            // A drum's pitch drop falls into the note, so it is part of the hit
            // rather than a wrong note, and shapeDrum sets how deep it goes.
            if (r.style == "Percussion" && source.rfind ("env_", 0) == 0)
                continue;

            const auto key = "modulation_" + std::to_string (i + 1) + "_amount";
            const auto amount = (float) settings.value (key, 0.0);
            if (std::abs (amount) > cap)
                settings[key] = amount < 0.0f ? -cap : cap;
        }

        // VARY keeps the riff it was given: its scale, its steps and its rate.
        if (pitchDrivers.empty() || ! sequenced || r.base != nullptr)
            return;

        /*  A scale, not all twelve semitones.

            The steps used to be chosen at random and snapped to the nearest
            semitone by Vital's quantiser, with every bit of the mask set. That
            is a scale of everything, which is why a sequence sounded like it was
            picking notes rather than playing them.

            Now the interval is chosen first, from a named scale, and the step is
            placed exactly where that interval falls. Nothing is snapped, which
            is what lets a maqam in: a quarter tone is not a semitone and no
            twelve bit mask can hold one.

            The masks lean toward scales that stay musical under a random walk,
            which is what a stepped LFO is. A pentatonic has no wrong note in it;
            a major scale has a semitone that wants arriving at rather than
            jumping to. The maqamat are the 24-TET approximations that theory
            uses, and real practice varies the intonation by region and player,
            so these are a fair rendering rather than the last word.
        */
        /*  Drawn whether or not it is used.

            Naming a scale must not change any other note in the patch, and
            the only way to guarantee that is to take the same number out of
            the stream either way. Skipping the draw when the scale is already
            known would shift everything downstream of it, so a recipe replayed
            from history would come back as a different sound.
        */
        const auto& all = sequenceScales();
        const auto drawn = std::uniform_int_distribution<size_t> (0, all.size() - 1) (rng);
        const auto chosen = r.scale >= 0 && r.scale < (int) all.size()
                                ? (size_t) r.scale
                                : drawn;
        const auto& scale = all[chosen];
        scaleUsed = (int) chosen;

        /*  One depth, fixed, because the placement depends on it.

            A step is put where it belongs by inverting the depth, so the depth
            has to be known when the shape is written. A quarter of the range is
            an octave either side of the played note, which is as far as a riff
            travels before the ear loses the root.
        */
        constexpr float kSequenceDepth = 0.25f;

        for (size_t i = 0; i < mods.size(); ++i)
        {
            if (modSource (mods[i]).empty() || ! axes::isPitchDestination (modDest (mods[i])))
                continue;

            /*  A riff belongs in transpose, not in tune.

                Tune and detune range are fractions of a semitone wide. A
                sequence written into one of them is a scale compressed into a
                wobble, which is not a wrong note so much as no note at all, and
                the depth below is calibrated against transpose in any case.
            */
            auto dest = modDest (mods[i]);
            for (const auto* tail : { "_tune", "_detune_range" })
            {
                const std::string suffix = tail;
                if (dest.size() > suffix.size()
                    && dest.compare (dest.size() - suffix.size(), suffix.size(), suffix) == 0)
                {
                    const auto moved = dest.substr (0, dest.size() - suffix.size()) + "_transpose";
                    if (settings.contains (moved)
                        && ! modDestExists (mods, modSource (mods[i]), moved))
                    {
                        mods[i]["destination"] = moved;
                        dest = moved;
                    }
                }
            }

            const auto n = std::to_string (i + 1);
            settings["modulation_" + n + "_amount"] =
                (float) settings.value ("modulation_" + n + "_amount", 0.0) < 0.0f
                    ? -kSequenceDepth : kSequenceDepth;
            settings["modulation_" + n + "_bipolar"] = 1.0;
        }

        /*  An envelope fires once, and the riff does not.

            Everything above assumes the key is pressed for each note. A
            sequence presses it once and lets the LFO do the playing, so an
            envelope with no sustain opens the filter or lifts the level for the
            first step and then shuts for every step after it. What that sounds
            like is a loud pluck and then the layer going away, which is what it
            is: the patch is shaped like a struck note and the part is not one.

            The amplitude envelope already leans long for this style. These are
            the other five, which nothing was holding, and only where they reach
            something that decides how loud or how open the patch is. An
            envelope on a wavetable position may decay as it likes; that is a
            timbre settling, not the riff being cut off.
        */
        static const std::vector<std::regex> gates = {
            std::regex (".*_level$"), std::regex ("^volume$"),
            std::regex ("^filter_(\\d|fx)_cutoff$"),
            std::regex ("^filter_(\\d|fx)_resonance$"),
            std::regex ("^distortion_drive$"),
        };

        for (const auto& slot : mods)
        {
            const auto source = modSource (slot);
            // Envelope one is the amplitude envelope and has its own rules.
            if (source.rfind ("env_", 0) != 0 || source == "env_1")
                continue;

            const auto dest = modDest (slot);
            auto gating = false;
            for (const auto& rx : gates)
                if (std::regex_match (dest, rx))
                    gating = true;
            if (! gating)
                continue;

            /*  Measured: at a floor of a half the worst layers still fell away
                by more than double, and at seven tenths the collapse went from
                two and a half times down to one and a bit. The envelope still
                has its attack, which is a real accent on the first step, it
                just no longer takes the rest of the riff with it.
            */
            const auto key = "env_" + source.substr (4) + "_sustain";
            if (settings.contains (key) && settings.value (key, 1.0) < 0.70)
                settings[key] = 0.70;
        }

        /*  Nothing to snap when the value is already exact, and snapping is
            what would round a quarter tone away.

            The exception is the scale with no degrees in it, where the step
            lands wherever the draw put it and there is nothing to be exact
            about. Every bit of the twelve set sends each step to its nearest
            semitone, which is the difference between a machine playing notes
            and a siren.
        */
        const auto quantize = std::abs (scale.randomStep - 1.0f) < 1.0e-4f ? 4095 : 0;
        for (const auto& osc : { "osc_1", "osc_2", "osc_3", "sample" })
        {
            const auto key = std::string (osc) + "_transpose_quantize";
            if (settings.contains (key))
                settings[key] = quantize;
        }

        if (settings.contains ("lfos") && settings["lfos"].is_array())
        {
            auto& lfos = settings["lfos"];
            for (const auto& source : pitchDrivers)
            {
                if (source.rfind ("lfo_", 0) != 0)
                    continue;
                const auto index = std::atoi (source.c_str() + 4) - 1;
                if (index < 0 || index >= (int) lfos.size())
                    continue;

                {
                    const auto shape = wavetable::createStepShape (
                        rng, scale.degrees, kSequenceDepth, r.gesture, scale.randomStep);
                    const auto stepCount = juce::jmax (1, (int) shape.value ("num_points", 2) / 2);
                    lfos[(size_t) index] = shape;

                    /*  A rate somebody could follow.

                        MOVE flips the sync mode of any LFO, and on this one that
                        is the difference between a sequence and a smear: free
                        running it changed pitch twenty three times a second.
                        Measured, a synced cycle lasts two to the power of seven
                        minus the tempo, in seconds, so the tempo that puts one
                        step on an eighth or a sixteenth at 120 BPM falls out of
                        the step count.

                        Working that through, a step lasts two to the power of
                        seven minus this number, over four. Six is an eighth and
                        seven is a sixteenth. Eight was in here as well and is a
                        thirty second, which at five or six steps to the cycle
                        arrives ten times a second and stops reading as notes.

                        Rounded down rather than to nearest, because a step
                        count that is not a power of two lands between two
                        tempos and the ear forgives the slower one.
                    */
                    const auto perStep = uniform (rng) < 0.6f ? 7.0 : 6.0;   // sixteenths or eighths
                    auto tempo = juce::jlimit (5, 9, (int) std::floor (
                        perStep - std::log2 ((double) stepCount / 4.0)));

                    /*  Then bounded by the step rather than by the tempo.

                        Only whole tempos exist, so a step count between two of
                        them gets pushed a whole octave and five steps at an
                        eighth came out at four fifths of a second each, which
                        wanders rather than repeats. Between a fifth of a second
                        and three fifths a step reads as a riff at either end,
                        so that is the range, and the tempo is moved until the
                        step falls inside it.
                    */
                    const auto stepSeconds = [stepCount] (int t)
                    { return std::pow (2.0, 7.0 - t) / (double) stepCount; };

                    while (stepSeconds (tempo) > 0.60 && tempo < 9)
                        ++tempo;
                    while (stepSeconds (tempo) < 0.20 && tempo > 5)
                        --tempo;

                    const auto lfo = "lfo_" + std::to_string (index + 1) + "_";
                    settings[lfo + "sync"] = 1.0;          // tempo, not free running
                    settings[lfo + "tempo"] = (double) tempo;
                    settings[lfo + "sync_type"] = 0.0;

                    carrySequenceToEveryVoice (settings, source, kSequenceDepth);
                }
            }
        }
    }

    void Generator::carrySequenceToEveryVoice (nlohmann::json& settings,
                                               const std::string& driver, float depth)
    {
        /*  If one voice steps and three hold, what you hear is a held note.

            A sequence used to be one oscillator's transpose, and whatever else
            the patch had switched on carried on sounding the note that was
            played. Three static sources against one moving one is a drone with
            a riff somewhere behind it, and a steady note is easier to hear than
            a moving one at the same level anyway, so the riff loses twice.

            Everything that is on and pitched now takes the same steps, at the
            same depth and the same sign, so the patch moves as one instrument.
            A pedal under a riff is a good sound and this gives it up, but the
            style is called Sequence, and the sequence should be the thing you
            hear.
        */
        auto& mods = settings["modulations"];

        auto sign = 1.0f;
        std::set<std::string> stepping;

        for (size_t i = 0; i < mods.size(); ++i)
        {
            const auto dest = modDest (mods[i]);
            if (modSource (mods[i]) != driver || ! axes::isPitchDestination (dest))
                continue;

            const auto amount = (float) settings.value (
                "modulation_" + std::to_string (i + 1) + "_amount", 0.0);
            if (amount < 0.0f)
                sign = -1.0f;

            for (const auto* name : { "osc_1", "osc_2", "osc_3", "sample" })
                if (dest.rfind (std::string (name) + "_", 0) == 0)
                    stepping.insert (name);
        }

        // Driving the whole voice at once already covers everything, and
        // doubling it up per oscillator would step twice as far.
        if (stepping.empty() || modDestExists (mods, driver, "voice_transpose"))
            return;

        for (const auto* name : { "osc_1", "osc_2", "osc_3", "sample" })
        {
            if (settings.value (std::string (name) + "_on", 0.0) < 0.5)
                continue;
            if (stepping.count (name) > 0)
                continue;
            setModSlot (settings, driver, std::string (name) + "_transpose",
                        sign * depth, true);
        }
    }

    // --------------------------------------------------------------- repair ---

    std::vector<std::string> Generator::repair (nlohmann::json& settings, int drumKind)
    {
        std::vector<std::string> notes;

        /*  Three of the rules below are wrong for a drum, and a drum's own
            bands already hold what they would. A hi-hat, a snare's rattle, a
            clap and a shaker are noise first, so the noise layer may be louder
            than the shared ceiling and louder than the oscillators, and a clap
            or shaker has no oscillator at all. Run on a drum, those rules put
            every snare's noise at the same quiet level under its body and
            switched an oscillator back on under every clap.
        */
        const auto* drum = drums::at (drumKind);

        // Nothing may sit outside the range it is allowed to be in.
        for (auto it = settings.begin(); it != settings.end(); ++it)
        {
            if (! it.value().is_number())
                continue;
            // A drum's own bands hold these, and a hat's cutoff and a high
            // pass's blend both sit above the shared ceiling.
            if (drum != nullptr && (it.key() == "sample_level" || it.key() == "filter_1_cutoff"
                                    || it.key() == "filter_1_blend"))
                continue;
            float low = 0.0f, high = 0.0f;
            if (! archetype::rangeFor (it.key(), low, high))
                continue;
            const auto v = it.value().get<float>();
            if (v < low || v > high)
                it.value() = juce::jlimit (low, high, v);
        }

        /*  An oscillator switched on at zero level is not a quiet oscillator,
            it is a switch left in the wrong position, and Vital shows it as an
            active oscillator making no sound.
        */
        for (int i = 1; i <= 3; ++i)
        {
            const auto on = "osc_" + std::to_string (i) + "_on";
            const auto level = "osc_" + std::to_string (i) + "_level";
            if (settings.value (on, 0.0) >= 0.5 && settings.value (level, 0.0) <= 0.02)
            {
                settings[on] = 0.0;
                notes.push_back ("osc " + std::to_string (i) + " was on but silent, switched off");
            }
        }

        /*  Whatever was aimed at an oscillator has to go when that oscillator
            does, or the patch keeps paying for it: modulation slots spent
            driving something silent, and a second filter left being fed by
            nothing at all.
        */
        for (int i = 1; i <= 4; ++i)
        {
            // 1..3 are the oscillators, 4 is the noise layer, which carries the
            // same switch and level pair and so goes wrong the same way.
            const auto name = i <= 3 ? "osc_" + std::to_string (i) : std::string ("sample");
            if (settings.value (name + "_on", 0.0) >= 0.5)
                continue;

            const auto prefix = name + "_";
            int cleared = 0;
            for (auto& slot : settings["modulations"])
            {
                if (modSource (slot).empty() || modDest (slot).rfind (prefix, 0) != 0)
                    continue;
                slot["source"] = "";
                slot["destination"] = "";
                ++cleared;
            }
            if (cleared > 0)
                notes.push_back (name + " is off, dropped " + std::to_string (cleared)
                                 + " routing(s) aimed at it");

            /*  Filter 2 in parallel is fed by this oscillator alone, so with the
                oscillator gone it processes silence. Put it back in series
                behind filter 1, where it still has something to work on.
            */
            if (settings.value (prefix + "destination", 0.0) >= 0.5
                && settings.value ("filter_2_on", 0.0) >= 0.5
                && settings.value ("filter_2_filter_input", 0.0) < 0.5)
            {
                settings[prefix + "destination"] = 0.0;
                settings["filter_2_filter_input"] = 1.0;
                notes.push_back ("filter 2 was fed only by the silent " + name
                                 + ", moved it in series");
            }
        }

        // A pitched instrument needs a pitched source. Noise alone is a sweep.
        int audibleOscs = 0;
        for (int i = 1; i <= 3; ++i)
            if (settings.value ("osc_" + std::to_string (i) + "_on", 0.0) >= 0.5
                && settings.value ("osc_" + std::to_string (i) + "_level", 0.0) > 0.02)
                ++audibleOscs;

        if (audibleOscs == 0 && (drum == nullptr || drum->tonal))
        {
            settings["osc_1_on"] = 1.0;
            settings["osc_1_level"] = std::max (0.6, settings.value ("osc_1_level", 0.0));
            notes.push_back ("no oscillator was audible, switched osc 1 on");
        }

        // Noise belongs under the oscillators, not over them, and a noise layer
        // switched on at no level is the same switch in the wrong position an
        // oscillator can be left in.
        if (drum == nullptr && settings.value ("sample_on", 0.0) >= 0.5
            && settings.value ("sample_level", 0.0) > 0.45)
        {
            settings["sample_level"] = 0.3;
            notes.push_back ("noise was louder than the oscillators, brought down");
        }
        if (settings.value ("sample_on", 0.0) >= 0.5
            && settings.value ("sample_level", 0.0) <= 0.02)
        {
            settings["sample_on"] = 0.0;
            notes.push_back ("noise was on but silent, switched off");
        }

        /*  Keep the patch centred. A randomised pan does not read as width, it
            reads as a broken channel.
        */
        for (const auto& panKey : { "osc_1_pan", "osc_2_pan", "osc_3_pan", "sample_pan" })
            if (settings.contains (panKey))
                settings[panKey] = 0.0;
        for (int i = 1; i <= 3; ++i)
        {
            const auto spread = "osc_" + std::to_string (i) + "_stereo_spread";
            if (settings.contains (spread) && settings[spread].get<double>() < 0.6)
                settings[spread] = 1.0;
        }

        // A zero sustain paired with an instant decay is a click and nothing
        // else, which reads as a broken patch rather than a short one.
        if (settings.value ("env_1_sustain", 1.0) < 0.02
            && settings.value ("env_1_decay", 1.0) < 0.05)
        {
            settings["env_1_decay"] = 0.35;
            notes.push_back ("amp envelope was inaudible, lengthened decay");
        }
        if (settings.value ("env_1_release", 0.0) < 0.01)
            settings["env_1_release"] = 0.15;

        // Self-oscillation at a very low cutoff is a screech, not a bass patch.
        for (int i = 1; i <= 2; ++i)
        {
            const auto cutoff = "filter_" + std::to_string (i) + "_cutoff";
            const auto reso = "filter_" + std::to_string (i) + "_resonance";
            if (settings.value (cutoff, 60.0) < 24.0 && settings.value (reso, 0.0) > 0.85)
            {
                settings[reso] = 0.7;
                notes.push_back ("tamed filter " + std::to_string (i) + " self-oscillation");
            }
        }

        // Level matching moves this afterwards, so it starts from the middle
        // with room in both directions.
        settings["volume"] = loudness::kVolumeDefault;
        return notes;
    }

    // ----------------------------------------------------------------- roll ---

    Result Generator::roll (const Request& request)
    {
        Result result;

        if (! isReady())
        {
            result.error = "the init patch has not been read from Vital yet";
            return result;
        }

        const auto* style = archetype::find (request.style);
        if (style == nullptr)
        {
            result.error = "unknown style: " + request.style;
            return result;
        }

        auto seed = request.seed;
        if (seed == 0)
            seed = std::random_device {}() | 1u;
        result.seed = seed;

        /*  Structure and values come from separate streams so a seed pins the
            shape of a patch while the sliders stay free to move. Sharing one
            stream means nudging a slider silently reshuffles everything and the
            patch the user was working on disappears.
        */
        std::mt19937 srng (seed);
        std::mt19937 prng (seed ^ 0x9E3779B9u);

        /*  And the synthesised content from streams of its own.

            The sample and the wavetables are built from BRIGHT and DIRT, and
            how many numbers they draw depends on both. They used to draw from
            the structure stream, so moving either slider moved every draw after
            them, the wiring included: two rolls of one seed at a low and a high
            DIRT differed in more than a third of their routing slots, which is
            exactly what the two streams above were meant to prevent.
        */
        std::mt19937 samplerng (seed ^ 0x5A3B1E00u);
        std::mt19937 contentrng (seed ^ 0xC0A7E700u);

        /*  One batch at one slider setting should still hold a sparse patch and
            a busy one. A real library varies because different people made the
            presets in it, so each roll wobbles around the setting instead of
            sitting exactly on it. Squaring the offset keeps most rolls near
            where the slider was put and lets the occasional one go a long way.
        */
        Request adjusted = request;
        {
            const auto asked = request.sliders.count ("complexity") > 0
                                   ? request.sliders.at ("complexity") : 0.5f;
            std::uniform_real_distribution<float> u (-1.0f, 1.0f);
            const auto off = u (srng);
            adjusted.sliders["complexity"] = juce::jlimit (0.0f, 1.0f,
                asked + off * std::abs (off) * 0.38f * request.complexityWobble);
        }
        /*  Which drum, for percussion, decided before anything is built.

            A VARY keeps the kind of the patch it started from, read back from
            the patch, so a VARY of a kick is never clamped into a hi-hat's
            bands by whatever the menu happens to allow. A fresh roll takes the
            kind it was asked for, or draws one from a stream of its own. The
            kind's archetype then stands in for the generic percussion one.
        */
        archetype::Archetype drumArchetype;
        if (request.style == "Percussion")
        {
            std::mt19937 krng (seed ^ 0xD7E50000u);
            const auto drawn = std::uniform_int_distribution<int> (
                0, (int) drums::all().size() - 1) (krng);
            auto kind = -1;
            if (request.base != nullptr)
                kind = drums::indexOf (request.base->value ("drum_kind", std::string {}));
            else
                kind = drums::at (request.drumKind) != nullptr ? request.drumKind : drawn;

            adjusted.drumKind = kind;
            result.drumKind = kind;
            if (const auto* k = drums::at (kind))
            {
                drumArchetype = *style;
                drumArchetype.settings.insert (drumArchetype.settings.end(),
                                               k->settings.begin(), k->settings.end());
                drumArchetype.routings.insert (drumArchetype.routings.end(),
                                               k->routings.begin(), k->routings.end());
                style = &drumArchetype;
            }
        }

        const Request& r = adjusted;
        result.amount = r.amount;

        // VARY keeps the patch it started from. A fresh roll starts from init
        // with the archetype over it.
        result.preset = r.base != nullptr ? *r.base : initPreset;

        if (! result.preset.contains ("settings") || ! result.preset["settings"].is_object())
        {
            result.error = "the starting patch had no settings block";
            return result;
        }

        auto& settings = result.preset["settings"];

        if (r.base == nullptr)
        {
            for (const auto& slot : { "modulations" })
                settings.erase (slot);
            ensureModSlots (settings);
            applyArchetype (*style, r, settings);
            applyComplexity (r, settings, srng);
            synthesiseSample (r, settings, samplerng);
        }

        /*  VARY keeps what was synthesised: the wavetables, the LFO shapes and
            with them a sequence's riff. They used to be written afresh on
            every VARY, whatever its depth, so even the smallest VARY swapped
            the oscillators' timbre and the shape of every LFO for new ones.
        */
        if (r.base == nullptr)
            synthesiseContent (r, settings, contentrng);
        applyAxes (r, settings, prng);
        switchDistortion (r, settings);
        chooseWarp (r, settings, seed);
        if (r.base == nullptr)
            wireRouting (*style, r, settings, srng);
        wireMacros (r, result.preset, settings, result);
        scaleModulationDepth (r, settings);
        varyMotion (r, settings, seed);
        keepStruckNotesStruck (r, settings);
        shapeDistortion (r, settings, seed);
        applyNoteShape (r, settings, prng);
        shapeDrum (r, settings, seed);
        constrainPitch (r, settings, srng, result.scale);
        result.repairs = repair (settings, r.drumKind);

        result.preset["preset_style"] = r.style;
        result.preset["preset_name"] = r.name;
        if (const auto* k = drums::at (r.drumKind))
        {
            result.preset["drum_kind"] = std::string (k->name);
            /*  Named for what it is, the way a sequence's line is named for its
                shape. Nothing in the patch said which drum it had been built
                as, so a batch file called Percussion 4 could have been any of
                twelve. */
            result.preset["preset_name"] = r.name.empty() ? std::string (k->name)
                                                          : r.name + " (" + k->name + ")";
            nameLfosForWhatTheyMove (settings);
        }
        result.preset["author"] = "";
        // Doubles as the marker the host checks to confirm Vital took the patch.
        result.preset["comments"] = "vr:" + std::to_string (seed);

        for (const auto& slot : settings["modulations"])
            if (! modSource (slot).empty())
                ++result.routings;

        result.ok = true;
        return result;
    }
}
