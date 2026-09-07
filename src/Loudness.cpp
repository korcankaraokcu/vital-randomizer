#include "Loudness.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace loudness
{
    namespace
    {
        // volume parameter -> output level in dB, relative to volume = 1000
        constexpr std::array<float, 9> kVolume = {
            1000.0f, 2000.0f, 3000.0f, 4000.0f, 5000.0f, 5473.0f, 6000.0f, 7000.0f, 7399.0f
        };
        constexpr std::array<float, 9> kDb = {
            0.0f, 13.1f, 23.2f, 31.6f, 39.1f, 42.5f, 45.8f, 52.0f, 54.4f
        };

        // Under a dB of correction is not worth making. Chasing it just means
        // every patch gets nudged for no audible reason.
        /*  Wide enough to clear the scatter in the thing being measured.

            Vital randomises unison phase at every note on and its modulators
            run free, so the same patch rendered three times reads three
            different levels: one pad measured 0.143, 0.112 and 0.099, a spread
            near 3 dB. Against a deadband of one, the correction could never
            settle, and running out of passes was the commonest single reason a
            candidate was thrown away.

            Two is above the scatter on most patches and still inside the band a
            listener would call level.
        */
        constexpr float kDeadband = 2.0f;

        float interp (const std::array<float, 9>& xs, const std::array<float, 9>& ys, float x)
        {
            if (x <= xs.front())
                return ys.front();
            if (x >= xs.back())
                return ys.back();
            for (size_t i = 1; i < xs.size(); ++i)
            {
                if (x <= xs[i])
                {
                    const auto span = xs[i] - xs[i - 1];
                    const auto frac = span > 1.0e-9f ? (x - xs[i - 1]) / span : 0.0f;
                    return ys[i - 1] + (ys[i] - ys[i - 1]) * frac;
                }
            }
            return ys.back();
        }
    }

    float volumeFor (float currentVolume, float gainDb)
    {
        const auto currentDb = interp (kVolume, kDb, currentVolume);
        const auto target = interp (kDb, kVolume, currentDb + gainDb);
        return std::max (kVolumeMin, std::min (kVolumeMax, target));
    }

    float correctionDb (float rms, float peak)
    {
        if (rms <= 1.0e-6f)
            return 0.0f;

        auto gainDb = 20.0f * std::log10 (kTargetRms / rms);

        // The peak ceiling only ever pulls down. A patch with a lot of headroom
        // should still be brought up to the target rather than being allowed to
        // sit quiet just because its transient is small.
        if (peak > 1.0e-6f)
        {
            const auto ceilingDb = 20.0f * std::log10 (kPeakCeiling / peak);
            gainDb = std::min (gainDb, ceilingDb);
        }

        gainDb = std::max (kMaxCutDb, std::min (kMaxBoostDb, gainDb));
        return std::abs (gainDb) < kDeadband ? 0.0f : gainDb;
    }

    float normalise (nlohmann::json& settings, float rms, float peak)
    {
        const auto gainDb = correctionDb (rms, peak);
        if (gainDb == 0.0f)
            return 0.0f;

        const auto current = static_cast<float> (
            settings.value ("volume", (double) kVolumeDefault));
        const auto updated = volumeFor (current, gainDb);
        settings["volume"] = updated;

        // Report what the clamp actually allowed, not what was asked for, so a
        // caller can tell whether a second pass is worth it.
        const auto beforeDb = interp (kVolume, kDb, current);
        const auto afterDb = interp (kVolume, kDb, updated);
        return afterDb - beforeDb;
    }
}
