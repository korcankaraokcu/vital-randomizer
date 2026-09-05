#include "Audition.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <algorithm>
#include <vector>

namespace audition
{
    namespace
    {
        /*  Long enough to catch a slow attack.

            A short window is what made this wrong before: a pad that swells
            over a second and a half measured quiet, got boosted, and then
            arrived far too loud once it actually opened up. Vital's amp
            envelope reaches well past a second, so the note is held for most of
            two seconds before the release is measured.
        */
        constexpr double kRenderSeconds = 2.4;
        constexpr double kNoteOffAt = 1.7;

        // Below this the patch is effectively silent.
        constexpr float kSilenceRms = 0.0015f;
        // The note has to still be there after the attack. A patch that is all
        // transient and no body reads as a broken preset rather than a short
        // one, and there is no point handing it to the user.
        constexpr float kClickRatio = 0.05f;

        /*  Hand-made presets run to about 25 dB of crest factor at their 90th
            percentile. Past that there is no level that works: bring the body
            up to where a preset should sit and the transient clips, hold the
            transient down and the patch is inaudible. Rolling again beats
            shipping something that cannot be made to sit right.
        */
        constexpr float kMaxCrestDb = 26.0f;

        /*  A patch that sits noticeably in one channel sounds broken rather
            than wide, and it is the single most obvious defect a listener
            notices. Six dB is generous: real stereo movement swings either way
            over time and averages out near the middle.
        */
        constexpr float kMaxBalanceDb = 6.0f;
    }

    PitchRule pitchRuleFor (const std::string& style)
    {
        if (style == "Bass" || style == "Keys" || style == "Lead" || style == "Sequence")
            return { true, 0.45f, 0.45f };
        // A pad may be hazier about its pitch than a lead, but it still has to
        // be playing the note somebody pressed.
        if (style == "Pad")
            return { true, 0.35f, 0.60f };
        return { false, 0.0f, 0.0f };
    }

    float maxHeldRatioFor (const std::string& style)
    {
        // A bass is a struck note. Holding the key should not keep it going,
        // and the same goes for a drum.
        // A decay of 1.30 still measures about 0.11 here, so this leaves the
        // longest body the style allows some room and nothing beyond it.
        if (style == "Bass")       return 0.15f;
        if (style == "Percussion") return 0.10f;
        if (style == "Keys")       return 0.60f;   // short, but a key can be held
        // One means no limit at all, and it has to mean that rather than a
        // ceiling of one. A pad swells, so late in a held note it is louder
        // than it was early on, and a limit of exactly one rejected pads for
        // doing the one thing a pad is for.
        return kNoHeldLimit;
    }

    Brightness brightnessFor (const std::string& style)
    {
        if (style == "Bass")       return { 0.0f,   2200.0f };
        if (style == "Keys")       return { 250.0f, 3400.0f };
        // Pads run brighter than the rest and the library agrees, with a p90
        // just past five kilohertz.
        if (style == "Pad")        return { 150.0f, 4300.0f };
        if (style == "Lead")       return { 400.0f, 5800.0f };
        if (style == "Percussion") return { 0.0f,   9000.0f };
        // Sequence, SFX and Experiment are allowed to go wherever they like.
        return { 0.0f, 20000.0f };
    }

    void settle (juce::AudioProcessor& synth, double sampleRate, int blockSize, int note)
    {
        if (sampleRate <= 0.0 || blockSize <= 0)
            return;

        synth.reset();
        juce::AudioBuffer<float> block (2, blockSize);
        const auto total = (int) (0.30 * sampleRate);

        for (int pos = 0; pos < total; pos += blockSize)
        {
            const auto n = juce::jmin (blockSize, total - pos);
            block.setSize (2, n, false, false, true);
            block.clear();

            juce::MidiBuffer midi;
            if (pos == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 110), 0);
            synth.processBlock (block, midi);
        }

        juce::MidiBuffer off;
        off.addEvent (juce::MidiMessage::noteOff (1, note), 0);
        block.clear();
        synth.processBlock (block, off);
        synth.reset();
    }

    Measurement audition (juce::AudioProcessor& synth, double sampleRate,
                          int blockSize, int note, bool resetFirst)
    {
        Measurement m;
        if (sampleRate <= 0.0 || blockSize <= 0)
            return m;

        const auto totalSamples = (int) (kRenderSeconds * sampleRate);
        const auto noteOffSample = (int) (kNoteOffAt * sampleRate);
        const auto sustainStart = (int) (0.20 * sampleRate);
        // Two windows taken while the note is still held, to see whether it falls.
        const auto earlyFrom = (int) (0.10 * sampleRate), earlyTo = (int) (0.45 * sampleRate);
        const auto lateFrom = (int) (1.20 * sampleRate), lateTo = (int) (1.65 * sampleRate);
        double earlySq = 0.0, lateSq = 0.0;
        int earlyCount = 0, lateCount = 0;
        const auto tailStart = (int) (1.85 * sampleRate);

        // Reset first. Vital's LFOs and random sources free-run, so measuring
        // without resetting means the numbers depend on whatever was playing
        // before and the same patch measures differently each time.
        if (resetFirst)
            synth.reset();

        // Kept so brightness can be measured once the note has settled. A
        // bass that reads at 8 kHz is not a bass, whatever else is right about
        // it, and that is only visible from the audio.
        const auto centroidFrom = (int) (0.05 * sampleRate);
        const auto centroidTo = (int) (1.60 * sampleRate);
        std::vector<float> mono;
        mono.reserve ((size_t) juce::jmax (0, centroidTo - centroidFrom));

        juce::AudioBuffer<float> block (2, blockSize);

        double sumSq = 0.0, sustainSq = 0.0, tailSq = 0.0;
        int counted = 0, sustainCount = 0, tailCount = 0;
        std::vector<float> envelope;
        envelope.reserve ((size_t) (totalSamples / blockSize) + 2);

        // Short-term level windows, about 90 ms each, across the whole render.
        const auto windowBlocks = juce::jmax (1, (int) (0.09 * sampleRate) / blockSize);
        std::vector<float> windows;
        double channelSq[2] = { 0.0, 0.0 };
        double windowSq = 0.0;
        int windowSamples = 0, blocksInWindow = 0;

        for (int pos = 0; pos < totalSamples; pos += blockSize)
        {
            const auto n = juce::jmin (blockSize, totalSamples - pos);
            block.setSize (2, n, false, false, true);
            block.clear();

            juce::MidiBuffer midi;
            if (pos == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 110), 0);
            if (pos <= noteOffSample && noteOffSample < pos + n)
                midi.addEvent (juce::MidiMessage::noteOff (1, note), noteOffSample - pos);

            synth.processBlock (block, midi);

            double blockSq = 0.0;
            for (int ch = 0; ch < block.getNumChannels(); ++ch)
            {
                m.peak = juce::jmax (m.peak, block.getMagnitude (ch, 0, n));
                const auto* d = block.getReadPointer (ch);
                double chSq = 0.0;
                for (int i = 0; i < n; ++i)
                    chSq += (double) d[i] * d[i];
                blockSq += chSq;
                if (ch < 2)
                    channelSq[(size_t) ch] += chSq;

                const auto* held = block.getReadPointer (ch);
                for (int i = 0; i < n; ++i)
                {
                    const auto at = pos + i;
                    const auto sq = (double) held[i] * held[i];
                    if (at >= earlyFrom && at < earlyTo) { earlySq += sq; ++earlyCount; }
                    else if (at >= lateFrom && at < lateTo) { lateSq += sq; ++lateCount; }
                }
            }

            const auto samples = n * juce::jmax (1, block.getNumChannels());
            sumSq += blockSq;
            counted += samples;

            for (int i = 0; i < n; ++i)
            {
                const auto at = pos + i;
                if (at >= centroidFrom && at < centroidTo)
                {
                    float sum = 0.0f;
                    for (int ch = 0; ch < block.getNumChannels(); ++ch)
                        sum += block.getSample (ch, i);
                    mono.push_back (sum / juce::jmax (1, block.getNumChannels()));
                }
            }

            windowSq += blockSq;
            windowSamples += samples;
            if (++blocksInWindow >= windowBlocks)
            {
                if (windowSamples > 0)
                    windows.push_back ((float) std::sqrt (windowSq / windowSamples));
                windowSq = 0.0;
                windowSamples = 0;
                blocksInWindow = 0;
            }

            if (pos >= sustainStart && pos < noteOffSample)
            {
                sustainSq += blockSq;
                sustainCount += samples;
                envelope.push_back ((float) std::sqrt (blockSq / juce::jmax (1, samples)));
            }
            if (pos >= tailStart)
            {
                tailSq += blockSq;
                tailCount += samples;
            }
        }

        /*  The level is the 90th percentile of the short-term windows, not the
            average over the whole render.

            An average is dominated by whatever the patch is doing most of the
            time, which is the wrong thing to match: a slow pad spends most of
            its note quiet and ends up boosted into the red, while a plucked
            patch spends most of its note decaying and ends up far too hot.
            What a listener judges is how loud the patch gets, and a high
            percentile of the short-term level tracks that for both shapes.
        */
        if (! windows.empty())
        {
            auto sorted = windows;
            std::sort (sorted.begin(), sorted.end());
            m.rms = sorted[(size_t) ((sorted.size() - 1) * 9 / 10)];
        }
        else
        {
            m.rms = counted > 0 ? (float) std::sqrt (sumSq / counted) : 0.0f;
        }
        m.meanRms = counted > 0 ? (float) std::sqrt (sumSq / counted) : 0.0f;
        m.sustainRms = sustainCount > 0 ? (float) std::sqrt (sustainSq / sustainCount) : 0.0f;
        m.tailRms = tailCount > 0 ? (float) std::sqrt (tailSq / tailCount) : 0.0f;

        if (envelope.size() > 2)
        {
            double mean = 0.0;
            for (auto v : envelope)
                mean += v;
            mean /= (double) envelope.size();

            double var = 0.0;
            for (auto v : envelope)
                var += (v - mean) * (v - mean);
            var /= (double) envelope.size();
            m.motion = mean > 1.0e-9 ? (float) (std::sqrt (var) / mean) : 0.0f;
        }

        m.silent = m.rms < kSilenceRms;
        m.clipping = m.peak >= 0.999f;
        m.clickOnly = ! m.silent && m.rms > 1.0e-9f
                          && (m.sustainRms / m.rms) < kClickRatio;
        m.crestDb = (m.rms > 1.0e-9f && m.peak > 1.0e-9f)
                        ? 20.0f * std::log10 (m.peak / m.rms) : 0.0f;
        m.tooPeaky = ! m.silent && m.crestDb > kMaxCrestDb;

        const auto left = std::sqrt (channelSq[0] / juce::jmax (1, counted / 2));
        const auto right = std::sqrt (channelSq[1] / juce::jmax (1, counted / 2));
        m.balanceDb = (left > 1.0e-9 && right > 1.0e-9)
                          ? (float) (20.0 * std::log10 (left / right)) : 0.0f;
        m.lopsided = ! m.silent && std::abs (m.balanceDb) > kMaxBalanceDb;

        const auto early = earlyCount > 0 ? std::sqrt (earlySq / earlyCount) : 0.0;
        const auto late = lateCount > 0 ? std::sqrt (lateSq / lateCount) : 0.0;
        m.heldRatio = early > 1.0e-9 ? (float) (late / early) : 1.0f;

        if (mono.size() >= 2048)
        {
            int order = 10;
            while ((1 << (order + 1)) <= (int) mono.size() && order < 15)
                ++order;
            const auto fftSize = 1 << order;

            // The brightness window starts a little after the onset so the
            // attack transient does not dominate it.
            const auto skip = juce::jmin ((size_t) (0.20 * sampleRate),
                                          mono.size() > 2048 ? mono.size() - 2048 : (size_t) 0);
            juce::dsp::FFT fft (order);
            std::vector<float> data ((size_t) fftSize * 2, 0.0f);
            for (int i = 0; i < fftSize; ++i)
            {
                const auto at = skip + (size_t) i;
                const auto w = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi
                                                       * (float) i / (float) fftSize);
                data[(size_t) i] = (at < mono.size() ? mono[at] : 0.0f) * w;
            }
            fft.performFrequencyOnlyForwardTransform (data.data());

            double weighted = 0.0, total = 0.0;
            for (int i = 1; i < fftSize / 2; ++i)
            {
                const auto mag = (double) data[(size_t) i];
                weighted += mag * (i * sampleRate / fftSize);
                total += mag;
            }
            m.centroidHz = total > 1.0e-9 ? (float) (weighted / total) : 0.0f;
        }
        /*  Find the note by autocorrelation, over the part of the sound that
            actually has energy. A fixed window measures silence on a short patch
            and reports it as noise.
        */
        if (mono.size() > (size_t) (0.4 * sampleRate))
        {
            const auto smoothing = (size_t) juce::jmax (1, (int) (sampleRate / 43.0));
            size_t loudest = 0;
            double best = 0.0, running = 0.0;
            for (size_t i = 0; i < mono.size(); ++i)
            {
                running += std::abs (mono[i]);
                if (i >= smoothing)
                    running -= std::abs (mono[i - smoothing]);
                if (i >= smoothing && running > best)
                {
                    best = running;
                    loudest = i - smoothing;
                }
            }

            const auto span = (size_t) (0.35 * sampleRate);
            const auto from = juce::jmin (loudest, mono.size() > span ? mono.size() - span : 0u);
            const auto count = juce::jmin (span, mono.size() - from);

            if (count > 2048)
            {
                double mean = 0.0;
                for (size_t i = 0; i < count; ++i)
                    mean += mono[from + i];
                mean /= (double) count;

                double zero = 0.0;
                for (size_t i = 0; i < count; ++i)
                {
                    const auto x = mono[from + i] - mean;
                    zero += x * x;
                }

                if (zero > 1.0e-12)
                {
                    const auto minLag = (size_t) (sampleRate / 2000.0);
                    const auto maxLag = juce::jmin ((size_t) (sampleRate / 40.0), count / 2);
                    double bestScore = 0.0;
                    size_t bestLag = 0;

                    for (size_t lag = minLag; lag < maxLag; ++lag)
                    {
                        double sum = 0.0;
                        for (size_t i = 0; i + lag < count; ++i)
                            sum += (mono[from + i] - mean) * (mono[from + i + lag] - mean);
                        const auto score = sum / zero;
                        if (score > bestScore)
                        {
                            bestScore = score;
                            bestLag = lag;
                        }
                    }

                    if (bestLag > 0)
                    {
                        m.pitchSalience = (float) juce::jlimit (0.0, 1.0, bestScore);
                        m.pitchHz = (float) (sampleRate / (double) bestLag);

                        // Distance from the nearest octave of the note played.
                        // An octave is still the same note; a fifth is not.
                        const auto expected = 440.0 * std::pow (2.0, (note - 69) / 12.0);
                        const auto semis = 12.0 * std::log2 (m.pitchHz / expected);
                        const auto octaves = std::round (semis / 12.0);
                        m.pitchErrorSemitones = (float) std::abs (semis - octaves * 12.0);
                    }
                }
            }
        }

        return m;
    }
}
