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

    bool isSteppedStyle (const std::string& style)
    {
        return style == "Sequence";
    }

    Measurement auditionAveraged (juce::AudioProcessor& synth, double sampleRate,
                                  int blockSize, int times, int note)
    {
        times = juce::jmax (1, times);

        Measurement sum;
        int votes[5] = { 0, 0, 0, 0, 0 };   // silent, clipping, clickOnly, peaky, lopsided
        int counted = 0;
        float firstRms = 0.0f, lastRms = 0.0f, loudestRms = 0.0f, loudestPeak = 0.0f;

        for (int i = 0; i < times; ++i)
        {
            const auto m = audition (synth, sampleRate, blockSize, note);
            if (i == 0)
                firstRms = m.rms;
            lastRms = m.rms;
            loudestRms = juce::jmax (loudestRms, m.rms);
            loudestPeak = juce::jmax (loudestPeak, m.peak);

            if (i == 0)
                sum = m;                    // so anything not averaged still has a value
            else
            {
                sum.rms += m.rms;           sum.peak += m.peak;
                sum.sustainRms += m.sustainRms; sum.tailRms += m.tailRms;
                sum.motion += m.motion;     sum.crestDb += m.crestDb;
                sum.balanceDb += m.balanceDb; sum.centroidHz += m.centroidHz;
                sum.lowRatio += m.lowRatio; sum.highSpike += m.highSpike;
                sum.spectralMotion += m.spectralMotion;
                sum.heldRatio += m.heldRatio;
                sum.pitchHz += m.pitchHz;   sum.pitchSalience += m.pitchSalience;
                sum.pitchErrorSemitones += m.pitchErrorSemitones;
                sum.pitchOffGridSemitones += m.pitchOffGridSemitones;
                sum.stepSalience += m.stepSalience;
                sum.stepErrorSemitones += m.stepErrorSemitones;
                sum.stepOffGridSemitones += m.stepOffGridSemitones;
                sum.steps = juce::jmax (sum.steps, m.steps);
            }

            votes[0] += m.silent ? 1 : 0;
            votes[1] += m.clipping ? 1 : 0;
            votes[2] += m.clickOnly ? 1 : 0;
            votes[3] += m.tooPeaky ? 1 : 0;
            votes[4] += m.lopsided ? 1 : 0;
            ++counted;
        }

        if (counted > 1)
        {
            const auto n = (float) counted;
            for (auto* v : { &sum.rms, &sum.peak, &sum.sustainRms, &sum.tailRms,
                             &sum.motion, &sum.crestDb, &sum.balanceDb, &sum.centroidHz,
                             &sum.lowRatio, &sum.highSpike, &sum.spectralMotion,
                             &sum.heldRatio, &sum.pitchHz, &sum.pitchSalience,
                             &sum.pitchErrorSemitones, &sum.pitchOffGridSemitones,
                             &sum.stepSalience, &sum.stepErrorSemitones,
                             &sum.stepOffGridSemitones })
                *v /= n;
        }

        /*  A fault has to show in most of the renders, not one of them.

            Throwing a patch away because a single draw came out lopsided is the
            same mistake as correcting its level from a single draw.
        */
        /*  A climb, not a wander.

            Fifteen percent between the first render and the last is past
            anything the phase scatter produces on its own, and it only counts
            in one direction: a patch that happened to start loud and end quiet
            is scatter like any other.
        */
        sum.climbing = counted > 1 && firstRms > 1.0e-6f && lastRms > firstRms * 1.15f;
        if (sum.climbing)
        {
            sum.rms = loudestRms;
            sum.peak = loudestPeak;
        }

        const auto majority = (counted / 2) + 1;
        sum.silent = votes[0] >= majority;
        sum.clipping = votes[1] >= majority;
        sum.clickOnly = votes[2] >= majority;
        sum.tooPeaky = votes[3] >= majority;
        sum.lopsided = votes[4] >= majority;
        return sum;
    }

    PitchRule pitchRuleFor (const std::string& style)
    {
        if (style == "Bass" || style == "Keys" || style == "Lead")
            return { true, 0.45f, 0.45f, true };
        /*  A sequence may sit on any step, it just has to be on one.

            The salience bar is lower than the rest because a stepped patch
            genuinely is less periodic than a held note, and because the number
            it is compared against got more honest: reading the real peak rather
            than the shoulder near lag zero took these from around 0.85 to around
            0.45. Patches that sound clean measure 0.26 and up, so the bar sits
            under that rather than where the old inflated figure put it.
        */
        if (style == "Sequence")
            return { true, 0.20f, 0.35f, false };
        // A pad may be hazier about its pitch than a lead, but it still has to
        // be playing the note somebody pressed.
        if (style == "Pad")
            return { true, 0.35f, 0.60f, true };
        return { false, 0.0f, 0.0f };
    }

    float maxHeldRatioFor (const std::string& style)
    {
        // A bass is a struck note. Holding the key should not keep it going,
        // and the same goes for a drum.
        /*  A fifth of the early level, late in a held note, is a note that has
            clearly stopped. Fifteen percent was tight enough to fight the decay
            band the style is meant to have: a bass is asked for roughly a second
            of body, and a second of body still has something left at 1.2s.
        */
        /*  Loosened once the attack was capped.

            A fifth was rejecting seven bass rolls in ten, and listening to what
            it threw away said none of them held on: they were slow to start, so
            more of their energy landed late in the hold, and this number read
            that as a note that would not stop. With the attack capped, what is
            left for this to catch is the real case, a note that is genuinely
            louder late than early.
        */
        if (style == "Bass")       return 1.00f;
        if (style == "Percussion") return 0.60f;
        if (style == "Keys")       return 0.60f;   // short, but a key can be held
        // One means no limit at all, and it has to mean that rather than a
        // ceiling of one. A pad swells, so late in a held note it is louder
        // than it was early on, and a limit of exactly one rejected pads for
        // doing the one thing a pad is for.
        return kNoHeldLimit;
    }

    BalanceRule balanceFor (const std::string& style)
    {
        /*  A bass is defined by its balance, not by its mean.

            Hand-made basses put a median of 97% of their energy below 400 Hz,
            with a p10 of 54%, so the floor sits low enough to allow the ones
            that carry real treble detail. Their loudest moment above 2 kHz runs
            a median of 0% of the loudest moment overall and a p90 of 21%.

            Measured here, every bass anybody has called good reads between 0%
            and 7%, while the one that prompted this reads 62% and the next
            worst 30%. Fifteen sits in the gap, with room either side.
        */
        if (style == "Bass")       return { 0.60f, 0.15f };
        // A kick is mostly bottom too, but a hat is the opposite, so only the
        // burst rule applies.
        if (style == "Percussion") return { 0.0f,  1.0f };
        return { 0.0f, 1.0f };
    }

    Brightness brightnessFor (const std::string& style, float complexity)
    {
        const auto widen = [complexity] (Brightness b)
        {
            b.high *= 1.0f + 0.45f * juce::jlimit (0.0f, 1.0f, complexity - 0.35f);
            return b;
        };
        /*  Recalibrated against the library once the centroid started
            measuring energy rather than magnitude, which moved every one of
            these by roughly a factor of five. Hand-made presets, p90 of the
            energy centroid: bass 413, keys 1225, pad 2294, lead 2060. The
            ceilings sit above those with room for character.

            Bass keeps one only as a backstop. What decides whether a bass is a
            bass is the balance rule below, since the sound is defined by how
            much of it is down low rather than by where its mean lands.
        */
        if (style == "Bass")       return widen ({ 0.0f,    900.0f });
        /*  Keys and lead sit higher than the library's p90 suggested.

            Every candidate these two threw away within 30% of the old ceiling
            was played back and none of them sounded wrong for its style, so the
            line was in the wrong place rather than the patches being bright.
            The p90 it came from was taken over fourteen presets a style, which
            is a thin sample to draw a hard edge from.

            The far side is untouched. Keys still rejects at four times its
            ceiling and lead at nearly three, and none of those were listened to.
        */
        if (style == "Keys")       return widen ({ 60.0f,  6000.0f });
        // Pads run brighter than the rest and the library agrees, with a p90
        // just past five kilohertz.
        if (style == "Pad")        return widen ({ 40.0f,  3500.0f });
        if (style == "Lead")       return widen ({ 120.0f, 6800.0f });
        // A hat or a click belongs up there, so this one is generous.
        if (style == "Percussion") return widen ({ 0.0f, 12000.0f });
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

            double weighted = 0.0, total = 0.0, lowEnergy = 0.0;
            for (int i = 1; i < fftSize / 2; ++i)
            {
                const auto mag = (double) data[(size_t) i];
                /*  Energy, not magnitude.

                    Weighting by magnitude lets a few thousand quiet high bins
                    outvote the handful of loud low ones, so a bass whose energy
                    is 98% below 400 Hz was reporting a centroid of 2915 Hz and
                    being thrown out for brightness. The same patch weighted by
                    energy reads 124 Hz.
                */
                const auto energy = (double) mag * mag;
                const auto hz = i * sampleRate / fftSize;
                weighted += energy * hz;
                total += energy;
                if (hz < 400.0)
                    lowEnergy += energy;
            }
            m.centroidHz = total > 1.0e-9 ? (float) (weighted / total) : 0.0f;
            m.lowRatio = total > 1.0e-9 ? (float) (lowEnergy / total) : 0.0f;

        }

        /*  The loudest moment up top, against the loudest moment overall.

            Relative to its own window would flag a decayed tail that is all
            treble and inaudible. What a listener objects to is a burst that is
            both loud and high, which is what an LFO swinging a filter wide open
            mid note sounds like.
        */
        if (mono.size() >= (size_t) (0.3 * sampleRate))
        {
            const auto window = (size_t) (0.05 * sampleRate);
            const auto skip = (size_t) (0.15 * sampleRate);
            juce::dsp::FFT fft (11);
            const auto n = (size_t) 1 << 11;
            double loudestTotal = 1.0e-12, loudestHigh = 0.0;
            std::vector<double> centroids;

            for (size_t at = 0; at + window < mono.size(); at += window / 2)
            {
                std::vector<float> data (n * 2, 0.0f);
                for (size_t i = 0; i < juce::jmin (n, window); ++i)
                {
                    const auto w = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi
                                                           * (float) i / (float) n);
                    data[i] = mono[at + i] * w;
                }
                fft.performFrequencyOnlyForwardTransform (data.data());

                double sum = 0.0, high = 0.0, weighted = 0.0;
                for (size_t i = 1; i < n / 2; ++i)
                {
                    const auto e = (double) data[i] * data[i];
                    const auto hz = (double) i * sampleRate / (double) n;
                    sum += e;
                    weighted += e * hz;
                    if (hz > 2000.0)
                        high += e;
                }
                loudestTotal = juce::jmax (loudestTotal, sum);
                if (at >= skip)
                    loudestHigh = juce::jmax (loudestHigh, high);

                // Only windows with something in them. A silent tail has no
                // colour to speak of and would read as wild movement.
                if (sum > 1.0e-9)
                    centroids.push_back (weighted / sum);
            }
            m.highSpike = (float) (loudestHigh / loudestTotal);

            if (centroids.size() > 2)
            {
                double mean = 0.0;
                for (const auto c : centroids)
                    mean += c;
                mean /= (double) centroids.size();

                double var = 0.0;
                for (const auto c : centroids)
                    var += (c - mean) * (c - mean);
                var /= (double) centroids.size();

                m.spectralMotion = mean > 1.0e-6 ? (float) (std::sqrt (var) / mean) : 0.0f;
            }
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

            const auto expected = 440.0 * std::pow (2.0, (note - 69) / 12.0);

            // Autocorrelation over one window. Returns false when the window
            // holds nothing worth measuring.
            const auto analyse = [&] (size_t from, size_t count, float top,
                                      float& hz, float& salience) -> bool
            {
                if (count <= 2048 || from + count > mono.size())
                    return false;

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
                if (zero <= 1.0e-12)
                    return false;

                /*  `top` is the highest pitch worth looking for, and it is a
                    trade. Two kilohertz keeps a bass on its fundamental, since
                    a shorter lag correlates on waveform shape rather than
                    period and one bass came back at 5880 Hz. But a sequence
                    stepping several octaves up sounds above 2 kHz, and with the
                    ceiling there every step returned the band edge instead: lag
                    pinned to 22 samples, 2004.5 Hz, a quarter tone off the
                    grid. That quarter tone was this number, not the patch.

                    So the stepped path raises it and the whole note path does
                    not.
                */
                const auto minLag = (size_t) (sampleRate / (double) top);
                const auto maxLag = juce::jmin ((size_t) (sampleRate / 40.0), count / 2);
                const auto scoreAt = [&] (size_t lag)
                {
                    double sum = 0.0;
                    for (size_t i = 0; i + lag < count; ++i)
                        sum += (mono[from + i] - mean) * (mono[from + i + lag] - mean);
                    return sum / zero;
                };

                double bestScore = 0.0;
                size_t bestLag = 0;

                /*  Autocorrelation starts at one and falls away, and that
                    opening slope is not a period. Taking the largest value in
                    the band kept picking a lag on the slope, so the answer was
                    whatever the band happened to start at: a bass came back at
                    5880 Hz and a sequence's steps all read the band edge.

                    Stepping past the descent first, so the peak that is found
                    is a real one, put the bass back on its fundamental and took
                    a sequence from a quarter tone off the grid to a fiftieth.
                */
                bool pastDescent = false;
                for (size_t lag = minLag; lag < maxLag; ++lag)
                {
                    const auto score = scoreAt (lag);
                    if (! pastDescent)
                    {
                        pastDescent = score <= 0.0;
                        continue;
                    }
                    if (score > bestScore)
                    {
                        bestScore = score;
                        bestLag = lag;
                    }
                }

                // A signal that never decorrelates has no descent to step past,
                // so fall back to the whole band rather than reporting nothing.
                if (bestLag == 0)
                {
                    for (size_t lag = minLag; lag < maxLag; ++lag)
                    {
                        const auto score = scoreAt (lag);
                        if (score > bestScore)
                        {
                            bestScore = score;
                            bestLag = lag;
                        }
                    }
                }

                if (bestLag == 0)
                    return false;

                /*  A whole sample of lag is a coarse unit up high: 0.77 of a
                    semitone between lag 22 and 23. Fitting a parabola through
                    the peak and its neighbours recovers the fraction, so a step
                    that is in tune can measure as in tune.
                */
                auto refined = (double) bestLag;
                if (bestLag > minLag && bestLag + 1 < count)
                {
                    const auto y1 = scoreAt (bestLag - 1), y2 = bestScore,
                               y3 = scoreAt (bestLag + 1);
                    const auto denom = y1 - 2.0 * y2 + y3;
                    if (std::abs (denom) > 1.0e-12)
                        refined += juce::jlimit (-0.5, 0.5, 0.5 * (y1 - y3) / denom);
                }

                salience = (float) juce::jlimit (0.0, 1.0, bestScore);
                hz = (float) (sampleRate / refined);
                return true;
            };

            const auto errorsFor = [&] (float hz, float& octaveError, float& offGrid)
            {
                const auto semis = 12.0 * std::log2 (hz / expected);
                // An octave is still the same note; a fifth is not.
                octaveError = (float) std::abs (semis - std::round (semis / 12.0) * 12.0);
                offGrid = (float) std::abs (semis - std::round (semis));
            };

            const auto span = (size_t) (0.35 * sampleRate);
            const auto from = juce::jmin (loudest, mono.size() > span ? mono.size() - span : 0u);
            const auto count = juce::jmin (span, mono.size() - from);

            if (analyse (from, count, 2000.0f, m.pitchHz, m.pitchSalience))
                errorsFor (m.pitchHz, m.pitchErrorSemitones, m.pitchOffGridSemitones);

            /*  The same again, a step at a time. 0.09s is shorter than a
                sixteenth at 120 BPM, so a window holds one step rather than
                three, and each one is measured on its own terms.
            */
            {
                const auto step = (size_t) (0.09 * sampleRate);
                const auto quiet = m.peak * 0.2f;
                std::vector<float> saliences, octaveErrors, offGrids;

                for (size_t at = from; at + step <= mono.size() && saliences.size() < 24u;
                     at += step)
                {
                    float loud = 0.0f;
                    for (size_t i = 0; i < step; ++i)
                        loud = juce::jmax (loud, std::abs (mono[at + i]));
                    if (loud < quiet)
                        continue;   // between notes, or past the end of one

                    float hz = 0.0f, salience = 0.0f;
                    if (! analyse (at, step, 5000.0f, hz, salience))
                        continue;

                    float octaveError = 0.0f, offGrid = 0.0f;
                    errorsFor (hz, octaveError, offGrid);
                    saliences.push_back (salience);
                    octaveErrors.push_back (octaveError);
                    offGrids.push_back (offGrid);
                }

                const auto median = [] (std::vector<float>& v)
                {
                    std::sort (v.begin(), v.end());
                    return v.empty() ? 0.0f : v[v.size() / 2];
                };

                m.steps = (int) saliences.size();
                if (m.steps > 0)
                {
                    m.stepSalience = median (saliences);
                    m.stepErrorSemitones = median (octaveErrors);
                    m.stepOffGridSemitones = median (offGrids);
                }
            }
        }

        return m;
    }
}
