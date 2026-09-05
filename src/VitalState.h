#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

/*
    Vital's plugin state chunk is the same JSON a .vital file contains.
    getStateInformation runs the same LoadSave::stateToJson that writes presets
    to disk, with a "tuning" key merged in, so a patch can be handed to a hosted
    instance without touching its GUI or the preset browser.

    The JSON sits inside three wrappers:

        VC2! + length
          -> XML <VST3PluginState><IComponent> ... </IComponent>
            -> JUCE's base64 variant
              -> a VstW / FXB block, 176 bytes of header
                -> the preset JSON, then JUCE's own private data

    Rather than build those wrappers from scratch, write() takes the live
    instance's own state as a template and swaps only the JSON body. The header,
    the plugin version and JUCE's trailing block then always come from the Vital
    the user actually has installed, so a Vital update cannot desync us.
*/
namespace vitalstate
{
    /** Pull the preset JSON out of a plugin state chunk. Returns a null json on
        anything unexpected rather than throwing. */
    nlohmann::json read (const juce::MemoryBlock& state);

    /** Build a new state chunk from `templateState`, carrying `preset` as its
        JSON body. Returns an empty block if the template could not be parsed. */
    juce::MemoryBlock write (const juce::MemoryBlock& templateState,
                             const nlohmann::json& preset);

    /** True if the block looks like a Vital VST3 state chunk we can work with. */
    bool looksValid (const juce::MemoryBlock& state);
}
