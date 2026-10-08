#include "Screen.h"

#include <cmath>

#include "Drums.h"
#include "Loudness.h"

namespace screen
{
    Verdict judge (VitalHost& vital, double sampleRate, int blockSize, gen::Result& result,
                   const std::string& style, float complexity, audition::Measurement& measured)
    {
        Verdict verdict;
        measured = {};

        if (! vital.isLoaded() || ! result.preset.contains ("settings"))
        {
            verdict.passed = true;     // nothing to screen with, so take what we were given
            return verdict;
        }

        const auto turnDown = [&verdict] (const char* why)
        {
            verdict.passed = false;
            verdict.why = why;
            return verdict;
        };

        /*  Correct, then re-measure, then correct again if needed.

            One pass is not enough. The volume calibration was swept on a clean
            patch, and a patch with heavy distortion or compression in its chain
            does not respond to master volume the same way, so the first
            correction can land several dB off. Measuring what actually came out
            and going again is the only way to be sure the patch a user hears is
            the level it was supposed to be.
        */
        for (int pass = 0; pass < 3; ++pass)
        {
            // The first load is verified. If Vital rejected the patch there is
            // nothing to measure and rolling again is the right answer.
            const auto loaded = pass == 0 ? vital.applyPresetVerified (result.preset)
                                          : vital.applyPreset (result.preset);
            if (! loaded)
                return turnDown ("Vital would not load it");

            // Vital's first render after taking a new state is not the settled
            // patch, so a short one is played and discarded before measuring.
            audition::settle (*vital.processor(), sampleRate, blockSize);

            measured = audition::auditionAveraged (*vital.processor(), sampleRate, blockSize, 3, 48,
                                                   audition::judgedAsHit (style));
            if (! measured.usable())
                return turnDown ("unusable");

            // A patch can be perfectly healthy and still be the wrong
            // instrument. Judged as the kind of drum it was built as, where it
            // is one.
            const auto styleName = drums::profile (style, result.drumKind);
            /*  Balance before brightness. A patch is allowed high frequency
                detail as long as it stays quiet: what makes a bass a bass is how
                much of it is down low, not where its mean lands.
            */
            const auto balance = audition::balanceFor (styleName);
            if (measured.lowRatio > 0.0f && measured.lowRatio < balance.minLowRatio)
                return turnDown ("not enough bottom");
            if (measured.highSpike > balance.maxHighSpike)
                return turnDown ("too harsh up top");

            const auto bounds = audition::brightnessFor (styleName, complexity);
            if (measured.centroidHz > 0.0f && measured.centroidHz < bounds.low)
                return turnDown ("too dark");
            if (measured.centroidHz > 0.0f && measured.centroidHz > bounds.high)
                return turnDown ("too bright");
            if (measured.heldRatio > audition::maxHeldRatioFor (styleName))
                return turnDown ("note keeps going");
            if (measured.presenceDb > audition::maxPresenceFor (styleName))
                return turnDown ("whistles when held");

            // Give back the note that was pressed, or roll again.
            /*  A stepping patch is read a step at a time. Measured whole, a
                clean sequence looks like noise, because the window spans
                several notes.
            */
            const auto pitch = audition::pitchRuleFor (styleName);
            const auto stepped = audition::isSteppedStyle (styleName) && measured.steps > 0;
            const auto salience = stepped ? measured.stepSalience : measured.pitchSalience;
            const auto pitchError = stepped ? measured.stepOffQuarterSemitones
                                  : pitch.octavesOnly ? measured.pitchErrorSemitones
                                                      : measured.pitchOffGridSemitones;
            if (pitch.required && salience < pitch.minSalience)
                return turnDown ("pitch unclear");
            if (pitch.required && pitchError > pitch.maxErrorSemitones)
                return turnDown ("wrong note");

            // And two octaves up, once: a level correction moves both notes alike.
            if (pass == 0)
                if (const auto* why = audition::failsUpTheKeyboard (*vital.processor(), sampleRate,
                                                                     blockSize, styleName, measured))
                    return turnDown (why);

            const auto wanted = loudness::correctionDb (measured.loudness, measured.peak);

            /*  Nothing left to confirm.

                The reading is already three renders averaged, taken before the
                correction was worked out rather than after it looked right.
                This used to measure once, correct by what that one render said
                and measure once more, which is how a patch could bounce either
                side of the target until the passes ran out and be thrown away
                for never settling. It was the commonest rejection there was.
            */
            if (wanted == 0.0f)
            {
                verdict.passed = true;
                return verdict;
            }

            // Match the level by moving the patch's own master volume rather
            // than trimming the output, so the level travels with the preset.
            const auto applied = loudness::normalisePreset (result.preset,
                                                            measured.loudness, measured.peak);

            // Running out of volume range means the patch is quiet at the
            // source, not quiet at the output, and turning it up will not fix
            // that.
            if (std::abs (wanted - applied) > 6.0f)
                return turnDown ("level out of reach");
            verdict.movedDb += applied;
        }
        return turnDown ("level never settled");
    }
}
