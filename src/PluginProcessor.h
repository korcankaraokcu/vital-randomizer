#pragma once

#include <atomic>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>

#include "Audition.h"
#include "CandidateStore.h"
#include "Generator.h"
#include "Loudness.h"
#include "VitalHost.h"

/*
    The DAW-facing plugin.

    It hosts Vital, passes the track's MIDI straight through so the keyboard
    still plays, and pushes generated patches into it. Everything slow happens
    on a worker thread: generating a candidate, auditioning it, and the state
    load itself.
*/
class VitalRandomizerProcessor : public juce::AudioProcessor,
                                 public juce::ChangeBroadcaster,
                                 private juce::Thread
{
public:
    enum class Job { none, rollNew, vary, recall, reloadCurrent, prefetch };

    struct Status
    {
        juce::String message;
        float progress = -1.0f;      // negative means indeterminate or idle
        bool working = false;
        bool vitalReady = false;
    };

    VitalRandomizerProcessor();
    ~VitalRandomizerProcessor() override;

    // --- AudioProcessor -----------------------------------------------------
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Vital Randomizer"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // --- randomizer ---------------------------------------------------------
    void rollNew();
    void vary();
    void stepHistory (int delta);
    void reloadCurrent();
    void starCurrent();
    void unstar (size_t index);
    void recallKeeper (size_t index);

    void setStyle (const juce::String& style);
    juce::String style() const { return currentStyle; }

    void setSlider (const juce::String& axis, float value);
    float slider (const juce::String& axis) const;

    void setLocked (schema::Section section, bool locked);
    bool isLocked (schema::Section section) const;

    void setVaryAmount (float amount);
    float varyAmount() const { return varyDepth.load(); }

    void setHistoryLimit (size_t limit);

    /** Point the randomizer at a different Vital binary. */
    void locateVital (const juce::File& vst3);

    Status status() const;
    std::vector<std::string> availableStyles() const;
    store::CandidateStore& candidates() { return candidateStore; }
    const store::CandidateStore& candidates() const { return candidateStore; }
    VitalHost& vital() { return host; }

    juce::String lastMacroSummary() const;
    juce::String lastError() const;
    juce::String lastAuditionSummary() const;

    /** Write the patch currently loaded in Vital to a .vital file. */
    bool exportCurrent (const juce::File& destination);

private:
    void run() override;
    void enqueue (Job job);
    void performRoll (Job job);
    /** Generate and screen until something passes. The slow half of a roll,
        and the half that can be done before the user asks for it. */
    gen::Result rollScreened (Job job);
    /** What a prefetched candidate has to still match to be usable. */
    juce::String requestSignature() const;
    void dropPrefetch();
    void setStatus (const juce::String& message, float progress, bool working);
    void applyResult (gen::Result& result, bool pushToHistory);
    gen::Request buildRequest() const;
    /** Render a candidate offline, match its level, and say whether it is
        worth handing to the user. */
    bool screen (gen::Result& result, audition::Measurement& measured);
    void applyStyleDefaults (const juce::String& style);

    VitalHost host;
    /*  A second Vital, never heard. Candidates are loaded into it and rendered
        offline so a silent or clipping roll can be replaced before it reaches
        the user, and so its level can be matched to where hand-made presets
        sit. The alternative, judging loudness from the live output while the
        user plays, only works if they happen to be holding a note and cannot
        reject a bad patch at all.
    */
    VitalHost preview;
    std::unique_ptr<gen::Generator> generator;
    store::CandidateStore candidateStore;

    juce::WaitableEvent wakeUp;
    std::atomic<bool> quitting { false };
    juce::CriticalSection jobLock;
    Job pendingJob = Job::none;
    size_t pendingKeeper = 0;

    /*  The next candidate, generated while the user is still listening to this
        one.

        Screening is most of what a roll costs: every candidate is rendered,
        measured, and often rendered again to confirm its level, and a batch of
        48 takes about 69 rolls to fill. None of that has to happen after the
        button is pressed, because the recipe for the next roll is known as soon
        as the last one lands.

        It is only usable while the settings it was built from still hold, so it
        carries a signature and is dropped whenever the style, a slider or a lock
        moves under it.
    */
    juce::CriticalSection prefetchLock;
    gen::Result prefetched;
    juce::String prefetchedFor;

    juce::String currentStyle { "Bass" };
    juce::CriticalSection settingsLock;
    std::map<juce::String, float> sliders;
    std::set<schema::Section> locks;
    std::atomic<float> varyDepth { 0.25f };

    juce::File vitalPath;

    mutable juce::CriticalSection statusLock;
    Status currentStatus;
    juce::String macroSummary;
    juce::String errorText;

    nlohmann::json livePreset;
    juce::CriticalSection livePresetLock;
    juce::String auditionSummary;

    void loadVitalNow();

    JUCE_DECLARE_WEAK_REFERENCEABLE (VitalRandomizerProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VitalRandomizerProcessor)
};
