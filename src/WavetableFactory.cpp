#include "WavetableFactory.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <juce_core/juce_core.h>

namespace wavetable
{
    namespace
    {
        constexpr int kFrameSize = 2048;      // Vital's wave frame length
        constexpr int kMaxPosition = 256;     // keyframe positions run 0..256
        constexpr int kMaxHarmonic = 96;

        float uniform (std::mt19937& rng, float lo = 0.0f, float hi = 1.0f)
        {
            return std::uniform_real_distribution<float> (lo, hi) (rng);
        }

        int pick (std::mt19937& rng, int lo, int hi)
        {
            return std::uniform_int_distribution<int> (lo, hi) (rng);
        }

        /*  The shape families. These are the recognisable starting points a
            synth patch is usually built on, rather than arbitrary spectra: a
            table of pure noise is technically more varied and sounds like a
            fault.
        */
        enum class Character { fundamental, saw, square, pulse, formant, metallic, vocal, airy };

        struct Spectrum
        {
            Character character = Character::saw;
            float rolloff = 1.0f;       // higher drops the upper harmonics faster
            float duty = 0.5f;          // pulse width
            float tilt = 0.0f;          // extra emphasis on odd harmonics
            std::array<float, 3> formants { 3.0f, 7.0f, 13.0f };
            float formantWidth = 1.5f;
            float inharmonic = 0.0f;    // detunes partials off the harmonic series
            unsigned int phaseSeed = 0;
        };

        float harmonicAmplitude (const Spectrum& s, int h)
        {
            const auto n = (float) h;
            float amp = 0.0f;

            switch (s.character)
            {
                case Character::fundamental:
                    amp = h == 1 ? 1.0f : 0.22f / (n * n);
                    break;
                case Character::saw:
                    amp = 1.0f / n;
                    break;
                case Character::square:
                    amp = (h % 2 == 1) ? 1.0f / n : 0.0f;
                    break;
                case Character::pulse:
                    // A pulse of width d has a sin(pi*h*d) shaped spectrum, which
                    // is what gives the hollow, nasal quality as the width moves.
                    amp = std::abs (std::sin (juce::MathConstants<float>::pi * n * s.duty)) / n;
                    break;
                case Character::formant:
                case Character::vocal:
                {
                    for (auto centre : s.formants)
                    {
                        const auto d = (n - centre) / s.formantWidth;
                        amp += std::exp (-d * d);
                    }
                    amp /= n * 0.35f + 1.0f;
                    break;
                }
                case Character::metallic:
                    amp = (h % 3 == 0 || h % 5 == 0) ? 1.0f / std::sqrt (n) : 0.25f / n;
                    break;
                case Character::airy:
                    amp = std::exp (-0.06f * n) + 0.12f / n;
                    break;
            }

            if (s.tilt > 0.0f && h % 2 == 0)
                amp *= 1.0f - s.tilt;

            amp *= std::pow (1.0f / n, s.rolloff - 1.0f);
            return amp;
        }

        std::vector<float> renderFrame (const Spectrum& s)
        {
            std::vector<float> frame ((size_t) kFrameSize, 0.0f);
            std::mt19937 phaseRng (s.phaseSeed);

            for (int h = 1; h <= kMaxHarmonic; ++h)
            {
                const auto amp = harmonicAmplitude (s, h);
                if (amp < 1.0e-4f)
                    continue;

                // A little phase scatter stops every table looking like the same
                // sawtooth ramp, and matters audibly once the wave is folded or
                // phase shifted later in the chain.
                const auto phase = uniform (phaseRng, 0.0f, juce::MathConstants<float>::twoPi)
                                       * (s.character == Character::saw ? 0.15f : 1.0f);
                const auto ratio = (float) h * (1.0f + s.inharmonic * (float) (h - 1) * 0.004f);

                for (int i = 0; i < kFrameSize; ++i)
                {
                    const auto t = juce::MathConstants<float>::twoPi * ratio * (float) i
                                       / (float) kFrameSize;
                    frame[(size_t) i] += amp * std::sin (t + phase);
                }
            }

            float peak = 0.0f;
            for (auto v : frame)
                peak = std::max (peak, std::abs (v));
            if (peak > 1.0e-6f)
                for (auto& v : frame)
                    v /= peak;
            return frame;
        }

        std::string encodeFrame (const std::vector<float>& frame)
        {
            // Vital stores a keyframe as 2048 little-endian float32 in plain
            // base64, not the JUCE variant used by the plugin state chunk.
            return juce::Base64::toBase64 (frame.data(), frame.size() * sizeof (float))
                       .toStdString();
        }

        Spectrum baseSpectrumFor (std::mt19937& rng, const Request& r)
        {
            Spectrum s;
            s.phaseSeed = rng();

            /*  Style decides which shapes are on the table. A bass built from an
                airy shape has no fundamental to speak of, and a pad built from a
                metallic one is a bell, so the families are picked to match what
                presets in that style are actually made of.
            */
            std::vector<Character> palette;
            if (r.style == "Bass")
                palette = { Character::saw, Character::square, Character::fundamental,
                            Character::pulse };
            else if (r.style == "Lead")
                palette = { Character::saw, Character::square, Character::pulse,
                            Character::formant };
            else if (r.style == "Pad")
                palette = { Character::airy, Character::saw, Character::vocal,
                            Character::formant, Character::fundamental };
            else if (r.style == "Keys")
                palette = { Character::fundamental, Character::metallic, Character::saw,
                            Character::airy };
            else if (r.style == "Percussion")
                palette = { Character::metallic, Character::square, Character::pulse };
            else if (r.style == "SFX" || r.style == "Experiment")
                palette = { Character::metallic, Character::vocal, Character::pulse,
                            Character::formant, Character::airy };
            else
                palette = { Character::saw, Character::square, Character::pulse,
                            Character::airy, Character::formant };

            s.character = palette[(size_t) pick (rng, 0, (int) palette.size() - 1)];

            /*  BRIGHT tilts the harmonic rolloff. Below one the upper harmonics
                survive, above one they fall away.

                The range sits darker than it first looks it should. A rolloff of
                one is a plain sawtooth, and centring the slider there made every
                style read brighter than the library: generated pads came back at
                3.4 kHz against a hand-made median of 1.4. Real wavetables are
                mostly gentler than a raw saw, so the middle of the slider is too.
            */
            s.rolloff = juce::jmap (r.bright, 0.0f, 1.0f, 1.95f, 0.72f)
                            + uniform (rng, -0.12f, 0.12f);
            s.duty = uniform (rng, 0.12f, 0.5f);
            s.tilt = uniform (rng, 0.0f, 0.35f);
            /*  Partials off the harmonic series are only wanted where the
                sound is meant to be unplaceable.

                Detuning them is what a bell or a scrape is made of, and it is
                also exactly what stops a note being a note: shift the partials
                and the ear picks a pitch that is not the one on the keyboard.
                A lead measured 20 semitones above the key it was given, at a
                ratio of 3.25, which is not a harmonic of anything. Instruments
                someone plays a melody on get a clean harmonic series.
            */
            const auto unpitched = r.style == "SFX" || r.style == "Experiment"
                                       || r.style == "Percussion";
            s.inharmonic = unpitched ? uniform (rng, 0.0f, 1.0f) : 0.0f;
            s.formantWidth = uniform (rng, 0.8f, 3.0f);
            for (auto& f : s.formants)
                f = uniform (rng, 1.5f, 22.0f);
            std::sort (s.formants.begin(), s.formants.end());
            return s;
        }

        /*  Every key Vital reads has to be present.

            Its loaders mostly index straight into the json rather than checking
            first, so a component missing one field does not degrade gracefully,
            it fails the whole preset with "preset is corrupted". WaveSource in
            particular reads "interpolation" unguarded, which is exactly how the
            first version of this broke every patch it produced.
        */
        nlohmann::json makeComponent (const char* type, nlohmann::json keyframes)
        {
            nlohmann::json c;
            c["type"] = type;
            c["interpolation_style"] = 1;
            c["keyframes"] = std::move (keyframes);
            return c;
        }

        nlohmann::json makeWaveSource (nlohmann::json keyframes)
        {
            auto c = makeComponent ("Wave Source", std::move (keyframes));
            c["interpolation"] = 0;     // read unguarded by WaveSource::jsonToState
            return c;
        }
    }

    nlohmann::json create (std::mt19937& rng, const Request& request)
    {
        auto spectrum = baseSpectrumFor (rng, request);

        /*  More keyframes means the table morphs as the wave frame moves, which
            is most of what makes a patch sound alive rather than sampled. A
            single frame is kept in the mix on purpose: some sounds want to sit
            still.
        */
        const auto ceiling = juce::jlimit (1, 8, juce::roundToInt (
            juce::jmap (request.complexity, 0.0f, 1.0f, 2.0f, 8.0f)));
        const auto frames = request.move > 0.5f ? pick (rng, juce::jmin (3, ceiling), ceiling)
                                                : pick (rng, 1, juce::jmax (1, ceiling / 2));

        // How far the spectrum drifts from one end of the table to the other.
        const auto drift = juce::jmap (request.move, 0.0f, 1.0f, 0.15f, 1.0f);
        /*  How far the timbre drifts across the table.

            This used to reach 0.45 of the starting rolloff, which meant the far
            end of a table could be far brighter than the BRIGHT slider ever
            asked for. Sweeping the wave frame then walked straight past the
            brightness the style wanted, and basses were being rejected for it.
            Morphing still happens, it just cannot outrun the setting.
        */
        const auto endRolloff = spectrum.rolloff * juce::jmap (uniform (rng), 0.78f, 1.5f);
        const auto endDuty = uniform (rng, 0.05f, 0.5f);

        nlohmann::json keyframes = nlohmann::json::array();
        for (int i = 0; i < frames; ++i)
        {
            const auto t = frames > 1 ? (float) i / (float) (frames - 1) : 0.0f;

            auto s = spectrum;
            s.rolloff = juce::jmap (t * drift, spectrum.rolloff, endRolloff);
            s.duty = juce::jmap (t * drift, spectrum.duty, endDuty);
            s.tilt = spectrum.tilt * (1.0f - t * drift * 0.7f);
            s.phaseSeed = spectrum.phaseSeed + (unsigned int) i * 7919u;

            nlohmann::json kf;
            kf["position"] = frames > 1
                ? (int) std::lround (t * (float) kMaxPosition) : 0;
            kf["wave_data"] = encodeFrame (renderFrame (s));
            keyframes.push_back (std::move (kf));
        }

        nlohmann::json components = nlohmann::json::array();
        components.push_back (makeWaveSource (std::move (keyframes)));

        // A modifier on top costs nothing and is where a lot of the character
        // lives. Vital's own components are algorithms, not data, so these are
        // free to use.
        if (request.dirt > 0.55f && uniform (rng) < request.dirt)
        {
            nlohmann::json kf;
            kf["position"] = 0;
            kf["fold_boost"] = juce::jmap (request.dirt, 0.55f, 1.0f, 1.05f, 3.4f);
            nlohmann::json arr = nlohmann::json::array();
            arr.push_back (std::move (kf));
            components.push_back (makeComponent ("Wave Folder", std::move (arr)));
        }
        else if (uniform (rng) < 0.15f + request.complexity * 0.45f)
        {
            nlohmann::json a, b;
            a["position"] = 0;
            a["horizontal_power"] = uniform (rng, -4.0f, 4.0f);
            a["vertical_power"] = uniform (rng, -4.0f, 4.0f);
            b["position"] = kMaxPosition;
            b["horizontal_power"] = uniform (rng, -4.0f, 4.0f);
            b["vertical_power"] = uniform (rng, -4.0f, 4.0f);
            nlohmann::json arr = nlohmann::json::array();
            arr.push_back (std::move (a));
            arr.push_back (std::move (b));
            auto warp = makeComponent ("Wave Warp", std::move (arr));
            warp["horizontal_asymmetric"] = uniform (rng) < 0.5f;
            warp["vertical_asymmetric"] = uniform (rng) < 0.5f;
            components.push_back (std::move (warp));
        }

        nlohmann::json group;
        group["components"] = std::move (components);

        nlohmann::json groups = nlohmann::json::array();
        groups.push_back (std::move (group));

        nlohmann::json table;
        table["author"] = "";
        table["full_normalize"] = true;
        table["remove_all_dc"] = true;
        table["name"] = "Generated " + std::to_string (request.oscillator);
        table["version"] = "1.0.7";
        table["groups"] = std::move (groups);
        return table;
    }

    nlohmann::json createStepShape (std::mt19937& rng)
    {
        /*  Vital's line is a list of (x, y) pairs, and a pair of points sharing
            an x is a vertical jump. Two points per step, so each value is held
            flat and then jumped from.
        */
        const auto steps = pick (rng, 2, 8);

        nlohmann::json xy = nlohmann::json::array();
        nlohmann::json powers = nlohmann::json::array();
        int count = 0;

        float previous = uniform (rng);
        for (int i = 0; i < steps; ++i)
        {
            const auto x0 = (float) i / (float) steps;
            const auto x1 = (float) (i + 1) / (float) steps;
            const auto value = i == 0 ? previous : uniform (rng);

            xy.push_back (x0); xy.push_back (value);
            xy.push_back (x1); xy.push_back (value);
            powers.push_back (0.0f);
            powers.push_back (0.0f);
            count += 2;
            previous = value;
        }

        nlohmann::json line;
        line["name"] = "Steps";
        line["num_points"] = count;
        line["points"] = std::move (xy);
        line["powers"] = std::move (powers);
        line["smooth"] = false;
        return line;
    }

    nlohmann::json createLfoShape (std::mt19937& rng, float move)
    {
        // Vital's LFO line is a list of (x, y) pairs with a curve power each.
        const auto points = pick (rng, 3, move > 0.6f ? 7 : 4);

        nlohmann::json xy = nlohmann::json::array();
        nlohmann::json powers = nlohmann::json::array();

        for (int i = 0; i < points; ++i)
        {
            const auto x = (float) i / (float) (points - 1);
            // The line has to start and end at the same height or the LFO steps
            // every time it wraps round.
            const auto y = (i == 0 || i == points - 1) ? 1.0f : uniform (rng, 0.0f, 1.0f);
            xy.push_back (x);
            xy.push_back (y);
            powers.push_back (uniform (rng, -3.0f, 3.0f));
        }

        nlohmann::json line;
        line["name"] = "Generated";
        line["num_points"] = points;
        line["points"] = std::move (xy);
        line["powers"] = std::move (powers);
        line["smooth"] = uniform (rng) < 0.4f;
        return line;
    }
}
