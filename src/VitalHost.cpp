#include "VitalHost.h"

#include "VitalState.h"

VitalHost::VitalHost()
{
    formatManager.addFormat (new juce::VST3PluginFormat());
}

VitalHost::~VitalHost()
{
    release();
    const juce::ScopedLock sl (processLock);
    instance.reset();
}

juce::Array<juce::File> VitalHost::searchPaths()
{
    juce::Array<juce::File> paths;

   #if JUCE_WINDOWS
    paths.add (juce::File ("C:\\Program Files\\Common Files\\VST3\\Vital.vst3"));
    paths.add (juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory)
                   .getChildFile ("Common Files").getChildFile ("VST3")
                   .getChildFile ("Vital.vst3"));
   #elif JUCE_MAC
    paths.add (juce::File ("/Library/Audio/Plug-Ins/VST3/Vital.vst3"));
    paths.add (juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                   .getChildFile ("Library/Audio/Plug-Ins/VST3/Vital.vst3"));
   #else
    paths.add (juce::File ("/usr/lib/vst3/Vital.vst3"));
    paths.add (juce::File ("/usr/local/lib/vst3/Vital.vst3"));
    paths.add (juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                   .getChildFile (".vst3/Vital.vst3"));
   #endif

    return paths;
}

juce::File VitalHost::findInstalled()
{
    for (const auto& p : searchPaths())
        if (p.exists())
            return p;
    return {};
}

bool VitalHost::load (const juce::File& vst3, double sampleRate, int blockSize,
                      juce::String& error)
{
    if (! vst3.exists())
    {
        error = "Vital was not found at " + vst3.getFullPathName();
        return false;
    }

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, vst3.getFullPathName());

    if (found.isEmpty())
    {
        error = "No VST3 plugin could be read from " + vst3.getFullPathName();
        return false;
    }

    juce::String createError;
    auto created = formatManager.createPluginInstance (*found[0], sampleRate, blockSize,
                                                       createError);
    if (created == nullptr)
    {
        error = "Vital failed to load: " + createError;
        return false;
    }

    created->enableAllBuses();
    created->setPlayConfigDetails (0, 2, sampleRate, blockSize);
    created->setPlayHead (&playHead);
    created->prepareToPlay (sampleRate, blockSize);

    {
        const juce::ScopedLock sl (processLock);
        instance = std::move (created);
        pluginFile = vst3;
        currentSampleRate = sampleRate;
        currentBlockSize = blockSize;
    }

    // The first state change into a fresh instance costs several seconds while
    // Vital builds its wavetables. Every load after it is a fifth of a second,
    // so the cost is paid here at construction rather than showing up as a
    // mystery stall on the user's first roll.
    juce::MemoryBlock warm;
    instance->getStateInformation (warm);
    if (vitalstate::looksValid (warm))
    {
        instance->setStateInformation (warm.getData(), (int) warm.getSize());
        const juce::ScopedLock sl (templateLock);
        cachedTemplate = warm;
    }
    else
    {
        error = "Vital loaded but its state chunk was not in the expected format";
        return false;
    }

    return true;
}

void VitalHost::prepare (double sampleRate, int blockSize)
{
    currentSampleRate = sampleRate;
    currentBlockSize = blockSize;

    const juce::ScopedLock sl (processLock);
    if (instance != nullptr)
    {
        instance->setPlayConfigDetails (0, 2, sampleRate, blockSize);
        instance->prepareToPlay (sampleRate, blockSize);
    }
}

void VitalHost::release()
{
    const juce::ScopedLock sl (processLock);
    if (instance != nullptr)
        instance->releaseResources();
}

bool VitalHost::process (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    // A try-lock, not a lock. A patch load holds this for a couple of hundred
    // milliseconds and blocking the audio thread on it would stall the whole
    // DAW rather than just going quiet for a moment.
    const juce::ScopedTryLock sl (processLock);
    if (! sl.isLocked() || instance == nullptr)
        return false;

    instance->processBlock (audio, midi);
    return true;
}

juce::MemoryBlock VitalHost::stateTemplate() const
{
    const juce::ScopedLock sl (templateLock);
    return cachedTemplate;
}

bool VitalHost::applyPreset (const nlohmann::json& preset)
{
    if (preset.is_null() || ! preset.contains ("settings"))
        return false;

    const auto tmpl = stateTemplate();
    if (tmpl.getSize() == 0)
        return false;

    const auto blob = vitalstate::write (tmpl, preset);
    if (blob.getSize() == 0)
        return false;

    const juce::ScopedLock sl (processLock);
    if (instance == nullptr)
        return false;

    instance->setStateInformation (blob.getData(), (int) blob.getSize());
    return true;
}

bool VitalHost::applyPresetVerified (const nlohmann::json& preset)
{
    if (! applyPreset (preset))
        return false;

    // The comment field is ours to use as a marker: it round-trips through
    // Vital's state and nothing else writes it.
    const auto expected = preset.value ("comments", std::string {});
    if (expected.empty())
        return true;

    const auto back = currentPreset();
    if (back.is_null() || ! back.contains ("settings"))
        return false;

    return back.value ("comments", std::string {}) == expected;
}

nlohmann::json VitalHost::currentPreset() const
{
    juce::MemoryBlock state;
    {
        const juce::ScopedLock sl (const_cast<juce::CriticalSection&> (processLock));
        if (instance == nullptr)
            return {};
        instance->getStateInformation (state);
    }
    return vitalstate::read (state);
}

juce::AudioProcessorEditor* VitalHost::createEditor()
{
    const juce::ScopedLock sl (processLock);
    if (instance == nullptr || ! instance->hasEditor())
        return nullptr;
    return instance->createEditorIfNeeded();
}
