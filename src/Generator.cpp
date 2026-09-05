#include "Generator.h"

#include <algorithm>
#include <cmath>
#include <regex>

#include <juce_core/juce_core.h>

#include "Axes.h"
#include "Loudness.h"
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

        bool setModSlot (nlohmann::json& settings, const std::string& source,
                         const std::string& dest, float amount, bool bipolar)
        {
            if (source.empty() || dest.empty() || ! settings.contains (dest))
                return false;

            auto& mods = settings["modulations"];
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
                { std::regex ("^distortion_"), "DRIVE" },
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
        };

        NoteShape noteShapeFor (const std::string& style)
        {
            //                        attack  decay  sustain release susCeil relLo relHi  decLo decHi
            if (style == "Bass")       return { -0.5f, +0.3f, -1.0f, -0.4f, 0.0f,  0.15f, 0.45f, 1.08f, 1.30f };
            if (style == "Percussion") return { -0.8f, -0.2f, -1.0f, -0.5f, 0.0f,  0.10f, 0.35f, 0.88f, 1.12f };
            if (style == "Keys")       return { -0.4f, +0.1f, -0.6f, -0.2f, 0.55f, 0.20f, 0.0f,  1.00f, 1.28f };
            if (style == "Lead")       return { -0.1f, +0.2f, +0.4f, +0.3f, 1.0f,  0.0f,  0.0f,  0.0f,  0.0f };
            if (style == "Pad")        return { +0.7f, +0.5f, +0.7f, +0.6f, 1.0f,  0.0f,  0.0f,  0.0f,  0.0f };
            if (style == "Sequence")   return { -0.3f, +0.1f, +0.5f, -0.2f, 1.0f,  0.0f,  0.0f,  0.0f,  0.0f };
            return { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
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

            if (! mustStopDead && chance (0.45f))
            {
                settings["sample_on"] = 1.0;
                // Kept low on purpose. Noise is flat all the way up, so it
                // shifts a patch's brightness far more than its level suggests.
                settings["sample_level"] = 0.07 + 0.11 * uniform (rng);
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

        if (r.locks.count (schema::Section::fx) == 0)
        {
            // Effects are where a simple patch and an involved one differ most
            // audibly, so the sparse end really does switch things off.
            if (complexity < 0.3f)
            {
                for (const auto& fx : { "phaser_on", "flanger_on", "chorus_on" })
                    if (settings.contains (fx))
                        settings[fx] = 0.0;
            }
            else if (! mustStopDead)
            {
                for (const auto& fx : { "phaser_on", "flanger_on" })
                    if (settings.contains (fx) && chance (0.55f))
                        settings[fx] = 1.0;
            }
        }
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
                const auto p = skew (uniform (rng), (value - 0.5f) * 2.0f * member.second);
                float sampled = 0.0f;
                if (sampleInRange (member.first, p, sampled))
                {
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
                if (! settings.contains (sw)
                    || r.locks.count (schema::sectionOf (sw)) > 0)
                    continue;
                if (uniform (rng) < push)
                    settings[sw] = value > 0.5f ? 1.0 : 0.0;
            }

            // Indexed parameters have no meaningful ordering, since Vital's
            // eight filter models are different circuits rather than a
            // brightness ramp, so an extreme slider flips them.
            for (const auto& param : axes::flippable (axis, keys))
            {
                if (r.locks.count (schema::sectionOf (param)) > 0)
                    continue;
                if (uniform (rng) < push * 0.5f)
                {
                    settings[param] = std::floor (uniform (rng) * 6.0f);
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
            if (section == schema::Section::excluded || r.locks.count (section) > 0)
                continue;
            if (uniform (rng) > r.amount * breadth)
                continue;

            float low = 0.0f, high = 0.0f;
            if (! archetype::rangeFor (key, low, high))
                continue;

            const auto current = settings[key].get<float>();
            const auto at = (current - low) / juce::jmax (1.0e-6f, high - low);
            const auto moved = juce::jlimit (0.0f, 1.0f, at + gaussian (rng, spread));
            settings[key] = low + moved * (high - low);
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

        for (const auto& stage : stages)
        {
            if (std::abs (stage.second) < 1.0e-6f)
                continue;
            float sampled = 0.0f;
            if (sampleInRange (stage.first, skew (uniform (rng), stage.second), sampled))
                settings[stage.first] = sampled;
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
            settings["env_1_decay"] = shape.decayFloor
                                          + uniform (rng) * (shape.decayCeiling - shape.decayFloor);

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
        if (settings.contains ("osc_1_transpose")
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
            // distortion_mix and distortion_drive both read as DRIVE, and two
            // identically named knobs are worse than one clumsy name.
            if (seen.count (name) > 0)
                name = rawName (dest, i + 1);
            seen.insert (name);
            result.macroNames[static_cast<size_t> (i)] = name;
            doc["macro" + std::to_string (i + 1)] = name;
        }
    }

    void Generator::constrainPitch (const Request& r, nlohmann::json& settings,
                                    std::mt19937& rng)
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

            const auto key = "modulation_" + std::to_string (i + 1) + "_amount";
            const auto amount = (float) settings.value (key, 0.0);
            if (std::abs (amount) > cap)
                settings[key] = amount < 0.0f ? -cap : cap;
        }

        if (pitchDrivers.empty() || ! sequenced)
            return;

        // All twelve semitones, so a swept transpose lands on notes.
        for (const auto& osc : { "osc_1", "osc_2", "osc_3", "sample" })
        {
            const auto key = std::string (osc) + "_transpose_quantize";
            if (settings.contains (key))
                settings[key] = 4095;
        }

        if (settings.contains ("lfos") && settings["lfos"].is_array())
        {
            auto& lfos = settings["lfos"];
            for (const auto& source : pitchDrivers)
            {
                if (source.rfind ("lfo_", 0) != 0)
                    continue;
                const auto index = std::atoi (source.c_str() + 4) - 1;
                if (index >= 0 && index < (int) lfos.size())
                    lfos[(size_t) index] = wavetable::createStepShape (rng);
            }
        }
    }

    // --------------------------------------------------------------- repair ---

    std::vector<std::string> Generator::repair (nlohmann::json& settings)
    {
        std::vector<std::string> notes;

        // Nothing may sit outside the range it is allowed to be in.
        for (auto it = settings.begin(); it != settings.end(); ++it)
        {
            if (! it.value().is_number())
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

        // A pitched instrument needs a pitched source. Noise alone is a sweep.
        int audibleOscs = 0;
        for (int i = 1; i <= 3; ++i)
            if (settings.value ("osc_" + std::to_string (i) + "_on", 0.0) >= 0.5
                && settings.value ("osc_" + std::to_string (i) + "_level", 0.0) > 0.02)
                ++audibleOscs;

        if (audibleOscs == 0)
        {
            settings["osc_1_on"] = 1.0;
            settings["osc_1_level"] = std::max (0.6, settings.value ("osc_1_level", 0.0));
            notes.push_back ("no oscillator was audible, switched osc 1 on");
        }

        // Noise belongs under the oscillators, not over them.
        if (settings.value ("sample_on", 0.0) >= 0.5
            && settings.value ("sample_level", 0.0) > 0.45)
        {
            settings["sample_level"] = 0.3;
            notes.push_back ("noise was louder than the oscillators, brought down");
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
        const Request& r = adjusted;

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
        }

        synthesiseContent (r, settings, srng);
        applyAxes (r, settings, prng);
        if (r.base == nullptr)
            wireRouting (*style, r, settings, srng);
        wireMacros (r, result.preset, settings, result);
        applyNoteShape (r, settings, prng);
        constrainPitch (r, settings, srng);
        result.repairs = repair (settings);

        result.preset["preset_style"] = r.style;
        result.preset["preset_name"] = r.name;
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
