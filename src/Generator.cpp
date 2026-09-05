#include "Generator.h"

#include <algorithm>
#include <cmath>
#include <regex>

#include <juce_core/juce_core.h>

#include "Axes.h"
#include "WavetableFactory.h"
#include "Loudness.h"

namespace gen
{
    namespace
    {
        constexpr size_t kDonorCacheLimit = 48;

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

        void setModSlot (nlohmann::json& settings, int index, const std::string& source,
                         const std::string& dest, float amount, bool bipolar)
        {
            settings["modulations"][static_cast<size_t> (index)] =
                { { "destination", dest }, { "source", source } };
            const auto n = std::to_string (index + 1);
            settings["modulation_" + n + "_amount"] = amount;
            settings["modulation_" + n + "_bypass"] = 0.0;
            settings["modulation_" + n + "_bipolar"] = bipolar ? 1.0 : 0.0;
            settings["modulation_" + n + "_power"] = 0.0;
            settings["modulation_" + n + "_stereo"] = 0.0;
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
                { std::regex ("^modulation_\\d+_amount$"), "DEPTH" },
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

    void Generator::clearDonorCache()
    {
        donorCache.clear();
        donorOrder.clear();
    }

    const nlohmann::json* Generator::donor (const std::string& path)
    {
        const auto it = donorCache.find (path);
        if (it != donorCache.end())
            return &it->second;

        const juce::File file { juce::String (path) };
        if (! file.existsAsFile())
            return nullptr;

        auto doc = nlohmann::json::parse (file.loadFileAsString().toStdString(), nullptr, false);
        if (doc.is_discarded() || ! doc.contains ("settings") || ! doc["settings"].is_object())
            return nullptr;

        if (donorOrder.size() >= kDonorCacheLimit)
        {
            donorCache.erase (donorOrder.front());
            donorOrder.erase (donorOrder.begin());
        }
        donorOrder.push_back (path);
        return &(donorCache[path] = std::move (doc));
    }

    const nlohmann::json* Generator::pickDonor (const model::Style& style, std::mt19937& rng)
    {
        if (style.donors.empty())
            return nullptr;
        std::uniform_int_distribution<size_t> pick (0, style.donors.size() - 1);
        for (int attempt = 0; attempt < 8; ++attempt)
            if (const auto* d = donor (style.donors[pick (rng)]))
                return d;
        return nullptr;
    }

    bool Generator::sampleValue (const model::Style& style, const std::string& param,
                                 float percentile, std::mt19937& rng, float& out) const
    {
        const auto c = style.continuous.find (param);
        if (c != style.continuous.end())
        {
            out = style.sample (c->second, percentile);
            return true;
        }

        const auto d = style.discrete.find (param);
        if (d != style.discrete.end() && ! d->second.empty())
        {
            int total = 0;
            for (const auto& choice : d->second)
                total += choice.second;
            if (total <= 0)
                return false;
            auto pick = std::uniform_int_distribution<int> (0, total - 1) (rng);
            for (const auto& choice : d->second)
            {
                pick -= choice.second;
                if (pick < 0)
                {
                    out = choice.first;
                    return true;
                }
            }
            out = d->second.front().first;
            return true;
        }
        return false;
    }

    void Generator::splice (const model::Style& style, const Request& r,
                            nlohmann::json& doc, std::mt19937& rng)
    {
        auto& settings = doc["settings"];

        for (int i = 0; i < schema::numSections; ++i)
        {
            const auto section = static_cast<schema::Section> (i);
            if (r.locks.count (section) > 0)
                continue;
            // VARY keeps the patch it started from and only respices the
            // sections it was asked to, so the sound drifts instead of being
            // replaced.
            if (r.base != nullptr && r.varySections.count (section) == 0)
                continue;

            const auto* d = pickDonor (style, rng);
            if (d == nullptr)
                continue;
            const auto& ds = (*d)["settings"];

            for (auto it = ds.begin(); it != ds.end(); ++it)
                if (it.value().is_number() && schema::sectionOf (it.key()) == section)
                    settings[it.key()] = it.value().get<double>();

            // Structural blobs travel with the section that owns them. Split
            // them apart and you get oscillator settings pointing at a
            // wavetable written for a completely different patch.
            // Wavetables, the sample and the LFO shapes are all replaced with
            // generated ones afterwards, so there is no point copying the
            // donor's. Everything else in the section still travels together.
            for (const auto& blob : schema::blobNames())
                if (schema::blobOwner (blob) == section && ds.contains (blob)
                    && blob != "wavetables" && blob != "sample")
                    settings[blob] = ds[blob];

            if (section == schema::Section::mod && ds.contains ("modulations"))
                settings["modulations"] = ds["modulations"];
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

        /*  Every wavetable is written here rather than carried over from the
            donor. That is a licensing matter first, since donor tables belong to
            whoever made the pack, but it is also where the variety comes from:
            a fixed set of donor tables meant the same timbres kept turning up.
        */
        nlohmann::json tables = nlohmann::json::array();
        for (int i = 1; i <= 3; ++i)
        {
            wavetable::Request wr;
            wr.style = r.style;
            wr.bright = bright;
            wr.dirt = dirt;
            wr.move = move;
            wr.oscillator = i;
            tables.push_back (wavetable::create (rng, wr));
        }
        settings["wavetables"] = std::move (tables);

        // LFO shapes are authored content too, and cheap to write.
        if (settings.contains ("lfos") && settings["lfos"].is_array())
        {
            auto& lfos = settings["lfos"];
            for (size_t i = 0; i < lfos.size(); ++i)
                if (i < 4 || uniform (rng) < 0.5f)
                    lfos[i] = wavetable::createLfoShape (rng, move);
        }

        if (! defaultSample.is_null())
            settings["sample"] = defaultSample;
    }

    void Generator::applyAxes (const model::Style& style, const Request& r,
                               nlohmann::json& settings, std::mt19937& rng)
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
                if (sampleValue (style, member.first, p, rng, sampled))
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
            // brightness ramp, so an extreme slider flips them rather than
            // interpolating between them.
            for (const auto& param : axes::flippable (axis, keys))
            {
                if (r.locks.count (schema::sectionOf (param)) > 0)
                    continue;
                if (uniform (rng) < push * 0.5f)
                {
                    float sampled = 0.0f;
                    if (sampleValue (style, param, uniform (rng), rng, sampled))
                    {
                        settings[param] = sampled;
                        touched.insert (param);
                    }
                }
            }
        }

        if (r.amount <= 0.0f)
            return;

        /*  `amount` jitters everything the axes did not claim, nudging each
            value along its own distribution from wherever the donor left it
            rather than drawing a fresh independent one. Independent draws look
            like more variety and are in fact worse: a low cutoff belongs with a
            short decay and a lot of resonance, and resampling the three
            separately throws away exactly the correlations that make a donor
            sound like a deliberate patch.
        */
        const auto spread = 0.10f * r.amount;
        for (const auto& key : keys)
        {
            if (touched.count (key) > 0)
                continue;
            const auto section = schema::sectionOf (key);
            if (section == schema::Section::excluded || r.locks.count (section) > 0)
                continue;
            if (uniform (rng) > r.amount * 0.5f)
                continue;

            const auto c = style.continuous.find (key);
            if (c == style.continuous.end())
            {
                if (uniform (rng) < r.amount * 0.06f)
                {
                    float sampled = 0.0f;
                    if (sampleValue (style, key, uniform (rng), rng, sampled))
                        settings[key] = sampled;
                }
                continue;
            }

            const auto current = settings[key].get<float>();
            auto p = model::Style::percentileOf (c->second, current);
            p = std::max (0.0f, std::min (1.0f, p + gaussian (rng, spread)));
            settings[key] = style.sample (c->second, p);
        }
    }

    void Generator::wireMovement (const model::Style& style, const Request& r,
                                  nlohmann::json& settings, std::mt19937& rng)
    {
        const auto* axis = axes::find ("move");
        if (axis == nullptr || ! axis->hasWiring)
            return;

        const auto slider = r.sliders.count ("move") > 0 ? r.sliders.at ("move") : 0.5f;

        ensureModSlots (settings);
        auto& mods = settings["modulations"];

        int used = 0;
        std::set<std::pair<std::string, std::string>> taken;
        for (const auto& slot : mods)
        {
            const auto src = modSource (slot);
            if (! src.empty())
            {
                ++used;
                taken.insert ({ src, modDest (slot) });
            }
        }

        /*  The slider scales around what presets in this style actually use,
            rather than adding on top of whatever the donor happened to carry.

            Getting this wrong is what made neutral settings produce static
            patches: a spliced donor can arrive with nothing wired but its
            macros, and a patch whose only modulation is four macros sitting at
            their default position does not move at all. Every hand-made preset
            in the library has real modulation, so a generated one starts from
            the same place and the slider moves it from there.
        */
        const auto median = juce::jlimit (4, 40, style.medianRoutings);
        const auto scale = slider <= 0.5f
            ? juce::jmap (slider, 0.0f, 0.5f, 0.35f, 1.0f)
            : juce::jmap (slider, 0.5f, 1.0f, 1.0f, 2.2f);
        const auto target = juce::jlimit (3, kModSlots - kMacros,
                                          juce::roundToInt (median * scale));

        /*  Routings come from what this style actually wires, not one list
            shared by every style.

            This is what separates a bass from a drone. The most common routing
            in the bass presets by a wide margin is a decaying envelope closing
            the filter, which is where the pluck comes from, while a sequence is
            LFOs sweeping pitch and level. Wiring both from a single hardcoded
            list gave basses seven LFOs at full depth and no filter envelope at
            all, which is a perfectly good experimental patch and not a bass.
        */
        const auto& available = style.modRoutings;
        if (available.empty())
            return;

        int totalWeight = 0;
        for (const auto& routing : available)
            totalWeight += routing.count;
        if (totalWeight <= 0)
            return;


        const auto depthFor = [&]
        {
            if (! style.hasModDepth)
                return 0.15f + uniform (rng) * 0.45f;
            // Drawn from the depths the style really uses. Bass sits near 0.38,
            // pad near 0.26, and a flat 0.75 everywhere was most of why patches
            // came out overwrought.
            const auto p = juce::jlimit (0.05f, 0.95f,
                                         uniform (rng) * juce::jmap (slider, 0.6f, 1.0f));
            return juce::jlimit (0.05f, 1.0f, style.sample (style.modDepth, p));
        };

        const auto place = [&] (const std::string& source, const std::string& dest) -> bool
        {
            if (source.empty() || dest.empty()
                || taken.count ({ source, dest }) > 0 || ! settings.contains (dest))
                return false;

            for (int slot = 0; slot < kModSlots; ++slot)
            {
                if (! modSource (mods[static_cast<size_t> (slot)]).empty())
                    continue;
                setModSlot (settings, slot, source, dest, depthFor(), uniform (rng) < 0.30f);
                taken.insert ({ source, dest });
                ++used;
                return true;
            }
            return false;
        };

        // The style's signature routing goes in first. It is the one every
        // patch in the style is built around, so leaving it to chance is what
        // let basses arrive without the thing that makes them basses.
        place (available.front().source, available.front().destination);

        int guard = 0;
        while (used < target && guard++ < 400)
        {

            auto pick = std::uniform_int_distribution<int> (0, totalWeight - 1) (rng);
            for (const auto& routing : available)
            {
                pick -= routing.count;
                if (pick < 0)
                {
                    place (routing.source, routing.destination);
                    break;
                }
            }
        }

    }

    void Generator::wireMacros (const model::Style& style, const Request& r,
                                nlohmann::json& doc, nlohmann::json& settings,
                                Result& result)
    {
        ensureModSlots (settings);
        auto& mods = settings["modulations"];

        /*  Donor macros were wired for the donor's patch. After splicing an
            oscillator section from one preset onto a filter from another they
            often point somewhere that no longer means anything, so they are
            cleared and rebuilt. Predictability matters more here than inherited
            cleverness: macro 1 should be the same axis on every roll.
        */
        for (auto& slot : mods)
        {
            if (modSource (slot).rfind ("macro_control", 0) == 0)
            {
                slot["source"] = "";
                slot["destination"] = "";
            }
        }

        const auto& axisList = axes::all();
        for (int i = 0; i < kMacros; ++i)
        {
            const auto& axis = axisList[static_cast<size_t> (i) % axisList.size()];
            const auto source = "macro_control_" + std::to_string (i + 1);

            std::set<std::string> taken;
            for (const auto& slot : mods)
                if (modSource (slot).rfind ("macro_control", 0) == 0)
                    taken.insert (modDest (slot));

            std::vector<std::string> candidates;
            for (const auto& d : axis.macroDests)
                if (settings.contains (d) && taken.count (d) == 0)
                    candidates.push_back (d);
            for (const auto& d : style.macroDests)
                if (settings.contains (d.first) && taken.count (d.first) == 0)
                    candidates.push_back (d.first);

            // Prefer somewhere the patch can actually hear, but fall back to a
            // dead destination rather than leaving the macro unwired.
            std::vector<std::string> live;
            for (const auto& d : candidates)
                if (destinationIsLive (d, settings))
                    live.push_back (d);
            const auto& wanted = live.empty() ? candidates : live;
            if (wanted.empty())
                continue;

            int free = -1;
            for (int slot = 0; slot < kModSlots; ++slot)
            {
                if (modSource (mods[static_cast<size_t> (slot)]).empty())
                {
                    free = slot;
                    break;
                }
            }
            if (free < 0)
                break;

            const auto strength = r.sliders.count (axis.key) > 0 ? r.sliders.at (axis.key) : 0.5f;
            setModSlot (settings, free, source, wanted.front(), 0.3f + 0.5f * strength, false);
            result.macroDests[static_cast<size_t> (i)] = wanted.front();
            if (! settings.contains (source))
                settings[source] = 0.5;
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

    void Generator::applyChoices (
        const std::unordered_map<std::string, std::vector<std::pair<float, int>>>& choices,
        const Request& r, nlohmann::json& settings, std::mt19937& rng)
    {
        for (const auto& entry : choices)
        {
            if (! settings.contains (entry.first) || entry.second.empty())
                continue;
            if (r.locks.count (schema::sectionOf (entry.first)) > 0)
                continue;

            int total = 0;
            for (const auto& choice : entry.second)
                total += choice.second;
            if (total <= 0)
                continue;

            auto pick = std::uniform_int_distribution<int> (0, total - 1) (rng);
            for (const auto& choice : entry.second)
            {
                pick -= choice.second;
                if (pick < 0)
                {
                    settings[entry.first] = choice.first;
                    break;
                }
            }
        }
    }

    namespace
    {
        /*  How long a note should ring, per style.

            This is the one place where a musical judgement overrides the
            corpus rather than following it. A bass preset usually shows a full
            sustain because the player is the one making the notes short, so
            reading the library literally gives a bass that drones when you hold
            a key. What a bass should do when you audition it is fall away, and
            the same reasoning runs the other way for a lead.

            Values run -1 for short to +1 for long, and they bias where in the
            style's own distribution each envelope stage is drawn from, so the
            numbers stay ones real presets use.
        */
        struct NoteShape
        {
            float attack, decay, sustain, release;
            /*  A hard ceiling on sustain, because skewing alone is not enough
                where the distribution is top heavy.

                Bass sustain in the library runs p10 0.29 and median 1.00, so
                everything from the halfway mark upward is a full sustain. Even
                a hard skew leaves roughly a third of draws sitting at the top,
                and a bass that holds the note forever is the thing being
                complained about. Where a style is meant to be short, it gets a
                lid as well as a lean.
            */
            float sustainCeiling;

            /*  A band for the release, because short is not the same as cut off.

                A bass wants the note to stop when you lift the key, but not
                instantly: no release at all is a click and reads as a fault
                rather than a tight sound. A floor keeps a tail on it and a
                ceiling stops it ringing into the next note. Zero means leave it
                alone.
            */
            float releaseFloor, releaseCeiling;

            /*  A band for the decay, which is where the length of a struck note
                actually comes from.

                Sustain says whether a note keeps going. Decay says how long it
                takes to get there, and that is the part a listener hears as the
                body of the note. Measured against a held note, a decay of 1.00
                is gone in half a second and 1.25 lasts about one and a tenth,
                while the library's basses sit at 0.85 to 1.12 and so arrive
                clipped short. Leaning on the release instead is the wrong shape:
                that only stretches what happens after the key comes up.
            */
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
    }

    void Generator::applyNoteShape (const model::Style& style, const Request& r,
                                    nlohmann::json& settings, std::mt19937& rng)
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
            const auto curve = style.continuous.find (stage.first);
            if (curve == style.continuous.end())
                continue;
            settings[stage.first] = style.sample (curve->second,
                                                  skew (uniform (rng), stage.second));
        }

        if (shape.sustainCeiling < 1.0f && settings.contains ("env_1_sustain"))
        {
            // A struck sound gets no sustain at all, not merely a small one.
            // Even a sixth of the peak held forever is a note that never stops.
            const auto sustain = settings["env_1_sustain"].get<float>();
            if (sustain > shape.sustainCeiling)
                settings["env_1_sustain"] = shape.sustainCeiling * uniform (rng);
        }

        /*  The body of the note is set here rather than left to the corpus,
            because the range real presets use is narrower than what the style
            wants. Bass lands around 0.7 to 1.2 seconds of decay: long enough to
            be a note rather than a click, and gone well before the key is.
        */
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

        /*  The instrument's own pitch stays where the keyboard put it.

            Presets in the library do transpose down, a bass sitting at -24 in a
            third of them, but that is a choice made alongside the notes the
            author was writing. Someone playing a bass part is already reaching
            for the bottom two octaves, and a patch that drops another two on top
            of that lands under the speaker rather than in the track.
        */
        if (settings.contains ("osc_1_transpose")
            && r.locks.count (schema::Section::osc) == 0)
        {
            // A bass or a drum has a register, and moving one up an octave puts
            // it somewhere it does not belong. Only the styles that live in the
            // middle get to go either way.
            const auto lowRegister = r.style == "Bass" || r.style == "Percussion";
            const auto roll = uniform (rng);
            if (roll < 0.86f)
                settings["osc_1_transpose"] = 0.0;
            else
                settings["osc_1_transpose"] = lowRegister ? -12.0
                                                          : (roll < 0.95f ? -12.0 : 12.0);
        }
    }

    void Generator::constrainPitch (const Request& r, nlohmann::json& settings,
                                    std::mt19937& rng)
    {
        /*  Keep the note on the note.

            A player pressing C expects to hear a C. This runs over the finished
            modulation matrix rather than over the routings this generator adds,
            because most of them arrive with the spliced donor and were sailing
            straight past a check that only looked at new ones. That is how a
            lead ended up with a full depth LFO on its tune, which is a fine
            sound effect and not a lead.

            Small depths are vibrato and stay. A sequence is the exception, since
            stepping between pitches is the entire idea, and there the steps get
            snapped to semitones and squared off instead of gliding.
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

        if (pitchDrivers.empty())
            return;

        if (sequenced)
        {
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
    }

    void Generator::applyStyleEssentials (const model::Style& style, const Request& r,
                                          nlohmann::json& settings, std::mt19937& rng)
    {
        /*  Which effects are on, which filter circuit, which distortion type.

            These drifted with whatever donor turned up, and the library is clear
            that they are not incidental: bass runs distortion in three quarters
            of presets and reverb in under half, while a pad is reverb in almost
            all of them. Applied here rather than at the end so that pushing
            SPACE or DIRT can still overrule the style.
        */
        applyChoices (style.early, r, settings, rng);
    }

    void Generator::applyIdentity (const model::Style& style, const Request& r,
                                   nlohmann::json& settings, std::mt19937& rng)
    {
        /*  Put the structural parameters back where the style keeps them.

            These drift during splicing and jitter like everything else, but
            unlike a cutoff they decide what instrument the patch is. Leads were
            coming out monophonic and pads were losing their unison entirely,
            which does not read as a variation on the style, it reads as the
            wrong sound. Each one is drawn from the distribution the style
            actually uses rather than pinned to its median, so a second
            oscillator sitting a fifth up is still on the table.
        */
        applyChoices (style.identity, r, settings, rng);
    }

    std::vector<std::string> Generator::repair (const model::Style& style,
                                                nlohmann::json& settings)
    {
        std::vector<std::string> notes;

        /*  Start from a neutral master volume. The donor's own volume arrives
            with the skeleton and is often near the top of the range, which
            leaves nothing to turn up with when the patch turns out quiet. From
            the default there is room in both directions.
        */
        settings["volume"] = loudness::kVolumeDefault;

        for (auto it = settings.begin(); it != settings.end(); ++it)
        {
            if (! it.value().is_number())
                continue;
            const auto c = style.continuous.find (it.key());
            if (c == style.continuous.end())
                continue;
            const auto v = it.value().get<float>();
            const auto lo = c->second.front(), hi = c->second.back();
            if (v < lo || v > hi)
                it.value() = std::max (lo, std::min (hi, v));
        }

        /*  An oscillator switched on at zero level is not a quiet oscillator,
            it is a switch left in the wrong position, and it shows up in Vital's
            interface as an active oscillator making no sound.
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

        /*  A pitched instrument needs a pitched source.

            Checking only that something was audible let a patch through whose
            single audible source was the noise sample, which is a noise sweep
            rather than a bass no matter what else is set. Across the library
            almost every preset in these styles has at least one oscillator
            running, and noise sits underneath as a layer.
        */
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
        if (settings.value ("sample_on", 0.0) >= 0.5)
        {
            const auto level = settings.value ("sample_level", 0.0);
            if (level > 0.55)
            {
                settings["sample_level"] = 0.38;    // the library's median
                notes.push_back ("noise was louder than the oscillators, brought down");
            }
        }

        // A zero sustain paired with an instant decay produces a click and
        // nothing else, which reads as a broken patch rather than a short one.
        if (settings.value ("env_1_sustain", 1.0) < 0.02
            && settings.value ("env_1_decay", 1.0) < 0.05)
        {
            settings["env_1_decay"] = 0.35;
            notes.push_back ("amp envelope was inaudible, lengthened decay");
        }
        if (settings.value ("env_1_release", 0.0) < 0.01)
            settings["env_1_release"] = 0.15;

        /*  Keep the patch centred.

            Hand-made presets leave the oscillator pans at zero 96 to 99 percent
            of the time, and every one of them runs stereo spread at full. A
            randomised pan does not read as width, it reads as a patch with a
            broken channel, which is exactly what it sounded like.
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
        return notes;
    }

    Result Generator::roll (const Request& request)
    {
        Result result;

        const auto* style = styleModel.style (request.style);
        if (style == nullptr)
        {
            result.error = "unknown style: " + request.style;
            return result;
        }
        if (style->donors.empty())
        {
            result.error = "style has no donor presets: " + request.style;
            return result;
        }

        auto seed = request.seed;
        if (seed == 0)
            seed = std::random_device {}() | 1u;
        result.seed = seed;

        /*  Structure and values come from separate streams so a seed pins which
            donors a patch is built from while the sliders stay free to move.
            Sharing one stream means nudging a slider silently reshuffles the
            donors and the patch the user was working on disappears.
        */
        std::mt19937 srng (seed);
        std::mt19937 prng (seed ^ 0x9E3779B9u);

        if (request.base != nullptr)
        {
            result.preset = *request.base;
        }
        else
        {
            const auto* d = pickDonor (*style, srng);
            if (d == nullptr)
            {
                result.error = "could not read any donor preset";
                return result;
            }
            result.preset = *d;
        }

        if (! result.preset.contains ("settings") || ! result.preset["settings"].is_object())
        {
            result.error = "donor preset had no settings block";
            return result;
        }

        splice (*style, request, result.preset, srng);
        auto& settings = result.preset["settings"];

        synthesiseContent (request, settings, srng);
        applyStyleEssentials (*style, request, settings, srng);
        applyAxes (*style, request, settings, prng);
        // Macros are wired first so their slots are already claimed. Movement
        // fills what is left, and a heavily wired patch cannot crowd out the
        // four knobs the player is meant to reach for.
        wireMacros (*style, request, result.preset, settings, result);
        if (request.locks.count (schema::Section::lfo) == 0
            && request.locks.count (schema::Section::mod) == 0)
            wireMovement (*style, request, settings, srng);
        applyIdentity (*style, request, settings, srng);
        applyNoteShape (*style, request, settings, prng);
        constrainPitch (request, settings, srng);
        result.repairs = repair (*style, settings);

        result.preset["preset_style"] = request.style;
        result.preset["preset_name"] = request.name;
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
