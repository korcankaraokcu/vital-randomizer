#pragma once

#include <atomic>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>
#include <nlohmann/json.hpp>

/*
    Owns the hosted Vital instance.

    Loading a patch means handing Vital bytes through setStateInformation,
    because that is the only door a plugin interface has for state. There is no
    "open this file" call: Vital's own file loading lives in its GUI and browser
    code, and it does not expose its presets as VST programs either.

    A load takes a couple of hundred milliseconds while Vital rebuilds its
    wavetables, which is far too long for the audio thread. So loads happen on a
    background thread holding a lock, and the audio thread outputs silence for
    the duration rather than blocking the host.
*/
class VitalHost
{
public:
    VitalHost();
    ~VitalHost();

    /** Standard install locations, in the order they are tried. */
    static juce::Array<juce::File> searchPaths();
    static juce::File findInstalled();

    bool load (const juce::File& vst3, double sampleRate, int blockSize,
               juce::String& error);
    bool isLoaded() const { return instance != nullptr; }
    juce::File loadedFrom() const { return pluginFile; }

    void prepare (double sampleRate, int blockSize);
    void release();

    /** Runs the hosted synth. Returns false if a patch load is in progress, in
        which case the caller should output silence. */
    bool process (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi);

    /** Push a patch into Vital. Blocks for the length of the load, so call it
        from a background thread, never from the audio thread or the message
        thread if you care about the UI staying responsive. */
    bool applyPreset (const nlohmann::json& preset);

    /*  Push a patch and confirm Vital actually took it.

        Vital's loaders index straight into the json rather than checking a key
        is there first, so one missing field fails the whole preset. When that
        happens the instance quietly keeps playing the previous patch, which is
        far worse than an error: the generator measures the old sound, decides
        it is fine, and the user gets "preset is corrupted" with no clue why.
        Reading the state back and looking for a marker turns that into a plain
        rejection the generator can handle.
    */
    bool applyPresetVerified (const nlohmann::json& preset);

    /** The patch currently loaded, as Vital reports it. */
    nlohmann::json currentPreset() const;

    juce::AudioProcessor* processor() const { return instance.get(); }

    /** Vital's own editor, for embedding. Ownership stays with the caller. */
    juce::AudioProcessorEditor* createEditor();

    juce::MemoryBlock stateTemplate() const;

private:
    /*  A tempo for the hosted Vital.

        Every real host gives a plugin a playhead. Without one Vital has no
        tempo to sync to, and anything tempo synced, an LFO rate above all,
        behaves differently from how it behaves in a DAW. The same patch
        measured here and played in Vital itself produced entirely different
        envelopes, one decaying away in a third of a second and the other
        holding for a full one, which made every level measurement a guess about
        a sound the user was never going to hear.
    */
    struct StaticPlayHead : juce::AudioPlayHead
    {
        juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override
        {
            juce::AudioPlayHead::PositionInfo info;
            info.setBpm (120.0);
            info.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
            info.setIsPlaying (false);
            info.setTimeInSamples (0);
            info.setTimeInSeconds (0.0);
            info.setPpqPosition (0.0);
            return info;
        }
    };

    StaticPlayHead playHead;
    juce::AudioPluginFormatManager formatManager;
    std::unique_ptr<juce::AudioPluginInstance> instance;
    juce::File pluginFile;

    juce::CriticalSection processLock;
    mutable juce::CriticalSection templateLock;
    juce::MemoryBlock cachedTemplate;

    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VitalHost)
};
