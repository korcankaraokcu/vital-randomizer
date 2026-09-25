#pragma once

#include <deque>
#include <memory>
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
    amount, locks, seed, and the level the screen settled on), and the generator
    splits its random streams so that recipe reproduces the same patch every
    time. A VARY's recipe adds the recipe of the patch it started from, so it
    replays by rebuilding that first. History therefore stores recipes at a few
    hundred bytes each rather than patches at a megabyte. The exception is a
    VARY of something no recipe rebuilds, a recalled keeper or a patch restored
    with a project, and every sixty-fourth link of a long run, which keep the
    patch itself.

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
        /*  Which scale a sequence walked. Chosen when the roll is built rather
            than inside the generator, because the user may have narrowed the
            choice to a handful, and a recipe that did not carry the answer
            would walk a different scale every time history replayed it. */
        int scale = -1;
        // Which kind of drum a percussion roll was built as, for the same reason.
        int drumKind = -1;

        /*  The level the screen settled on, as Vital's master volume.

            The screen matches every patch's level by moving its volume, after
            the generator has finished with it, so a recipe replayed without it
            came back at whatever level it happened to be generated at. Stepping
            back through history jumped in level from one patch to the next.
            Negative when unknown, which is every recipe saved before this.
        */
        float volume = -1.0f;

        /*  What a VARY started from.

            A VARY is not a fresh roll: it is a patch moved by a depth, and
            without the patch it moved there is nothing to replay. So a VARY's
            recipe points at the recipe of the patch it started from, which may
            itself be a VARY, and replaying walks back to the fresh roll at the
            root. Where the starting patch cannot be rebuilt from a recipe, a
            recalled keeper or a patch restored with a project, or where the
            chain has grown long, the patch itself is kept instead.
        */
        std::shared_ptr<const Recipe> variedFrom;
        std::shared_ptr<const nlohmann::json> variedFromPatch;
        std::vector<std::string> varySections;

        /** How many VARYs deep this is, 0 for a fresh roll. */
        int chainLength() const;

        gen::Request toRequest() const;
        static Recipe fromRequest (const gen::Request& r, unsigned int seed);

        nlohmann::json toJson() const;
        static Recipe fromJson (const nlohmann::json& j);
    };

    /** Build the patch a recipe describes, following a VARY back to what it
        started from and putting the screened level back. */
    gen::Result rebuild (gen::Generator& generator, const Recipe& recipe);

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
