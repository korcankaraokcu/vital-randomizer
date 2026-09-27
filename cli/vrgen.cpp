/*
    vrgen, Vital Randomizer on the command line.

    Rolls batches of presets, or variations of one you already have, through
    the same generator and the same screen as the plugin, so every preset it
    writes has been rendered through Vital, checked for its style and levelled
    to -16 LUFS. It needs Vital installed, as the plugin does.

        vrgen roll    [options]          a batch, every style or the ones named
        vrgen vary    <preset> [options] neighbours of a preset
        vrgen measure <folder>           the loudness of every preset in a folder
        vrgen --list                     styles, drum kinds and scales
        vrgen --help
*/

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "../src/Archetypes.h"
#include "../src/Audition.h"
#include "../src/Drums.h"
#include "../src/Generator.h"
#include "../src/Loudness.h"
#include "../src/Screen.h"
#include "../src/VitalHost.h"

namespace
{
    constexpr double kSampleRate = 44100.0;
    constexpr int kBlockSize = 512;
    // How many candidates a preset may take before the best of them is kept.
    // The plugin stops at eight, where someone is waiting on a button; a batch
    // can afford to look harder.
    constexpr int kAttempts = 16;

    const char* kHelp = R"(vrgen, Vital Randomizer on the command line

Every preset is rolled by the same generator as the plugin and passes the same
screen: rendered through Vital, checked against its style, levelled to -16 LUFS.
Vital has to be installed.

  vrgen roll [options]
      A batch of presets.

      --styles=Bass,Lead,...   styles to roll, default all of them
      --count=N                presets per style, default 6
      --drums=Kick,Snare,...   drum kinds for Percussion, default any
      --scales=...             scales for Sequence, default any
      --bright=  --move=  --dirt=  --space=  --complexity=
                               a slider, as a value (0.7), a range (0.3-0.8,
                               drawn per preset), or ramp (low to high across
                               each style's presets). Default: each style's own.
      --seed=N                 the same seed gives the same batch, almost
                               always (see below)
      --out=<folder>           where to write, default ./vrgen presets
      --into-vital             write into Vital's user presets folder instead,
                               so the batch shows up in Vital's browser
      --name="{style}_{n}"     file names: {style} {n} {kind} {scale} {seed}
      --vital=<path>           Vital.vst3, if it is not in a standard place

  vrgen vary <preset.vital> [options]
      Neighbours of a preset you already like, as VARY does in the plugin.

      --count=N                how many, default 8
      --depth=0.3              how far they wander, 0 to 1
      --seed, --out, --into-vital, --name, --vital   as above

  vrgen measure <folder>
      The loudness of every preset in a folder, ours or anyone's, measured as
      the screen measures it.

  vrgen --list                 styles, drum kinds and scales

About seeds. Every preset stores the seed it was rolled from, and the generator
rebuilds a preset from its seed exactly. A batch seed picks those seeds, so the
same command writes the same batch. The screen judges each candidate by
rendering it, though, and Vital's renders vary very slightly, so a candidate on
the edge of a rule can pass on one run and be rolled again on another. Nearly
every batch repeats exactly, and every preset in it can always be rebuilt.
)";

    struct Options
    {
        std::map<std::string, std::string> values;
        std::vector<std::string> loose;
        bool has (const std::string& k) const { return values.count (k) > 0; }
        std::string get (const std::string& k, const std::string& fallback = {}) const
        {
            const auto it = values.find (k);
            return it == values.end() ? fallback : it->second;
        }
    };

    Options parse (int argc, char** argv, int from)
    {
        Options o;
        for (int i = from; i < argc; ++i)
        {
            const std::string a (argv[i]);
            if (a.rfind ("--", 0) == 0)
            {
                const auto eq = a.find ('=');
                o.values[a.substr (2, eq == std::string::npos ? std::string::npos : eq - 2)]
                    = eq == std::string::npos ? "" : a.substr (eq + 1);
            }
            else
                o.loose.push_back (a);
        }
        return o;
    }

    std::vector<std::string> splitList (const std::string& s)
    {
        std::vector<std::string> out;
        for (const auto& t : juce::StringArray::fromTokens (juce::String (s), ",", "\""))
            if (t.trim().isNotEmpty())
                out.push_back (t.trim().toStdString());
        return out;
    }

    std::string lower (std::string s)
    {
        std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
        return s;
    }

    // A style's own name, whatever case it was typed in, or empty.
    std::string styleNamed (const std::string& typed)
    {
        for (const auto& s : gen::Generator::styles())
            if (lower (s) == lower (typed))
                return s;
        return {};
    }

    int drumNamed (const std::string& typed)
    {
        for (int i = 0; i < (int) drums::all().size(); ++i)
            if (lower (drums::all()[(size_t) i].name) == lower (typed))
                return i;
        return -2;
    }

    // A scale's index in the sequence table, -1 for any, -2 when unknown.
    int scaleNamed (const std::string& typed)
    {
        if (lower (typed) == "random" || lower (typed) == "any")
            return -1;
        // A maqam answers to its name alone as well: hijaz for maqam hijaz.
        for (const auto& [name, index] : gen::sequenceScaleMenu())
            if (lower (name) == lower (typed) || lower (name) == "maqam " + lower (typed))
                return index;
        return -2;
    }

    std::string scaleName (int index)
    {
        for (const auto& [name, i] : gen::sequenceScaleMenu())
            if (i == index)
                return name;
        return "random";
    }

    /*  A slider as it was asked for: nothing (the style's own), one value, a
        range drawn per preset, or a ramp across a style's presets. */
    struct SliderAsk
    {
        enum class Kind { styleOwn, value, range, ramp } kind = Kind::styleOwn;
        float low = 0.0f, high = 0.0f;
    };

    bool parseSlider (const std::string& text, SliderAsk& ask, std::string& error)
    {
        if (text.empty())
            return true;
        if (lower (text) == "ramp")
        {
            ask.kind = SliderAsk::Kind::ramp;
            return true;
        }
        const auto dash = text.find ('-', 1);
        try
        {
            if (dash != std::string::npos)
            {
                ask.kind = SliderAsk::Kind::range;
                ask.low = std::stof (text.substr (0, dash));
                ask.high = std::stof (text.substr (dash + 1));
            }
            else
            {
                ask.kind = SliderAsk::Kind::value;
                ask.low = ask.high = std::stof (text);
            }
        }
        catch (...)
        {
            error = "could not read \"" + text + "\" as a value, a range like 0.3-0.8, or ramp";
            return false;
        }
        if (ask.low < 0.0f || ask.high > 1.0f || ask.low > ask.high)
        {
            error = "\"" + text + "\" is outside 0 to 1";
            return false;
        }
        return true;
    }

    juce::String safeFileName (juce::String s)
    {
        return juce::File::createLegalFileName (s.replace (" ", "_").replace ("/", "_"));
    }

    juce::String fillName (const juce::String& pattern, const std::map<std::string, juce::String>& fields)
    {
        auto out = pattern;
        for (const auto& [k, v] : fields)
            out = out.replace ("{" + juce::String (k) + "}", v);
        return out;
    }

    juce::File outputFolder (const Options& o, const juce::String& batchName)
    {
        if (o.has ("into-vital"))
            return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                .getChildFile ("Vital").getChildFile ("User").getChildFile ("Presets").getChildFile (batchName);
        if (o.has ("out") && ! o.get ("out").empty())
            return juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (o.get ("out")));
        return juce::File::getCurrentWorkingDirectory().getChildFile ("vrgen presets");
    }

    juce::String clock (double seconds)
    {
        const auto s = juce::roundToInt (seconds);
        return s >= 60 ? juce::String (s / 60) + "m " + juce::String (s % 60).paddedLeft ('0', 2) + "s"
                       : juce::String (s) + "s";
    }

    /*  Vital, loaded once and warmed, and the generator started from its init
        patch, as the plugin does. */
    struct Session
    {
        VitalHost vital;
        gen::Generator generator;

        bool open (const Options& o)
        {
            auto path = o.has ("vital") ? juce::File (juce::String (o.get ("vital"))) : VitalHost::findInstalled();
            if (path == juce::File() || ! path.exists())
            {
                std::cerr << "Vital.vst3 was not found. Install Vital, or pass --vital=<path to Vital.vst3>.\n";
                return false;
            }
            juce::String error;
            std::cout << "Loading Vital from " << path.getFullPathName() << "\n";
            if (! vital.load (path, kSampleRate, kBlockSize, error))
            {
                std::cerr << "Vital would not load: " << error << "\n";
                return false;
            }
            auto init = vital.currentPreset();
            if (init.is_null() || ! init.contains ("settings"))
            {
                std::cerr << "Could not read Vital's init patch.\n";
                return false;
            }
            init.erase ("tuning");
            generator.setInitPreset (std::move (init));
            return true;
        }

        /*  Roll until a candidate passes the screen, or keep the best of what
            was tried. Returns false only when nothing could be rolled at all. */
        bool rollOne (std::function<gen::Request()> makeRequest, gen::Result& out, int& tries, std::string& lastWhy)
        {
            gen::Result best;
            float bestScore = -1.0f;
            for (tries = 1; tries <= kAttempts; ++tries)
            {
                auto request = makeRequest();
                auto result = generator.roll (request);
                if (! result.ok)
                    continue;
                audition::Measurement measured;
                const auto verdict = screen::judge (vital, kSampleRate, kBlockSize, result, request.style,
                                                    request.sliders.count ("complexity") > 0
                                                        ? request.sliders.at ("complexity") : 0.5f,
                                                    measured);
                if (verdict.passed)
                {
                    out = std::move (result);
                    return true;
                }
                lastWhy = verdict.why;
                const auto score = measured.usable() ? measured.sustainRms : 0.0f;
                if (score > bestScore)
                {
                    bestScore = score;
                    best = std::move (result);
                }
            }
            tries = kAttempts;
            if (bestScore > 0.0f)
            {
                out = std::move (best);
                return true;
            }
            return false;
        }
    };

    bool writePreset (const juce::File& folder, const juce::String& base, nlohmann::json preset)
    {
        preset.erase ("tuning");
        preset["preset_name"] = base.replace ("_", " ").toStdString();
        const auto file = folder.getChildFile (safeFileName (base) + ".vital");
        return file.replaceWithText (juce::String (preset.dump()));
    }

    int roll (const Options& o)
    {
        // What to roll.
        std::vector<std::string> styles;
        if (o.has ("styles"))
        {
            for (const auto& typed : splitList (o.get ("styles")))
            {
                const auto s = styleNamed (typed);
                if (s.empty())
                {
                    std::cerr << "Unknown style \"" << typed << "\". Run vrgen --list for the styles.\n";
                    return 1;
                }
                styles.push_back (s);
            }
        }
        else
            styles = gen::Generator::styles();

        std::vector<int> drumsWanted, scalesWanted;
        for (const auto& typed : splitList (o.get ("drums")))
        {
            const auto k = drumNamed (typed);
            if (k < 0)
            {
                std::cerr << "Unknown drum kind \"" << typed << "\". Run vrgen --list for the kinds.\n";
                return 1;
            }
            drumsWanted.push_back (k);
        }
        for (const auto& typed : splitList (o.get ("scales")))
        {
            const auto k = scaleNamed (typed);
            if (k == -2)
            {
                std::cerr << "Unknown scale \"" << typed << "\". Run vrgen --list for the scales.\n";
                return 1;
            }
            scalesWanted.push_back (k);
        }

        const int count = o.has ("count") ? std::max (1, std::atoi (o.get ("count").c_str())) : 6;
        std::map<std::string, SliderAsk> asks;
        for (const auto* key : { "bright", "move", "dirt", "space", "complexity" })
        {
            std::string error;
            if (! parseSlider (o.get (key), asks[key], error))
            {
                std::cerr << "--" << key << ": " << error << "\n";
                return 1;
            }
        }

        const auto seed = o.has ("seed") ? (unsigned int) std::stoul (o.get ("seed"))
                                          : (unsigned int) std::random_device {}();
        std::mt19937 batch (seed);
        const auto pattern = juce::String (o.get ("name", "{style}_{n}"));
        const auto folder = outputFolder (o, "vrgen " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S"));
        if (! folder.createDirectory())
        {
            std::cerr << "Could not create " << folder.getFullPathName() << "\n";
            return 1;
        }

        Session session;
        if (! session.open (o))
            return 1;

        std::cout << "Rolling " << count << " per style into " << folder.getFullPathName()
                  << "\nBatch seed " << seed << "\n\n";

        const auto total = (int) styles.size() * count;
        int done = 0, written = 0;
        const auto started = juce::Time::getMillisecondCounterHiRes();
        std::uniform_real_distribution<float> unit (0.0f, 1.0f);

        for (const auto& style : styles)
        {
            const auto defaults = archetype::defaultSlidersFor (style);
            const std::map<std::string, float> own = {
                { "bright", defaults.bright }, { "move", defaults.move }, { "dirt", defaults.dirt },
                { "space", defaults.space }, { "complexity", defaults.complexity } };

            for (int n = 1; n <= count; ++n)
            {
                // Everything a preset needs is drawn before it is rolled, so
                // the batch seed alone decides the batch.
                std::map<std::string, float> sliders;
                for (const auto& [key, ask] : asks)
                {
                    switch (ask.kind)
                    {
                        case SliderAsk::Kind::styleOwn: sliders[key] = own.at (key); break;
                        case SliderAsk::Kind::value:    sliders[key] = ask.low; break;
                        case SliderAsk::Kind::range:    sliders[key] = ask.low + unit (batch) * (ask.high - ask.low); break;
                        case SliderAsk::Kind::ramp:     sliders[key] = count > 1 ? (float) (n - 1) / (float) (count - 1) : 0.5f; break;
                    }
                }
                std::vector<unsigned int> seeds;
                std::vector<int> kinds, scaleIndices;
                for (int t = 0; t < kAttempts; ++t)
                {
                    seeds.push_back ((unsigned int) batch() | 1u);
                    kinds.push_back (drumsWanted.empty() ? -1 : drumsWanted[(size_t) (batch() % drumsWanted.size())]);
                    scaleIndices.push_back (scalesWanted.empty() ? -1 : scalesWanted[(size_t) (batch() % scalesWanted.size())]);
                }

                int attempt = 0;
                const auto makeRequest = [&]
                {
                    gen::Request r;
                    r.style = style;
                    for (const auto& [k, v] : sliders)
                        r.sliders[k] = v;
                    const auto t = (size_t) std::min (attempt++, kAttempts - 1);
                    r.seed = seeds[t];
                    r.amount = 1.0f;
                    if (style == "Percussion")
                        r.drumKind = kinds[t];
                    if (style == "Sequence")
                        r.scale = scaleIndices[t];
                    return r;
                };

                gen::Result result;
                int tries = 0;
                std::string why;
                const auto ok = session.rollOne (makeRequest, result, tries, why);
                ++done;

                const auto* kind = drums::at (result.drumKind);
                std::map<std::string, juce::String> fields = {
                    { "style", juce::String (style) },
                    { "n", juce::String (n).paddedLeft ('0', std::max (2, (int) juce::String (count).length())) },
                    { "kind", kind != nullptr ? juce::String (kind->name) : juce::String() },
                    { "scale", style == "Sequence" ? juce::String (scaleName (result.scale)) : juce::String() },
                    { "seed", juce::String (result.seed) } };
                auto base = fillName (pattern, fields);
                // A drum says which one it is, unless the pattern already does.
                if (kind != nullptr && ! pattern.contains ("{kind}"))
                    base << "_" << kind->name;
                base = base.trimCharactersAtEnd ("_");

                const auto elapsed = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
                const auto left = elapsed / done * (total - done);
                std::cout << "[" << juce::String (done).paddedLeft (' ', (int) juce::String (total).length())
                          << "/" << total << "] " << base.paddedRight (' ', 27) << " ";
                if (ok && writePreset (folder, base, result.preset))
                {
                    ++written;
                    std::cout << (tries > 1 ? "ok after " + juce::String (tries) + " tries" : juce::String ("ok"));
                    if (tries >= kAttempts)
                        std::cout << ", the best of them (last: " << why << ")";
                }
                else
                    std::cout << "nothing usable in " << kAttempts << " tries (" << why << ")";
                std::cout << "   " << clock (left) << " left" << std::endl;
            }
        }

        std::cout << "\n" << written << " of " << total << " presets written to "
                  << folder.getFullPathName() << "\n";
        return written == total ? 0 : 2;
    }

    int vary (const Options& o)
    {
        if (o.loose.empty())
        {
            std::cerr << "vrgen vary needs a preset: vrgen vary <preset.vital>\n";
            return 1;
        }
        const juce::File source (juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (o.loose[0])));
        nlohmann::json base;
        try { base = nlohmann::json::parse (source.loadFileAsString().toStdString()); }
        catch (...) {}
        if (! base.contains ("settings"))
        {
            std::cerr << "Could not read " << source.getFullPathName() << " as a Vital preset.\n";
            return 1;
        }
        base.erase ("tuning");
        auto style = styleNamed (base.value ("preset_style", std::string()));
        if (style.empty())
            style = "Lead";

        const int count = o.has ("count") ? std::max (1, std::atoi (o.get ("count").c_str())) : 8;
        const float depth = o.has ("depth") ? juce::jlimit (0.0f, 1.0f, std::stof (o.get ("depth"))) : 0.3f;
        const auto seed = o.has ("seed") ? (unsigned int) std::stoul (o.get ("seed"))
                                          : (unsigned int) std::random_device {}();
        std::mt19937 batch (seed);
        const auto stem = source.getFileNameWithoutExtension();
        const auto pattern = juce::String (o.get ("name", (stem + " vary {n}").toStdString()));
        const auto folder = outputFolder (o, stem + " variations");
        if (! folder.createDirectory())
        {
            std::cerr << "Could not create " << folder.getFullPathName() << "\n";
            return 1;
        }

        Session session;
        if (! session.open (o))
            return 1;
        std::cout << "Varying " << source.getFileName() << " (" << style << ") " << count
                  << " times at a depth of " << depth << " into " << folder.getFullPathName()
                  << "\nBatch seed " << seed << "\n\n";

        const auto defaults = archetype::defaultSlidersFor (style);
        int written = 0;
        for (int n = 1; n <= count; ++n)
        {
            std::vector<unsigned int> seeds;
            for (int t = 0; t < kAttempts; ++t)
                seeds.push_back ((unsigned int) batch() | 1u);
            int attempt = 0;
            const auto makeRequest = [&]
            {
                gen::Request r;
                r.style = style;
                r.sliders = { { "bright", defaults.bright }, { "move", defaults.move }, { "dirt", defaults.dirt },
                              { "space", defaults.space }, { "complexity", defaults.complexity } };
                r.base = &base;
                r.amount = depth;
                // What VARY moves in the plugin: the filters and envelopes,
                // with the rates and depths of the motion.
                r.varySections = { schema::Section::filter, schema::Section::env };
                r.seed = seeds[(size_t) std::min (attempt++, kAttempts - 1)];
                return r;
            };
            gen::Result result;
            int tries = 0;
            std::string why;
            const auto ok = session.rollOne (makeRequest, result, tries, why);
            const auto name = fillName (pattern, { { "n", juce::String (n).paddedLeft ('0', 2) },
                                                    { "style", juce::String (style) },
                                                    { "seed", juce::String (result.seed) },
                                                    { "kind", "" }, { "scale", "" } });
            std::cout << "[" << n << "/" << count << "] " << name.paddedRight (' ', 27) << " ";
            if (ok && writePreset (folder, name, result.preset))
            {
                ++written;
                std::cout << (tries > 1 ? "ok after " + juce::String (tries) + " tries" : juce::String ("ok")) << std::endl;
            }
            else
                std::cout << "nothing usable (" << why << ")" << std::endl;
        }
        std::cout << "\n" << written << " of " << count << " written to " << folder.getFullPathName() << "\n";
        return written == count ? 0 : 2;
    }

    int measure (const Options& o)
    {
        if (o.loose.empty())
        {
            std::cerr << "vrgen measure needs a folder: vrgen measure <folder>\n";
            return 1;
        }
        const juce::File folder (juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (o.loose[0])));
        auto files = folder.findChildFiles (juce::File::findFiles, true, "*.vital");
        files.sort();
        if (files.isEmpty())
        {
            std::cerr << "No .vital presets in " << folder.getFullPathName() << "\n";
            return 1;
        }
        Session session;
        if (! session.open (o))
            return 1;

        std::cout << "\nLoudness as the screen measures it: a held note at C3, or a drum's loudest\n"
                  << "400 ms. vrgen levels every preset to " << loudness::kTargetLufs << " LUFS.\n\n";
        std::vector<float> all;
        for (const auto& f : files)
        {
            nlohmann::json doc;
            try { doc = nlohmann::json::parse (f.loadFileAsString().toStdString()); }
            catch (...) { continue; }
            if (! doc.contains ("settings"))
                continue;
            doc.erase ("tuning");
            if (! session.vital.applyPreset (doc))
                continue;
            const auto style = doc.value ("preset_style", std::string());
            audition::settle (*session.vital.processor(), kSampleRate, kBlockSize);
            const auto m = audition::auditionAveraged (*session.vital.processor(), kSampleRate, kBlockSize, 3, 48,
                                                       audition::judgedAsHit (style));
            const auto name = f.getRelativePathFrom (folder);
            if (m.silent || m.loudness <= 1.0e-6f)
            {
                std::cout << name.paddedRight (' ', 40) << " silent at C3" << std::endl;
                continue;
            }
            const auto lufs = -0.691f + 20.0f * std::log10 (m.loudness);
            const auto peak = 20.0f * std::log10 (std::max (1.0e-6f, m.peak));
            all.push_back (lufs);
            std::cout << name.paddedRight (' ', 40) << juce::String (lufs, 1).paddedLeft (' ', 6) << " LUFS   peak "
                      << juce::String (peak, 1).paddedLeft (' ', 5) << " dBFS" << std::endl;
        }
        if (! all.empty())
        {
            std::sort (all.begin(), all.end());
            const auto at = [&] (int pct) { return all[(all.size() - 1) * (size_t) pct / 100]; };
            std::cout << "\n" << all.size() << " presets, median " << juce::String (at (50), 1)
                      << " LUFS, 80% between " << juce::String (at (10), 1) << " and " << juce::String (at (90), 1) << "\n";
        }
        return 0;
    }

    int list()
    {
        std::cout << "Styles\n";
        for (const auto& s : gen::Generator::styles())
            std::cout << "  " << s << "\n";
        std::cout << "\nDrum kinds, for --drums with Percussion\n";
        for (const auto& k : drums::all())
            std::cout << "  " << k.name << "\n";
        std::cout << "\nScales, for --scales with Sequence\n  random\n";
        for (const auto& [name, index] : gen::sequenceScaleMenu())
            std::cout << "  " << name << "\n";
        return 0;
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const std::string command = argc > 1 ? argv[1] : "";
    if (command.empty() || command == "--help" || command == "-h" || command == "help")
    {
        std::cout << kHelp;
        return command.empty() ? 1 : 0;
    }
    if (command == "--list" || command == "list")
        return list();

    const auto options = parse (argc, argv, 2);
    if (options.has ("help"))
    {
        std::cout << kHelp;
        return 0;
    }
    if (command == "roll")
        return roll (options);
    if (command == "vary")
        return vary (options);
    if (command == "measure")
        return measure (options);

    std::cerr << "Unknown command \"" << command << "\". Run vrgen --help.\n";
    return 1;
}
