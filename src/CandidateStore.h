#pragma once

#include <deque>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include "Generator.h"

/*
    History and keepers.

    Rolling past something good and losing it forever is the main way a
    randomizer frustrates people, so history is not optional. It also must not
    grow without bound: a patch is most of a megabyte, and almost all of that is
    wavetable and sample data.

    The trick is that a roll is fully determined by its recipe (style, sliders,
    amount, locks, seed), and the generator splits its random streams so that
    recipe reproduces the same patch every time. History therefore stores
    recipes at about a hundred bytes each rather than patches at a megabyte, and
    a thousand-deep history costs less than a tenth of a megabyte.

    Keepers are different. Once a patch is starred the user may have hand edited
    it in Vital's own GUI, and a hand edit cannot be regenerated from a recipe,
    so keepers hold the real JSON. There are only ever a handful of them.
*/
namespace store
{
    struct Recipe
    {
        std::string style;
        std::vector<std::pair<std::string, float>> sliders;
        float amount = 1.0f;
        unsigned int seed = 0;
        std::vector<std::string> locks;
        std::string label;

        gen::Request toRequest() const;
        static Recipe fromRequest (const gen::Request& r, unsigned int seed);

        nlohmann::json toJson() const;
        static Recipe fromJson (const nlohmann::json& j);
    };

    struct Keeper
    {
        std::string label;
        Recipe recipe;
        nlohmann::json preset;      // the real patch, hand edits included
        bool edited = false;
    };

    class CandidateStore
    {
    public:
        void setHistoryLimit (size_t limit);
        size_t historyLimit() const { return limit; }

        void push (const Recipe& recipe);
        size_t size() const { return history.size(); }
        bool empty() const { return history.empty(); }

        /** Index of the candidate currently being auditioned. */
        int cursor() const { return position; }
        bool moveCursor (int delta);
        void setCursor (int index);

        const Recipe* current() const;
        const Recipe* at (int index) const;
        /** The one to preload, which follows whichever way the user is moving. */
        const Recipe* neighbour() const;

        const std::vector<Keeper>& keepers() const { return kept; }
        void star (const Keeper& keeper);
        void unstar (size_t index);
        void clearHistory();

        nlohmann::json toJson() const;
        void fromJson (const nlohmann::json& j);

    private:
        std::deque<Recipe> history;
        std::vector<Keeper> kept;
        size_t limit = 1000;
        int position = -1;
        int lastDirection = 1;
    };
}
