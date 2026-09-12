#include "SampleFactory.h"

#include <algorithm>
#include <cmath>
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
        };

        //                name      build            loop   track  level range
        static const Model bed    { "Bed",    &noiseBed,      true,  false, 0.08f, 0.22f };
        static const Model struck { "Struck", &struckBody,    false, true,  0.18f, 0.46f };
        static const Model pluck  { "Pluck",  &pluckedString, false, true,  0.18f, 0.42f };
        static const Model gritty { "Grit",   &grit,          true,  false, 0.30f, 0.60f };
        static const Model rising { "Swell",  &swell,         false, false, 0.15f, 0.38f };
        static const Model vocal  { "Voice",  &voice,         true,  false, 0.08f, 0.20f };
        static const Model thumped{ "Thump",  &thump,         false, true,  0.20f, 0.50f };

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
        else if (request.style == "Keys" || request.style == "Lead"
                 || request.style == "Sequence")
        {
            /*  Only what has a pitch.

                A bed, a crackle or a vowel under a played melody is a second
                sound sitting behind the note rather than part of it, and these
                are the three styles somebody plays a line on. What is left is
                what a player would layer in by hand: an attack, or a string.
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
                                                   &gritty, &rising, &vocal, &thumped };

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
                                model->loop ? kLoopRate : kRate);
        result.loop = model->loop;
        result.keytrack = model->keytrack;
        result.level = uniform (rng, model->lowLevel, model->highLevel);
        return result;
    }
}
