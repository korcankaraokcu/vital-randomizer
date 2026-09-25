#include "CandidateStore.h"

#include <algorithm>
#include <functional>
#include <map>

namespace store
{
    gen::Request Recipe::toRequest() const
    {
        gen::Request r;
        r.style = style;
        for (const auto& s : sliders)
            r.sliders[s.first] = s.second;
        r.amount = amount;
        r.seed = seed;
        for (const auto& l : locks)
            r.locks.insert (schema::sectionFromName (l));
        r.name = label;
        r.scale = scale;
        for (const auto& s : varySections)
            r.varySections.insert (schema::sectionFromName (s));
        return r;
    }

    int Recipe::chainLength() const
    {
        int n = 0;
        for (auto p = variedFrom; p != nullptr; p = p->variedFrom)
            ++n;
        return variedFrom != nullptr || variedFromPatch != nullptr ? juce::jmax (1, n) : 0;
    }

    gen::Result rebuild (gen::Generator& generator, const Recipe& recipe)
    {
        nlohmann::json from;
        if (recipe.variedFrom != nullptr)
        {
            auto base = rebuild (generator, *recipe.variedFrom);
            if (! base.ok)
                return base;
            from = std::move (base.preset);
        }
        else if (recipe.variedFromPatch != nullptr)
        {
            from = *recipe.variedFromPatch;
        }

        auto request = recipe.toRequest();
        if (! from.is_null())
            request.base = &from;

        auto result = generator.roll (request);
        if (result.ok && recipe.volume >= 0.0f && result.preset.contains ("settings"))
            result.preset["settings"]["volume"] = recipe.volume;
        return result;
    }

    Recipe Recipe::fromRequest (const gen::Request& r, unsigned int seed)
    {
        Recipe recipe;
        recipe.style = r.style;
        recipe.sliders.assign (r.sliders.begin(), r.sliders.end());
        std::sort (recipe.sliders.begin(), recipe.sliders.end());
        recipe.amount = r.amount;
        recipe.seed = seed;
        for (const auto& l : r.locks)
            recipe.locks.push_back (schema::sectionName (l));
        recipe.label = r.name;
        recipe.scale = r.scale;
        for (const auto s : r.varySections)
            recipe.varySections.push_back (schema::sectionName (s));
        return recipe;
    }

    nlohmann::json Recipe::toJson() const
    {
        nlohmann::json j;
        j["style"] = style;
        j["amount"] = amount;
        j["seed"] = seed;
        j["locks"] = locks;
        j["label"] = label;
        j["scale"] = scale;
        j["volume"] = volume;
        if (! varySections.empty())
            j["vary_sections"] = varySections;
        for (const auto& s : sliders)
            j["sliders"][s.first] = s.second;
        return j;
    }

    Recipe Recipe::fromJson (const nlohmann::json& j)
    {
        Recipe r;
        r.style = j.value ("style", std::string {});
        r.amount = j.value ("amount", 1.0f);
        r.seed = j.value ("seed", 0u);
        r.label = j.value ("label", std::string {});
        r.scale = j.value ("scale", -1);
        r.volume = j.value ("volume", -1.0f);
        if (j.contains ("vary_sections") && j["vary_sections"].is_array())
            r.varySections = j["vary_sections"].get<std::vector<std::string>>();
        if (j.contains ("locks") && j["locks"].is_array())
            r.locks = j["locks"].get<std::vector<std::string>>();
        if (j.contains ("sliders") && j["sliders"].is_object())
            for (auto it = j["sliders"].begin(); it != j["sliders"].end(); ++it)
                r.sliders.emplace_back (it.key(), it.value().get<float>());
        return r;
    }

    void CandidateStore::setHistoryLimit (size_t newLimit)
    {
        limit = std::max<size_t> (8, newLimit);
        while (history.size() > limit)
        {
            history.pop_front();
            if (position > 0)
                --position;
        }
    }

    void CandidateStore::push (const Recipe& recipe)
    {
        /*  Always onto the end, whatever is being auditioned.

            A roll or a VARY made while sitting back in history used to delete
            everything after it, the way an edit after an undo does. History is
            not an undo stack, though, it is the candidates someone is choosing
            between, and going back to the first of three VARYs to try another
            threw the other two away. Nothing depends on where an entry sits,
            since a VARY finds what it started from through its recipe, so the
            new one simply goes last and only the limit ever removes anything.
        */
        history.push_back (recipe);
        while (history.size() > limit)
        {
            history.pop_front();
            if (position > 0)
                --position;
        }
        position = static_cast<int> (history.size()) - 1;
        lastDirection = 1;
    }

    bool CandidateStore::moveCursor (int delta)
    {
        if (history.empty() || delta == 0)
            return false;
        const auto target = position + delta;
        if (target < 0 || target >= static_cast<int> (history.size()))
            return false;
        position = target;
        lastDirection = delta > 0 ? 1 : -1;
        return true;
    }

    void CandidateStore::setCursor (int index)
    {
        if (index >= 0 && index < static_cast<int> (history.size()))
        {
            lastDirection = index >= position ? 1 : -1;
            position = index;
        }
    }

    const Recipe* CandidateStore::current() const
    {
        return at (position);
    }

    const Recipe* CandidateStore::at (int index) const
    {
        if (index < 0 || index >= static_cast<int> (history.size()))
            return nullptr;
        return &history[static_cast<size_t> (index)];
    }

    const Recipe* CandidateStore::neighbour() const
    {
        // Preload follows travel. Stepping backwards through history and
        // preloading forwards means every step pays the load in full, which is
        // the difference between the strip feeling instant and feeling laggy.
        return at (position + lastDirection);
    }

    void CandidateStore::star (const Keeper& keeper)
    {
        kept.push_back (keeper);
    }

    void CandidateStore::unstar (size_t index)
    {
        if (index < kept.size())
            kept.erase (kept.begin() + static_cast<long> (index));
    }

    void CandidateStore::clearHistory()
    {
        history.clear();
        position = -1;
    }

    nlohmann::json CandidateStore::toJson() const
    {
        nlohmann::json j;
        j["limit"] = limit;
        j["position"] = position;

        /*  The patches VARYs started from, each written once.

            A chain of VARYs shares its links, so writing each recipe with its
            whole chain nested inside would write the root once per VARY after
            it and grow with the square of the chain. Links go in a table,
            parents before children, and a recipe names its parent by index.
        */
        nlohmann::json links = nlohmann::json::array();
        std::map<const Recipe*, int> written;
        std::function<int (const std::shared_ptr<const Recipe>&)> linkOf;
        linkOf = [&] (const std::shared_ptr<const Recipe>& r) -> int
        {
            if (r == nullptr)
                return -1;
            const auto found = written.find (r.get());
            if (found != written.end())
                return found->second;
            const auto parent = linkOf (r->variedFrom);
            auto node = r->toJson();
            node["varied_from"] = parent;
            if (r->variedFromPatch != nullptr)
                node["varied_from_patch"] = *r->variedFromPatch;
            const auto id = (int) links.size();
            links.push_back (std::move (node));
            written[r.get()] = id;
            return id;
        };
        const auto encode = [&] (const Recipe& r)
        {
            auto e = r.toJson();
            e["varied_from"] = linkOf (r.variedFrom);
            if (r.variedFromPatch != nullptr)
                e["varied_from_patch"] = *r.variedFromPatch;
            return e;
        };

        nlohmann::json hist = nlohmann::json::array();
        for (const auto& r : history)
            hist.push_back (encode (r));
        j["history"] = std::move (hist);

        // Keepers carry their full patch so a hand edit survives a project
        // reload. History is recipes, apart from the rare VARY that had to keep
        // the patch it started from, so it stays small even at a thousand.
        nlohmann::json keep = nlohmann::json::array();
        for (const auto& k : kept)
            keep.push_back ({ { "label", k.label },
                              { "recipe", encode (k.recipe) },
                              { "edited", k.edited },
                              { "preset", k.preset } });
        j["keepers"] = std::move (keep);
        j["links"] = std::move (links);
        return j;
    }

    void CandidateStore::fromJson (const nlohmann::json& j)
    {
        history.clear();
        kept.clear();
        limit = j.value ("limit", size_t { 1000 });
        position = j.value ("position", -1);

        std::vector<std::shared_ptr<const Recipe>> links;
        const auto decode = [&links] (const nlohmann::json& e)
        {
            auto r = Recipe::fromJson (e);
            const auto parent = e.value ("varied_from", -1);
            if (parent >= 0 && parent < (int) links.size())
                r.variedFrom = links[(size_t) parent];
            if (e.contains ("varied_from_patch"))
                r.variedFromPatch = std::make_shared<const nlohmann::json> (e["varied_from_patch"]);
            return r;
        };
        // Parents were written before children, so one pass rebuilds them.
        if (j.contains ("links") && j["links"].is_array())
            for (const auto& node : j["links"])
                links.push_back (std::make_shared<const Recipe> (decode (node)));

        if (j.contains ("history") && j["history"].is_array())
            for (const auto& r : j["history"])
                history.push_back (decode (r));

        if (j.contains ("keepers") && j["keepers"].is_array())
        {
            for (const auto& k : j["keepers"])
            {
                Keeper keeper;
                keeper.label = k.value ("label", std::string {});
                keeper.edited = k.value ("edited", false);
                if (k.contains ("recipe"))
                    keeper.recipe = decode (k["recipe"]);
                if (k.contains ("preset"))
                    keeper.preset = k["preset"];
                kept.push_back (std::move (keeper));
            }
        }

        if (position >= static_cast<int> (history.size()))
            position = static_cast<int> (history.size()) - 1;
    }
}
