/*
    Console harness.

    Loads Vital, rolls patches, pushes each one into Vital and renders a note
    offline, then scores what came back. A patch that looks plausible as JSON
    can still be silent, clipped or a burst of noise, and none of that is
    visible without hearing it.

    It is what every claim about the generator in docs/notes.md was measured
    with, so a change that sounds fine can still be shown not to have moved the
    numbers backwards.
*/
#include <cmath>
#include <iostream>
#include <algorithm>
#include <functional>
#include <set>
#include <map>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "../src/Audition.h"
#include "../src/Archetypes.h"
#include "../src/SampleFactory.h"
#include "../src/WavetableFactory.h"
#include "../src/Axes.h"
#include "../src/Generator.h"
#include "../src/Loudness.h"
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

    /*  The held note's spectrum in third octaves, level matched, in dB.

        Normalised to unit RMS first, so two renders that differ only in how
        loud they are come out the same, and what is left to compare is tone.
    */
    std::vector<float> thirdOctaves (const juce::AudioBuffer<float>& audio)
    {
        const auto start = (int) (kSampleRate * 0.1);
        const auto end = juce::jmin (audio.getNumSamples(), (int) (kSampleRate * 0.95));
        const auto n = end - start;
        if (n < 4096)
            return {};

        constexpr int order = 16;
        constexpr int fftSize = 1 << order;
        std::vector<float> mono ((size_t) n, 0.0f);
        double energy = 0.0;
        for (int i = 0; i < n; ++i)
        {
            float sum = 0.0f;
            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
                sum += audio.getSample (ch, start + i);
            mono[(size_t) i] = sum / (float) juce::jmax (1, audio.getNumChannels());
            energy += mono[(size_t) i] * mono[(size_t) i];
        }
        const auto rms = std::sqrt (energy / n);
        if (rms < 1.0e-6)
            return {};

        std::vector<float> data ((size_t) fftSize * 2, 0.0f);
        for (int i = 0; i < juce::jmin (n, fftSize); ++i)
        {
            const auto window = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi
                                                        * (float) i / (float) n);
            data[(size_t) i] = (float) (mono[(size_t) i] / rms) * window;
        }
        juce::dsp::FFT fft (order);
        fft.performFrequencyOnlyForwardTransform (data.data());

        std::vector<float> bands;
        for (double low = 50.0; low < 16000.0; low *= std::pow (2.0, 1.0 / 3.0))
        {
            const auto high = low * std::pow (2.0, 1.0 / 3.0);
            double power = 0.0;
            for (int bin = (int) std::ceil (low * fftSize / kSampleRate);
                 bin < (int) (high * fftSize / kSampleRate) && bin < fftSize / 2; ++bin)
                power += (double) data[(size_t) bin] * data[(size_t) bin];
            bands.push_back ((float) (10.0 * std::log10 (power + 1.0e-20)));
        }
        return bands;
    }

    float bandDistance (const std::vector<float>& a, const std::vector<float>& b)
    {
        if (a.empty() || a.size() != b.size())
            return 0.0f;
        double sum = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
            sum += (double) (a[i] - b[i]) * (a[i] - b[i]);
        return (float) std::sqrt (sum / (double) a.size());
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
    juce::StringArray styles;
    for (const auto& name : archetype::styleNames())
        styles.add (juce::String (name));
    // Negative means "use the style's own default", which is what the plugin
    // does. Testing every style at one neutral setting measures a patch
    // nobody would ever hear.
    float bright = -1.0f, dirt = -1.0f, space = -1.0f, move = -1.0f;
    float complexity = -1.0f;
    bool rampComplexity = false;
    juce::File saveTo = juce::File::getCurrentWorkingDirectory().getChildFile ("out");
    juce::File diagFile;
    juce::String axisKey;
    int trials = 24;
    /*  Roll a fixed number of candidates and count what happens to them, rather
        than rolling until a batch is full.

        A batch stops as soon as it has its six, so the number it throws away
        getting there depends on how lucky the first few were. Five identical
        runs rejected between 19 and 61 candidates, which is a wider spread than
        most changes worth testing, and that made the tally useless as evidence.
        This rolls the same number every time and writes nothing.
    */
    int statsRolls = 0;
    /*  Where to write the candidates the screen threw away.

        A threshold that cannot be listened to is a threshold nobody can argue
        with. Bass and keys are rejected for brightness far more than any other
        style, and whether that is the generator making dull patches or the
        ceiling being set too low is a question for ears, not for another
        histogram.
    */
    juce::File rejectsTo;
    juce::File modelsTo;
    juce::File scalesTo;
    juce::File gesturesTo;
    // With --axis, write the low and high patches instead of scoring them.
    juce::File pairsTo;
    juce::File dirtLadderTo;
    juce::File typesTo;
    // Which scale the gesture demo walks. The shapes read differently on a
    // seven note maqam than on a pentatonic, and the note floor bites hardest
    // on the big scales, so it has to be possible to ask for one.
    juce::String gestureScale { "minor pentatonic" };
    /*  How many renders each measurement averages.
    
        Three is what the screen uses. Whether three is needed or two would do
        the same has never been measured, and this is what measures it.
    */
    int renders = 3;
    // The axis test used to score one render per side. Only when asked does
    // it average, so numbers from before stay comparable.
    bool rendersGiven = false;
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
        if (arg.startsWith ("--complexity="))
        {
            // --complexity=ramp walks the whole range across a style's presets,
            // the first at nothing and the last at everything, so they can be
            // compared side by side rather than heard one at a time.
            if (value.trim() == "ramp")
                rampComplexity = true;
            else
                complexity = value.getFloatValue();
        }
        if (arg.startsWith ("--models="))    modelsTo = juce::File (value);
        if (arg.startsWith ("--scales="))    scalesTo = juce::File (value);
        if (arg.startsWith ("--gestures=")) gesturesTo = juce::File (value);
        if (arg.startsWith ("--pairs="))    pairsTo = juce::File (value);
        if (arg.startsWith ("--dirt-ladder=")) dirtLadderTo = juce::File (value);
        if (arg.startsWith ("--distortion-types=")) typesTo = juce::File (value);
        if (arg.startsWith ("--gesture-scale=")) gestureScale = value;
        if (arg.startsWith ("--renders="))
        {
            renders = juce::jlimit (1, 8, value.getIntValue());
            rendersGiven = true;
        }
        if (arg.startsWith ("--stats="))     statsRolls = value.getIntValue();
        if (arg.startsWith ("--rejects="))   rejectsTo = juce::File (value);
        if (arg.startsWith ("--axis="))      axisKey = value.trim();
        if (arg.startsWith ("--trials="))    trials = value.getIntValue();
        if (arg.startsWith ("--save="))      saveTo = juce::File (value);
        if (arg.startsWith ("--diag="))      diagFile = juce::File (value);
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
            std::cout << "  " << juce::String (label).paddedRight (' ', 30)
                      << "rms=" << juce::String (m.rms, 4)
                      << "  peak=" << juce::String (m.peak, 3)
                      << "  centroid=" << juce::String ((int) m.centroidHz) << "Hz"
                      << "  low=" << juce::String ((int) (100.0f * m.lowRatio)) << "%"
                      << "  spike=" << juce::String ((int) (100.0f * m.highSpike)) << "%"
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

    gen::Generator generator;
    {
        auto init = host.currentPreset();
        if (init.is_null() || ! init.contains ("settings"))
        {
            std::cout << "could not read Vital's init patch" << std::endl;
            return 1;
        }
        init.erase ("tuning");
        generator.setInitPreset (std::move (init));
    }

    /*  Does moving a slider do what it says?

        Build the same patch twice from one seed, once with the slider low and
        once high, and see whether the sound moved the way the slider claims it
        should. One seed means the structure is identical on both sides, so
        anything that differs is the axis's doing and nothing else.

        Screening is deliberately skipped here. A rejected candidate would be
        rolled again into a different patch, and then the two sides are no longer
        the same patch with one slider moved.
    */
    /*  One patch in each of Vital's six distortion circuits, for listening.

        DIRT 0.75, where all six are open, one seed per style, and everything
        but the type held still, so the files differ in the circuit alone. The
        names are the order Vital's menu lists them in; the menu itself is the
        authority if they ever disagree.
    */
    if (typesTo != juce::File())
    {
        auto init = host.currentPreset();
        if (init.is_null() || ! init.contains ("settings"))
        {
            std::cout << "could not read Vital's init patch" << std::endl;
            return 1;
        }
        init.erase ("tuning");
        generator.setInitPreset (std::move (init));
        typesTo.createDirectory();

        const char* names[] = { "soft_clip", "hard_clip", "linear_fold",
                                "sine_fold", "bit_crush", "down_sample" };
        for (const auto& styleName : styles)
        {
            const auto defaults = archetype::defaultSlidersFor (styleName.toStdString());
            for (int type = 0; type < 6; ++type)
            {
                gen::Request request;
                request.style = styleName.toStdString();
                request.seed = 0x7E5E0001u;
                request.amount = 1.0f;
                request.complexityWobble = 0.0f;
                request.distortionType = type;
                request.sliders = {
                    { "bright", defaults.bright }, { "dirt", 0.75f },
                    { "space", defaults.space },   { "move", defaults.move },
                    { "complexity", defaults.complexity } };

                auto result = generator.roll (request);
                if (! result.ok || ! host.applyPreset (result.preset))
                    continue;
                audition::settle (*host.processor(), kSampleRate, kBlockSize);
                const auto m = audition::auditionAveraged (*host.processor(), kSampleRate, kBlockSize);
                loudness::normalise (result.preset["settings"], m.rms, m.peak);

                auto out = result.preset;
                out.erase ("tuning");
                out["preset_name"] = (styleName + " type " + juce::String (type) + " "
                                      + juce::String (names[type]).replace ("_", " ")).toStdString();
                out["preset_style"] = styleName.toStdString();
                out["comments"] = "one patch in each distortion type, DIRT 0.75, level matched";
                typesTo.getChildFile (styleName + "_type" + juce::String (type) + "_"
                                      + names[type] + ".vital")
                       .replaceWithText (juce::String (out.dump()));
            }
            std::cout << "  " << styleName << std::endl;
        }
        std::cout << "written to " << typesTo.getFullPathName() << std::endl;
        return 0;
    }

    /*  The same patches at five DIRT settings, for listening.

        Two seeds per style, each built at DIRT 0, 0.25, 0.5, 0.75 and 1, with
        every other slider at the style's default. The content and the wiring
        have streams of their own, so within one seed the only thing that moves
        between files is what DIRT sets, and each file is level matched like a
        roll would be, so louder never passes for dirtier.
    */
    if (dirtLadderTo != juce::File())
    {
        auto init = host.currentPreset();
        if (init.is_null() || ! init.contains ("settings"))
        {
            std::cout << "could not read Vital's init patch" << std::endl;
            return 1;
        }
        init.erase ("tuning");
        generator.setInitPreset (std::move (init));
        dirtLadderTo.createDirectory();

        for (const auto& styleName : styles)
        {
            const auto defaults = archetype::defaultSlidersFor (styleName.toStdString());
            for (int s = 0; s < 2; ++s)
            {
                const auto seed = (unsigned int) (0xD1B70000u + (unsigned int) s * 2654435761u) | 1u;
                for (const auto dirt : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
                {
                    gen::Request request;
                    request.style = styleName.toStdString();
                    request.seed = seed;
                    request.amount = 1.0f;
                    request.complexityWobble = 0.0f;
                    request.sliders = {
                        { "bright", defaults.bright }, { "dirt", dirt },
                        { "space", defaults.space },   { "move", defaults.move },
                        { "complexity", defaults.complexity } };

                    auto result = generator.roll (request);
                    if (! result.ok || ! host.applyPreset (result.preset))
                        continue;
                    audition::settle (*host.processor(), kSampleRate, kBlockSize);
                    const auto m = audition::auditionAveraged (*host.processor(), kSampleRate, kBlockSize);
                    loudness::normalise (result.preset["settings"], m.rms, m.peak);

                    const auto label = juce::String (juce::roundToInt (dirt * 100.0f)).paddedLeft ('0', 3);
                    auto out = result.preset;
                    out.erase ("tuning");
                    out["preset_name"] = (styleName + " " + juce::String (s + 1) + " dirt " + label).toStdString();
                    out["preset_style"] = styleName.toStdString();
                    out["comments"] = "one seed at five DIRT settings, level matched";
                    dirtLadderTo.getChildFile (styleName + "_" + juce::String (s + 1)
                                               + "_dirt" + label + ".vital")
                                .replaceWithText (juce::String (out.dump()));
                }
            }
            std::cout << "  " << styleName << std::endl;
        }
        std::cout << "\nwritten to " << dirtLadderTo.getFullPathName() << std::endl;
        return 0;
    }

    /*  One sequence per gesture, on one scale, so the shape of the line is the
        only thing that differs.

        Minor pentatonic throughout, because it has no wrong note in it and so
        gets out of the way of the question being asked, which is what a run
        sounds like next to a pedal.
    */
    if (gesturesTo != juce::File())
    {
        auto init = host.currentPreset();
        if (init.is_null() || ! init.contains ("settings"))
        {
            std::cout << "could not read Vital's init patch" << std::endl;
            return 1;
        }
        init.erase ("tuning");
        generator.setInitPreset (std::move (init));
        gesturesTo.createDirectory();

        const auto& scales = gen::sequenceScales();
        int pentatonic = 0;
        for (size_t i = 0; i < scales.size(); ++i)
            if (juce::String (scales[i].name) == gestureScale)
            {
                pentatonic = (int) i;
                break;
            }
        std::cout << "gestures on " << scales[(size_t) pentatonic].name << std::endl;

        const auto names = wavetable::stepGestureNames();
        for (size_t g = 0; g < names.size(); ++g)
        {
            gen::Request request;
            request.style = "Sequence";
            request.amount = 1.0f;
            const auto preset = archetype::defaultSlidersFor ("Sequence");
            request.sliders = {
                { "bright", preset.bright }, { "dirt", preset.dirt },
                { "space", preset.space },   { "move", preset.move },
                { "complexity", preset.complexity } };
            request.complexityWobble = 0.0f;
            request.seed = 0x9E571Bu;
            request.scale = pentatonic;
            request.gesture = (int) g;
            request.name = names[g];

            auto result = generator.roll (request);
            if (! result.ok)
            {
                std::cout << "  " << names[g] << ": " << result.error << std::endl;
                continue;
            }

            host.applyPreset (result.preset);
            audition::settle (*host.processor(), kSampleRate, kBlockSize);
            const auto m = audition::auditionAveraged (*host.processor(), kSampleRate, kBlockSize);
            loudness::normalise (result.preset["settings"], m.rms, m.peak);

            auto out = result.preset;
            out.erase ("tuning");
            out["preset_name"] = names[g];
            out["preset_style"] = "Sequence";
            out["comments"] = "one gesture, " + gestureScale.toStdString()
                              + ", everything else held still";

            gesturesTo.getChildFile ("Gesture_" + juce::String (names[g]) + ".vital")
                      .replaceWithText (juce::String (out.dump()));
            std::cout << "  " << names[g] << std::endl;
        }
        /*  And some with the phrases left to fall where they will, which is
            what a rolled sequence actually does. The five above are each one
            shape end to end and exist to be compared against these.
        */
        for (int mixed = 1; mixed <= 6; ++mixed)
        {
            gen::Request request;
            request.style = "Sequence";
            request.amount = 1.0f;
            const auto preset = archetype::defaultSlidersFor ("Sequence");
            request.sliders = {
                { "bright", preset.bright }, { "dirt", preset.dirt },
                { "space", preset.space },   { "move", preset.move },
                { "complexity", preset.complexity } };
            request.complexityWobble = 0.0f;
            request.seed = 0x9E571Bu + (unsigned int) mixed * 2654435761u;
            request.scale = pentatonic;
            request.gesture = -1;          // let it cut its own phrases
            request.name = "mixed " + std::to_string (mixed);

            auto result = generator.roll (request);
            if (! result.ok)
                continue;

            host.applyPreset (result.preset);
            audition::settle (*host.processor(), kSampleRate, kBlockSize);
            const auto m = audition::auditionAveraged (*host.processor(), kSampleRate, kBlockSize);
            loudness::normalise (result.preset["settings"], m.rms, m.peak);

            /*  The LFO named itself while it was built, so which phrases it
                chose is readable off the patch rather than worked out from its
                points. Vital shows that name in its own editor too.
            */
            juce::String shape;
            {
                // The one wired at the pitch, not whichever is named something
                // interesting: every patch has a Triangle in it somewhere.
                const auto& mods = result.preset["settings"]["modulations"];
                for (size_t k = 0; k < mods.size(); ++k)
                {
                    const auto source = mods[k].value ("source", std::string());
                    const auto dest = mods[k].value ("destination", std::string());
                    if (source.rfind ("lfo_", 0) != 0 || dest.find ("transpose") == std::string::npos)
                        continue;

                    const auto index = std::atoi (source.c_str() + 4) - 1;
                    const auto& lfos = result.preset["settings"]["lfos"];
                    if (index >= 0 && index < (int) lfos.size() && lfos[(size_t) index].contains ("name"))
                        shape = juce::String (lfos[(size_t) index]["name"].get<std::string>());
                    break;
                }
            }

            auto out = result.preset;
            out.erase ("tuning");
            out["preset_name"] = "mixed: " + shape.toStdString();
            out["preset_style"] = "Sequence";
            out["comments"] = "phrases chosen freely, " + gestureScale.toStdString();

            gesturesTo.getChildFile ("Mixed_" + juce::String (mixed) + "_"
                                     + shape.replace (" + ", "-").replace (" ", "")
                                     + ".vital")
                      .replaceWithText (juce::String (out.dump()));
            std::cout << "  mixed " << mixed << ":  " << shape << std::endl;
        }

        std::cout << "\nwritten to " << gesturesTo.getFullPathName() << std::endl;
        return 0;
    }

    /*  One sequence per scale, so a scale can be heard rather than read.

        These are ordinary sequences apart from the scale being pinned: the
        oscillator, the filter and the effects are left alone, because a scale
        played through nothing is a test tone and tells you nothing about
        whether it works in a patch. What is fixed is the scale, the step rate
        and the seed, so the only thing that differs between two of these files
        is the set of intervals.
    */
    if (scalesTo != juce::File())
    {
        auto init = host.currentPreset();
        if (init.is_null() || ! init.contains ("settings"))
        {
            std::cout << "could not read Vital's init patch" << std::endl;
            return 1;
        }
        init.erase ("tuning");
        generator.setInitPreset (std::move (init));
        scalesTo.createDirectory();

        const auto& scales = gen::sequenceScales();
        std::set<std::string> written;

        for (size_t i = 0; i < scales.size(); ++i)
        {
            const std::string name = scales[i].name;
            if (! written.insert (name).second)
                continue;       // the weighted ones appear more than once

            gen::Request request;
            request.style = "Sequence";
            request.amount = 1.0f;
            const auto preset = archetype::defaultSlidersFor ("Sequence");
            request.sliders = {
                { "bright", preset.bright }, { "dirt", preset.dirt },
                { "space", preset.space },   { "move", preset.move },
                { "complexity", preset.complexity } };
            request.complexityWobble = 0.0f;
            // One seed for all of them, so the patch is the same and only the
            // intervals move.
            request.seed = 0x5CA1E5u;
            request.scale = (int) i;
            // A run, every time. A sequence normally cuts itself into phrases
            // with a shape each, which is the point of it, and the wrong thing
            // for hearing what a scale is: that wants one walk straight up.
            request.gesture = 0;
            request.name = name;

            auto result = generator.roll (request);
            if (! result.ok)
            {
                std::cout << "  " << name << ": " << result.error << std::endl;
                continue;
            }

            audition::Measurement m;
            host.applyPreset (result.preset);
            audition::settle (*host.processor(), kSampleRate, kBlockSize);
            m = audition::auditionAveraged (*host.processor(), kSampleRate, kBlockSize);
            loudness::normalise (result.preset["settings"], m.rms, m.peak);

            auto out = result.preset;
            out.erase ("tuning");
            out["preset_name"] = name;
            out["preset_style"] = "Sequence";
            out["comments"] = "one scale, everything else held still";

            auto file = juce::String (name).replace (" ", "_");
            scalesTo.getChildFile ("Scale_" + file + ".vital")
                    .replaceWithText (juce::String (out.dump()));
            std::cout << "  " << name << "  (" << scales[i].degrees.size()
                      << " degrees)" << std::endl;
        }
        std::cout << "\nwritten to " << scalesTo.getFullPathName() << std::endl;
        return 0;
    }

    /*  One preset per sample model, with everything else taken out.

        The layer is normally heard underneath two oscillators, a filter and a
        reverb, which is the right way to use it and the wrong way to judge it.
        These have the oscillators off, the filters bypassed, every effect off
        and an envelope that just gets out of the way, so what comes out of the
        speaker is the model and nothing else.
    */
    if (modelsTo != juce::File())
    {
        auto init = host.currentPreset();
        if (init.is_null() || ! init.contains ("settings"))
        {
            std::cout << "could not read Vital's init patch" << std::endl;
            return 1;
        }
        init.erase ("tuning");
        modelsTo.createDirectory();

        std::mt19937 rng { 0x5A11E5u };
        /*  Three of each.

            Every model draws its own parameters per patch, so two instances of
            one are as different from each other as two presets are. One of each
            shows what the models are and hides that, which is the more obvious
            question when hearing them for the first time.
        */
        for (int variant = 1; variant <= 3; ++variant)
        for (const auto& name : sampler::modelNames())
        {
            auto preset = init;
            auto& s = preset["settings"];

            sampler::Request sr;
            sr.style = "Experiment";        // no style rules to narrow the model
            sr.model = name;
            sr.bright = 0.5f;
            sr.dirt = 0.5f;
            sr.complexity = 0.6f;
            auto built = sampler::create (rng, sr);

            for (const auto* off : { "osc_1_on", "osc_2_on", "osc_3_on",
                                     "filter_1_on", "filter_2_on", "filter_fx_on",
                                     "reverb_on", "delay_on", "chorus_on", "phaser_on",
                                     "flanger_on", "distortion_on", "compressor_on",
                                     "eq_on" })
                if (s.contains (off))
                    s[off] = 0.0;

            s["sample"] = std::move (built.sample);
            s["sample_on"] = 1.0;
            s["sample_loop"] = built.loop ? 1.0 : 0.0;
            s["sample_keytrack"] = built.keytrack ? 1.0 : 0.0;
            s["sample_pan"] = 0.0;
            // Loud, because it is the only thing making a sound.
            s["sample_level"] = 0.85;

            // An envelope that does not shape it, so the model is what is heard.
            s["env_1_attack"] = 0.0;
            s["env_1_decay"] = 1.0;
            s["env_1_sustain"] = 1.0;
            s["env_1_release"] = 0.35;

            s["modulations"] = nlohmann::json::array();
            for (int i = 0; i < gen::kModSlots; ++i)
                s["modulations"].push_back ({ { "destination", "" }, { "source", "" } });

            s["volume"] = loudness::kVolumeDefault;
            preset["preset_name"] = name + " " + std::to_string (variant) + " (bare)";
            preset["preset_style"] = "Experiment";
            preset["comments"] = "one sample model, nothing else";
            preset["author"] = "";

            const auto file = modelsTo.getChildFile ("Model_" + juce::String (name)
                                                      + "_" + juce::String (variant) + ".vital");
            file.replaceWithText (juce::String (preset.dump()));
            std::cout << "  " << file.getFileName()
                      << (built.loop ? "   looping" : "   one shot")
                      << (built.keytrack ? ", follows the keyboard" : "") << std::endl;
        }
        std::cout << "\nwritten to " << modelsTo.getFullPathName() << std::endl;
        return 0;
    }

    if (axisKey.isNotEmpty())
    {
        if (pairsTo != juce::File())
            pairsTo.createDirectory();

        /*  Each axis needs the number that says whether it did its job, and
            they are not the same number.

            BRIGHT moves where the energy sits. SPACE leaves sound behind after
            the key is released, so it shows in the tail rather than in the note.
            MOVE is the envelope refusing to sit still.

            DIRT is scored against the patch itself. Each side is rendered as
            rolled and again with its grit taken out, the distortion switched
            off and the filter drive at zero, and the number is how far apart
            the two sound, level matched. It used to be the crest factor, on the
            reasoning that distortion fills in the gap between peak and average,
            but a clipper resting at unity barely does and a bit crusher does
            not at all, and crest agreed with the slider half the time. Of eleven
            candidates tried on saved pairs, this one agreed 93% of the time and
            was the only one above 70% that did not simply rise with BRIGHT or
            SPACE: it sat at 38% and 35% on their pairs, since a brighter or
            wetter patch buries a little of its own grit. Here it reads about
            80%, and why it reads lower than on the saved pairs is not known.

            Scoring all four on the centroid, which is what happened before this
            existed, only ever measured brightness.
        */
        struct Metric
        {
            const char* label;
            bool higherIsMore;
            std::function<float (const audition::Measurement&)> of;
        };

        const std::map<juce::String, Metric> metrics = {
            { "bright", { "energy centroid, Hz", true,
                          [] (const audition::Measurement& m) { return m.centroidHz; } } },
            { "space",  { "tail against note, %", true,
                          [] (const audition::Measurement& m)
                          { return m.rms > 1.0e-6f ? 100.0f * m.tailRms / m.rms : 0.0f; } } },
            { "move",   { "tone motion, %", true,
                          [] (const audition::Measurement& m)
                          { return 100.0f * m.spectralMotion; } } },
            // Scored in the loop below, since it needs a second render.
            { "dirt",   { "grit against its own clean twin, dB", true,
                          [] (const audition::Measurement&) { return 0.0f; } } },
        };

        const auto found = metrics.find (axisKey);
        if (found == metrics.end())
        {
            std::cout << "no metric for axis " << axisKey
                      << ". Known: bright, dirt, space, move" << std::endl;
            return 1;
        }
        const auto metric = found->second;
        std::cout << "\n" << axisKey << ": the same patch built low and high, "
                  << trials << " seeds per style\n" << std::endl;
        std::cout << "scored on " << metric.label << "\n" << std::endl;
        std::cout << juce::String ("style").paddedRight (' ', 12)
                  << juce::String ("agreement").paddedRight (' ', 12)
                  << "median move" << std::endl;

        int agreedAll = 0, testedAll = 0;
        for (const auto& styleName : styles)
        {
            const auto defaults = archetype::defaultSlidersFor (styleName.toStdString());
            int agreed = 0, tested = 0;
            std::vector<float> moves;

            for (int t = 0; t < trials; ++t)
            {
                const auto seed = (unsigned int) (0x51ED0000u + (unsigned int) t * 2654435761u) | 1u;
                float value[2] = { 0.0f, 0.0f };
                bool ok = true;

                for (int side = 0; side < 2 && ok; ++side)
                {
                    gen::Request request;
                    request.style = styleName.toStdString();
                    request.seed = seed;
                    request.amount = 1.0f;
                    request.sliders = {
                        { "bright", defaults.bright }, { "dirt", defaults.dirt },
                        { "space", defaults.space },   { "move", defaults.move },
                        { "complexity", defaults.complexity } };
                    request.sliders[axisKey.toStdString()] = side == 0 ? 0.15f : 0.85f;

                    auto rolled = generator.roll (request);

                    /*  Or just write the pair and move on.

                        A new metric is quicker to try against saved patches
                        than to build into this loop and rerun eight minutes at
                        a time, so this writes the exact low and high patches
                        the test would have scored and leaves the scoring to
                        whatever is being tried.
                    */
                    if (pairsTo != juce::File() && rolled.ok)
                    {
                        auto out = rolled.preset;
                        out.erase ("tuning");
                        pairsTo.getChildFile (styleName + "_" + juce::String (t).paddedLeft ('0', 2)
                                              + (side == 0 ? "_lo" : "_hi") + ".vital")
                               .replaceWithText (juce::String (out.dump()));
                        continue;
                    }

                    if (! rolled.ok || ! host.applyPreset (rolled.preset))
                    {
                        ok = false;
                        break;
                    }
                    audition::settle (*host.processor(), kSampleRate, kBlockSize);
                    const auto m = rendersGiven
                        ? audition::auditionAveraged (*host.processor(), kSampleRate,
                                                      kBlockSize, renders)
                        : audition::audition (*host.processor(), kSampleRate, kBlockSize);
                    if (! m.usable())
                    {
                        ok = false;
                    }
                    else if (axisKey == "dirt")
                    {
                        // Three takes each way, because Vital's unison phase
                        // moves every take and the pairing has to average it.
                        auto clean = rolled.preset;
                        auto& cs = clean["settings"];
                        cs["distortion_on"] = 0.0;
                        cs["filter_1_drive"] = 0.0;
                        cs["filter_2_drive"] = 0.0;

                        std::vector<std::vector<float>> dirty, plain;
                        for (int i = 0; i < 3; ++i)
                            dirty.push_back (thirdOctaves (renderNote (*host.processor())));
                        if (! host.applyPreset (clean))
                        {
                            ok = false;
                            break;
                        }
                        audition::settle (*host.processor(), kSampleRate, kBlockSize);
                        for (int i = 0; i < 3; ++i)
                            plain.push_back (thirdOctaves (renderNote (*host.processor())));

                        std::vector<float> distances;
                        for (int i = 0; i < 3; ++i)
                            distances.push_back (bandDistance (dirty[(size_t) i], plain[(size_t) i]));
                        std::sort (distances.begin(), distances.end());
                        value[side] = distances[1];
                    }
                    else
                    {
                        value[side] = metric.of (m);
                    }
                }

                if (! ok)
                    continue;

                ++tested;
                const auto delta = metric.higherIsMore ? value[1] - value[0]
                                                       : value[0] - value[1];
                if (delta > 0.0f)
                    ++agreed;
                moves.push_back (delta);
            }

            agreedAll += agreed;
            testedAll += tested;
            std::sort (moves.begin(), moves.end());
            const auto median = moves.empty() ? 0.0f : moves[moves.size() / 2];
            std::cout << styleName.paddedRight (' ', 12)
                      << (tested > 0 ? juce::String (100 * agreed / tested) + "%"
                                     : juce::String ("-")).paddedRight (' ', 12)
                      << juce::String (median, 1) << std::endl;
        }

        std::cout << "\noverall " << (testedAll > 0 ? 100 * agreedAll / testedAll : 0)
                  << "% over " << testedAll << " pairs" << std::endl;
        return 0;
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
        if (archetype::find (style.toStdString()) == nullptr)
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
        for (int i = 0; i < (statsRolls > 0 ? statsRolls : perStyle * 8)
                        && (statsRolls > 0 || saved < perStyle); ++i)
        {
            gen::Request request;
            request.style = style.toStdString();
            request.amount = 1.0f;
            const auto preset = archetype::defaultSlidersFor (style.toStdString());
            request.sliders = {
                { "bright", bright >= 0.0f ? bright : preset.bright },
                { "dirt",   dirt   >= 0.0f ? dirt   : preset.dirt },
                { "space",  space  >= 0.0f ? space  : preset.space },
                { "move",   move   >= 0.0f ? move   : preset.move },
                { "complexity", complexity >= 0.0f ? complexity : preset.complexity } };

            if (rampComplexity)
            {
                // Stepped on what has been kept, not on what has been tried, so
                // a rejected candidate does not put the ramp out of step.
                request.sliders["complexity"] = perStyle > 1
                    ? (float) saved / (float) (perStyle - 1) : 0.5f;
                request.complexityWobble = 0.0f;
            }
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

            // One look each in stats mode. Retrying until something passes is
            // what makes the count depend on luck.
            for (int attempt = 0; attempt < (statsRolls > 0 ? 1 : 8) && ! accepted; ++attempt)
            {
    /*  Correct, then re-measure, then correct again if needed.

        One pass is not enough. The volume calibration was swept on a clean
        patch, and a patch with heavy distortion or compression in its chain
        does not respond to master volume the same way, so the first correction
        can land several dB off. Measuring what actually came out and going
        again is the only way to be sure the patch a user hears is the level it
        was supposed to be.
    */
                bool exhausted = true;
                // What each pass asked the level correction for, kept so a
                // rejected patch can say why it never landed.
                std::vector<float> asked;
                auto writeLevelReject = [&] (const juce::String& why)
                {
                    if (rejectsTo == juce::File())
                        return;
                    rejectsTo.createDirectory();
                    const auto& st = result.preset["settings"];
                    const auto knob = [] (double db) { return juce::roundToInt ((db + 30.0) / 60.0 * 100.0); };
                    juce::String trail;
                    for (const auto a : asked)
                        trail << (a >= 0.0f ? "+" : "") << juce::String (a, 1) << "dB ";
                    auto out = result.preset;
                    out["comments"] = (why + ". Corrections asked for, pass by pass: " + trail.trim()
                                       + ". Distortion " + (st.value ("distortion_on", 0.0) >= 0.5 ? "on" : "off")
                                       + ", drive at " + juce::String (knob (st.value ("distortion_drive", 0.0))) + "% of the knob"
                                       + ", mix " + juce::String (juce::roundToInt (100.0 * st.value ("distortion_mix", 0.0))) + "%.")
                                          .toStdString();
                    rejectsTo.getChildFile (style + "_" + why.replace (" ", "")
                                            + "_drive" + juce::String (knob (st.value ("distortion_drive", 0.0)))
                                            + "_mix" + juce::String (juce::roundToInt (100.0 * st.value ("distortion_mix", 0.0)))
                                            + "_" + juce::String (result.seed) + ".vital")
                              .replaceWithText (juce::String (out.dump (2)));
                };
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

                    auto m = audition::auditionAveraged (*host.processor(), kSampleRate,
                                                         kBlockSize, renders);
                    if (! m.usable())
                    {
                        exhausted = false;
                        rejects[style][m.silent ? "silent"
                                      : m.clickOnly ? "click only"
                                      : m.tooPeaky ? "too peaky"
                                      : m.lopsided ? "lopsided" : "unusable"]++;
                        break;
                    }

                    const auto settings_decay =
                        result.preset["settings"].value ("env_1_decay", 0.0);

                    /*  Balance before brightness. High frequency detail is
                        fine as long as it is quiet, so what decides whether a
                        bass is a bass is how much of it sits down low.
                    */
                    const auto balance = audition::balanceFor (style.toStdString());
                    if (m.lowRatio > 0.0f && m.lowRatio < balance.minLowRatio)
                    {
                        exhausted = false;
                        rejects[style]["not enough bottom"]++;
                        break;
                    }
                    if (m.highSpike > balance.maxHighSpike)
                    {
                        exhausted = false;
                        rejects[style]["a burst up top"]++;
                        break;
                    }

                    const auto bounds = audition::brightnessFor (style.toStdString(),
                                                 request.sliders["complexity"]);
                    if (m.centroidHz > 0.0f
                        && (m.centroidHz < bounds.low || m.centroidHz > bounds.high))
                    {
                        exhausted = false;
                        const auto why = m.centroidHz > bounds.high ? "too bright" : "too dark";
                        rejects[style][why]++;

                        if (rejectsTo != juce::File())
                        {
                            /*  Levelled before it is written.

                                A candidate is screened for its style before the
                                loudness pass runs, so a rejected patch saved as
                                it stands is several dB hotter than anything that
                                passed. Handed over that way, two of them were
                                called loud when what was being asked about was
                                whether they were bright.
                            */
                            auto levelled = result.preset;
                            loudness::normalise (levelled["settings"], m.rms, m.peak);

                            // Named with the reading and the ceiling it missed,
                            // so a listen and the number are side by side.
                            rejectsTo.createDirectory();
                            const auto name = style + "_" + juce::String (why).replace (" ", "")
                                            + "_" + juce::String ((int) m.centroidHz) + "Hz"
                                            + "_limit" + juce::String ((int) bounds.high)
                                            + "_" + juce::String (result.seed) + ".vital";
                            rejectsTo.getChildFile (name)
                                     .replaceWithText (levelled.dump (2));
                        }
                        break;
                    }
                    if (m.heldRatio > audition::maxHeldRatioFor (style.toStdString()))
                    {
                        exhausted = false;
                        rejects[style]["note keeps going"]++;
                        if (rejectsTo != juce::File())
                        {
                            rejectsTo.createDirectory();
                            rejectsTo.getChildFile (style + "_keepsgoing_held"
                                + juce::String ((int) (100.0f * m.heldRatio))
                                + "_decay" + juce::String ((int) (100.0 * settings_decay))
                                + "_" + juce::String (result.seed) + ".vital")
                                     .replaceWithText (result.preset.dump (2));
                        }
                        break;
                    }

                    const auto pitch = audition::pitchRuleFor (style.toStdString());
                    const auto stepped = audition::isSteppedStyle (style.toStdString())
                                             && m.steps > 0;
                    const auto salience = stepped ? m.stepSalience : m.pitchSalience;
                    const auto pitchError = stepped ? m.stepOffQuarterSemitones
                                          : pitch.octavesOnly ? m.pitchErrorSemitones
                                                              : m.pitchOffGridSemitones;
                    if (pitch.required
                        && (salience < pitch.minSalience
                            || pitchError > pitch.maxErrorSemitones))
                    {
                        exhausted = false;
                        const auto why = salience < pitch.minSalience
                                             ? "pitch unclear" : "wrong note";
                        rejects[style][why]++;

                        if (rejectsTo != juce::File())
                        {
                            rejectsTo.createDirectory();
                            auto levelled = result.preset;
                            loudness::normalise (levelled["settings"], m.rms, m.peak);
                            const auto name = style + "_" + juce::String (why).replace (" ", "")
                                + "_off" + juce::String ((int) (100.0f * pitchError))
                                + "_sal" + juce::String ((int) (100.0f * salience))
                                + "_" + juce::String (result.seed) + ".vital";
                            rejectsTo.getChildFile (name).replaceWithText (levelled.dump (2));
                        }
                        break;
                    }

                    auto wanted = loudness::correctionDb (m.rms, m.peak);
                    asked.push_back (wanted);

                    // The reading is already three renders averaged, so there is
                    // nothing left to confirm it against.
                    if (wanted == 0.0f)
                    {
                        accepted = true;
                        exhausted = false;
                        accepted_m = m;
                        break;
                    }

                    const auto got = loudness::normalise (result.preset["settings"],
                                                          m.rms, m.peak);
                    if (std::abs (wanted - got) > 6.0f)
                    {
                        exhausted = false;
                        rejects[style]["level out of reach"]++;
                        writeLevelReject ("level out of reach");
                        break;
                    }
                }

                // Running out of level passes without ever tripping a check is
                // its own outcome, and it was invisible: a style could fail to
                // fill its quota with nothing at all recorded against it.
                if (! accepted && exhausted)
                {
                    rejects[style]["level never settled"]++;
                    writeLevelReject ("level never settled");
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

            /*  The screen already decided this patch is worth keeping, and it
                is far more thorough than a second opinion taken here. A plain
                average over a two second window, which is what scoring the
                render measures, reads a short bass as silent because most of
                that window is silence, and short is exactly what a bass is
                supposed to be. So the render is kept only for the brightness
                figure and the verdict comes from the screen.
            */
            auto audio = renderNote (*host.processor());
            auto score = scoreAudio (audio);
            score.ok = true;
            score.verdict = "ok";
            score.rms = accepted_m.rms;
            score.peak = accepted_m.peak;
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
            const auto acceptedStepped = audition::isSteppedStyle (style.toStdString())
                                             && accepted_m.steps > 0;
            stylePitch[style].push_back (acceptedStepped ? accepted_m.stepOffGridSemitones
                                                         : accepted_m.pitchErrorSemitones);
            styleSalience[style].push_back (acceptedStepped ? accepted_m.stepSalience
                                                            : accepted_m.pitchSalience);

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
                if (saveTo != juce::File() && statsRolls == 0)
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
        that, and tighter than their spread, is the difference between a
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
