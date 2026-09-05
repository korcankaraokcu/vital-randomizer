/*
    Loads an arbitrary VST3 the way a DAW does and plays a note through it.

    This exists because a plugin that hosts another plugin has an unusual
    requirement: creating a VST3 needs a real message thread with COM
    initialised. Lightweight scanners that lack one report a plain scan failure
    and give no clue whether the plugin is actually broken or whether the
    scanner simply cannot support nested hosting. Running the same calls a DAW
    makes, from a proper JUCE message thread, answers that.
*/
#include <iostream>

#include <juce_audio_processors/juce_audio_processors.h>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    if (argc < 2)
    {
        std::cout << "usage: vrscan <path to .vst3> [seconds to wait before playing]\n";
        return 1;
    }

    const juce::File file { juce::String (argv[1]) };
    const auto settleSeconds = argc > 2 ? juce::String (argv[2]).getIntValue() : 20;

    std::cout << "scanning " << file.getFullPathName() << "\n";

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, file.getFullPathName());

    if (found.isEmpty())
    {
        std::cout << "FAIL: no plugin types found in that file\n";
        return 2;
    }

    for (auto* d : found)
        std::cout << "  found: " << d->name << "  (" << d->manufacturerName
                  << ", instrument=" << (d->isInstrument ? "yes" : "no")
                  << ", version " << d->version << ")\n";

    juce::AudioPluginFormatManager manager;
    manager.addFormat (new juce::VST3PluginFormat());

    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 512;

    juce::String error;
    // Timed, because a host scans by constructing the plugin. A constructor
    // that takes seconds is a constructor that hangs a plugin scan.
    const auto startedAt = juce::Time::getMillisecondCounterHiRes();
    auto instance = manager.createPluginInstance (*found[0], sampleRate, blockSize, error);
    const auto constructMs = juce::Time::getMillisecondCounterHiRes() - startedAt;
    if (instance == nullptr)
    {
        std::cout << "FAIL: could not instantiate: " << error << "\n";
        return 3;
    }

    std::cout << "instantiated ok in " << (int) constructMs << " ms" << std::endl;
    instance->enableAllBuses();
    instance->setPlayConfigDetails (0, 2, sampleRate, blockSize);
    instance->prepareToPlay (sampleRate, blockSize);

    std::cout << "  inputs=" << instance->getTotalNumInputChannels()
              << " outputs=" << instance->getTotalNumOutputChannels()
              << " acceptsMidi=" << (instance->acceptsMidi() ? "yes" : "no")
              << " hasEditor=" << (instance->hasEditor() ? "yes" : "no") << "\n";

    // The plugin loads Vital and reads the preset library on a worker thread,
    // so it needs a moment before it can make a sound. Pumping the message loop
    // rather than sleeping keeps that work able to finish.
    std::cout << "letting it start up for " << settleSeconds << "s\n";
    const auto until = juce::Time::getMillisecondCounter() + (juce::uint32) settleSeconds * 1000;
    while (juce::Time::getMillisecondCounter() < until)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);

    const auto renderAndMeasure = [&] (const char* label)
    {
        juce::AudioBuffer<float> block (2, blockSize);
        float peak = 0.0f;
        double sum = 0.0;
        int counted = 0;

        const auto totalBlocks = (int) (sampleRate * 2.0 / blockSize);
        for (int i = 0; i < totalBlocks; ++i)
        {
            block.clear();
            juce::MidiBuffer midi;
            if (i == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 110), 0);
            if (i == totalBlocks / 2)
                midi.addEvent (juce::MidiMessage::noteOff (1, 48), 0);

            instance->processBlock (block, midi);

            for (int ch = 0; ch < block.getNumChannels(); ++ch)
            {
                peak = juce::jmax (peak, block.getMagnitude (ch, 0, block.getNumSamples()));
                const auto rms = block.getRMSLevel (ch, 0, block.getNumSamples());
                sum += rms * rms;
                ++counted;
            }
        }
        const auto rms = counted > 0 ? std::sqrt (sum / counted) : 0.0;
        std::cout << "  " << label << ": peak=" << juce::String (peak, 4)
                  << " rms=" << juce::String (rms, 5)
                  << (rms > 1.0e-4 ? "   SOUND" : "   silent") << "\n";
        return rms > 1.0e-4;
    };

    const auto audible = renderAndMeasure ("note through the plugin");

    instance->releaseResources();
    instance.reset();

    std::cout << (audible ? "\nPASS: the plugin loads and produces audio\n"
                          : "\nFAIL: the plugin loaded but stayed silent\n");
    return audible ? 0 : 4;
}
