#include "CandidateStore.h"

#include <algorithm>

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
        return r;
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
        // A fresh roll while sitting back in history truncates the future, the
        // way an edit after an undo does. Keeping both would leave the strip
        // showing candidates that no longer follow from what is playing.
        if (position >= 0 && position + 1 < static_cast<int> (history.size()))
            history.erase (history.begin() + position + 1, history.end());

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

        nlohmann::json hist = nlohmann::json::array();
        for (const auto& r : history)
            hist.push_back (r.toJson());
        j["history"] = std::move (hist);

        // Keepers carry their full patch so a hand edit survives a project
        // reload. History is only recipes, so it stays small even at a thousand.
        nlohmann::json keep = nlohmann::json::array();
        for (const auto& k : kept)
            keep.push_back ({ { "label", k.label },
                              { "recipe", k.recipe.toJson() },
                              { "edited", k.edited },
                              { "preset", k.preset } });
        j["keepers"] = std::move (keep);
        return j;
    }

    void CandidateStore::fromJson (const nlohmann::json& j)
    {
        history.clear();
        kept.clear();
        limit = j.value ("limit", size_t { 1000 });
        position = j.value ("position", -1);

        if (j.contains ("history") && j["history"].is_array())
            for (const auto& r : j["history"])
                history.push_back (Recipe::fromJson (r));

        if (j.contains ("keepers") && j["keepers"].is_array())
        {
            for (const auto& k : j["keepers"])
            {
                Keeper keeper;
                keeper.label = k.value ("label", std::string {});
                keeper.edited = k.value ("edited", false);
                if (k.contains ("recipe"))
                    keeper.recipe = Recipe::fromJson (k["recipe"]);
                if (k.contains ("preset"))
                    keeper.preset = k["preset"];
                kept.push_back (std::move (keeper));
            }
        }

        if (position >= static_cast<int> (history.size()))
            position = static_cast<int> (history.size()) - 1;
    }
}
