#include "Archetypes.h"

#include <regex>

namespace archetype
{
    namespace
    {
        /*  Shared ground for everything pitched.

            Oscillator one carries the note, oscillator two sits an octave below
            as body, the filter is on and the compressor is on because nearly
            every preset in a real library switches it on regardless of style.
        */
        std::vector<Setting> common()
        {
            return {
                { "osc_1_on", 1.0 }, { "osc_1_level", 0.62 },
                { "osc_2_on", 1.0 }, { "osc_2_level", 0.32 }, { "osc_2_transpose", -12.0 },
                { "osc_3_on", 0.0 }, { "sample_on", 0.0 },
                { "filter_1_on", 1.0 }, { "filter_1_model", 0.0 },
                { "compressor_on", 1.0 },
                { "volume", 5473.04 },
            };
        }

        std::vector<Setting> with (std::vector<Setting> base, std::vector<Setting> extra)
        {
            base.insert (base.end(), extra.begin(), extra.end());
            return base;
        }

        std::vector<Archetype> build()
        {
            std::vector<Archetype> list;

            /*  Bass. Monophonic, low, and struck rather than held: the amp
                envelope falls away while the key is still down and a decaying
                envelope closes the filter behind it, which is where the pluck
                comes from. Reverb stays off more often than not, because a
                bass with a tail on it stops sitting under a mix.
            */
            list.push_back ({ "Bass",
                with (common(), {
                    { "osc_1_unison_voices", 1.0 }, { "osc_1_unison_detune", 1.6 },
                    { "osc_2_level", 0.38 },
                    { "polyphony", 1.0 }, { "legato", 1.0 },
                    { "portamento_time", -8.5 },
                    { "filter_1_cutoff", 50.0 }, { "filter_1_resonance", 0.14 },
                    { "filter_1_drive", 3.0 }, { "filter_1_keytrack", 0.3 },
                    { "env_1_attack", 0.0 }, { "env_1_decay", 1.18 },
                    { "env_1_sustain", 0.0 }, { "env_1_release", 0.35 },
                    { "env_2_attack", 0.0 }, { "env_2_decay", 0.85 },
                    { "env_2_sustain", 0.0 }, { "env_2_release", 0.4 },
                    { "distortion_on", 1.0 }, { "distortion_drive", 5.7 },
                    { "distortion_mix", 0.7 },
                    { "reverb_on", 0.0 }, { "reverb_dry_wet", 0.06 },
                    { "eq_on", 1.0 },
                }),
                {
                    { "env_2", "filter_1_cutoff", 0.38f, false },
                    { "env_1", "distortion_drive", 0.2f, false },
                    { "lfo_1", "osc_1_wave_frame", 0.25f, false },
                } });

            /*  Lead. The brightest style in the library by a wide margin, and
                the one meant to carry a melody, so it holds the note, runs
                unison for width and sits in reverb and delay.
            */
            list.push_back ({ "Lead",
                with (common(), {
                    { "osc_1_unison_voices", 6.0 }, { "osc_1_unison_detune", 2.2 },
                    { "osc_2_level", 0.48 }, { "osc_2_transpose", 0.0 },
                    { "osc_2_unison_voices", 4.0 },
                    { "polyphony", 8.0 },
                    { "filter_1_cutoff", 86.0 }, { "filter_1_resonance", 0.08 },
                    { "env_1_attack", 0.1 }, { "env_1_decay", 1.0 },
                    { "env_1_sustain", 0.95 }, { "env_1_release", 0.9 },
                    { "env_2_attack", 0.0 }, { "env_2_decay", 0.9 },
                    { "env_2_sustain", 0.2 }, { "env_2_release", 0.6 },
                    { "distortion_on", 1.0 }, { "distortion_drive", 3.3 },
                    { "distortion_mix", 0.5 },
                    { "reverb_on", 1.0 }, { "reverb_dry_wet", 0.31 },
                    { "reverb_decay_time", 0.72 },
                    { "delay_on", 1.0 }, { "delay_dry_wet", 0.12 },
                    { "eq_on", 1.0 },
                }),
                {
                    { "env_2", "filter_1_cutoff", 0.30f, false },
                    { "lfo_1", "osc_1_wave_frame", 0.28f, false },
                    { "env_2", "osc_2_level", 0.2f, false },
                } });

            /*  Keys. Struck like a bass but allowed to be held, so the amp
                envelope decays to a low sustain rather than to nothing. The
                library gives this style the most resonant filter of the
                playable ones and a slow LFO on the cutoff rather than an
                envelope.
            */
            list.push_back ({ "Keys",
                with (common(), {
                    { "osc_1_unison_voices", 4.0 }, { "osc_1_unison_detune", 2.1 },
                    { "osc_2_level", 0.22 },
                    { "polyphony", 8.0 },
                    { "filter_1_cutoff", 74.0 }, { "filter_1_resonance", 0.22 },
                    { "filter_1_keytrack", 0.5 },
                    { "env_1_attack", 0.0 }, { "env_1_decay", 1.12 },
                    { "env_1_sustain", 0.18 }, { "env_1_release", 0.75 },
                    { "env_2_attack", 0.0 }, { "env_2_decay", 0.8 },
                    { "env_2_sustain", 0.05 }, { "env_2_release", 0.5 },
                    { "reverb_on", 1.0 }, { "reverb_dry_wet", 0.25 },
                    { "reverb_decay_time", 1.32 },
                    { "delay_on", 1.0 }, { "delay_dry_wet", 0.14 },
                    { "chorus_on", 1.0 }, { "chorus_dry_wet", 0.17 },
                    { "eq_on", 1.0 },
                }),
                {
                    { "lfo_1", "filter_1_cutoff", 0.24f, true },
                    { "env_2", "osc_2_level", 0.3f, false },
                    { "random_1", "osc_1_tune", 0.04f, true },
                } });

            /*  Pad. Slow in, slow out, wide. The longest attack and release of
                any style, the most unison, and reverb in almost every preset.
            */
            list.push_back ({ "Pad",
                with (common(), {
                    { "osc_1_unison_voices", 7.0 }, { "osc_1_unison_detune", 2.6 },
                    { "osc_2_level", 0.4 }, { "osc_2_transpose", 0.0 },
                    { "osc_2_unison_voices", 5.0 },
                    { "osc_3_on", 1.0 }, { "osc_3_level", 0.25 }, { "osc_3_transpose", -12.0 },
                    { "polyphony", 8.0 },
                    { "filter_1_cutoff", 68.0 }, { "filter_1_resonance", 0.12 },
                    { "env_1_attack", 0.8 }, { "env_1_decay", 1.2 },
                    { "env_1_sustain", 1.0 }, { "env_1_release", 1.1 },
                    { "env_2_attack", 0.4 }, { "env_2_decay", 1.0 },
                    { "env_2_sustain", 0.6 }, { "env_2_release", 0.9 },
                    { "reverb_on", 1.0 }, { "reverb_dry_wet", 0.3 },
                    { "reverb_decay_time", 1.99 }, { "reverb_size", 0.7 },
                    { "chorus_on", 1.0 }, { "chorus_dry_wet", 0.14 },
                    { "delay_on", 1.0 }, { "delay_dry_wet", 0.11 },
                    { "eq_on", 1.0 },
                }),
                {
                    { "env_2", "filter_1_cutoff", 0.26f, false },
                    { "lfo_1", "osc_1_wave_frame", 0.3f, false },
                    { "lfo_2", "filter_1_cutoff", 0.18f, true },
                    { "lfo_1", "osc_1_unison_detune", 0.15f, true },
                } });

            /*  Sequence. Holds its level and moves instead, with LFOs on pitch
                and level rather than an envelope on the filter. The deepest
                modulation of any style in the library. Pitch steps are snapped
                to semitones elsewhere, so the LFO on transpose is deliberate.
            */
            list.push_back ({ "Sequence",
                with (common(), {
                    { "osc_1_unison_voices", 3.0 }, { "osc_1_unison_detune", 2.0 },
                    { "osc_2_level", 0.3 },
                    { "polyphony", 8.0 },
                    { "filter_1_cutoff", 64.0 }, { "filter_1_resonance", 0.1 },
                    { "env_1_attack", 0.0 }, { "env_1_decay", 1.0 },
                    { "env_1_sustain", 1.0 }, { "env_1_release", 0.35 },
                    { "env_2_attack", 0.0 }, { "env_2_decay", 0.7 },
                    { "env_2_sustain", 0.0 }, { "env_2_release", 0.4 },
                    { "distortion_on", 1.0 }, { "distortion_drive", 5.7 },
                    { "distortion_mix", 0.65 },
                    { "reverb_on", 1.0 }, { "reverb_dry_wet", 0.15 },
                    { "delay_on", 1.0 }, { "delay_dry_wet", 0.18 },
                    { "eq_on", 1.0 },
                }),
                {
                    { "lfo_1", "osc_1_level", 0.5f, false },
                    { "lfo_2", "filter_1_cutoff", 0.5f, true },
                    { "lfo_1", "osc_1_transpose", 0.45f, true },
                    { "lfo_3", "osc_1_wave_frame", 0.4f, false },
                } });

            /*  Percussion. A hit. Everything about it is short, the filter sits
                open, and velocity does the work a keyboard player expects it to.
            */
            list.push_back ({ "Percussion",
                with (common(), {
                    { "osc_1_unison_voices", 1.0 }, { "osc_1_unison_detune", 0.0 },
                    { "osc_2_level", 0.25 },
                    { "sample_on", 1.0 }, { "sample_level", 0.3 },
                    { "polyphony", 6.0 },
                    { "filter_1_cutoff", 72.0 }, { "filter_1_resonance", 0.05 },
                    { "env_1_attack", 0.0 }, { "env_1_decay", 0.95 },
                    { "env_1_sustain", 0.0 }, { "env_1_release", 0.25 },
                    { "env_2_attack", 0.0 }, { "env_2_decay", 0.55 },
                    { "env_2_sustain", 0.0 }, { "env_2_release", 0.2 },
                    { "distortion_on", 1.0 }, { "distortion_drive", 6.3 },
                    { "distortion_mix", 0.8 },
                    { "reverb_on", 1.0 }, { "reverb_dry_wet", 0.2 },
                    { "reverb_decay_time", -0.1 },
                    { "eq_on", 1.0 },
                }),
                {
                    { "env_2", "filter_1_cutoff", 0.5f, false },
                    { "velocity", "osc_1_level", 0.4f, false },
                    { "velocity", "filter_1_cutoff", 0.3f, false },
                } });

            /*  SFX. Under no obligation to be a note, so this one is allowed
                everything the playable styles are not: heavy modulation, deep
                reverb, and whatever pitch it lands on.
            */
            list.push_back ({ "SFX",
                with (common(), {
                    { "osc_1_unison_voices", 5.0 }, { "osc_1_unison_detune", 4.5 },
                    { "osc_2_level", 0.35 },
                    { "sample_on", 1.0 }, { "sample_level", 0.2 },
                    { "polyphony", 8.0 },
                    { "filter_1_cutoff", 57.0 }, { "filter_1_resonance", 0.24 },
                    { "env_1_attack", 0.5 }, { "env_1_decay", 1.0 },
                    { "env_1_sustain", 0.8 }, { "env_1_release", 0.9 },
                    { "env_2_attack", 0.2 }, { "env_2_decay", 1.0 },
                    { "env_2_sustain", 0.4 }, { "env_2_release", 0.8 },
                    { "reverb_on", 1.0 }, { "reverb_dry_wet", 0.45 },
                    { "reverb_decay_time", 1.47 }, { "reverb_size", 0.8 },
                    { "delay_on", 1.0 }, { "delay_dry_wet", 0.2 },
                    { "phaser_on", 1.0 },
                }),
                {
                    { "env_2", "filter_1_cutoff", 0.5f, true },
                    { "lfo_1", "osc_1_wave_frame", 0.6f, false },
                    { "random_1", "osc_1_spectral_morph_amount", 0.5f, true },
                    { "lfo_2", "filter_1_cutoff", 0.4f, true },
                } });

            /*  Experiment. The most resonant filter in the library and the
                loosest rules. Where a lead is required to give back the note
                that was pressed, this one is not.
            */
            list.push_back ({ "Experiment",
                with (common(), {
                    { "osc_1_unison_voices", 4.0 }, { "osc_1_unison_detune", 1.1 },
                    { "osc_2_level", 0.4 }, { "osc_2_transpose", 7.0 },
                    { "osc_3_on", 1.0 }, { "osc_3_level", 0.2 },
                    { "polyphony", 8.0 },
                    { "filter_1_cutoff", 76.0 }, { "filter_1_resonance", 0.7 },
                    { "env_1_attack", 0.55 }, { "env_1_decay", 1.14 },
                    { "env_1_sustain", 0.9 }, { "env_1_release", 1.14 },
                    { "env_2_attack", 0.1 }, { "env_2_decay", 1.0 },
                    { "env_2_sustain", 0.3 }, { "env_2_release", 0.9 },
                    { "distortion_on", 1.0 }, { "distortion_drive", 3.6 },
                    { "reverb_on", 1.0 }, { "reverb_dry_wet", 0.48 },
                    { "reverb_decay_time", 1.98 },
                    { "delay_on", 1.0 }, { "delay_dry_wet", 0.25 },
                    { "phaser_on", 1.0 },
                }),
                {
                    { "lfo_1", "osc_1_wave_frame", 0.6f, false },
                    { "random_1", "osc_1_spectral_morph_amount", 0.5f, true },
                    { "lfo_2", "filter_1_cutoff", 0.5f, true },
                    { "env_2", "osc_2_level", 0.4f, false },
                } });

            return list;
        }
    }

    const std::vector<Archetype>& all()
    {
        static const std::vector<Archetype> list = build();
        return list;
    }

    const Archetype* find (const std::string& style)
    {
        for (const auto& a : all())
            if (a.style == style)
                return &a;
        return nullptr;
    }

    std::vector<std::string> styleNames()
    {
        std::vector<std::string> names;
        for (const auto& a : all())
            names.push_back (a.style);
        return names;
    }
}

// ---------------------------------------------------------------- ranges ---

namespace archetype
{
    const std::vector<Range>& ranges()
    {
        /*  Only what should move. Values are Vital's own units, kept inside the
            part of each range that stays musical rather than the full sweep the
            parameter technically allows. A filter can go to 136, but nothing
            useful lives up there.
        */
        static const std::vector<Range> table = {
            // tone
            { R"(^filter_\d_cutoff$)",                   28.0f, 110.0f },
            { R"(^filter_fx_cutoff$)",                   28.0f, 110.0f },
            { R"(^filter_\d_resonance$)",                 0.0f,   0.75f },
            { R"(^filter_\d_blend$)",                     0.0f,   1.0f },
            { R"(^filter_2_mix$)",                        0.3f,   1.0f },
            { R"(^filter_\d_keytrack$)",                  0.0f,   1.0f },
            { R"(^filter_\d_drive$)",                     0.0f,   7.0f },
            { R"(^eq_high_gain$)",                      -10.0f,  10.0f },
            { R"(^eq_low_gain$)",                       -10.0f,  10.0f },

            // oscillators
            /*  Never down to zero. Whether an oscillator sounds is a
                structural decision the archetype and COMPLEX make by switching
                it on or off. Letting the jitter reach zero decided it a second
                way, and left an oscillator switched on, wired up and silent.
            */
            { R"(^osc_\d_level$)",                        0.08f,  0.85f },
            { R"(^osc_\d_wave_frame$)",                   0.0f, 256.0f },
            { R"(^osc_\d_spectral_morph_amount$)",        0.0f,   1.0f },
            { R"(^osc_\d_distortion_amount$)",            0.0f,   0.85f },
            { R"(^osc_\d_unison_detune$)",                0.0f,   5.5f },
            { R"(^osc_\d_unison_voices$)",                1.0f,   9.0f },
            { R"(^osc_\d_pan$)",                          0.0f,   0.0f },   // never
            /*  The noise layer is Vital's own sample, so it is the same audio
                in every patch and only these decide what it sounds like. With
                just the level here it was one fixed sound turned up and down.
                Where it is routed matters most: through a filter it is a bed,
                past one it is hiss.
            */
            /*  Lower than a real library uses, and deliberately.

                Hand-made presets run their sample as loud as 0.94, but each of
                those is a different recording. This is always Vital's own white
                noise, which is flat to the top of the range, so the same level
                buys far more brightness and no character at all. A keys patch
                measuring 7 kHz turned into an ordinary one by muting it.
            */
            { R"(^sample_level$)",                        0.05f,  0.26f },
            { R"(^sample_destination$)",                  0.0f,   3.0f },
            { R"(^sample_transpose$)",                  -12.0f,   0.0f },

            // envelopes
            { R"(^env_1_attack$)",                        0.0f,   1.3f },
            { R"(^env_1_decay$)",                         0.6f,   1.4f },
            { R"(^env_1_sustain$)",                       0.0f,   1.0f },
            { R"(^env_1_release$)",                       0.1f,   1.3f },
            { R"(^env_\d_attack$)",                       0.0f,   0.9f },
            { R"(^env_\d_decay$)",                        0.4f,   1.2f },
            { R"(^env_\d_sustain$)",                      0.0f,   1.0f },
            { R"(^env_\d_release$)",                      0.1f,   1.0f },

            // movement
            { R"(^lfo_\d_frequency$)",                   -3.0f,   4.0f },
            { R"(^random_\d_frequency$)",                -3.0f,   3.0f },
            { R"(^portamento_time$)",                   -10.0f,  -3.0f },

            // effects
            { R"(^distortion_drive$)",                    0.0f,  11.0f },
            /*  Fully wet distortion leaves nothing of the note underneath it.
                A lead that measured as far too bright came back to something
                usable simply by taking the distortion out, and the two things
                doing the damage were the mix at the top of its range and the
                drive being swung by a fast LFO. Both are held back now.
            */
            { R"(^distortion_mix$)",                      0.15f,  0.70f },
            { R"(^reverb_dry_wet$)",                      0.0f,   0.6f },
            { R"(^reverb_decay_time$)",                  -1.0f,   2.5f },
            { R"(^reverb_size$)",                         0.2f,   1.0f },
            { R"(^reverb_pre_low_cutoff$)",               0.0f,  60.0f },
            { R"(^delay_dry_wet$)",                       0.0f,   0.45f },
            { R"(^delay_feedback$)",                      0.0f,   0.7f },
            { R"(^chorus_dry_wet$)",                      0.0f,   0.45f },
            { R"(^chorus_frequency$)",                   -5.0f,   1.0f },
            /*  The modulation effects, from p10 to p90 of the presets in a real
                library that use them.

                Only the frequencies used to be here, and both are dead
                parameters: sync is on, so the rate comes from the tempo instead
                and the frequency is ignored. Everything else sat at Vital's
                factory value, which meant every generated flanger was the same
                flanger, at the wettest setting real presets use. That is what
                made half a batch sound alike.

                The wet floors are deliberately above zero. An effect switched on
                at no wet level is the same switch in the wrong position an
                oscillator can be left in.
            */
            { R"(^phaser_dry_wet$)",                      0.10f,  0.75f },
            { R"(^phaser_center$)",                      49.0f, 107.0f },
            { R"(^phaser_feedback$)",                     0.0f,   0.78f },
            { R"(^phaser_mod_depth$)",                    2.0f,  33.0f },
            { R"(^phaser_phase_offset$)",                 0.0f,   0.75f },
            { R"(^phaser_tempo$)",                        1.0f,   5.0f },
            { R"(^flanger_dry_wet$)",                     0.10f,  0.45f },
            { R"(^flanger_center$)",                     16.0f,  94.0f },
            { R"(^flanger_feedback$)",                    0.0f,   0.85f },
            { R"(^flanger_mod_depth$)",                   0.05f,  1.0f },
            { R"(^flanger_phase_offset$)",                0.0f,   0.5f },
            { R"(^flanger_tempo$)",                       1.0f,   6.0f },
        };
        return table;
    }

    bool rangeFor (const std::string& parameter, float& low, float& high)
    {
        // Compiled once. Building a regex per lookup would run thousands of
        // times per roll.
        static const std::vector<std::regex> compiled = []
        {
            std::vector<std::regex> out;
            out.reserve (ranges().size());
            for (const auto& entry : ranges())
                out.emplace_back (entry.pattern);
            return out;
        }();

        for (size_t i = 0; i < compiled.size(); ++i)
        {
            if (std::regex_match (parameter, compiled[i]))
            {
                low = ranges()[i].low;
                high = ranges()[i].high;
                return high > low;
            }
        }
        return false;
    }

    Sliders defaultSlidersFor (const std::string& style)
    {
        //                                         bright  move   dirt  space  cplx
        if (style == "Bass")       return Sliders { 0.26f, 0.55f, 0.62f, 0.20f, 0.45f };
        if (style == "Lead")       return Sliders { 0.80f, 0.65f, 0.55f, 0.45f, 0.68f };
        if (style == "Keys")       return Sliders { 0.60f, 0.45f, 0.20f, 0.55f, 0.58f };
        if (style == "Pad")        return Sliders { 0.45f, 0.70f, 0.15f, 0.90f, 0.78f };
        if (style == "Sequence")   return Sliders { 0.65f, 0.85f, 0.45f, 0.40f, 0.72f };
        if (style == "Percussion") return Sliders { 0.55f, 0.30f, 0.60f, 0.25f, 0.40f };
        if (style == "SFX")        return Sliders { 0.60f, 0.90f, 0.60f, 0.75f, 0.85f };
        if (style == "Experiment") return Sliders { 0.55f, 0.80f, 0.65f, 0.60f, 0.90f };
        return Sliders { 0.55f, 0.60f, 0.45f, 0.55f, 0.50f };
    }

    const std::vector<const char*>& macroDestinationsFor (const std::string& style)
    {
        /*  Where a macro should point, per style.

            Named after what they move, the way preset authors do. In a real
            library REVERB, CUTOFF and DELAY are the three most common macro
            names, and every one of them is just the destination.
        */
        static const std::vector<const char*> bass = {
            "filter_1_cutoff", "distortion_mix", "osc_1_spectral_morph_amount",
            "osc_1_distortion_amount", "filter_1_resonance" };
        static const std::vector<const char*> lead = {
            "filter_1_cutoff", "reverb_dry_wet", "osc_1_spectral_morph_amount",
            "delay_dry_wet", "distortion_mix" };
        static const std::vector<const char*> keys = {
            "filter_1_cutoff", "reverb_dry_wet", "chorus_dry_wet",
            "osc_1_spectral_morph_amount", "delay_dry_wet" };
        static const std::vector<const char*> pad = {
            "filter_1_cutoff", "reverb_dry_wet", "chorus_dry_wet",
            "osc_1_wave_frame", "reverb_decay_time" };
        static const std::vector<const char*> sequence = {
            "filter_1_cutoff", "osc_1_wave_frame", "distortion_mix",
            "delay_dry_wet", "lfo_1_frequency" };
        static const std::vector<const char*> generic = {
            "filter_1_cutoff", "reverb_dry_wet", "osc_1_spectral_morph_amount",
            "distortion_mix", "delay_dry_wet" };

        if (style == "Bass")     return bass;
        if (style == "Lead")     return lead;
        if (style == "Keys")     return keys;
        if (style == "Pad")      return pad;
        if (style == "Sequence") return sequence;
        return generic;
    }
}
