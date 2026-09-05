/*
    Console harness: the C++ equivalent of tools/validate.py.

    Loads Vital, learns the user's library, rolls patches, pushes each one into
    Vital and renders a note offline, then scores what came back. A patch that
    looks plausible as JSON can still be silent, clipped or a burst of noise,
    and none of that is visible without hearing it.

    This exists so the C++ generator can be checked against the same bar the
    Python reference implementation was measured on, rather than assumed to have
    survived the port.
*/
#include <cmath>
#include <iostream>
#include <algorithm>
#include <map>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "../src/Audition.h"
#include "../src/Axes.h"
#include "../src/Generator.h"
#include "../src/Loudness.h"
#include "../src/StyleModel.h"
#include "../src/VitalHost.h"
#include "../src/VitalState.h"

namespace
{
    constexpr double kSampleRate = 44100.0;
    constexpr int kBlockSize = 512;
    constexpr int kNote = 48;
    constexpr double kRenderSeconds = 2.0;

    struct Score
    {
        float peak = 0.0f, rms = 0.0f, centroid = 0.0f;
        bool ok = false;
        juce::String verdict;
    };

    float spectralCentroid (const juce::AudioBuffer<float>& audio)
    {
        // Measured over the sustain. Including the release tail mostly measures
        // whatever reverb the patch has and reports every style as alike.
        const auto start = (int) (kSampleRate * 0.15);
        const auto end = juce::jmin (audio.getNumSamples(), (int) (kSampleRate * 0.90));
        const auto n = end - start;
        if (n < 1024)
            return 0.0f;

        int order = 1;
        while ((1 << order) < n && order < 15)
            ++order;
        const int fftSize = 1 << order;

        juce::dsp::FFT fft (order);
        std::vector<float> data ((size_t) fftSize * 2, 0.0f);
        for (int i = 0; i < juce::jmin (n, fftSize); ++i)
        {
            const auto window = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi
                                                        * (float) i / (float) fftSize);
            float sum = 0.0f;
            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
                sum += audio.getSample (ch, start + i);
            data[(size_t) i] = sum / juce::jmax (1, audio.getNumChannels()) * window;
        }
        fft.performFrequencyOnlyForwardTransform (data.data());

        double weighted = 0.0, total = 0.0;
        for (int i = 1; i < fftSize / 2; ++i)
        {
            const auto mag = (double) data[(size_t) i];
            weighted += mag * (i * kSampleRate / fftSize);
            total += mag;
        }
        return total > 1.0e-9 ? (float) (weighted / total) : 0.0f;
    }

    Score scoreAudio (const juce::AudioBuffer<float>& audio)
    {
        Score s;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            s.peak = juce::jmax (s.peak, audio.getMagnitude (ch, 0, audio.getNumSamples()));
        s.rms = audio.getRMSLevel (0, 0, audio.getNumSamples());
        s.centroid = spectralCentroid (audio);

        if (s.rms < 0.002f)          s.verdict = "silent";
        else if (s.peak >= 0.999f)   s.verdict = "clipping";
        else                       { s.verdict = "ok"; s.ok = true; }
        return s;
    }

    juce::AudioBuffer<float> renderNote (juce::AudioProcessor& synth)
    {
        synth.reset();
        const auto total = (int) (kRenderSeconds * kSampleRate);
        juce::AudioBuffer<float> out (2, total);
        out.clear();

        juce::AudioBuffer<float> block (2, kBlockSize);
        const auto noteOffAt = (int) (kSampleRate * 1.0);

        for (int pos = 0; pos < total; pos += kBlockSize)
        {
            const auto n = juce::jmin (kBlockSize, total - pos);
            block.setSize (2, n, false, false, true);
            block.clear();

            juce::MidiBuffer midi;
            if (pos == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, kNote, (juce::uint8) 110), 0);
            if (pos <= noteOffAt && noteOffAt < pos + n)
                midi.addEvent (juce::MidiMessage::noteOff (1, kNote), noteOffAt - pos);

            synth.processBlock (block, midi);
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, pos, block, ch, 0, n);
        }
        return out;
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    /*  Six of every style, into out/, unless told otherwise.

        Comparing styles against each other only works if every style is
        present and the counts match, so that is the default rather than
        something to remember on the command line each time. Template and
        Unlabelled are left out on purpose: neither is a sound anyone sets out
        to make.
    */
    int perStyle = 6;
    juce::StringArray styles { "Bass", "Lead", "Keys", "Pad",
                               "Sequence", "Percussion", "SFX", "Experiment" };
    float bright = 0.55f, dirt = 0.55f, space = 0.55f, move = 0.65f;
    juce::File saveTo = juce::File::getCurrentWorkingDirectory().getChildFile ("out");
    juce::File diagFile;
    int corpusCount = 0;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        const auto value = arg.fromFirstOccurrenceOf ("=", false, false);
        if (arg.startsWith ("--per-style=")) perStyle = value.getIntValue();
        if (arg.startsWith ("--styles="))    styles = juce::StringArray::fromTokens (value, ",", "");
        if (arg.startsWith ("--bright="))    bright = value.getFloatValue();
        if (arg.startsWith ("--dirt="))      dirt = value.getFloatValue();
        if (arg.startsWith ("--space="))     space = value.getFloatValue();
        if (arg.startsWith ("--move="))      move = value.getFloatValue();
        if (arg.startsWith ("--save="))      saveTo = juce::File (value);
        if (arg.startsWith ("--diag="))      diagFile = juce::File (value);
        if (arg.startsWith ("--corpus="))    corpusCount = value.getIntValue();
    }

    /*  Diagnostic: load one patch and measure it repeatedly.

        Two hosts disagreeing about the same patch means the measurement is not
        stable, and a level matched against an unstable measurement is worse
        than no level matching at all.
    */
    if (diagFile != juce::File() && diagFile.existsAsFile())
    {
        VitalHost h;
        juce::String err;
        if (! h.load (VitalHost::findInstalled(), kSampleRate, kBlockSize, err))
        {
            std::cout << "could not load Vital: " << err << std::endl;
            return 1;
        }

        auto doc = nlohmann::json::parse (diagFile.loadFileAsString().toStdString(),
                                          nullptr, false);
        if (doc.is_discarded())
        {
            std::cout << "could not parse that file" << std::endl;
            return 1;
        }

        std::cout << "diagnosing " << diagFile.getFileName() << std::endl;
        std::cout << "  stated volume: "
                  << doc["settings"].value ("volume", 0.0) << std::endl << std::endl;

        const auto report = [&] (const char* label, bool resetFirst = true)
        {
            const auto m = audition::audition (*h.processor(), kSampleRate, kBlockSize,
                                               48, resetFirst);
            std::cout << "  " << juce::String (label).paddedRight (' ', 36)
                      << "rms=" << juce::String (m.rms, 4)
                      << "  peak=" << juce::String (m.peak, 3)
                      << "  crest=" << juce::String (m.crestDb, 1) << " dB"
                      << std::endl;
        };

        h.applyPreset (doc);
        report ("first render after load");
        report ("second render, same load");
        report ("third render, same load");
        h.applyPreset (doc);
        report ("after reloading the same patch");
        h.applyPreset (doc);
        report ("and reloading again");

        // Same patch, same host, but without the reset, to show whether the
        // reset is what the two hosts disagree about.
        h.applyPreset (doc);
        report ("no reset, first render", false);
        report ("no reset, second render", false);
        report ("no reset, third render", false);
        return 0;
    }

    /*  Measure hand-made presets from the user's library with the same code the
        plugin uses, so the level target is derived from the same metric it will
        be compared against. A target carried over from a different measurement
        is just a number that happens to look right.
    */
    if (corpusCount > 0)
    {
        VitalHost h;
        juce::String err;
        if (! h.load (VitalHost::findInstalled(), kSampleRate, kBlockSize, err))
        {
            std::cout << "could not load Vital: " << err << std::endl;
            return 1;
        }

        juce::Array<juce::File> files;
        model::StyleModel::defaultLibraryRoot()
            .findChildFiles (files, juce::File::findFiles, true, "*.vital");
        if (files.isEmpty())
        {
            std::cout << "no presets found" << std::endl;
            return 1;
        }

        juce::String onlyStyle;
        for (int i = 1; i < argc; ++i)
        {
            const juce::String arg (argv[i]);
            if (arg.startsWith ("--styles="))
                onlyStyle = arg.fromFirstOccurrenceOf ("=", false, false);
        }

        juce::Random rng (7);
        std::vector<float> levels, peaks, crests, centroids;
        for (int i = 0; i < files.size() && (int) levels.size() < corpusCount; ++i)
        {
            const auto& f = files[i];
            auto doc = nlohmann::json::parse (f.loadFileAsString().toStdString(), nullptr, false);
            if (doc.is_discarded() || ! doc.contains ("settings"))
                continue;
            if (onlyStyle.isNotEmpty())
            {
                const auto s = doc.value ("preset_style", std::string {});
                if (! onlyStyle.equalsIgnoreCase (juce::String (s)))
                    continue;
            }
            if (! h.applyPreset (doc))
                continue;

            const auto m = audition::audition (*h.processor(), kSampleRate, kBlockSize);
            if (m.silent)
                continue;
            levels.push_back (m.rms);
            peaks.push_back (m.peak);
            crests.push_back (m.crestDb);
            centroids.push_back (m.centroidHz);
        }

        const auto report = [] (const char* label, std::vector<float> v)
        {
            if (v.size() < 5)
                return;
            std::sort (v.begin(), v.end());
            std::cout << "  " << juce::String (label).paddedRight (' ', 10)
                      << "p10 " << juce::String (v[v.size() / 10], 4)
                      << "   median " << juce::String (v[v.size() / 2], 4)
                      << "   p90 " << juce::String (v[v.size() * 9 / 10], 4)
                      << std::endl;
        };

        std::cout << "hand-made presets, measured the way the plugin measures (n="
                  << levels.size() << ")" << std::endl;
        report ("level", levels);
        report ("peak", peaks);
        report ("crest dB", crests);
        report ("centroid", centroids);
        return 0;
    }

    const auto vitalPath = VitalHost::findInstalled();
    if (vitalPath == juce::File())
    {
        std::cout << "Vital.vst3 was not found in any standard location\n";
        return 1;
    }
    std::cout << "Vital: " << vitalPath.getFullPathName() << "\n";

    VitalHost host;
    juce::String error;
    auto t0 = juce::Time::getMillisecondCounterHiRes();
    if (! host.load (vitalPath, kSampleRate, kBlockSize, error))
    {
        std::cout << "failed to load Vital: " << error << "\n";
        return 1;
    }
    std::cout << "loaded and warmed in "
              << juce::String (juce::Time::getMillisecondCounterHiRes() - t0, 0) << " ms\n";

    model::StyleModel styleModel;
    const auto cache = model::StyleModel::defaultCacheFile();
    if (! styleModel.loadFromFile (cache))
    {
        const auto root = model::StyleModel::defaultLibraryRoot();
        std::cout << "learning from " << root.getFullPathName() << "\n";
        if (styleModel.buildFromLibrary (root) == 0)
        {
            std::cout << "no presets found\n";
            return 1;
        }
        styleModel.saveToFile (cache);
    }
    std::cout << "style model: " << styleModel.presetCount() << " presets\n\n";

    gen::Generator generator (styleModel);
    {
        const auto init = host.currentPreset();
        if (! init.is_null() && init.contains ("settings")
            && init["settings"].contains ("sample"))
            generator.setDefaultSample (init["settings"]["sample"]);
    }

    std::cout << juce::String ("style").paddedRight (' ', 10)
              << juce::String ("#").paddedRight (' ', 4)
              << juce::String ("verdict").paddedRight (' ', 11)
              << juce::String ("rms").paddedRight (' ', 9)
              << juce::String ("peak").paddedRight (' ', 9)
              << juce::String ("centroid").paddedRight (' ', 11)
              << juce::String ("load").paddedRight (' ', 9)
              << "routings / macros\n";
    std::cout << juce::String::repeatedString ("-", 92) << "\n";

    int pass = 0, total = 0;
    std::vector<float> levels;
    std::map<juce::String, std::vector<float>> styleCentroid, styleSustain, styleTail, styleHeld;
    std::map<juce::String, int> styleShort;
    // Why candidates get thrown away, so thresholds can be set from evidence
    // rather than by nudging numbers until the counts come out right.
    std::map<juce::String, std::map<juce::String, int>> rejects;
    std::map<juce::String, std::vector<float>> stylePitch, styleSalience;
    for (const auto& style : styles)
    {
        if (styleModel.style (style.toStdString()) == nullptr)
        {
            std::cout << "skipping unknown style " << style << "\n";
            continue;
        }

        /*  Keep rolling until the style has its six.

            Counting attempts rather than results left the batch lopsided, with
            two pads against six basses, which makes styles impossible to compare
            against each other. Screening throws a lot away by design, so the
            loop runs until it has what it came for and gives up only if a style
            genuinely cannot produce them.
        */
        int saved = 0;
        for (int i = 0; saved < perStyle && i < perStyle * 8; ++i)
        {
            gen::Request request;
            request.style = style.toStdString();
            request.amount = 1.0f;
            request.sliders = { { "bright", bright }, { "dirt", dirt },
                                { "space", space }, { "move", move } };
            request.name = (style + " " + juce::String (saved + 1)).toStdString();

            auto result = generator.roll (request);
            if (! result.ok)
            {
                std::cout << style << " " << (i + 1) << "  roll failed: "
                          << result.error << "\n";
                ++total;
                continue;
            }

            /*  The same screen the plugin runs: render the candidate, reject it
                if it is unusable, and match its level into its own master
                volume. Measuring after that is the point, since it is what the
                user actually hears.
            */
            const auto loadStart = juce::Time::getMillisecondCounterHiRes();
            int rerolls = 0;
            bool accepted = false;
            audition::Measurement accepted_m;

            for (int attempt = 0; attempt < 8 && ! accepted; ++attempt)
            {
    /*  Correct, then re-measure, then correct again if needed.

        One pass is not enough. The volume calibration was swept on a clean
        patch, and a patch with heavy distortion or compression in its chain
        does not respond to master volume the same way, so the first correction
        can land several dB off. Measuring what actually came out and going
        again is the only way to be sure the patch a user hears is the level it
        was supposed to be.
    */
                for (int pass = 0; pass < 3; ++pass)
                {
                    const auto loaded = pass == 0
                        ? host.applyPresetVerified (result.preset)
                        : host.applyPreset (result.preset);
                    if (! loaded)
                    {
                        std::cout << "  (Vital rejected the patch)" << std::endl;
                        break;
                    }

                    // Let it settle after the load, as the plugin does.
                    audition::settle (*host.processor(), kSampleRate, kBlockSize);

                    auto m = audition::audition (*host.processor(), kSampleRate, kBlockSize);
                    if (! m.usable())
                    {
                        rejects[style][m.silent ? "silent"
                                      : m.clickOnly ? "click only"
                                      : m.tooPeaky ? "too peaky"
                                      : m.lopsided ? "lopsided" : "unusable"]++;
                        break;
                    }

                    const auto bounds = audition::brightnessFor (style.toStdString());
                    if (m.centroidHz > 0.0f
                        && (m.centroidHz < bounds.low || m.centroidHz > bounds.high))
                    {
                        rejects[style][m.centroidHz > bounds.high ? "too bright" : "too dark"]++;
                        break;
                    }
                    if (m.heldRatio > audition::maxHeldRatioFor (style.toStdString()))
                    {
                        rejects[style]["note keeps going"]++;
                        break;
                    }

                    const auto pitch = audition::pitchRuleFor (style.toStdString());
                    if (pitch.required
                        && (m.pitchSalience < pitch.minSalience
                            || m.pitchErrorSemitones > pitch.maxErrorSemitones))
                    {
                        rejects[style][m.pitchSalience < pitch.minSalience
                                           ? "pitch unclear" : "wrong note"]++;
                        break;
                    }

                    auto wanted = loudness::correctionDb (m.rms, m.peak);

                    if (wanted == 0.0f)
                    {
                        // Confirm with a second render. A patch whose level
                        // creeps gets caught at its quietest otherwise.
                        auto again = audition::audition (*host.processor(), kSampleRate, kBlockSize);
                        if (! again.usable())
                            break;

                        m.rms = std::max (m.rms, again.rms);
                        m.peak = std::max (m.peak, again.peak);
                        wanted = loudness::correctionDb (m.rms, m.peak);

                        if (wanted == 0.0f)
                        {
                            accepted = true;
                            accepted_m = m;
                            break;
                        }
                    }

                    const auto got = loudness::normalise (result.preset["settings"],
                                                          m.rms, m.peak);
                    if (std::abs (wanted - got) > 6.0f)
                    {
                        rejects[style]["level out of reach"]++;
                        break;
                    }
                }

                if (! accepted)
                {
                    ++rerolls;
                    request.seed = 0;
                    result = generator.roll (request);
                    if (! result.ok)
                        break;
                }
            }
            const auto loadMs = juce::Time::getMillisecondCounterHiRes() - loadStart;

            if (! accepted)
            {
                std::cout << style << " " << (i + 1) << "  no usable patch in 8 tries\n";
                ++total;
                continue;
            }

            auto audio = renderNote (*host.processor());
            auto score = scoreAudio (audio);
            if (rerolls > 0)
                score.verdict << " (" << rerolls << "x)";
            /*  Report the level the plugin actually targets, not the mean over
                the render. A plucked patch is near silent for most of a two
                second window and its mean says so, which makes a perfectly
                reasonable patch look broken.
            */
            levels.push_back (accepted_m.rms);
            styleCentroid[style].push_back (accepted_m.centroidHz);
            styleSustain[style].push_back (
                (float) result.preset["settings"].value ("env_1_sustain", 0.0));
            styleTail[style].push_back (accepted_m.rms > 1.0e-9f
                                            ? accepted_m.tailRms / accepted_m.rms : 0.0f);
            styleHeld[style].push_back (accepted_m.heldRatio);
            stylePitch[style].push_back (accepted_m.pitchErrorSemitones);
            styleSalience[style].push_back (accepted_m.pitchSalience);

            juce::StringArray macros;
            for (int m = 0; m < gen::kMacros; ++m)
                if (! result.macroNames[(size_t) m].empty())
                    macros.add (juce::String (result.macroNames[(size_t) m]));

            std::cout << style.paddedRight (' ', 10)
                      << juce::String (saved + 1).paddedRight (' ', 4)
                      << score.verdict.paddedRight (' ', 11)
                      << juce::String (score.rms, 4).paddedRight (' ', 9)
                      << juce::String (score.peak, 3).paddedRight (' ', 9)
                      << (juce::String ((int) score.centroid) + "Hz").paddedRight (' ', 11)
                      << (juce::String ((int) loadMs) + "ms").paddedRight (' ', 9)
                      << result.routings << "  " << macros.joinIntoString ("/") << "\n";

            if (score.ok)
            {
                ++saved;
                if (saveTo != juce::File())
                {
                    saveTo.createDirectory();
                    auto out = result.preset;
                    out.erase ("tuning");
                    saveTo.getChildFile (style + "_" + juce::String (saved).paddedLeft ('0', 2)
                                             + ".vital")
                          .replaceWithText (juce::String (out.dump()));
                }
            }

            pass += score.ok ? 1 : 0;
            ++total;
        }

        if (saved < perStyle)
            styleShort[style] = saved;
    }

    if (! rejects.empty())
    {
        std::cout << std::endl << "why candidates were rejected" << std::endl;
        for (const auto& style : rejects)
        {
            std::cout << "  " << style.first.paddedRight (' ', 12);
            for (const auto& reason : style.second)
                std::cout << reason.first << " " << reason.second << "   ";
            std::cout << std::endl;
        }
        std::cout << std::endl;
    }

    for (const auto& kv : styleShort)
        std::cout << "only produced " << kv.second << " of " << perStyle << " for "
                  << kv.first << std::endl;

    std::cout << std::endl << pass << " of " << total << " usable";
    if (total > 0)
        std::cout << " (" << (int) std::round (100.0 * pass / total) << "%)";
    std::cout << "\n";

    /*  Loudness consistency is the number that matters here. Hand-made presets
        from a real library sit at a median of 0.135 on this metric. Landing near
        that, and tighter than the corpus spread, is the difference between a
        batch you can audition and the "some are far too loud, some barely
        audible" problem the screen exists to solve.
    */
    if (levels.size() > 4)
    {
        std::sort (levels.begin(), levels.end());
        const auto p10 = levels[levels.size() / 10];
        const auto p90 = levels[levels.size() * 9 / 10];
        std::cout << "\nlevel: median rms " << juce::String (levels[levels.size() / 2], 4)
                  << "   p10 " << juce::String (p10, 4)
                  << "   p90 " << juce::String (p90, 4)
                  << "   spread "
                  << juce::String (20.0f * std::log10 (p90 / juce::jmax (p10, 1.0e-6f)), 1)
                  << " dB      (hand-made presets: median 0.1346, p10 0.0126, p90 0.2858)\n";
    }
    /*  Reported with the same code that screens, so the numbers mean the same
        thing the generator was aiming at. Measuring in a second host gives
        different absolute values and turns verification into guesswork.
    */
    if (! styleCentroid.empty())
    {
        const auto med = [] (std::vector<float> v)
        {
            if (v.empty()) return 0.0f;
            std::sort (v.begin(), v.end());
            return v[v.size() / 2];
        };
        std::cout << std::endl << juce::String ("style").paddedRight (' ', 11)
                  << juce::String ("centroid").paddedRight (' ', 12)
                  << juce::String ("amp sustain").paddedRight (' ', 14)
                  << juce::String ("held note").paddedRight (0x20, 12)
                  << juce::String ("tail").paddedRight (' ', 8)
                  << juce::String ("salience").paddedRight (' ', 11)
                  << "pitch error" << std::endl;
        for (const auto& kv : styleCentroid)
            std::cout << kv.first.paddedRight (' ', 11)
                      << (juce::String ((int) med (kv.second)) + " Hz").paddedRight (' ', 12)
                      << juce::String (med (styleSustain[kv.first]), 2).paddedRight (' ', 14)
                      << juce::String (med (styleHeld[kv.first]), 2).paddedRight (' ', 12)
                      << juce::String (med (styleTail[kv.first]), 2).paddedRight (' ', 8)
                      << juce::String (med (styleSalience[kv.first]), 2).paddedRight (' ', 11)
                      << juce::String (med (stylePitch[kv.first]), 2) << " st" << std::endl;
    }
    return pass == total ? 0 : 2;
}
