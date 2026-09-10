#include "PluginProcessor.h"

#include "PluginEditor.h"
#include "Axes.h"

#include <algorithm>

VitalRandomizerProcessor::VitalRandomizerProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      Thread ("VitalRandomizer worker")
{
    for (const auto& axis : axes::all())
        sliders[juce::String (axis.key)] = 0.5f;
    // Not an axis. The four axes shape the tone, this one decides how much of
    // the synth the patch uses at all.
    sliders["complexity"] = 0.5f;

    applyStyleDefaults (currentStyle);
    vitalPath = VitalHost::findInstalled();

    /*  Vital is loaded a moment later, not here.

        A host scans by constructing the plugin, and this constructor used to
        spin up two Vital instances before returning. Loading a VST3 from inside
        another VST3's constructor, while the host is part way through its own
        scan, is enough to hang the scan outright, which is what froze Reaper.

        Deferring to the message queue means the constructor returns at once and
        the scan finishes. The load still happens on the message thread, which is
        what it needs: instantiating a VST3 on Windows requires COM initialised
        on the calling thread, and a plain worker thread does not have it.
    */
    setStatus ("Starting up", -1.0f, true);
    startThread (juce::Thread::Priority::normal);

    juce::WeakReference<VitalRandomizerProcessor> self (this);
    juce::MessageManager::callAsync ([self]
    {
        if (auto* p = self.get())
            p->loadVitalNow();
    });
}

VitalRandomizerProcessor::~VitalRandomizerProcessor()
{
    quitting = true;
    wakeUp.signal();
    stopThread (8000);
}

void VitalRandomizerProcessor::loadVitalNow()
{
    if (host.isLoaded())
        return;

    if (vitalPath == juce::File() || ! vitalPath.exists())
    {
        errorText = "Vital was not found. Use Locate Vital to point at Vital.vst3.";
        setStatus ("Vital not found", -1.0f, false);
        sendChangeMessage();
        return;
    }

    juce::String error;
    if (! host.load (vitalPath, 44100.0, 512, error))
    {
        errorText = error;
        setStatus ("Could not load Vital", -1.0f, false);
        sendChangeMessage();
        return;
    }

    // The offline one is optional. Without it rolls still work, they just stop
    // being screened and level matched.
    juce::String previewError;
    preview.load (vitalPath, 44100.0, 512, previewError);

    /*  Every patch is built on top of Vital's own init state, so the generator
        needs it before it can do anything. Reading it from the instance rather
        than shipping a copy means it always matches the Vital installed here.
    */
    generator = std::make_unique<gen::Generator>();
    auto init = host.currentPreset();
    if (! init.is_null() && init.contains ("settings"))
    {
        init.erase ("tuning");
        generator->setInitPreset (std::move (init));
    }

    const auto names = gen::Generator::styles();
    if (! names.empty()
        && std::find (names.begin(), names.end(), currentStyle.toStdString()) == names.end())
        setStyle (juce::String (names.front()));

    setStatus (generator->isReady() ? "Ready" : "Could not read Vital's init patch",
               -1.0f, false);
    sendChangeMessage();
}

// ---------------------------------------------------------------- audio ----

void VitalRandomizerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    if (host.isLoaded())
        host.prepare (sampleRate, samplesPerBlock);
}

void VitalRandomizerProcessor::releaseResources()
{
    host.release();
}

bool VitalRandomizerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void VitalRandomizerProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                             juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // MIDI goes straight through to Vital untouched, so the keyboard plays the
    // hosted synth exactly as it would if it were on the track directly.
    if (! host.process (buffer, midi))
    {
        // A patch is loading. Going quiet for a moment beats stalling the DAW.
        buffer.clear();
        return;
    }

    const auto numSamples = buffer.getNumSamples();

    /*  Levels are matched into the patch itself before it ever gets here, by
        rendering it through the offline instance. What is left is a safety net
        for the gap between measuring one note and the user playing another: a
        patch with a deep LFO on its level can swing well past the peak it
        measured at, depending on where the LFO happens to be.

        It saturates rather than clipping. A hard clamp on a synth that
        overshoots by a few dB sounds like a fault, while a soft knee sounds
        like a synth being driven, and this only ever engages on the overshoot.
    */
    constexpr float knee = 0.80f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        auto* d = buffer.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
        {
            const auto x = d[i];
            const auto magnitude = std::abs (x);
            if (magnitude > knee)
            {
                const auto over = (magnitude - knee) / (1.0f - knee);
                const auto shaped = knee + (1.0f - knee) * std::tanh (over);
                d[i] = std::copysign (shaped, x);
            }
        }
    }
}

// --------------------------------------------------------------- worker ----

void VitalRandomizerProcessor::enqueue (Job job)
{
    {
        const juce::ScopedLock sl (jobLock);
        pendingJob = job;
    }
    wakeUp.signal();
}

void VitalRandomizerProcessor::run()
{
    while (! threadShouldExit() && ! quitting.load())
    {
        wakeUp.wait (250);
        if (threadShouldExit() || quitting.load())
            break;

        Job job = Job::none;
        {
            const juce::ScopedLock sl (jobLock);
            job = pendingJob;
            pendingJob = Job::none;
        }

        switch (job)
        {
            case Job::rollNew:
            case Job::vary:
            case Job::recall:
            case Job::reloadCurrent:   performRoll (job); break;
            case Job::prefetch:
            {
                /*  Built with the settings as they stand now. If they move
                    before the user rolls, the signature stops matching and it
                    is thrown away rather than handed over as a stale patch.
                */
                const auto signature = requestSignature();
                auto ready = rollScreened (Job::rollNew);
                if (ready.ok)
                {
                    const juce::ScopedLock sl (prefetchLock);
                    prefetched = std::move (ready);
                    prefetchedFor = signature;
                }
                break;
            }
            default: break;
        }
    }
}

gen::Request VitalRandomizerProcessor::buildRequest() const
{
    gen::Request r;
    const juce::ScopedLock sl (settingsLock);
    r.style = currentStyle.toStdString();
    for (const auto& s : sliders)
        r.sliders[s.first.toStdString()] = s.second;
    r.locks = locks;
    return r;
}

void VitalRandomizerProcessor::performRoll (Job job)
{
    if (generator == nullptr || ! host.isLoaded())
        return;

    setStatus (job == Job::vary ? "Varying" : "Rolling", -1.0f, true);
    sendChangeMessage();

    gen::Result result;

    if (job == Job::recall)
    {
        size_t index = 0;
        {
            const juce::ScopedLock sl (jobLock);
            index = pendingKeeper;
        }
        const auto& kept = candidateStore.keepers();
        if (index >= kept.size())
            return;

        // A keeper carries its real patch, because once it has been hand edited
        // in Vital's own GUI there is no recipe that reproduces it.
        result.preset = kept[index].preset;
        result.ok = ! result.preset.is_null();
    }
    else if (job == Job::reloadCurrent)
    {
        const auto* recipe = candidateStore.current();
        if (recipe == nullptr)
            return;
        result = generator->roll (recipe->toRequest());
    }
    else
    {
        /*  A candidate prepared while the user was listening to the last one,
            if the settings have not moved since it was built.
        */
        if (job == Job::rollNew)
        {
            const juce::ScopedLock sl (prefetchLock);
            if (prefetched.ok && prefetchedFor == requestSignature())
            {
                result = std::move (prefetched);
                prefetched = {};
                prefetchedFor.clear();
            }
        }

        if (! result.ok)
            result = rollScreened (job);
    }

    if (! result.ok)
    {
        errorText = juce::String (result.error);
        setStatus ("Roll failed", -1.0f, false);
        sendChangeMessage();
        return;
    }

    applyResult (result, job == Job::rollNew || job == Job::vary);

    // Start on the next one straight away, so the wait lands while the user is
    // playing this one rather than after they ask for another.
    if (job == Job::rollNew || job == Job::vary)
    {
        const juce::ScopedLock sl (jobLock);
        if (pendingJob == Job::none)
            pendingJob = Job::prefetch;
        wakeUp.signal();
    }
}

gen::Result VitalRandomizerProcessor::rollScreened (Job job)
{
    gen::Result result;

    /*  Roll until something worth hearing comes out.

        Screening is the difference between a randomizer and a slot machine.
        A generated patch can be silent, all click and no body, or 20 dB
        hotter than the last one, and none of that is worth spending the
        user's attention on when the offline instance can find it in a
        quarter of a second and roll again.
    */
    // Screening got stricter as the style rules got tighter, so more of the
    // rolls get thrown away. A rejected candidate costs a fraction of a
    // second on a worker thread, and a bad one costs the user's attention.
    {
        constexpr int maxAttempts = 8;
        audition::Measurement measured;
        gen::Result best;
        float bestScore = -1.0f;

        for (int attempt = 0; attempt < maxAttempts; ++attempt)
        {
            auto request = buildRequest();
            if (job == Job::vary)
            {
                const juce::ScopedLock sl (livePresetLock);
                if (! livePreset.is_null())
                {
                    request.base = &livePreset;
                    request.amount = varyDepth.load();
                    // VARY keeps the patch's structure and only respices the
                    // sections most responsible for its character, so the
                    // result is recognisably the same sound, not a new one.
                    request.varySections = { schema::Section::filter,
                                             schema::Section::env };
                }
                result = generator->roll (request);
            }
            else
            {
                request.amount = 1.0f;
                result = generator->roll (request);
            }

            if (! result.ok)
                continue;

            if (screen (result, measured))
                break;

            // Keep the least bad one seen. Rolling five times and giving up
            // with nothing would be worse than handing over a flawed patch.
            const auto score = measured.usable() ? measured.sustainRms : 0.0f;
            if (score > bestScore)
            {
                bestScore = score;
                best = result;
            }
            if (attempt == maxAttempts - 1 && bestScore > 0.0f)
                result = best;
        }
    }

    return result;
}

juce::String VitalRandomizerProcessor::requestSignature() const
{
    const juce::ScopedLock sl (settingsLock);
    juce::String s = currentStyle;
    for (const auto& slider : sliders)
        s << "|" << slider.first << "=" << juce::String (slider.second, 3);
    for (const auto& lock : locks)
        s << "|L" << (int) lock;
    return s;
}

void VitalRandomizerProcessor::dropPrefetch()
{
    const juce::ScopedLock sl (prefetchLock);
    prefetched = {};
    prefetchedFor.clear();
}

bool VitalRandomizerProcessor::screen (gen::Result& result, audition::Measurement& measured)
{
    measured = {};

    if (! preview.isLoaded() || ! result.preset.contains ("settings"))
        return true;    // nothing to screen with, so take what we were given

    const auto sr = getSampleRate() > 0 ? getSampleRate() : 44100.0;

    /*  Correct, then re-measure, then correct again if needed.

        One pass is not enough. The volume calibration was swept on a clean
        patch, and a patch with heavy distortion or compression in its chain
        does not respond to master volume the same way, so the first correction
        can land several dB off. Measuring what actually came out and going
        again is the only way to be sure the patch a user hears is the level it
        was supposed to be.
    */
    float totalDb = 0.0f;
    for (int pass = 0; pass < 3; ++pass)
    {
        // The first load is verified. If Vital rejected the patch there is
        // nothing to measure and rolling again is the right answer.
        const auto loaded = pass == 0 ? preview.applyPresetVerified (result.preset)
                                      : preview.applyPreset (result.preset);
        if (! loaded)
            return false;

        // Vital's first render after taking a new state is not the settled
        // patch, so a short one is played and discarded before measuring.
        audition::settle (*preview.processor(), sr, 512);

        measured = audition::audition (*preview.processor(), sr, 512);
        if (! measured.usable())
            return false;

        // A patch can be perfectly healthy and still be the wrong instrument.
        const auto styleName = currentStyle.toStdString();
        /*  Balance before brightness. A patch is allowed high frequency detail
            as long as it stays quiet: what makes a bass a bass is how much of it
            is down low, not where its mean lands.
        */
        const auto balance = audition::balanceFor (styleName);
        if (measured.lowRatio > 0.0f && measured.lowRatio < balance.minLowRatio)
            return false;
        if (measured.highSpike > balance.maxHighSpike)
            return false;

        const auto bounds = audition::brightnessFor (styleName, slider ("complexity"));
        if (measured.centroidHz > 0.0f
            && (measured.centroidHz < bounds.low || measured.centroidHz > bounds.high))
            return false;
        if (measured.heldRatio > audition::maxHeldRatioFor (styleName))
            return false;

        // Give back the note that was pressed, or roll again.
        /*  A stepping patch is read a step at a time. Measured whole, a clean
            sequence looks like noise, because the window spans several notes.
        */
        const auto pitch = audition::pitchRuleFor (styleName);
        const auto stepped = audition::isSteppedStyle (styleName) && measured.steps > 0;
        const auto salience = stepped ? measured.stepSalience : measured.pitchSalience;
        const auto pitchError = stepped ? measured.stepOffGridSemitones
                              : pitch.octavesOnly ? measured.pitchErrorSemitones
                                                  : measured.pitchOffGridSemitones;
        if (pitch.required
            && (salience < pitch.minSalience
                || pitchError > pitch.maxErrorSemitones))
            return false;

        auto wanted = loudness::correctionDb (measured.rms, measured.peak);

        if (wanted == 0.0f)
        {
            /*  Confirm with a second render before believing it.

                Some patches do not settle. A delay feeding back, a filter close
                to self oscillation, a free running LFO landing somewhere else:
                the level climbs each time the note is played. Measuring once
                catches those at their quietest, which is how a lead that runs
                twelve dB over the rest of a batch was accepted as being on
                target. Two readings that agree means the number is real, and
                the louder of the two is the one worth matching.
            */
            const auto again = audition::audition (*preview.processor(), sr, 512);
            if (! again.usable())
                return false;

            /*  The mean of the two, not the louder. Taking the louder
                of two noisy readings finds the top of the scatter rather than
                the level the patch sits at, and moves the answer up every time
                it is asked. The peak ceiling still catches a real climb.
            */
            auto third = audition::audition (*preview.processor(), sr, 512);
            if (! third.usable())
                third = again;

            measured.rms = (measured.rms + again.rms + third.rms) / 3.0f;
            measured.peak = (measured.peak + again.peak + third.peak) / 3.0f;
            wanted = loudness::correctionDb (measured.rms, measured.peak);

            if (wanted == 0.0f)
            {
                auditionSummary = juce::String (measured.rms, 3) + " rms";
                if (std::abs (totalDb) >= 1.0f)
                    auditionSummary << ", " << (totalDb > 0 ? "+" : "")
                                    << juce::String (juce::roundToInt (totalDb)) << " dB";
                return true;
            }
        }

        // Match the level by moving the patch's own master volume rather than
        // trimming our output, so the level travels with the preset when it is
        // exported.
        const auto applied = loudness::normalise (result.preset["settings"],
                                                  measured.rms, measured.peak);

        // Running out of volume range means the patch is quiet at the source,
        // not quiet at the output, and turning it up will not fix that.
        if (std::abs (wanted - applied) > 6.0f)
            return false;
        totalDb += applied;
    }
    return false;
}

void VitalRandomizerProcessor::applyResult (gen::Result& result, bool pushToHistory)
{
    if (pushToHistory)
    {
        auto request = buildRequest();
        request.seed = result.seed;
        candidateStore.push (store::Recipe::fromRequest (request, result.seed));
    }

    if (! host.applyPreset (result.preset))
    {
        errorText = "Vital rejected the generated patch";
        setStatus ("Load failed", -1.0f, false);
        sendChangeMessage();
        return;
    }

    {
        const juce::ScopedLock sl (livePresetLock);
        livePreset = result.preset;
    }

    juce::StringArray names;
    for (int i = 0; i < gen::kMacros; ++i)
        if (! result.macroNames[(size_t) i].empty())
            names.add (juce::String (result.macroNames[(size_t) i]));
    macroSummary = names.joinIntoString (" / ");

    errorText.clear();

    setStatus (juce::String (result.routings) + " routings", -1.0f, false);
    sendChangeMessage();
}

// ---------------------------------------------------------------- public ---

void VitalRandomizerProcessor::rollNew()          { enqueue (Job::rollNew); }
void VitalRandomizerProcessor::vary()             { enqueue (Job::vary); }

void VitalRandomizerProcessor::stepHistory (int delta)
{
    if (candidateStore.moveCursor (delta))
        enqueue (Job::reloadCurrent);
}

void VitalRandomizerProcessor::reloadCurrent()
{
    enqueue (Job::reloadCurrent);
}

void VitalRandomizerProcessor::starCurrent()
{
    store::Keeper keeper;
    if (const auto* recipe = candidateStore.current())
    {
        keeper.recipe = *recipe;
        keeper.label = recipe->style + " " + juce::String (candidateStore.cursor() + 1).toStdString();
    }

    // Read the patch back out of Vital rather than using what was generated, so
    // anything the user tweaked by hand in Vital's own GUI is what gets kept.
    auto live = host.currentPreset();
    if (! live.is_null() && live.contains ("settings"))
    {
        const juce::ScopedLock sl (livePresetLock);
        keeper.edited = livePreset.is_null() ? false : (live["settings"] != livePreset["settings"]);
        keeper.preset = std::move (live);
    }
    else
    {
        const juce::ScopedLock sl (livePresetLock);
        keeper.preset = livePreset;
    }

    if (! keeper.preset.is_null())
    {
        candidateStore.star (keeper);
        sendChangeMessage();
    }
}

void VitalRandomizerProcessor::unstar (size_t index)
{
    candidateStore.unstar (index);
    sendChangeMessage();
}

void VitalRandomizerProcessor::recallKeeper (size_t index)
{
    {
        const juce::ScopedLock sl (jobLock);
        pendingKeeper = index;
    }
    enqueue (Job::recall);
}

void VitalRandomizerProcessor::applyStyleDefaults (const juce::String& style)
{
    const auto d = archetype::defaultSlidersFor (style.toStdString());
    const juce::ScopedLock sl (settingsLock);
    sliders["bright"]     = d.bright;
    sliders["move"]       = d.move;
    sliders["dirt"]       = d.dirt;
    sliders["space"]      = d.space;
    sliders["complexity"] = d.complexity;
}

void VitalRandomizerProcessor::setStyle (const juce::String& s)
{
    bool changed = false;
    {
        const juce::ScopedLock sl (settingsLock);
        changed = (currentStyle != s);
        currentStyle = s;
    }
    if (changed)
    {
        applyStyleDefaults (s);
        dropPrefetch();
        sendChangeMessage();
    }
}

void VitalRandomizerProcessor::setSlider (const juce::String& axis, float value)
{
    {
        const juce::ScopedLock sl (settingsLock);
        sliders[axis] = juce::jlimit (0.0f, 1.0f, value);
    }
    dropPrefetch();
}

float VitalRandomizerProcessor::slider (const juce::String& axis) const
{
    const juce::ScopedLock sl (settingsLock);
    const auto it = sliders.find (axis);
    return it == sliders.end() ? 0.5f : it->second;
}

void VitalRandomizerProcessor::setLocked (schema::Section section, bool locked)
{
    dropPrefetch();
    const juce::ScopedLock sl (settingsLock);
    if (locked)
        locks.insert (section);
    else
        locks.erase (section);
}

bool VitalRandomizerProcessor::isLocked (schema::Section section) const
{
    const juce::ScopedLock sl (settingsLock);
    return locks.count (section) > 0;
}

void VitalRandomizerProcessor::setVaryAmount (float amount)
{
    varyDepth = juce::jlimit (0.02f, 1.0f, amount);
}

void VitalRandomizerProcessor::setHistoryLimit (size_t limit)
{
    candidateStore.setHistoryLimit (limit);
}

void VitalRandomizerProcessor::locateVital (const juce::File& vst3)
{
    // Called from the editor, so already on the message thread, which is where
    // a VST3 instance has to be created.
    vitalPath = vst3;
    errorText.clear();
    loadVitalNow();
}

void VitalRandomizerProcessor::setStatus (const juce::String& message, float progress,
                                          bool working)
{
    const juce::ScopedLock sl (statusLock);
    currentStatus.message = message;
    currentStatus.progress = progress;
    currentStatus.working = working;
    currentStatus.vitalReady = host.isLoaded();
}

VitalRandomizerProcessor::Status VitalRandomizerProcessor::status() const
{
    const juce::ScopedLock sl (statusLock);
    auto s = currentStatus;
    s.vitalReady = host.isLoaded();
    return s;
}

std::vector<std::string> VitalRandomizerProcessor::availableStyles() const
{
    return gen::Generator::styles();
}

juce::String VitalRandomizerProcessor::lastMacroSummary() const     { return macroSummary; }
juce::String VitalRandomizerProcessor::lastError() const            { return errorText; }
juce::String VitalRandomizerProcessor::lastAuditionSummary() const  { return auditionSummary; }

bool VitalRandomizerProcessor::exportCurrent (const juce::File& destination)
{
    auto preset = host.currentPreset();
    if (preset.is_null())
        return false;
    // The level is already in the patch's own master volume, so an exported
    // file sounds exactly like what was playing.
    preset.erase ("tuning");     // belongs to the plugin state, not to a .vital file
    return destination.replaceWithText (juce::String (preset.dump()));
}

// ----------------------------------------------------------------- state ---

void VitalRandomizerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    nlohmann::json j;
    j["version"] = 1;
    j["style"] = currentStyle.toStdString();
    j["vary"] = varyDepth.load();
    j["vital"] = vitalPath.getFullPathName().toStdString();

    {
        const juce::ScopedLock sl (settingsLock);
        for (const auto& s : sliders)
            j["sliders"][s.first.toStdString()] = s.second;
        for (const auto& l : locks)
            j["locks"].push_back (schema::sectionName (l));
    }

    j["candidates"] = candidateStore.toJson();

    // The patch itself rides along, so reopening the project restores exactly
    // what was playing rather than a fresh roll.
    auto live = host.currentPreset();
    if (! live.is_null())
        j["preset"] = std::move (live);

    const auto text = j.dump();
    destData.setSize (0);
    destData.append (text.data(), text.size());
}

void VitalRandomizerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    auto j = nlohmann::json::parse (static_cast<const char*> (data),
                                    static_cast<const char*> (data) + sizeInBytes,
                                    nullptr, false);
    if (j.is_discarded() || ! j.is_object())
        return;

    {
        const juce::ScopedLock sl (settingsLock);
        currentStyle = juce::String (j.value ("style", currentStyle.toStdString()));
        if (j.contains ("sliders") && j["sliders"].is_object())
            for (auto it = j["sliders"].begin(); it != j["sliders"].end(); ++it)
                sliders[juce::String (it.key())] = it.value().get<float>();
        locks.clear();
        if (j.contains ("locks") && j["locks"].is_array())
            for (const auto& l : j["locks"])
                locks.insert (schema::sectionFromName (l.get<std::string>()));
    }

    varyDepth = j.value ("vary", 0.25f);

    if (j.contains ("vital"))
    {
        const juce::File v (juce::String (j["vital"].get<std::string>()));
        if (v.exists())
            vitalPath = v;
    }

    if (j.contains ("candidates"))
        candidateStore.fromJson (j["candidates"]);

    if (j.contains ("preset"))
    {
        const juce::ScopedLock sl (livePresetLock);
        livePreset = j["preset"];
    }
}

juce::AudioProcessorEditor* VitalRandomizerProcessor::createEditor()
{
    return new VitalRandomizerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VitalRandomizerProcessor();
}
