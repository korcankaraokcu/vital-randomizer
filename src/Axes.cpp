#include "Axes.h"

namespace axes
{
    namespace
    {
        Weighted w (const char* pattern, float weight)
        {
            return { std::regex (pattern), weight };
        }

        std::vector<Axis> build()
        {
            std::vector<Axis> list;

            Axis bright;
            bright.key = "bright";
            bright.label = "BRIGHT";
            bright.params = {
                w ("^filter_\\d_cutoff$", 1.0f),
                w ("^filter_fx_cutoff$", 0.8f),
                w ("^osc_\\d_spectral_morph_amount$", 0.6f),
                w ("^filter_\\d_blend$", 0.4f),
                w ("^osc_\\d_wave_frame$", 0.5f),
                w ("^filter_\\d_keytrack$", 0.3f),
                // Filter cutoff is the obvious brightness control and also the
                // most modulated destination in hand-made presets, so biasing it
                // statically gets overridden by whatever envelope is already
                // driving it. The EQ is the one tone control nothing else is
                // fighting over, which makes it the axis's most reliable lever.
                w ("^eq_high_gain$", 0.9f),
                w ("^eq_low_gain$", -0.8f),
            };
            bright.flip = { std::regex ("^filter_\\d_model$") };
            bright.enables = { "filter_1_on", "eq_on" };
            bright.macroDests = { "filter_1_cutoff", "filter_2_cutoff",
                                  "filter_fx_cutoff", "osc_1_spectral_morph_amount" };
            list.push_back (std::move (bright));

            Axis move;
            move.key = "move";
            move.label = "MOVE";
            move.params = {
                w ("^lfo_\\d_frequency$", 0.7f),
                w ("^random_\\d_frequency$", 0.6f),
                w ("^osc_\\d_unison_detune$", 0.5f),
                w ("^chorus_frequency$", 0.4f),
                w ("^flanger_frequency$", 0.4f),
                w ("^phaser_frequency$", 0.4f),
                w ("^portamento_time$", 0.3f),
            };
            move.flip = { std::regex ("^lfo_\\d_sync$") };
            move.enables = { "chorus_on", "flanger_on", "phaser_on" };
            move.macroDests = { "osc_1_spectral_morph_amount", "osc_1_wave_frame",
                                "lfo_1_frequency" };
            move.hasWiring = true;
            move.wiring.sources = { "lfo_1", "lfo_2", "lfo_3", "random_1", "env_2" };
            move.wiring.destinations = {
                "osc_1_wave_frame", "osc_2_wave_frame", "filter_1_cutoff",
                "osc_1_spectral_morph_amount", "filter_2_cutoff",
                "osc_1_level", "distortion_drive", "osc_1_unison_detune",
                "osc_2_wave_frame", "filter_1_resonance",
            };
            list.push_back (std::move (move));

            Axis dirt;
            dirt.key = "dirt";
            dirt.label = "DIRT";
            dirt.params = {
                w ("^distortion_drive$", 1.0f),
                w ("^distortion_mix$", 0.9f),
                w ("^osc_\\d_distortion_amount$", 0.7f),
                w ("^filter_\\d_drive$", 0.6f),
                w ("^osc_\\d_unison_voices$", 0.4f),
            };
            dirt.flip = { std::regex ("^distortion_type$"),
                          std::regex ("^osc_\\d_distortion_type$") };
            dirt.enables = { "distortion_on" };
            dirt.macroDests = { "distortion_mix", "distortion_drive",
                                "osc_1_distortion_amount" };
            list.push_back (std::move (dirt));

            Axis space;
            space.key = "space";
            space.label = "SPACE";
            space.params = {
                w ("^reverb_dry_wet$", 1.0f),
                w ("^reverb_decay_time$", 0.8f),
                w ("^reverb_size$", 0.6f),
                w ("^delay_dry_wet$", 0.7f),
                w ("^delay_feedback$", 0.4f),
                w ("^chorus_dry_wet$", 0.5f),
                w ("^phaser_dry_wet$", 0.4f),
                w ("^reverb_pre_low_cutoff$", -0.3f),
                // Stereo spread is deliberately not a lever here. Every
                // hand-made preset in the library runs it at full, so the only
                // thing moving it can do is narrow the patch, which is not what
                // a space control should ever do.
            };
            space.enables = { "reverb_on", "delay_on", "chorus_on" };
            space.macroDests = { "reverb_dry_wet", "delay_dry_wet",
                                 "chorus_dry_wet", "reverb_decay_time" };
            list.push_back (std::move (space));

            return list;
        }
    }

    const std::vector<Axis>& all()
    {
        static const std::vector<Axis> list = build();
        return list;
    }

    const Axis* find (const std::string& key)
    {
        for (const auto& a : all())
            if (a.key == key)
                return &a;
        return nullptr;
    }

    bool isPitchDestination (const std::string& destination)
    {
        static const std::vector<std::regex> patterns = {
            std::regex ("^osc_\\d_transpose$"), std::regex ("^osc_\\d_tune$"),
            std::regex ("^sample_transpose$"),   std::regex ("^sample_tune$"),
            std::regex ("^voice_transpose$"),    std::regex ("^voice_tune$"),
            std::regex ("^osc_\\d_detune_range$"),
        };
        for (const auto& rx : patterns)
            if (std::regex_match (destination, rx))
                return true;
        return false;
    }

    std::vector<std::pair<std::string, float>>
        members (const Axis& axis, const std::vector<std::string>& names)
    {
        std::vector<std::pair<std::string, float>> out;
        for (const auto& p : axis.params)
            for (const auto& n : names)
                if (std::regex_match (n, p.pattern))
                    out.emplace_back (n, p.weight);
        return out;
    }

    std::vector<std::string>
        flippable (const Axis& axis, const std::vector<std::string>& names)
    {
        std::vector<std::string> out;
        for (const auto& rx : axis.flip)
            for (const auto& n : names)
                if (std::regex_match (n, rx))
                    out.push_back (n);
        return out;
    }
}
