#include "Schema.h"

#include <algorithm>
#include <array>
#include <unordered_set>

namespace schema
{
    namespace
    {
        bool startsWith (const std::string& s, const char* prefix)
        {
            const auto n = std::strlen (prefix);
            return s.size() >= n && s.compare (0, n, prefix) == 0;
        }

        bool endsWith (const std::string& s, const char* suffix)
        {
            const auto n = std::strlen (suffix);
            return s.size() >= n && s.compare (s.size() - n, n, suffix) == 0;
        }

        /** Matches "<prefix><digits>_", e.g. osc_1_, env_2_, modulation_37_. */
        bool numberedPrefix (const std::string& s, const char* prefix)
        {
            const auto n = std::strlen (prefix);
            if (s.size() <= n || s.compare (0, n, prefix) != 0)
                return false;
            size_t i = n;
            while (i < s.size() && std::isdigit (static_cast<unsigned char> (s[i])))
                ++i;
            return i > n && i < s.size() && s[i] == '_';
        }

        const std::unordered_set<std::string>& excludedSet()
        {
            static const std::unordered_set<std::string> s = {
                "beats_per_minute", "polyphony", "oversampling", "bypass", "volume",
                "effect_chain_order", "voice_amplitude", "voice_priority",
                "voice_override", "velocity_track", "pitch_bend_range", "mpe_enabled",
                "legato", "view_spectrogram", "delay_sync", "delay_tempo",
            };
            return s;
        }
    }

    bool isExcluded (const std::string& param)
    {
        return excludedSet().count (param) > 0 || startsWith (param, "view_");
    }

    Section sectionOf (const std::string& param)
    {
        if (isExcluded (param))
            return Section::excluded;

        if (numberedPrefix (param, "osc_") || startsWith (param, "sample_"))
            return Section::osc;
        if (numberedPrefix (param, "filter_") || startsWith (param, "filter_fx_"))
            return Section::filter;
        if (numberedPrefix (param, "env_"))
            return Section::env;
        if (numberedPrefix (param, "lfo_") || numberedPrefix (param, "random_"))
            return Section::lfo;
        if (numberedPrefix (param, "modulation_"))
            return Section::mod;
        if (startsWith (param, "reverb_") || startsWith (param, "delay_")
            || startsWith (param, "chorus_") || startsWith (param, "phaser_")
            || startsWith (param, "flanger_") || startsWith (param, "distortion_")
            || startsWith (param, "compressor_") || startsWith (param, "eq_"))
            return Section::fx;

        return Section::global;
    }

    const char* sectionName (Section s)
    {
        switch (s)
        {
            case Section::osc:    return "osc";
            case Section::filter: return "filter";
            case Section::env:    return "env";
            case Section::lfo:    return "lfo";
            case Section::fx:     return "fx";
            case Section::mod:    return "mod";
            case Section::global: return "global";
            default:              return "excluded";
        }
    }

    Section sectionFromName (const std::string& name)
    {
        for (int i = 0; i < numSections; ++i)
        {
            const auto s = static_cast<Section> (i);
            if (name == sectionName (s))
                return s;
        }
        return Section::excluded;
    }

    bool isIndexed (const std::string& param)
    {
        static const std::array<const char*, 22> suffixes = {
            "_model", "_type", "_sync", "_mode", "_style", "_source", "_destination",
            "_routing", "_wave", "_voices", "_transpose", "_octave", "_on", "_switch",
            "_bipolar", "_stereo", "_snap", "_keytrack", "_dc", "_normalize",
            "_smooth", "_stack",
        };
        return std::any_of (suffixes.begin(), suffixes.end(),
                            [&] (const char* s) { return endsWith (param, s); });
    }

    const std::vector<std::string>& blobNames()
    {
        static const std::vector<std::string> names = { "wavetables", "sample", "lfos" };
        return names;
    }

    Section blobOwner (const std::string& blob)
    {
        if (blob == "lfos")
            return Section::lfo;
        if (blob == "wavetables" || blob == "sample")
            return Section::osc;
        return Section::excluded;
    }
}
