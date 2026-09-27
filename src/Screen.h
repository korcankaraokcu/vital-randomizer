#pragma once

#include <string>

#include "Audition.h"
#include "Generator.h"
#include "VitalHost.h"

/*
    The screen a generated patch has to pass, in one place.

    The plugin and vrgen both call this, so a preset made with the CLI
    has been through exactly what one rolled in the plugin has. It renders the
    candidate through a hosted Vital, turns it down if it is unusable or the
    wrong instrument for its style, and levels it into its own master volume.
*/
namespace screen
{
    struct Verdict
    {
        bool passed = false;
        // Why it was turned down, in words, or empty when it passed.
        std::string why;
        // How far the level was moved in all, in dB.
        float movedDb = 0.0f;
    };

    /*  Judge `result` for `style`, levelling it in place when it passes.

        `complexity` is the COMPLEX slider the patch was rolled at, which moves
        how bright a style may be. `measured` holds the last reading taken.
    */
    Verdict judge (VitalHost& vital, double sampleRate, int blockSize, gen::Result& result,
                   const std::string& style, float complexity, audition::Measurement& measured);
}
