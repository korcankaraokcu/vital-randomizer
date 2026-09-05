#include "StyleModel.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "Schema.h"

namespace model
{
    namespace
    {
        constexpr int kMinStyleCount = 12;   // below this, blend with the global pool
        constexpr float kBlendFloor = 0.35f;

        float quantile (std::vector<float>& sorted, float q)
        {
            if (sorted.empty())
                return 0.0f;
            const auto pos = q * static_cast<float> (sorted.size() - 1);
            const auto lo = static_cast<size_t> (std::floor (pos));
            const auto hi = std::min (lo + 1, sorted.size() - 1);
            const auto frac = pos - static_cast<float> (lo);
            return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
        }

        Curve curveOf (std::vector<float> values)
        {
            std::sort (values.begin(), values.end());
            Curve c {};
            for (int i = 0; i < kQuantiles; ++i)
                c[static_cast<size_t> (i)] =
                    quantile (values, static_cast<float> (i) / (kQuantiles - 1));
            return c;
        }

        /*  Structural identity, learned per style. Some of these are excluded
            from ordinary randomisation, so they are collected separately rather
            than riding along with the rest of the parameters.
        */
        const std::vector<std::string>& identityParams()
        {
            static const std::vector<std::string> params = {
                "polyphony",
                "osc_1_unison_voices", "osc_2_unison_voices", "osc_3_unison_voices",
                /*  Only the second and third oscillators. Their transpose is
                    voicing, a fifth or an octave against the first, and worth
                    keeping. The first oscillator's transpose is the whole
                    instrument's pitch, which is the player's to choose.
                */
                "osc_2_transpose", "osc_3_transpose",

                /*  The modulation envelope's shape, which is what actually makes
                    a bass plucky.

                    These are collected only from presets where env_2 is really
                    wired to something, and that condition is the whole point.
                    Across all bass presets the sustain is bimodal, 18 sitting at
                    full and 16 at zero, so its median of 0.22 describes nothing
                    and sampling it hands out a sustained envelope two times in
                    five. Among the presets that actually route env_2 at a filter
                    the median is 0.12 and not one of them sustains, because an
                    envelope nobody uses just sits at its default. Since the
                    generator always wires this envelope now, the conditional
                    distribution is the only one that means anything.
                */
                "env_2_attack", "env_2_decay", "env_2_sustain", "env_2_release",

                /*  How the style is played. A bass is monophonic and a good
                    share of them glide, which is as much a part of sounding
                    like a bass as the filter envelope is.
                */
                "legato", "portamento_time", "portamento_slope", "portamento_force",
            };
            return params;
        }

        /*  Applied before the axes rather than after, so a pushed axis can still
            overrule them.
        */
        const std::vector<std::string>& earlyParams()
        {
            static const std::vector<std::string> params = {
                "reverb_on", "delay_on", "chorus_on", "phaser_on", "flanger_on",
                "distortion_on", "compressor_on", "eq_on",
                "filter_1_on", "filter_2_on", "filter_fx_on",
                // Vital's eight filter models are different circuits, not points
                // on a scale, and styles favour particular ones.
                "filter_1_model", "filter_2_model", "distortion_type",
            };
            return params;
        }

        struct Bucket
        {
            std::vector<std::string> donors;
            std::map<std::string, std::vector<float>> values;
            std::map<std::string, int> macroDests;
            std::vector<int> routingCounts;
            std::map<std::string, std::map<float, int>> identity;
            std::map<std::string, std::map<float, int>> early;
            std::map<std::pair<std::string, std::string>, int> routings;
            std::vector<float> depths;
        };

        int medianOf (std::vector<int> values, int fallback)
        {
            if (values.empty())
                return fallback;
            std::sort (values.begin(), values.end());
            return values[values.size() / 2];
        }
    }

    float Style::sample (const Curve& curve, float percentile) const
    {
        const auto p = std::min (1.0f, std::max (0.0f, percentile));
        const auto pos = p * (kQuantiles - 1);
        const auto lo = static_cast<size_t> (std::floor (pos));
        const auto hi = std::min (lo + 1, static_cast<size_t> (kQuantiles - 1));
        const auto frac = pos - static_cast<float> (lo);
        return curve[lo] + (curve[hi] - curve[lo]) * frac;
    }

    float Style::percentileOf (const Curve& curve, float value)
    {
        if (value <= curve.front())
            return 0.0f;
        if (value >= curve.back())
            return 1.0f;
        for (size_t i = 1; i < kQuantiles; ++i)
        {
            if (value <= curve[i])
            {
                const auto span = curve[i] - curve[i - 1];
                const auto frac = span > 1e-9f ? (value - curve[i - 1]) / span : 0.0f;
                return (static_cast<float> (i - 1) + frac) / (kQuantiles - 1);
            }
        }
        return 1.0f;
    }

    std::vector<std::string> StyleModel::styleNames() const
    {
        std::vector<std::string> names;
        names.reserve (styles.size());
        for (const auto& kv : styles)
            names.push_back (kv.first);
        std::sort (names.begin(), names.end(),
                   [this] (const std::string& a, const std::string& b)
                   {
                       return styles.at (a).count > styles.at (b).count;
                   });
        return names;
    }

    const Style* StyleModel::style (const std::string& name) const
    {
        const auto it = styles.find (name);
        return it == styles.end() ? nullptr : &it->second;
    }

    int StyleModel::buildFromLibrary (const juce::File& root,
                                      std::function<void (float)> progress)
    {
        styles.clear();
        totalPresets = 0;

        juce::Array<juce::File> files;
        root.findChildFiles (files, juce::File::findFiles, true, "*.vital");
        if (files.isEmpty())
            return 0;

        std::map<std::string, Bucket> buckets;
        std::map<std::string, std::vector<float>> pooled;
        std::vector<std::string> indexed;

        for (int i = 0; i < files.size(); ++i)
        {
            if (progress)
                progress (static_cast<float> (i) / static_cast<float> (files.size()));

            const auto text = files[i].loadFileAsString().toStdString();
            auto doc = nlohmann::json::parse (text, nullptr, false);
            if (doc.is_discarded() || ! doc.contains ("settings")
                || ! doc["settings"].is_object())
                continue;

            ++totalPresets;
            const auto& settings = doc["settings"];

            std::string styleName = "Unlabelled";
            if (doc.contains ("preset_style") && doc["preset_style"].is_string())
            {
                const auto s = juce::String (doc["preset_style"].get<std::string>()).trim();
                if (s.isNotEmpty())
                    styleName = s.toStdString();
            }

            auto& bucket = buckets[styleName];
            bucket.donors.push_back (files[i].getFullPathName().toStdString());

            for (auto it = settings.begin(); it != settings.end(); ++it)
            {
                if (! it.value().is_number())
                    continue;
                const auto& key = it.key();
                if (schema::sectionOf (key) == schema::Section::excluded)
                    continue;
                const auto v = it.value().get<float>();
                bucket.values[key].push_back (v);
                pooled[key].push_back (v);
            }

            bool env2Routed = false;
            if (settings.contains ("modulations") && settings["modulations"].is_array())
                for (const auto& m : settings["modulations"])
                    if (m.is_object() && m.contains ("source") && m["source"].is_string()
                        && m["source"].get<std::string>().rfind ("env_2", 0) == 0)
                        env2Routed = true;

            for (const auto& key : identityParams())
            {
                if (! settings.contains (key) || ! settings[key].is_number())
                    continue;
                // An envelope nobody uses sits at its default, so counting it
                // would drown the shape the presets that do use it settled on.
                if (key.rfind ("env_2_", 0) == 0 && ! env2Routed)
                    continue;
                ++bucket.identity[key][std::round (settings[key].get<float>() * 100.0f) / 100.0f];
            }

            for (const auto& key : earlyParams())
                if (settings.contains (key) && settings[key].is_number())
                    ++bucket.early[key][std::round (settings[key].get<float>() * 100.0f) / 100.0f];

            if (settings.contains ("modulations") && settings["modulations"].is_array())
            {
                int routings = 0;
                int index = -1;
                for (const auto& m : settings["modulations"])
                {
                    ++index;
                    if (! m.is_object() || ! m.contains ("source") || ! m.contains ("destination"))
                        continue;
                    const auto src = m["source"].is_string() ? m["source"].get<std::string>() : "";
                    const auto dst = m["destination"].is_string()
                                         ? m["destination"].get<std::string>() : "";
                    if (src.empty())
                        continue;
                    ++routings;
                    if (src.rfind ("macro_control", 0) == 0 && ! dst.empty())
                    {
                        ++bucket.macroDests[dst];
                    }
                    else if (! dst.empty())
                    {
                        ++bucket.routings[{ src, dst }];
                        const auto slot = "modulation_" + std::to_string (index + 1) + "_amount";
                        if (settings.contains (slot) && settings[slot].is_number())
                            bucket.depths.push_back (std::abs (settings[slot].get<float>()));
                    }
                }
                if (routings > 0)
                    bucket.routingCounts.push_back (routings);
            }
        }

        if (progress)
            progress (1.0f);

        // Global fallbacks, used to prop up styles that have too few presets to
        // describe themselves. Percussion in a typical library is a handful of
        // patches, and a distribution built from seven examples is mostly noise.
        std::unordered_map<std::string, Curve> globalContinuous;
        for (auto& kv : pooled)
            if (! kv.second.empty())
                globalContinuous[kv.first] = curveOf (kv.second);

        for (auto& kv : buckets)
        {
            Style s;
            s.count = static_cast<int> (kv.second.donors.size());
            s.donors = std::move (kv.second.donors);
            s.blended = s.count < kMinStyleCount;

            const auto weight = s.blended
                ? std::max (kBlendFloor, static_cast<float> (s.count) / kMinStyleCount)
                : 1.0f;

            for (auto& pv : kv.second.values)
            {
                if (pv.second.empty())
                    continue;

                if (schema::isIndexed (pv.first))
                {
                    std::map<float, int> counts;
                    for (auto v : pv.second)
                        ++counts[std::round (v * 10000.0f) / 10000.0f];
                    std::vector<std::pair<float, int>> choices (counts.begin(), counts.end());
                    std::sort (choices.begin(), choices.end(),
                               [] (auto& a, auto& b) { return a.second > b.second; });
                    s.discrete[pv.first] = std::move (choices);
                    continue;
                }

                auto curve = curveOf (pv.second);
                if (curve.back() - curve.front() < 1e-9f)
                {
                    s.discrete[pv.first] = { { curve.front(),
                                               static_cast<int> (pv.second.size()) } };
                    continue;
                }

                if (s.blended)
                {
                    const auto g = globalContinuous.find (pv.first);
                    if (g != globalContinuous.end())
                        for (size_t i = 0; i < kQuantiles; ++i)
                            curve[i] = weight * curve[i] + (1.0f - weight) * g->second[i];
                }
                s.continuous[pv.first] = curve;
            }

            if (s.blended)
                for (const auto& g : globalContinuous)
                    if (s.continuous.find (g.first) == s.continuous.end()
                        && s.discrete.find (g.first) == s.discrete.end())
                        s.continuous[g.first] = g.second;

            s.medianRoutings = medianOf (kv.second.routingCounts, 9);
            const auto collapse = [] (const std::map<std::string, std::map<float, int>>& src,
                                      std::unordered_map<std::string,
                                                         std::vector<std::pair<float, int>>>& dst)
            {
                for (auto& entry : src)
                {
                    std::vector<std::pair<float, int>> choices (entry.second.begin(),
                                                                entry.second.end());
                    std::sort (choices.begin(), choices.end(),
                               [] (auto& a, auto& b) { return a.second > b.second; });
                    dst[entry.first] = std::move (choices);
                }
            };
            collapse (kv.second.identity, s.identity);
            collapse (kv.second.early, s.early);
            for (const auto& r : kv.second.routings)
                s.modRoutings.push_back ({ r.first.first, r.first.second, r.second });
            std::sort (s.modRoutings.begin(), s.modRoutings.end(),
                       [] (const Style::Routing& a, const Style::Routing& b)
                       { return a.count > b.count; });
            if (s.modRoutings.size() > 120)
                s.modRoutings.resize (120);

            if (kv.second.depths.size() >= 5)
            {
                s.modDepth = curveOf (kv.second.depths);
                s.hasModDepth = true;
            }

            s.macroDests.assign (kv.second.macroDests.begin(), kv.second.macroDests.end());
            std::sort (s.macroDests.begin(), s.macroDests.end(),
                       [] (auto& a, auto& b) { return a.second > b.second; });
            if (s.macroDests.size() > 40)
                s.macroDests.resize (40);

            styles[kv.first] = std::move (s);
        }

        return totalPresets;
    }

    bool StyleModel::saveToFile (const juce::File& file) const
    {
        nlohmann::json out;
        out["model_version"] = kModelVersion;
        out["n_presets"] = totalPresets;
        for (const auto& kv : styles)
        {
            nlohmann::json s;
            s["n"] = kv.second.count;
            s["blended"] = kv.second.blended;
            s["median_routings"] = kv.second.medianRoutings;
            s["donors"] = kv.second.donors;
            for (const auto& c : kv.second.continuous)
                s["continuous"][c.first] = c.second;
            for (const auto& d : kv.second.discrete)
            {
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& choice : d.second)
                    arr.push_back ({ choice.first, choice.second });
                s["discrete"][d.first] = arr;
            }
            const auto writeChoices = [&s] (const char* field,
                const std::unordered_map<std::string, std::vector<std::pair<float, int>>>& src)
            {
                for (const auto& entry : src)
                {
                    nlohmann::json arr = nlohmann::json::array();
                    for (const auto& choice : entry.second)
                        arr.push_back ({ choice.first, choice.second });
                    s[field][entry.first] = arr;
                }
            };
            writeChoices ("identity", kv.second.identity);
            writeChoices ("early", kv.second.early);

            nlohmann::json routings = nlohmann::json::array();
            for (const auto& r : kv.second.modRoutings)
                routings.push_back ({ r.source, r.destination, r.count });
            s["mod_routings"] = std::move (routings);
            if (kv.second.hasModDepth)
                s["mod_depth"] = kv.second.modDepth;

            nlohmann::json dests = nlohmann::json::array();
            for (const auto& d : kv.second.macroDests)
                dests.push_back ({ d.first, d.second });
            s["macro_dests"] = dests;
            out["styles"][kv.first] = std::move (s);
        }

        file.getParentDirectory().createDirectory();
        return file.replaceWithText (juce::String (out.dump()));
    }

    bool StyleModel::loadFromFile (const juce::File& file)
    {
        if (! file.existsAsFile())
            return false;

        auto doc = nlohmann::json::parse (file.loadFileAsString().toStdString(), nullptr, false);
        if (doc.is_discarded() || ! doc.contains ("styles"))
            return false;

        // A cache from an older build is discarded rather than half read.
        if (doc.value ("model_version", 0) != kModelVersion)
            return false;

        styles.clear();
        totalPresets = doc.value ("n_presets", 0);

        for (auto it = doc["styles"].begin(); it != doc["styles"].end(); ++it)
        {
            const auto& src = it.value();
            Style s;
            s.count = src.value ("n", 0);
            s.blended = src.value ("blended", false);
            s.medianRoutings = src.value ("median_routings", 9);
            if (src.contains ("donors"))
                s.donors = src["donors"].get<std::vector<std::string>>();

            if (src.contains ("continuous"))
            {
                for (auto c = src["continuous"].begin(); c != src["continuous"].end(); ++c)
                {
                    const auto vals = c.value().get<std::vector<float>>();
                    if (vals.size() != kQuantiles)
                        continue;
                    Curve curve {};
                    std::copy (vals.begin(), vals.end(), curve.begin());
                    s.continuous[c.key()] = curve;
                }
            }

            if (src.contains ("discrete"))
            {
                for (auto d = src["discrete"].begin(); d != src["discrete"].end(); ++d)
                {
                    std::vector<std::pair<float, int>> choices;
                    for (const auto& pair : d.value())
                        if (pair.is_array() && pair.size() == 2)
                            choices.emplace_back (pair[0].get<float>(), pair[1].get<int>());
                    if (! choices.empty())
                        s.discrete[d.key()] = std::move (choices);
                }
            }

            const auto readChoices = [&src] (const char* field,
                std::unordered_map<std::string, std::vector<std::pair<float, int>>>& dst)
            {
                if (! src.contains (field) || ! src[field].is_object())
                    return;
                for (auto it = src[field].begin(); it != src[field].end(); ++it)
                {
                    std::vector<std::pair<float, int>> choices;
                    for (const auto& pair : it.value())
                        if (pair.is_array() && pair.size() == 2)
                            choices.emplace_back (pair[0].get<float>(), pair[1].get<int>());
                    if (! choices.empty())
                        dst[it.key()] = std::move (choices);
                }
            };
            readChoices ("identity", s.identity);
            readChoices ("early", s.early);

            if (src.contains ("mod_routings") && src["mod_routings"].is_array())
                for (const auto& r : src["mod_routings"])
                    if (r.is_array() && r.size() == 3)
                        s.modRoutings.push_back ({ r[0].get<std::string>(),
                                                   r[1].get<std::string>(),
                                                   r[2].get<int>() });
            if (src.contains ("mod_depth"))
            {
                const auto vals = src["mod_depth"].get<std::vector<float>>();
                if (vals.size() == kQuantiles)
                {
                    std::copy (vals.begin(), vals.end(), s.modDepth.begin());
                    s.hasModDepth = true;
                }
            }

            if (src.contains ("macro_dests"))
                for (const auto& pair : src["macro_dests"])
                    if (pair.is_array() && pair.size() == 2)
                        s.macroDests.emplace_back (pair[0].get<std::string>(),
                                                   pair[1].get<int>());

            styles[it.key()] = std::move (s);
        }
        return ! styles.empty();
    }

    juce::File StyleModel::defaultCacheFile()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("VitalRandomizer")
                   .getChildFile ("style_model.json");
    }

    juce::File StyleModel::defaultLibraryRoot()
    {
        const auto docs = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
        for (const auto* name : { "Vital", "Vital Audio", "vital" })
        {
            const auto candidate = docs.getChildFile (name);
            if (candidate.isDirectory())
                return candidate;
        }
        return docs.getChildFile ("Vital");
    }
}
