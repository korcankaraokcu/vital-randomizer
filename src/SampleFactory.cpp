#include "SampleFactory.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include <juce_core/juce_core.h>

namespace sampler
{
    namespace
    {
        constexpr int kRate = 44100;
        constexpr int kLength = kRate;      // one second, which is what Vital ships

        /*  A loop that is a whole bar.

            Vital's sampler has no sync of its own: the LFOs, the delay, the
            chorus and the flanger all take a tempo and the sample player does
            not, so a loop plays at its own rate whatever the song is doing.
            What can be done is to make the loop an exact musical length at the
            120 BPM the hosted playhead reports, which is the tempo every synced
            LFO in the patch is already running against.

            Two seconds is four beats there, one bar in four four, so a bed sits
            under the patch in time with it rather than sliding against it. At
            any other host tempo it drifts, and there is nothing in the format
            that can prevent that.
        */
        /*  Long, because a loop gives itself away by being recognised.

            Vital's own Waves preset carries ten seconds of sample and its
            spectrum moves less than a tenth across the whole of it. That is two
            tricks at once: nothing distinctive happens, and what does happen
            takes a long time to come round. Two seconds with a wander in it was
            the opposite of both, and the wander was the worse half, since a
            feature the ear can learn is a feature it can hear returning.

            The length is free, because the rate is not fixed. Vital reads the
            sample_rate field and resamples, which was worth testing rather than
            assuming: the same audio declared at half the rate comes back at the
            same pitch, while the same bytes declared at full rate come back an
            octave up. A bed has nothing above ten kilohertz in it, so storing
            one at 22050 costs nothing and buys twice the seconds.

            Sixteen beats at 120 BPM, which is four bars.
        */
        constexpr int kLoopRate = 22050;
        constexpr int kLoopSeconds = 8;
        constexpr int kLoopLength = kLoopRate * kLoopSeconds;
        // Below the half rate Nyquist, with room to spare.
        constexpr float kLoopCeilingHz = 9500.0f;

        float uniform (std::mt19937& rng, float lo = 0.0f, float hi = 1.0f)
        {
            return std::uniform_real_distribution<float> (lo, hi) (rng);
        }

        int pick (std::mt19937& rng, int lo, int hi)
        {
            return std::uniform_int_distribution<int> (lo, hi) (rng);
        }

        /*  A one pole pair, used as a resonant band. Enough to colour noise into
            something with a voice, and small enough to be obvious.
        */
        struct Resonator
        {
            float b1 = 0.0f, b2 = 0.0f, gain = 1.0f;
            float z1 = 0.0f, z2 = 0.0f;

            void set (float hz, float q, float rate = (float) kRate)
            {
                const auto r = std::exp (-juce::MathConstants<float>::pi * hz / (q * rate));
                const auto theta = 2.0f * juce::MathConstants<float>::pi * hz / rate;
                b1 = 2.0f * r * std::cos (theta);
                b2 = -r * r;
                gain = (1.0f - r) * std::sqrt (1.0f - 2.0f * r * std::cos (2.0f * theta) + r * r);
            }

            float operator() (float x)
            {
                const auto y = gain * x + b1 * z1 + b2 * z2;
                z2 = z1;
                z1 = y;
                return y;
            }
        };

        void removeDcAndNormalise (std::vector<float>& a, float peak = 0.9f)
        {
            if (a.empty())
                return;

            double mean = 0.0;
            for (const auto x : a)
                mean += x;
            mean /= (double) a.size();

            float loudest = 0.0f;
            for (auto& x : a)
            {
                x -= (float) mean;
                loudest = std::max (loudest, std::abs (x));
            }
            if (loudest > 1.0e-9f)
                for (auto& x : a)
                    x *= peak / loudest;
        }

        /*  A bed has to join back onto itself.

            Vital loops the sample, and a loop whose end does not meet its start
            is a click once a cycle, which is the single most recognisable way
            synthesised audio gives itself away. Crossfading the tail over the
            head costs a few milliseconds of the sound and removes it entirely.
        */
        void crossfadeToLength (std::vector<float>& a, int target, int fade)
        {
            /*  Blend what comes after the end over the beginning, then cut to
                the exact length asked for.

                Trimming by the fade instead, which is the obvious way round,
                leaves a loop of whatever length is left over. That is fine for
                a seam and useless for a bar: the length has to be the number
                that was chosen, not the number that survived.
            */
            if ((int) a.size() < target + fade || fade <= 1)
            {
                a.resize ((size_t) std::min ((int) a.size(), target));
                return;
            }

            /*  Equal power, not equal amplitude.

                Fading one out linearly while the other fades in linearly is
                right for two takes of the same thing, and wrong for two pieces
                of noise. Uncorrelated signals add as powers rather than as
                amplitudes, so weights of a half and a half leave 0.707 of the
                level and the loop point drops three decibels into a hole and
                climbs back out. Measured on a bed it was four.

                Weighting by the square root keeps the powers summing to one,
                which is what a steady bed needs across the join.
            */
            for (int i = 0; i < fade; ++i)
            {
                const auto t = (float) i / (float) fade;
                a[(size_t) i] = a[(size_t) i] * std::sqrt (t)
                                  + a[(size_t) (target + i)] * std::sqrt (1.0f - t);
            }
            a.resize ((size_t) target);
        }

        /*  How many times something goes round over the whole loop.

            Anything that moves inside a looping sample has to come back to where
            it started, or the loop point is a jump in the modulation that no
            crossfade hides. Counting in whole cycles over the loop rather than
            in Hz makes that automatic, and makes the movement a division of the
            bar rather than a rate that happens to land near one.
        */
        float phaseAt (float cycles, size_t i, size_t total)
        {
            return 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * cycles
                                           * (float) i / (float) total);
        }

        /*  Noise with a voice.

            White noise through two resonant bands and a rolloff. Where the bands
            sit is what turns it from hiss into air, wind, rumble or a room, and
            it is also why this does not carry a patch to seven kilohertz the way
            flat noise does.
        */
        std::vector<float> noiseBed (std::mt19937& rng, const Request& r)
        {
            const auto fade = (int) (0.05f * kLoopRate);
            const auto total = (size_t) (kLoopLength + fade + 2048);
            std::vector<float> a (total);

            const auto centre = juce::jmap (r.bright, 0.0f, 1.0f, 90.0f, 2600.0f)
                                    * uniform (rng, 0.75f, 1.3f);
            const auto second = centre * uniform (rng, 1.6f, 3.2f);

            Resonator low, high;
            low.set (juce::jlimit (40.0f, kLoopCeilingHz, centre),
                     uniform (rng, 1.2f, 6.0f), (float) kLoopRate);
            high.set (juce::jlimit (40.0f, kLoopCeilingHz, second),
                      uniform (rng, 2.0f, 9.0f), (float) kLoopRate);

            const auto blend = uniform (rng, 0.15f, 0.6f);
            /*  Barely anything.

                This was a fifth to two fifths of the level, once or twice a
                bar, and it was the single thing that made the loop audible. A
                bed is supposed to be the thing nobody notices. What is left is
                enough to stop it being a frozen block of noise and not enough
                to be remembered.
            */
            const auto wanderCycles = (float) pick (rng, 1, 2);
            const auto wanderDepth = uniform (rng, 0.0f, 0.10f);

            std::normal_distribution<float> white (0.0f, 0.3f);
            for (size_t i = 0; i < a.size(); ++i)
            {
                const auto x = white (rng);
                const auto wander = 1.0f + wanderDepth * phaseAt (wanderCycles, i, total);
                a[i] = (low (x) * (1.0f - blend) + high (x) * blend) * wander;
            }

            // The filters start from silence, so the first pass is a fade in
            // rather than the sound. Throw it away.
            a.erase (a.begin(), a.begin() + 2048);
            removeDcAndNormalise (a);
            crossfadeToLength (a, kLoopLength, fade);
            return a;
        }

        /*  Something struck.

            A bell, a bar, a block: a handful of partials that are not a harmonic
            series, each decaying at its own rate, with the high ones going first.
            That is what those objects are physically, which is why so little code
            sounds like one.
        */
        std::vector<float> struckBody (std::mt19937& rng, const Request& r)
        {
            std::vector<float> a ((size_t) kLength, 0.0f);

            const auto root = juce::jmap (r.bright, 0.0f, 1.0f, 70.0f, 520.0f)
                                  * uniform (rng, 0.8f, 1.25f);
            // The upper bound has to stay above the lower one. At a complexity
            // of zero this asked for between five and four partials, which is
            // undefined rather than empty, and produced an eleven millisecond
            // click where a struck sound should have been.
            const auto partials = pick (rng, 5,
                                        std::max (7, 4 + (int) std::lround (8.0f * r.complexity)));
            // Zero is a harmonic series and one is a long way from it. DIRT
            // decides how far, since inharmonic is what reads as metallic.
            const auto stretch = juce::jmap (r.dirt, 0.0f, 1.0f, 0.02f, 0.55f);
            const auto ring = juce::jmap (r.complexity, 0.0f, 1.0f, 0.25f, 0.9f)
                                  * uniform (rng, 0.7f, 1.3f);

            for (int p = 0; p < partials; ++p)
            {
                const auto n = (float) (p + 1);
                const auto ratio = n * (1.0f + stretch * (n - 1.0f) * uniform (rng, 0.4f, 1.0f));
                const auto hz = root * ratio;
                if (hz > 0.45f * kRate)
                    break;

                // The top of a struck sound goes first, which is most of what
                // makes it read as struck rather than as a chord.
                const auto decay = ring / (1.0f + 1.5f * (float) p) * uniform (rng, 0.7f, 1.3f);
                const auto amp = uniform (rng, 0.5f, 1.0f) / n;
                const auto phase = uniform (rng, 0.0f, juce::MathConstants<float>::twoPi);

                for (int i = 0; i < kLength; ++i)
                {
                    const auto t = (float) i / (float) kRate;
                    a[(size_t) i] += amp * std::exp (-t / std::max (0.02f, decay))
                                         * std::sin (2.0f * juce::MathConstants<float>::pi * hz * t + phase);
                }
            }

            // A strike starts with noise before it starts ringing.
            const auto hitMs = uniform (rng, 3.0f, 18.0f);
            std::normal_distribution<float> white (0.0f, 0.5f);
            Resonator body;
            body.set (juce::jlimit (200.0f, 9000.0f, root * uniform (rng, 3.0f, 9.0f)),
                      uniform (rng, 0.8f, 3.0f));
            for (int i = 0; i < (int) (hitMs * 0.001f * kRate); ++i)
            {
                const auto t = (float) i / (hitMs * 0.001f * kRate);
                a[(size_t) i] += body (white (rng)) * (1.0f - t) * 0.8f;
            }

            removeDcAndNormalise (a);
            return a;
        }


        /*  Plucked.

            Karplus-Strong: a burst of noise pushed round a delay line the length
            of one cycle, losing its top every time round. The result is a string,
            and it is a harmonic one, which is what makes it read as a different
            object from the struck body rather than a variation on it.
        */
        std::vector<float> pluckedString (std::mt19937& rng, const Request& r)
        {
            std::vector<float> a ((size_t) kLength, 0.0f);

            const auto hz = juce::jmap (r.bright, 0.0f, 1.0f, 80.0f, 600.0f)
                                * uniform (rng, 0.8f, 1.25f);
            const auto n = std::max (8, (int) std::lround (kRate / hz));
            std::vector<float> loopBuf ((size_t) n);

            // How bright the pluck starts. A pick near the bridge is thin and
            // near the middle is round, which is one filter on the excitation.
            std::normal_distribution<float> white (0.0f, 0.5f);
            Resonator pick;
            pick.set (juce::jlimit (200.0f, 12000.0f, hz * uniform (rng, 4.0f, 14.0f)),
                      uniform (rng, 0.6f, 2.0f));
            for (auto& x : loopBuf)
                x = pick (white (rng));

            // Damping decides how long it rings and how fast the top goes.
            const auto damping = juce::jmap (r.complexity, 0.0f, 1.0f, 0.986f, 0.9995f);
            const auto tone = uniform (rng, 0.35f, 0.5f);

            float previous = 0.0f;
            for (int i = 0; i < kLength; ++i)
            {
                const auto index = (size_t) (i % n);
                const auto current = loopBuf[index];
                const auto filtered = tone * current + (1.0f - tone) * previous;
                previous = current;
                loopBuf[index] = filtered * damping;
                a[(size_t) i] = current;
            }

            removeDcAndNormalise (a);
            return a;
        }

        /*  Grit.

            Sparse crackle over almost nothing, the way dust on a record or a bad
            connection sounds. It is the opposite of the bed in the one way that
            matters to the ear: the bed is dense and constant, this is empty with
            events in it, so the two never get mistaken for each other.
        */
        std::vector<float> grit (std::mt19937& rng, const Request& r)
        {
            const auto fade = (int) (0.03f * kLoopRate);
            std::vector<float> a ((size_t) (kLoopLength + fade), 0.0f);

            std::normal_distribution<float> white (0.0f, 0.25f);
            const auto floorLevel = uniform (rng, 0.004f, 0.03f);
            for (auto& x : a)
                x = white (rng) * floorLevel;

            const auto perSecond = juce::jmap (r.complexity, 0.0f, 1.0f, 12.0f, 160.0f)
                                       * uniform (rng, 0.7f, 1.4f);
            const auto ticks = std::max (4, (int) std::lround (perSecond
                                             * (float) a.size() / (float) kLoopRate));

            Resonator body;
            for (int t = 0; t < ticks; ++t)
            {
                const auto at = pick (rng, 0, (int) a.size() - 1);
                const auto lengthSamples = (int) (uniform (rng, 0.4f, 6.0f) * 0.001f * kLoopRate);
                const auto amp = uniform (rng, 0.15f, 1.0f);
                body.set (juce::jlimit (300.0f, kLoopCeilingHz,
                                        juce::jmap (r.bright, 0.0f, 1.0f, 600.0f, 5200.0f)
                                            * uniform (rng, 0.5f, 1.8f)),
                          uniform (rng, 0.5f, 3.0f), (float) kLoopRate);

                for (int i = 0; i < lengthSamples && at + i < (int) a.size(); ++i)
                {
                    const auto taper = 1.0f - (float) i / (float) lengthSamples;
                    a[(size_t) (at + i)] += body (white (rng)) * taper * taper * amp;
                }
            }

            removeDcAndNormalise (a);
            crossfadeToLength (a, kLoopLength, fade);
            return a;
        }

        /*  A swell.

            Backwards: quiet at the start, loudest at the end, then it stops. The
            shape alone makes it unmistakable next to anything that decays, which
            is what every other model here does.
        */
        std::vector<float> swell (std::mt19937& rng, const Request& r)
        {
            std::vector<float> a ((size_t) kLength, 0.0f);

            std::normal_distribution<float> white (0.0f, 0.4f);
            Resonator band;
            const auto start = juce::jmap (r.bright, 0.0f, 1.0f, 200.0f, 2000.0f);
            const auto finish = start * uniform (rng, 2.0f, 7.0f);
            const auto q = uniform (rng, 1.5f, 6.0f);
            const auto curve = uniform (rng, 1.6f, 4.0f);

            for (int i = 0; i < kLength; ++i)
            {
                const auto t = (float) i / (float) kLength;
                // The band opens as it rises, so it gets brighter as it gets
                // louder, which is what makes a swell feel like it is arriving.
                band.set (juce::jlimit (40.0f, 15000.0f, start + (finish - start) * t), q);
                a[(size_t) i] = band (white (rng)) * std::pow (t, curve);
            }

            // Cut rather than fade, so it lands on the note instead of dribbling.
            const auto tail = (int) (0.004f * kRate);
            for (int i = 0; i < tail; ++i)
                a[(size_t) (kLength - 1 - i)] *= (float) i / (float) tail;

            removeDcAndNormalise (a);
            return a;
        }

        /*  A voice.

            Noise through three formants, which is roughly what a throat does to
            it. It lands somewhere between a vowel and a filter sweep, and the
            reason it belongs here is that the ear names it: a bed is a texture
            and this is somebody in the room.
        */
        std::vector<float> voice (std::mt19937& rng, const Request& r)
        {
            const auto fade = (int) (0.06f * kLoopRate);
            const auto total = (size_t) (kLoopLength + fade + 2048);
            std::vector<float> a (total, 0.0f);

            // The vowels, near enough. Shifted by BRIGHT and wandered slowly, so
            // it moves between them rather than sitting on one.
            static const float vowels[5][3] = {
                { 730.0f, 1090.0f, 2440.0f },   // ah
                { 530.0f,  1840.0f, 2480.0f },  // eh
                { 270.0f, 2290.0f, 3010.0f },   // ee
                { 570.0f,  840.0f, 2410.0f },   // oh
                { 300.0f,  870.0f, 2240.0f },   // oo
            };
            const auto from = pick (rng, 0, 4);
            auto to = pick (rng, 0, 4);
            if (to == from)
                to = (to + 1 + pick (rng, 0, 3)) % 5;

            const auto tilt = juce::jmap (r.bright, 0.0f, 1.0f, 0.7f, 1.5f);
            /*  Once a bar, and not all the way there.

                Two things were making this read as a sweep rather than a voice:
                it crossed between vowels up to twice a bar, and when it did it
                went the whole distance, which is a bigger move than a throat
                makes while holding a note. One crossing a bar, covering a third
                to two thirds of the way, leaves it sounding like somebody
                sustaining and changing shape slightly.

                One cycle over the loop is as slow as a loop this long can go and
                still come back to where it started.
            */
            const auto crossings = 1.0f;
            const auto travel = uniform (rng, 0.3f, 0.65f);
            Resonator f1, f2, f3;
            std::normal_distribution<float> white (0.0f, 0.35f);

            for (size_t i = 0; i < a.size(); ++i)
            {
                const auto phase = travel * phaseAt (crossings, i, total);
                const auto blend = [&] (int band)
                {
                    return juce::jlimit (80.0f, kLoopCeilingHz,
                        (vowels[from][band] + (vowels[to][band] - vowels[from][band]) * phase) * tilt);
                };
                f1.set (blend (0), uniform (rng, 7.0f, 8.0f), (float) kLoopRate);
                f2.set (blend (1), uniform (rng, 9.0f, 11.0f), (float) kLoopRate);
                f3.set (blend (2), uniform (rng, 11.0f, 13.0f), (float) kLoopRate);

                const auto x = white (rng);
                a[i] = f1 (x) + 0.55f * f2 (x) + 0.3f * f3 (x);
            }

            a.erase (a.begin(), a.begin() + 2048);
            removeDcAndNormalise (a);
            crossfadeToLength (a, kLoopLength, fade);
            return a;
        }


        /*  A thump.

            A sine whose pitch falls from several times the root down onto it in
            the first few tens of milliseconds. That drop is what the ear reads
            as something being hit hard, and it is how an 808 and a kick are
            built, so it is the one model here that belongs to bass rather than
            being lent to it.

            It is also the only one with a moving frequency, which is what keeps
            it from sounding like a relative of the struck body: that rings at
            fixed pitches and this arrives at one.
        */
        /*  Noise, and nothing else.

            The snare's rattle, a clap, the hats, the cymbals and a shaker are
            all noise before anything else, and none of the models above is: a
            bed is shaped into a texture and grit is sparse crackle. This is
            flat, and the drum's own filter decides what part of it is heard.
            It loops, since a crash rings for longer than one pass, and a seam
            in white noise is inaudible, so it needs no crossfade. It is kept at
            the full rate, where the looping models are halved, because a hi-hat
            lives in the octave that halving would throw away.
        */
        std::vector<float> whiteNoise (std::mt19937& rng, const Request& r)
        {
            std::vector<float> a ((size_t) kLength, 0.0f);
            for (auto& v : a)
                v = 0.95f * uniform (rng, -1.0f, 1.0f);
            if (r.corner <= 0.0f)
                return a;

            /*  Given a corner, kept to the band a clap is heard in, from it up
                to 7 kHz, and saturated. Heard through a band pass, white noise
                at full scale spends most of its energy where it is thrown away,
                and the clap came out up to 12 dB quieter than it had to be,
                more than the master volume could make up. Narrowing the band
                alone gains little, since filtered noise has taller peaks for
                the same energy. Saturating it brings the peaks back down to
                the body of the noise, which is still heard as noise, and at the
                same peak the band plays several decibels louder. The loop runs
                twice so the second pass starts from a settled filter. */
            const auto pi = juce::MathConstants<double>::pi;
            const auto lowPole = std::exp (-2.0 * pi * (double) r.corner / kRate);
            const auto highPole = std::exp (-2.0 * pi * 7000.0 / kRate);
            double l1 = 0.0, l2 = 0.0, h1 = 0.0, h2 = 0.0;
            std::vector<double> band (a.size());
            for (int pass = 0; pass < 2; ++pass)
                for (size_t i = 0; i < a.size(); ++i)
                {
                    const auto x = (double) a[i];
                    l1 = (1.0 - lowPole) * x + lowPole * l1;
                    l2 = (1.0 - lowPole) * l1 + lowPole * l2;
                    const auto above = x - l2;
                    h1 = (1.0 - highPole) * above + highPole * h1;
                    h2 = (1.0 - highPole) * h1 + highPole * h2;
                    band[i] = h2;
                }
            double power = 0.0;
            for (const auto v : band)
                power += v * v;
            const auto rms = std::sqrt (power / (double) band.size()) + 1.0e-12;
            for (size_t i = 0; i < a.size(); ++i)
                a[i] = (float) (0.95 * std::tanh (1.6 * band[i] / rms) / std::tanh (1.6 * 3.5));
            for (auto& v : a)
                v = juce::jlimit (-0.95f, 0.95f, v);
            return a;
        }

        /*  The metal of a hi-hat or a cymbal, the way the 808 makes it.

            A drum machine's hats, cymbal and cowbell share a bank of six square
            wave oscillators at frequencies that add up to no pitch, 205.3,
            304.4, 369.6, 522.7, 540 and 800 Hz on the 808, band passed at 3440
            and 7100 Hz and high passed. What matters is not the exact
            frequencies but that the sum is an inharmonic hum. An oscillator in
            Vital cannot make that, since a wavetable is one repeating cycle and
            every partial in it is a harmonic of one pitch: the metallic tables
            the cymbals used to be built on measured a clear note at 1046 Hz,
            which is what made them sound wrong however far down they were
            turned.

            So the cluster is built here, as the sample. The six are moved up
            or down together and a little apart, for variety between two hats,
            and each is rounded to a whole number of hertz so it completes whole
            cycles in the one second loop and the seam is silent. They are band
            limited, odd harmonics up to the top of the band only, since a naive
            square at these pitches folds a second, unintended cluster back down
            from the top, and tilted up, as the 808 band passes its own. Plain
            noise is mixed in to taste, and the drum's own filter chooses which
            part of it is heard.
        */
        std::vector<float> metalCluster (std::mt19937& rng, const Request& r)
        {
            static const float base[] = { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f };
            std::vector<double> sum ((size_t) kLength, 0.0);
            const auto shift = uniform (rng, 0.85f, 1.25f);
            const auto corner = r.corner > 0.0f ? (double) r.corner : 2500.0;

            /*  A second bank, when asked for, sits at an unrelated ratio above
                the first. Twelve partials ringing together blur into a sheen
                where six can be picked out one by one, which is what makes a
                long hat sound like a struck chord. */
            std::vector<double> bank { 1.0 };
            for (int b = 1; b < r.banks; ++b)
                bank.push_back ((double) uniform (rng, 1.31f, 1.47f) * (double) b);

            for (const auto scale : bank)
            for (const auto f0 : base)
            {
                const auto hz = std::round ((double) f0 * shift * scale * uniform (rng, 0.96f, 1.04f));
                const auto phase = (double) uniform (rng, 0.0f, 6.2831853f);
                for (int k = 1; k * hz < kRate * 0.5; k += 2)
                {
                    // A rotating phasor rather than a sine per sample, which is
                    // the difference between a few million sines and none.
                    const auto step = 2.0 * juce::MathConstants<double>::pi * k * hz / kRate;
                    const std::complex<double> turn (std::cos (step), std::sin (step));
                    std::complex<double> z (std::cos (k * phase), std::sin (k * phase));
                    /*  Tilted up the way the 808's own band passes tilt it.
                        The squares' fundamentals, 200 to 800 Hz, are the
                        loudest part of a raw cluster, and a synth's filter
                        does not take enough off them: the first cymbals built
                        this way had a fifth of their energy under 400 Hz. */
                    const auto ratio = (k * hz / corner) * (k * hz / corner);
                    const auto amp = ratio / (1.0 + ratio) / k;
                    for (auto& v : sum)
                    {
                        v += amp * z.imag();
                        z *= turn;
                    }
                }
            }

            double peak = 1.0e-9;
            for (const auto v : sum)
                peak = std::max (peak, std::abs (v));

            const auto noise = r.noise >= 0.0f ? juce::jlimit (0.0f, 1.0f, r.noise) : 0.2f;
            std::vector<float> a ((size_t) kLength, 0.0f);
            for (size_t i = 0; i < a.size(); ++i)
                a[i] = 0.95f * ((1.0f - noise) * (float) (sum[i] / peak) + noise * uniform (rng, -1.0f, 1.0f));
            return a;
        }

        /*  A cymbal's plate: noise rung through a bank of resonances.

            A cymbal has thousands of modes, crowding closer together the
            higher they go, each ringing for a while and each slightly
            different from strike to strike. That is heard as a wash with some
            ringing in it, which sampled crashes and rides show as a spectrum
            with twenty or thirty peaks standing 10 to 20 dB clear of a noisy
            floor. A cluster of square waves is the other way round: a few
            hundred exact lines on almost no floor.

            So white noise is rung through a few dozen to a couple of hundred
            two pole resonators, spread from 500 Hz to 16 kHz and crowding
            toward the top, weighted toward the 3 to 9 kHz a cymbal lives in,
            and mixed with noise high passed at 2 kHz. The resonances' sharpness
            decides between wash and ping. The loop is crossfaded on equal power
            into its own start, as the noise beds are, since a ringing
            resonance is not silent at the seam.
        */
        std::vector<float> cymbalPlate (std::mt19937& rng, const Request& r)
        {
            const auto modes = r.modes > 0 ? r.modes : 120;
            const auto q = r.q > 0.0f ? (double) r.q : 40.0;
            const auto fade = (int) (kRate * 0.05);
            const auto total = kLength + fade;
            const auto pi = juce::MathConstants<double>::pi;

            std::vector<double> input ((size_t) (total + kRate / 4));
            for (auto& v : input)
                v = (double) uniform (rng, -1.0f, 1.0f);
            const auto warm = (int) input.size() - total;   // settle the resonators first

            std::vector<double> ring ((size_t) total, 0.0);
            for (int m = 0; m < modes; ++m)
            {
                const auto u = (double) uniform (rng);
                const auto hz = 500.0 * std::pow (32.0, std::pow (u, 0.75));
                const auto sharp = q * (double) uniform (rng, 0.6f, 1.4f);
                const auto radius = std::exp (-pi * hz / (sharp * kRate));
                const auto a1 = 2.0 * radius * std::cos (2.0 * pi * hz / kRate);
                const auto a2 = radius * radius;
                const auto gain = (1.0 - a2) * 0.5;
                const auto lift = (hz / 3000.0) * (hz / 3000.0);
                const auto weight = lift / (1.0 + lift) / (1.0 + (hz / 11000.0) * (hz / 11000.0))
                                    * (double) uniform (rng, 0.3f, 1.0f);
                double y1 = 0.0, y2 = 0.0;
                for (int i = 2; i < (int) input.size(); ++i)
                {
                    const auto y = gain * (input[(size_t) i] - input[(size_t) i - 2]) + a1 * y1 - a2 * y2;
                    y2 = y1; y1 = y;
                    if (i >= warm)
                        ring[(size_t) (i - warm)] += weight * y;
                }
            }

            // The wash under the ringing: noise with its lows taken off.
            std::vector<double> hiss ((size_t) total, 0.0);
            const auto pole = std::exp (-2.0 * pi * 2000.0 / kRate);
            double lo1 = 0.0, lo2 = 0.0;
            for (int i = 0; i < (int) input.size(); ++i)
            {
                const auto x = (double) uniform (rng, -1.0f, 1.0f);
                lo1 = (1.0 - pole) * x + pole * lo1;
                lo2 = (1.0 - pole) * lo1 + pole * lo2;
                if (i >= warm)
                    hiss[(size_t) (i - warm)] = x - lo2;
            }

            const auto rmsOf = [] (const std::vector<double>& v)
            {
                double p = 0.0;
                for (const auto x : v)
                    p += x * x;
                return std::sqrt (p / (double) v.size()) + 1.0e-12;
            };
            const auto noise = r.noise >= 0.0f ? (double) juce::jlimit (0.0f, 1.0f, r.noise) : 0.3;
            const auto ringGain = (1.0 - noise) / rmsOf (ring), hissGain = noise / rmsOf (hiss);

            std::vector<double> mixed ((size_t) total);
            for (size_t i = 0; i < mixed.size(); ++i)
                mixed[i] = ringGain * ring[i] + hissGain * hiss[i];

            std::vector<float> a ((size_t) kLength);
            for (int i = 0; i < kLength; ++i)
            {
                auto v = mixed[(size_t) i];
                if (i < fade)
                {
                    const auto t = (double) i / fade;
                    v = std::sqrt (t) * v + std::sqrt (1.0 - t) * mixed[(size_t) (kLength + i)];
                }
                a[(size_t) i] = (float) v;
            }
            const auto level = rmsOf (std::vector<double> (a.begin(), a.end()));
            for (auto& v : a)
                v = juce::jlimit (-0.95f, 0.95f, (float) (v * 0.25 / level));
            return a;
        }

        /*  Noise through a two pole band pass, a burst at a time. */
        struct BandPass
        {
            double a1 = 0.0, a2 = 0.0, gain = 0.0, x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

            BandPass (double hz, double q)
            {
                const auto pi = juce::MathConstants<double>::pi;
                const auto radius = std::exp (-pi * hz / (q * kRate));
                a1 = 2.0 * radius * std::cos (2.0 * pi * hz / kRate);
                a2 = radius * radius;
                gain = (1.0 - a2) * 0.5;
            }

            double operator() (double x)
            {
                const auto y = gain * (x - x2) + a1 * y1 - a2 * y2;
                x2 = x1; x1 = x; y2 = y1; y1 = y;
                return y;
            }
        };

        std::vector<float> normalisedTo (std::vector<double> v, double peak)
        {
            double most = 1.0e-12;
            for (const auto x : v)
                most = std::max (most, std::abs (x));
            std::vector<float> a (v.size());
            for (size_t i = 0; i < v.size(); ++i)
                a[i] = (float) (v[i] * peak / most);
            return a;
        }

        /*  A kick's beater on the head: a slap of noise a few milliseconds
            long, rung through a broad resonance between 2 and 5 kHz, with a
            knock under it where the head gives. Silent after about 30 ms. */
        std::vector<float> beater (std::mt19937& rng, const Request&)
        {
            BandPass slap (uniform (rng, 2000.0f, 5000.0f), uniform (rng, 1.5f, 3.0f));
            BandPass knock (uniform (rng, 600.0f, 1400.0f), uniform (rng, 2.0f, 4.0f));
            const auto slapTau = uniform (rng, 0.0015f, 0.004f) * kRate;
            const auto knockTau = uniform (rng, 0.006f, 0.015f) * kRate;
            const auto knockShare = (double) uniform (rng, 0.3f, 0.6f);
            std::vector<double> v ((size_t) kLength, 0.0);
            for (int i = 0; i < (int) (kRate * 0.12); ++i)
            {
                const auto x = (double) uniform (rng, -1.0f, 1.0f);
                v[(size_t) i] = slap (x) * std::exp (-i / slapTau)
                                + knockShare * knock (x) * std::exp (-i / knockTau);
            }
            return normalisedTo (std::move (v), 0.9);
        }

        /*  A drum head's overtones, for a tom. An ideal circular membrane rings
            at 1.59, 2.14, 2.30, 2.65, 2.92, 3.16 and 3.50 times its lowest
            mode, the higher ones dying faster, and the stick adds a crack of
            noise. The fundamental is the oscillator's, so it is left out here.
            Built on middle C and played keytracked, since Vital plays a sample
            at its own pitch on C4, so these land over whatever note is
            played. */
        std::vector<float> drumHead (std::mt19937& rng, const Request&)
        {
            static const double modes[] = { 1.594, 2.136, 2.296, 2.653, 2.918, 3.156, 3.501 };
            const auto base = 261.63;
            const auto pi = juce::MathConstants<double>::pi;
            std::vector<double> v ((size_t) kLength, 0.0);
            int n = 0;
            for (const auto ratio : modes)
            {
                const auto hz = base * ratio * (double) uniform (rng, 0.985f, 1.015f);
                // The first overtone is kept down: it is the loudest, and at
                // 1.59, near a minor sixth, it pulled a tom's pitch off the note.
                const auto amp = (double) uniform (rng, 0.4f, 1.0f) / (1.0 + 0.35 * n) * (n == 0 ? 0.4 : 1.0);
                const auto tau = (double) uniform (rng, 0.05f, 0.14f) / (1.0 + 0.4 * n) * kRate;
                const auto phase = (double) uniform (rng, 0.0f, 6.2831853f);
                for (int i = 0; i < kLength; ++i)
                    v[(size_t) i] += amp * std::exp (-i / tau) * std::sin (2.0 * pi * hz * i / kRate + phase);
                ++n;
            }
            BandPass crack (uniform (rng, 1500.0f, 4000.0f), 1.2);
            const auto crackTau = uniform (rng, 0.002f, 0.006f) * kRate;
            const auto crackShare = (double) uniform (rng, 0.6f, 1.2f);
            for (int i = 0; i < (int) (kRate * 0.03); ++i)
                v[(size_t) i] += crackShare * 4.0 * crack (uniform (rng, -1.0f, 1.0f)) * std::exp (-i / crackTau);
            return normalisedTo (std::move (v), 0.9);
        }

        /*  A stick on a rim or a wood block: a crack of noise rung through two
            or three wooden resonances between 900 Hz and 3.2 kHz, gone in a
            few tens of milliseconds. */
        std::vector<float> stick (std::mt19937& rng, const Request&)
        {
            std::vector<BandPass> wood;
            const auto count = pick (rng, 2, 3);
            for (int i = 0; i < count; ++i)
                wood.emplace_back (uniform (rng, 900.0f, 3200.0f), uniform (rng, 8.0f, 20.0f));
            BandPass crack (uniform (rng, 3000.0f, 6000.0f), 1.2);
            const auto tau = uniform (rng, 0.010f, 0.025f) * kRate;
            const auto crackTau = uniform (rng, 0.0008f, 0.002f) * kRate;
            std::vector<double> v ((size_t) kLength, 0.0);
            for (int i = 0; i < (int) (kRate * 0.15); ++i)
            {
                const auto x = (double) uniform (rng, -1.0f, 1.0f);
                // The strike excites the wood for a moment, and the wood rings on.
                const auto hit = x * std::exp (-i / tau);
                double ring = 0.0;
                for (auto& w : wood)
                    ring += w (hit);
                v[(size_t) i] = 3.0 * ring + 1.5 * crack (x) * std::exp (-i / crackTau);
            }
            return normalisedTo (std::move (v), 0.9);
        }

        /*  A kettle drum, from orchestral recordings.

            Each mode of the head at its own ratio, level and rate of decay,
            measured on five timpani from Versilian's VSCO 2 at two dynamics
            and on the modes a kettle drum is known for: the principal and the
            fifth above it about equal and ringing longest, the octave and the
            tenth a little under them, two more above those, and the head's
            own thud below the principal, loud at the strike and gone in a few
            hundred milliseconds. Over them the felt, a thud of low passed
            noise with a brighter edge on a harder strike.

            Built with the principal on C3, 130.81 Hz, three seconds long, to
            be played keytracked a twelfth up, which is C4, Vital's root. So
            the preview note plays it at its own speed, and a lower drum rings
            longer and a higher one shorter, as real ones do.
        */
        std::vector<float> kettle (std::mt19937& rng, const Request&)
        {
            struct Mode { float ratio, db, decay; };   // decay in dB a second
            static const Mode modes[] = {
                { 0.58f, -8.0f, 50.0f }, { 0.64f, -6.0f, 46.0f },     // the thud
                { 0.70f, -2.0f, 40.0f },
                { 1.00f,  0.0f, 16.0f }, { 1.50f, -1.0f, 12.0f },     // principal, fifth
                { 1.98f, -7.0f, 16.0f }, { 2.44f, -11.0f, 22.0f },    // octave, tenth
                { 2.90f, -15.0f, 28.0f }, { 3.37f, -18.0f, 34.0f },
                { 2.18f, -18.0f, 38.0f }, { 2.83f, -20.0f, 45.0f },   // off the series
                { 3.83f, -22.0f, 34.0f }, { 4.32f, -24.0f, 38.0f },
                { 4.80f, -26.0f, 40.0f },
            };
            // How hard the strike is. A harder one brings the upper modes up
            // with the felt's edge, as the loud recordings did by 6 to 10 dB.
            const auto edge = (double) uniform (rng, 0.0f, 1.0f);
            const auto length = kRate * 3;
            const auto base = 130.81;
            const auto pi = juce::MathConstants<double>::pi;
            std::vector<double> v ((size_t) length, 0.0);

            for (const auto& m : modes)
            {
                const auto hz = base * m.ratio * (double) uniform (rng, 0.99f, 1.01f);
                const auto lift = m.ratio > 2.5f ? 8.0 * edge : 0.0;
                const auto amp = std::pow (10.0, (m.db + lift + uniform (rng, -2.5f, 2.5f)) / 20.0);
                const auto rate = m.decay * uniform (rng, 0.8f, 1.25f);
                const auto perSample = std::pow (10.0, -rate / 20.0 / kRate);
                const auto phase = (double) uniform (rng, 0.0f, 6.2831853f);
                // A few milliseconds to speak, as a head does under a felt.
                const auto rise = (int) (kRate * 0.003);
                double level = amp;
                for (int i = 0; i < length; ++i)
                {
                    const auto onset = i < rise ? (double) i / rise : 1.0;
                    v[(size_t) i] += onset * level * std::sin (2.0 * pi * hz * i / kRate + phase);
                    level *= perSample;
                }
            }

            // The felt: a low passed thud, with an edge on a harder strike.
            const auto corner = 900.0 + 1600.0 * edge;
            const auto pole = std::exp (-2.0 * pi * corner / kRate);
            const auto tau = (double) uniform (rng, 0.006f, 0.014f) * kRate;
            const auto felt = 0.5 + 0.7 * edge;
            double l1 = 0.0, l2 = 0.0;
            for (int i = 0; i < (int) (kRate * 0.12); ++i)
            {
                l1 = (1.0 - pole) * (double) uniform (rng, -1.0f, 1.0f) + pole * l1;
                l2 = (1.0 - pole) * l1 + pole * l2;
                v[(size_t) i] += felt * 6.0 * l2 * std::exp (-i / tau);
            }
            return normalisedTo (std::move (v), 0.9);
        }

        /*  Beads in a shell, the way Perry Cook's PhISEM shakers make them.

            A shaker's sound is many small collisions at random, each a burst
            of noise that dies in a fraction of a millisecond, all rung through
            the resonance of the shell. Cook's models give the numbers: a
            maraca's shell at 3.2 kHz with a pole radius of 0.96, a cabasa's at
            3 kHz and 0.7, a sekere's at 5.5 kHz and 0.6, each burst decaying by
            0.95 or 0.96 a sample. The shaking itself, how hard and when, is
            left to the patch, so this is a steady shake that loops: plain noise
            under a filter is a hiss, where this has grain.
        */
        std::vector<float> beads (std::mt19937& rng, const Request&)
        {
            const auto shell = (double) uniform (rng, 2800.0f, 5800.0f);
            const auto radius = (double) uniform (rng, 0.60f, 0.96f);
            const auto rate = (double) uniform (rng, 500.0f, 1600.0f);     // collisions a second
            const auto burstDecay = (double) uniform (rng, 0.94f, 0.965f);

            const auto w = 2.0 * juce::MathConstants<double>::pi * shell / kRate;
            const auto a1 = 2.0 * radius * std::cos (w);
            const auto a2 = radius * radius;

            std::vector<float> a ((size_t) kLength, 0.0f);
            double energy = 0.0, x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
            // One pass to settle the filter, the second kept, so the loop's
            // first sample follows on from a shell already ringing.
            for (int pass = 0; pass < 2; ++pass)
                for (int i = 0; i < kLength; ++i)
                {
                    if (uniform (rng) < rate / kRate)
                        energy += (double) uniform (rng, 0.3f, 1.0f);
                    const auto x = energy * (double) uniform (rng, -1.0f, 1.0f);
                    energy *= burstDecay;
                    const auto y = x - x2 + a1 * y1 - a2 * y2;
                    x2 = x1; x1 = x; y2 = y1; y1 = y;
                    if (pass == 1)
                        a[(size_t) i] = (float) y;
                }

            double power = 0.0;
            for (const auto v : a)
                power += (double) v * v;
            const auto rms = std::sqrt (power / kLength);
            const auto gain = rms > 1.0e-9 ? 0.25 / rms : 0.0;
            for (auto& v : a)
                v = juce::jlimit (-0.95f, 0.95f, (float) (v * gain));
            return a;
        }

        /*  A felt mallet on a drum head: a thud rather than a click, a short
            burst of noise under a low pass, gone in a few tens of milliseconds,
            with the rest of the second silent. */
        std::vector<float> mallet (std::mt19937& rng, const Request&)
        {
            const auto corner = (double) uniform (rng, 700.0f, 1800.0f);
            const auto tau = (double) uniform (rng, 0.008f, 0.020f) * kRate;
            const auto rise = (int) (kRate * 0.0015);
            const auto pole = std::exp (-2.0 * juce::MathConstants<double>::pi * corner / kRate);

            std::vector<float> a ((size_t) kLength, 0.0f);
            double lp1 = 0.0, lp2 = 0.0, peak = 1.0e-9;
            for (int i = 0; i < kLength; ++i)
            {
                const auto shape = (i < rise ? 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * i / rise) : 1.0)
                                   * std::exp (-(double) i / tau);
                lp1 = (1.0 - pole) * (double) uniform (rng, -1.0f, 1.0f) + pole * lp1;
                lp2 = (1.0 - pole) * lp1 + pole * lp2;
                a[(size_t) i] = (float) (shape * lp2);
                peak = std::max (peak, std::abs ((double) a[(size_t) i]));
            }
            for (auto& v : a)
                v = (float) (0.9 * v / peak);
            return a;
        }

        std::vector<float> thump (std::mt19937& rng, const Request& r)
        {
            std::vector<float> a ((size_t) kLength, 0.0f);

            // Low, and lower still when BRIGHT is down. Above about 90 Hz this
            // stops being weight and starts being a tom.
            const auto root = juce::jmap (r.bright, 0.0f, 1.0f, 33.0f, 88.0f)
                                  * uniform (rng, 0.9f, 1.15f);
            /*  Short, and not far.

                The drop has to land before the ear has time to follow it, or it
                stops reading as impact and starts reading as a slide. An
                exponential is most of the way down after about three times its
                constant, so twenty eight milliseconds is already an eighty
                millisecond fall. Starting nine times above the root made that
                fall audible as a glissando rather than as a hit.
            */
            const auto from = uniform (rng, 1.8f, 4.0f);
            const auto dropMs = juce::jmap (r.complexity, 0.0f, 1.0f, 5.0f, 26.0f)
                                    * uniform (rng, 0.8f, 1.3f);
            const auto decay = juce::jmap (r.complexity, 0.0f, 1.0f, 0.12f, 0.6f)
                                   * uniform (rng, 0.8f, 1.3f);
            // DIRT folds the sine over, which is how a clean 808 becomes a dirty
            // one without any of it becoming noise.
            const auto fold = 1.0f + juce::jmap (r.dirt, 0.0f, 1.0f, 0.0f, 3.5f);

            float phase = 0.0f;
            for (int i = 0; i < kLength; ++i)
            {
                const auto t = (float) i / (float) kRate;
                const auto hz = root * (1.0f + (from - 1.0f)
                                            * std::exp (-t / (dropMs * 0.001f)));
                phase += juce::MathConstants<float>::twoPi * hz / (float) kRate;
                if (phase > juce::MathConstants<float>::twoPi)
                    phase -= juce::MathConstants<float>::twoPi;

                const auto amp = std::exp (-t / decay);
                a[(size_t) i] = std::tanh (std::sin (phase) * fold) * amp;
            }

            /*  The click on the front.

                A bass with weight and no definition disappears on a small
                speaker, which is the whole reason anybody layers a sample under
                one. A few milliseconds of filtered noise puts it back.
            */
            if (uniform (rng) < 0.75f)
            {
                const auto clickMs = uniform (rng, 1.5f, 9.0f);
                const auto amount = uniform (rng, 0.1f, 0.45f);
                std::normal_distribution<float> white (0.0f, 0.5f);
                Resonator tick;
                tick.set (juce::jlimit (800.0f, 11000.0f,
                                        juce::jmap (r.bright, 0.0f, 1.0f, 1200.0f, 6500.0f)
                                            * uniform (rng, 0.7f, 1.6f)),
                          uniform (rng, 0.7f, 2.5f));

                const auto samples = (int) (clickMs * 0.001f * kRate);
                for (int i = 0; i < samples && i < kLength; ++i)
                {
                    const auto fade = 1.0f - (float) i / (float) samples;
                    a[(size_t) i] += tick (white (rng)) * fade * fade * amount;
                }
            }

            removeDcAndNormalise (a);
            return a;
        }

        nlohmann::json encode (const std::vector<float>& a, const juce::String& name,
                               int rate)
        {
            std::vector<juce::int16> pcm (a.size());
            for (size_t i = 0; i < a.size(); ++i)
                pcm[i] = (juce::int16) juce::jlimit (-32767, 32767,
                                                     (int) std::lround (a[i] * 32767.0f));

            nlohmann::json sample;
            sample["length"] = (int) pcm.size();
            sample["name"] = name.toStdString();
            sample["sample_rate"] = rate;
            sample["samples"] = juce::Base64::toBase64 (pcm.data(),
                                                        pcm.size() * sizeof (juce::int16))
                                    .toStdString();
            return sample;
        }
    }

    std::vector<std::string> modelNames()
    {
        return { "Bed", "Struck", "Pluck", "Grit", "Swell", "Voice", "Thump" };
    }

    Result create (std::mt19937& rng, const Request& request)
    {
        /*  Six models, chosen to be far apart rather than to cover a range.

            Two textures that could be mistaken for each other are worth less
            than one, so each of these differs from the others in a way the ear
            names immediately: dense against sparse, harmonic against
            inharmonic, rising against decaying, texture against voice.

            Which ones a style may draw from is about what the layer is for. A
            struck body under a pad is a bell in the middle of a chord, which is
            a second instrument rather than a texture. A bed under a bass never
            stops, and a bass has to.
        */
        struct Model
        {
            const char* name;
            std::vector<float> (*build) (std::mt19937&, const Request&);
            bool loop, keytrack;
            float lowLevel, highLevel;
            bool fullRate = false;   // looping, but kept at the full rate
        };

        //                name      build            loop   track  level range
        static const Model bed    { "Bed",    &noiseBed,      true,  false, 0.08f, 0.22f };
        static const Model struck { "Struck", &struckBody,    false, true,  0.18f, 0.46f };
        static const Model pluck  { "Pluck",  &pluckedString, false, true,  0.18f, 0.42f };
        static const Model gritty { "Grit",   &grit,          true,  false, 0.30f, 0.60f };
        static const Model rising { "Swell",  &swell,         false, false, 0.15f, 0.38f };
        static const Model vocal  { "Voice",  &voice,         true,  false, 0.08f, 0.20f };
        static const Model thumped{ "Thump",  &thump,         false, true,  0.20f, 0.50f };
        // Only for the drum kinds that ask for it by name.
        static const Model noise  { "Noise",  &whiteNoise,    true,  false, 0.30f, 0.70f, true };
        static const Model metal  { "Metal",  &metalCluster,  true,  false, 0.40f, 0.80f, true };
        static const Model beaded { "Beads",  &beads,         true,  false, 0.40f, 0.80f, true };
        static const Model plate  { "Cymbal", &cymbalPlate,   true,  false, 0.40f, 0.80f, true };
        static const Model slap   { "Beater", &beater,        false, false, 0.20f, 0.40f };
        static const Model head   { "Head",   &drumHead,      false, true,  0.30f, 0.50f };
        static const Model wood   { "Stick",  &stick,         false, false, 0.50f, 0.80f };
        static const Model drum   { "Kettle", &kettle,        false, true,  0.80f, 1.00f };
        static const Model felt   { "Mallet", &mallet,        false, false, 0.20f, 0.45f };

        std::vector<const Model*> allowed;
        if (request.style == "Bass")
        {
            // Weight, or the string that would be carrying it.
            allowed = { &thumped, &thumped, &pluck };
        }
        else if (request.struckOnly)
        {
            // Only what stops on its own. Everything that loops is out.
            allowed = { &struck, &pluck, &thumped };
        }
        else if (request.style == "Pad")
        {
            allowed = { &bed, &vocal, &rising, &gritty };
        }
        else if (request.style == "Sequence")
        {
            /*  Only what keeps sounding.

                A sequence is one held note with an LFO doing the playing, so
                the key goes down once and everything that fires on a key press
                fires once. A struck body or a plucked string under that is a
                loud attack on the first step and silence for the rest of the
                riff, which is heard as the layer dropping out rather than as a
                layer at all.

                These three run for as long as the key is held, so the body is
                under every step and not just the first.
            */
            allowed = { &bed, &gritty, &vocal };
        }
        else if (request.style == "Keys" || request.style == "Lead")
        {
            /*  Only what has a pitch.

                A bed, a crackle or a vowel under a played melody is a second
                sound sitting behind the note rather than part of it, and these
                are the two styles somebody plays a line on a key at a time.
                What is left is what a player would layer in by hand: an attack,
                or a string.
            */
            allowed = { &struck, &pluck };
            if (request.style == "Lead")
                allowed = { &pluck, &struck };
        }
        else
        {
            // SFX and Experiment are meant to be unplaceable, so they get all of it.
            allowed = { &bed, &struck, &pluck, &gritty, &rising, &vocal, &thumped };
        }

        static const Model* const everything[] = { &bed, &struck, &pluck,
                                                   &gritty, &rising, &vocal, &thumped, &noise, &metal,
                                                   &beaded, &felt, &plate, &slap, &head, &wood,
                                                   &drum };

        const Model* model = nullptr;
        if (! request.model.empty())
        {
            for (const auto* m : everything)
                if (request.model == m->name)
                    model = m;
        }
        if (model == nullptr)
            model = allowed[(size_t) pick (rng, 0, (int) allowed.size() - 1)];

        Result result;
        result.sample = encode (model->build (rng, request), model->name,
                                model->loop && ! model->fullRate ? kLoopRate : kRate);
        result.loop = model->loop;
        result.keytrack = model->keytrack;
        result.level = uniform (rng, model->lowLevel, model->highLevel);
        return result;
    }
}
