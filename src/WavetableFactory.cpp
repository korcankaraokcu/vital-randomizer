#include "WavetableFactory.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
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
        enum class Character { fundamental, saw, square, pulse, formant, metallic, vocal, airy, membrane, sine };

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
                case Character::sine:
                    return h == 1 ? 1.0f : 0.0f;
                case Character::membrane:
                {
                    /*  A timpani's preferred modes, principal, fifth, octave
                        and tenth, as harmonics two to five, in 5 to 4 to 3 to
                        1, with a trace of the note below them. Returned as they
                        are: a rolloff or a tilt on top would change the drum's
                        own balance. */
                    static const float modes[] = { 0.10f, 1.0f, 0.8f, 0.6f, 0.2f, 0.05f };
                    return h <= 6 ? modes[h - 1] : 0.0f;
                }
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
            /*  A drum kind's own body, applied over the style's draws rather
                than instead of them, so every other style draws exactly the
                numbers it always did. */
            if (! r.character.empty())
            {
                static const std::pair<const char*, Character> names[] = {
                    { "fundamental", Character::fundamental }, { "saw", Character::saw },
                    { "square", Character::square }, { "pulse", Character::pulse },
                    { "formant", Character::formant }, { "metallic", Character::metallic },
                    { "vocal", Character::vocal }, { "airy", Character::airy },
                    { "membrane", Character::membrane }, { "sine", Character::sine } };
                for (const auto& n : names)
                    if (r.character == n.first)
                        s.character = n.second;
            }
            if (r.inharmonicLow >= 0.0f)
                s.inharmonic = uniform (rng, r.inharmonicLow, juce::jmax (r.inharmonicLow, r.inharmonicHigh));
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
        /*  Except on a pure sine, which asked to be one. A tom's body is,
            and a fold or a warp on it put a second harmonic 9 dB under the
            note, lines a drum head does not have. */
        if (request.character == "sine")
        {
        }
        else if (request.dirt > 0.55f && uniform (rng) < request.dirt)
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

    std::vector<std::string> stepGestureNames()
    {
        return { "run", "arpeggio", "wander", "motif", "pedal" };
    }

    nlohmann::json createStepShape (std::mt19937& rng,
                                    const std::vector<float>& degrees, float depth,
                                    int gesture, float randomStep)
    {
        /*  Vital's line is a list of (x, y) pairs, and a pair of points sharing
            an x is a vertical jump. Two points per step, so each value is held
            flat and then jumped from.
        */
        /*  How much of the scale has to actually turn up.

            A maqam is seven notes and what identifies it is the particular
            ones, so a riff that lands on two of them is not in that maqam in
            any way a listener could tell. A triad is three notes and a riff on
            two of those is missing a third of the chord. Either way the scale
            was chosen and then not played.

            So every scale carries a floor, and it goes up with the size of the
            scale rather than sitting at one number, because three notes out of
            three is the whole chord while three out of seven is a fragment:

                3 or 4 notes in the scale  ->  at least 3 of them
                5 or 6                     ->  at least 4
                7 or more                  ->  at least 5

            Counted by pitch class, so the same note an octave up does not
            count twice. A triad written as root, third, fifth, octave is three
            notes and is asked for three.
        */
        const auto pitchClass = [] (float semitones)
        {
            // In quarter tones, as integers, because a maqam has notes between
            // the semitones and floats do not compare cleanly.
            return (int) std::lround (std::fmod ((double) semitones + 24.0, 12.0) * 4.0);
        };

        std::set<int> scaleClasses;
        for (const auto d : degrees)
            scaleClasses.insert (pitchClass (d));

        const auto wantedNotes = juce::jlimit (3, 5, ((int) scaleClasses.size() + 3) / 2);

        /*  Two is a trill rather than a sequence, and a pattern also needs one
            step more than the notes it has to contain, or every step is a new
            note and what comes out is a scale exercise rather than a riff.
            Eight covers a bar of quavers, which is as long as a step pattern
            reads as one idea.
        */
        const auto steps = pick (rng, juce::jmax (3, wantedNotes + 1), 8);

        nlohmann::json xy = nlohmann::json::array();
        nlohmann::json powers = nlohmann::json::array();
        int count = 0;

        /*  Move like a line, not like a shuffle.

            Choosing the right notes is only half of it. Dealt out and shuffled,
            a scale still leaps about at random, and what that sounds like is a
            scale being demonstrated rather than a part being played.

            What separates the two is contour. A melody mostly moves by one step
            of the scale, leaps now and then, repeats figures, and keeps coming
            back to the tonic. So the shape is chosen first and the degrees are
            walked rather than drawn: a run, an arpeggio, a wander, a repeated
            motif, or a pedal against the root. Each is a thing players do, and
            between them they cover most of what a sequence ever is.
        */
        juce::String shapeName;
        std::vector<float> chosen;
        chosen.reserve ((size_t) steps);

        if (degrees.empty() || depth <= 1.0e-4f)
        {
            // No ladder, no contour, no gesture. Each step is its own draw.
            shapeName = "random";

            /*  A whole semitone is left to Vital, which snaps the draw for us
                and is what this did before there were scales at all. A grid
                narrower than that is below what its quantiser can say, so the
                step is put exactly where it belongs instead, by the same
                inversion the scales use.
            */
            const auto placed = randomStep > 1.0e-4f
                                && std::abs (randomStep - 1.0f) > 1.0e-4f;
            // An octave either side of the note played, which is what the
            // depth allows and what the two octave ladder covers.
            const auto rungs = placed ? (int) std::lround (24.0f / randomStep) : 0;

            for (int i = 0; i < steps; ++i)
            {
                if (! placed)
                {
                    chosen.push_back (uniform (rng));
                    continue;
                }
                const auto wanted = -12.0f + randomStep * (float) pick (rng, 0, rungs);
                chosen.push_back (juce::jlimit (0.0f, 1.0f,
                                                0.5f - wanted / (96.0f * depth)));
            }
        }
        else
        {
            /*  Two octaves to move in, the scale and the one below it, so a run
                has somewhere to go and a leap has somewhere to land. The depth
                allows an octave either side of the played note, and this fills
                exactly that.
            */
            std::vector<float> ladder;
            for (const auto d : degrees)
                ladder.push_back (d - 12.0f);
            for (const auto d : degrees)
                ladder.push_back (d);
            std::sort (ladder.begin(), ladder.end());

            const auto rungs = (int) ladder.size();
            // Where the played note itself sits, which is what a line gravitates to.
            auto home = 0;
            for (int i = 0; i < rungs; ++i)
                if (std::abs (ladder[(size_t) i]) < std::abs (ladder[(size_t) home]))
                    home = i;

            /*  One gesture per phrase, not per sequence.

                Five shapes is a small vocabulary, and a sequence that is one of
                them from beginning to end gives itself away quickly. Music does
                not work that way either: a riff runs up and then sits on a
                pedal, or states a motif and answers it with a descent.

                So the steps are cut into one to three phrases and each is given
                its own shape, each starting from wherever the last one finished
                so the line stays joined up. Five gestures across three phrases
                is a hundred and fifty five orderings before the lengths and the
                scale are counted, which is enough to stop the ear predicting it.
            */
            /*  Named as it is built.

                Vital shows the LFO's name in its editor, so writing the phrases
                into it means the shape of a line can be read off the patch
                rather than worked out from the points. "run + pedal" is a
                better label than "Steps" for something that is a run and then a
                pedal.
            */
            juce::StringArray phraseNames;

            const auto walk = [&] (int gesture, int from, int count,
                                   std::vector<int>& path) -> int
            {
                phraseNames.add (stepGestureNames()[(size_t) juce::jlimit (0, 4, gesture)]);
                auto at = juce::jlimit (0, rungs - 1, from);

                switch (gesture)
                {
                    case 0:
                    {
                        // A run. Straight up or down the scale, which is the most
                        // obviously musical thing a sequence can do.
                        const auto up = uniform (rng) < 0.5f;
                        for (int i = 0; i < count; ++i)
                        {
                            path.push_back (at);
                            at += up ? 1 : -1;
                            // Turn around rather than run off the end.
                            if (at < 0 || at >= rungs)
                                at = juce::jlimit (0, rungs - 1, at - (up ? 2 : -2));
                        }
                        break;
                    }
                    case 1:
                    {
                        // An arpeggio, every other degree, which on most of these
                        // scales is the chord inside them.
                        const auto up = uniform (rng) < 0.5f;
                        for (int i = 0; i < count; ++i)
                        {
                            path.push_back (at);
                            at += up ? 2 : -2;
                            if (at < 0 || at >= rungs)
                                at = juce::jlimit (0, rungs - 1, home);
                        }
                        break;
                    }
                    case 2:
                    {
                        /*  A wander. Mostly one step, sometimes two, occasionally
                            a leap, and pulled back toward the root when it strays,
                            which is roughly what a melody does when nobody is
                            counting.
                        */
                        for (int i = 0; i < count; ++i)
                        {
                            path.push_back (at);
                            const auto roll = uniform (rng);
                            auto move = roll < 0.55f ? 1 : roll < 0.8f ? 2 : pick (rng, 3, 5);
                            if (uniform (rng) < 0.5f)
                                move = -move;
                            const auto away = at - home;
                            if (std::abs (away) > 2 && uniform (rng) < 0.6f)
                                move = away > 0 ? -std::abs (move) : std::abs (move);
                            at = juce::jlimit (0, rungs - 1, at + move);
                        }
                        break;
                    }
                    case 3:
                    {
                        /*  A motif, stated and restated. Two or three notes
                            repeated with the whole figure nudged along, which is
                            what makes a sequence sound composed rather than
                            ongoing.
                        */
                        const auto figure = pick (rng, 2, 3);
                        std::vector<int> seed;
                        auto cursor = at;
                        for (int i = 0; i < figure; ++i)
                        {
                            seed.push_back (cursor);
                            /*  Never nothing.

                                Drawn from minus two to two, the step between the
                                figure's notes could be zero, and a figure whose
                                notes are the same note is not a figure. Restated
                                twice that gave four of the same note in a row.
                            */
                            const auto away = pick (rng, 1, 2) * (uniform (rng) < 0.5f ? -1 : 1);
                            cursor = juce::jlimit (0, rungs - 1, cursor + away);
                        }
                        auto shift = 0;
                        for (int i = 0; i < count; ++i)
                        {
                            if (i > 0 && i % figure == 0)
                                shift += pick (rng, -1, 1);
                            at = juce::jlimit (0, rungs - 1, seed[(size_t) (i % figure)] + shift);
                            path.push_back (at);
                        }
                        break;
                    }
                    default:
                    {
                        /*  A pedal. The root on every other step with the scale
                            moving against it, which is most of what a bassline
                            or an acid line is doing.

                            The answering note climbs rather than being redrawn
                            each time. Redrawn, it kept landing on the same
                            degree, and a pedal that answers itself with one
                            note is two notes long however many steps it has.
                        */
                        auto answer = pick (rng, 1, 3);
                        for (int i = 0; i < count; ++i)
                        {
                            if (i % 2 == 0)
                            {
                                at = home;
                            }
                            else
                            {
                                at = juce::jlimit (0, rungs - 1, home + answer);
                                answer += pick (rng, 1, 2);
                                if (home + answer >= rungs)
                                    answer = pick (rng, 1, 3);
                            }
                            path.push_back (at);
                        }
                        break;
                    }
                }
                return at;
            };

            std::vector<int> path;
            path.reserve ((size_t) steps);

            if (gesture >= 0 && gesture < 5)
            {
                // Asked for one shape, so the whole line is that shape.
                walk (gesture, home + pick (rng, -1, 1), steps, path);
            }
            else
            {
                // Short lines stay in one phrase; there is nothing to divide.
                const auto roll = uniform (rng);
                auto phrases = steps < 4 ? 1 : roll < 0.35f ? 1 : roll < 0.78f ? 2 : 3;
                phrases = juce::jmin (phrases, steps / 2);
                phrases = juce::jmax (1, phrases);

                auto at = home + pick (rng, -1, 1);
                auto left = steps;
                for (int p = 0; p < phrases; ++p)
                {
                    const auto remaining = phrases - p - 1;
                    const auto count = p == phrases - 1
                                           ? left
                                           : pick (rng, 2, juce::jmax (2, left - remaining * 2));
                    at = walk (pick (rng, 0, 4), at, count, path);
                    left -= count;
                    if (left <= 0)
                        break;
                }
            }

            shapeName = phraseNames.joinIntoString (" + ");

            /*  Then make sure the scale is actually in there.

                The gestures are contours and none of them is counting notes.
                A pedal answers the root, a motif restates itself, and a wander
                that turns around early covers three rungs out of fourteen. Any
                of those can finish a phrase having played two degrees of a
                seven note maqam, and the scale is then a label on a patch
                rather than something a listener can hear.

                So the line is checked against the floor and short of it the
                repeats are spent on degrees that have not been heard yet. The
                first time each note appears is left alone, which is what keeps
                the opening of the phrase intact, and the nearest unheard rung
                is used so a repair is a step rather than a leap.
            */
            const auto classAt = [&] (int rung)
            {
                return pitchClass (ladder[(size_t) juce::jlimit (0, rungs - 1, rung)]);
            };

            const auto heard = [&]
            {
                std::set<int> present;
                for (const auto rung : path)
                    present.insert (classAt (rung));
                return present;
            };

            for (int guard = 0; guard < 32; ++guard)
            {
                const auto present = heard();
                if ((int) present.size() >= wantedNotes)
                    break;

                // The first step that says nothing new. Its own first outing
                // stays; this is the one that only repeats it.
                auto spare = -1;
                std::set<int> seen;
                for (size_t i = 0; i < path.size(); ++i)
                    if (! seen.insert (classAt (path[i])).second)
                    {
                        spare = (int) i;
                        break;
                    }
                if (spare < 0)
                    break;

                // The nearest rung carrying a degree nobody has played.
                auto best = -1;
                for (int rung = 0; rung < rungs; ++rung)
                {
                    if (present.count (classAt (rung)) > 0)
                        continue;
                    if (best < 0 || std::abs (rung - path[(size_t) spare])
                                        < std::abs (best - path[(size_t) spare]))
                        best = rung;
                }
                if (best < 0)
                    break;

                path[(size_t) spare] = best;
            }

            for (const auto rung : path)
            {
                const auto wanted = ladder[(size_t) juce::jlimit (0, rungs - 1, rung)];
                chosen.push_back (juce::jlimit (0.0f, 1.0f,
                                                0.5f - wanted / (96.0f * depth)));
            }
        }

        int taken = 0;
        const auto place = [&] () -> float
        { return chosen[(size_t) (taken++ % (int) chosen.size())]; };

        float previous = place();
        for (int i = 0; i < steps; ++i)
        {
            const auto x0 = (float) i / (float) steps;
            const auto x1 = (float) (i + 1) / (float) steps;
            const auto value = i == 0 ? previous : place();

            xy.push_back (x0); xy.push_back (value);
            xy.push_back (x1); xy.push_back (value);
            powers.push_back (0.0f);
            powers.push_back (0.0f);
            count += 2;
            previous = value;
        }

        nlohmann::json line;
        line["name"] = shapeName.isEmpty() ? std::string ("Steps") : shapeName.toStdString();
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
