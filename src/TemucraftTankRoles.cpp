/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

// Group tank roles (mod-temucraft-threat):
// - Dungeon Finder: whoever took the tank role without a tank spec gets "Not Playing Your Spec" (20% less threat
//   instead of the tank bonus, 10% less health, armor and damage, 20% less maximum mana, rage and energy). A tank spec
//   that queued for another role doesn't get the tank bonus.
// - Walk-in groups: the spec decides. With more tank specs than places (1 in a party, 2 in a 10 player raid, 3 in a
//   bigger one) the tanks claim the place; if more claim than fit, the group votes (ties: the leader's vote, then who
//   claimed first). Tanks not chosen play without the tank bonus.
// - No tank: out of combat, once the party is full or inside a dungeon or raid, everyone is asked whether someone
//   stands in as tank (themselves, or someone they pick, who has to accept). A stand-in tank gets the tank bonus,
//   Taunt and 1% more power regeneration, but 10% less health, armor and damage. It ends when a real tank joins.
// The Temucraft interface (TemucraftTankRoles.lua, prefix TTR) shows the questions with what each choice means;
// .tankrole does the same in chat.

#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "GameTime.h"
#include "Group.h"
#include "LFGMgr.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "TemucraftAddonMessage.h"
#include "TemucraftModuleText.h"
#include "TemucraftThreatShared.h"
#include <algorithm>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    constexpr char const* MODULE = "mod-temucraft-threat";
    constexpr char const* PREFIX = "TTR";

    enum Text : uint32
    {
        T_CLAIM = 20, T_VOTE, T_NO_TANK, T_PICKED, T_CHOSEN, T_STAND_IN, T_NOBODY_CLAIMED, T_NOBODY_STANDS_IN,
        T_STAND_IN_ENDS, T_PICK_SENT, T_PICK_DECLINED, T_NOT_NOW, T_DONE, T_NO_GROUP, T_OFF_SPEC, T_STATUS_NONE,
        T_STATUS_CHOSEN, T_STATUS_STAND_IN, T_STATUS_LFG, T_STEP_DOWN, T_UNKNOWN_NAME, T_PLACES_FULL, T_PICK_TIMEOUT
    };
    char const* Say(Player* p, uint32 id, char const* fallback) { return TemucraftModuleText(MODULE, p, id, fallback); }

    struct Settings
    {
        bool enable = true;
        uint32 partyTanks = 1, raid10Tanks = 2, raid25Tanks = 3;
        bool offSpec = true;
        float offSpecThreat = -20.0f;
        bool standIn = true;
        bool standInTaunt = true;
        bool promptFull = true, promptInstance = true;
        uint32 cooldown = 600;
        uint32 claimSeconds = 30, voteSeconds = 30, noTankSeconds = 60, pickSeconds = 30;
    } settings;

    enum class Phase { None, Claim, Vote, NoTank };

    struct Pick { uint32 nominator = 0; int64 until = 0; };

    struct GroupRoles
    {
        Phase phase = Phase::None;
        int64 until = 0;
        bool voteForStandIn = false;
        std::vector<uint32> claims;                    // claimers / volunteers, in order
        std::unordered_set<uint32> answered;           // answered the current question
        std::vector<uint32> candidates;
        std::unordered_map<uint32, uint32> votes;      // voter -> candidate
        std::unordered_set<uint32> chosen;             // the group's tanks, when there are too many
        bool decided = false;
        bool claimSuppressed = false;                  // nobody claimed: don't ask again until the group changes
        std::unordered_set<uint32> standIns;
        std::unordered_map<uint32, Pick> picks;        // nominee -> who picked them
        int64 nextNoTank = 0;
        std::vector<uint32> members;                   // sorted, to see changes
    };

    std::unordered_map<uint32, GroupRoles> groups;     // group guid counter -> roles
    std::unordered_map<uint32, uint32> timers;         // player -> ms since the last look
    std::recursive_mutex stateLock;                    // map update threads share groups and timers

    int64 Now() { return int64(GameTime::GetGameTime().count()); }
    uint32 Key(Group* g) { return g->GetGUID().GetCounter(); }
    uint32 Id(Player* p) { return p->GetGUID().GetCounter(); }

    void LoadSettings()
    {
        settings.enable = sConfigMgr->GetOption<bool>("Temucraft.Threat.Roles", true);
        settings.partyTanks = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Temucraft.Threat.Roles.PartyTanks", 1));
        settings.raid10Tanks = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Temucraft.Threat.Roles.Raid10Tanks", 2));
        settings.raid25Tanks = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Temucraft.Threat.Roles.Raid25Tanks", 3));
        settings.offSpec = sConfigMgr->GetOption<bool>("Temucraft.Threat.Roles.OffSpec", true);
        settings.offSpecThreat = std::clamp(sConfigMgr->GetOption<float>("Temucraft.Threat.Roles.OffSpec.Threat", -20.0f), -90.0f, 0.0f);
        settings.standIn = sConfigMgr->GetOption<bool>("Temucraft.Threat.Roles.StandIn", true);
        settings.standInTaunt = sConfigMgr->GetOption<bool>("Temucraft.Threat.Roles.StandIn.Taunt", true);
        std::string when = sConfigMgr->GetOption<std::string>("Temucraft.Threat.Roles.NoTank.Prompt", "full,instance");
        settings.promptFull = when.find("full") != std::string::npos;
        settings.promptInstance = when.find("instance") != std::string::npos;
        settings.cooldown = sConfigMgr->GetOption<uint32>("Temucraft.Threat.Roles.NoTank.Cooldown", 600);
        settings.claimSeconds = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Temucraft.Threat.Roles.ClaimSeconds", 30), 10, 300);
        settings.voteSeconds = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Temucraft.Threat.Roles.VoteSeconds", 30), 10, 300);
        settings.noTankSeconds = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Temucraft.Threat.Roles.NoTankSeconds", 60), 10, 300);
        settings.pickSeconds = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Temucraft.Threat.Roles.PickSeconds", 30), 10, 300);
    }

    bool Managed(Group* g) { return g && !g->isBGGroup() && !g->isBFGroup(); }

    uint32 Places(Group* g)
    {
        if (!g->isRaidGroup())
            return settings.partyTanks;
        return g->GetMemberSlots().size() > 10 ? settings.raid25Tanks : settings.raid10Tanks;
    }

    std::vector<Player*> Online(Group* g)
    {
        std::vector<Player*> out;
        for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
            if (Player* m = ref->GetSource())
                if (m->IsInWorld() && m->GetSession())
                    out.push_back(m);
        return out;
    }

    Player* Find(Group* g, uint32 id)
    {
        for (Player* m : Online(g))
            if (Id(m) == id)
                return m;
        return nullptr;
    }

    Player* FindByName(Group* g, std::string name)
    {
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        for (Player* m : Online(g))
        {
            std::string n = m->GetName();
            std::transform(n.begin(), n.end(), n.begin(), ::tolower);
            if (n == name)
                return m;
        }
        return nullptr;
    }

    std::string Names(Group* g, std::vector<uint32> const& ids)
    {
        std::string out;
        for (uint32 id : ids)
            if (Player* m = Find(g, id))
                out += (out.empty() ? "" : ",") + TemucraftAddonClean(m->GetName());
        return out;
    }

    std::string NameList(Group* g, std::vector<uint32> const& ids)
    {
        std::string out = Names(g, ids);
        std::replace(out.begin(), out.end(), ',', '/');
        std::string pretty;
        for (char c : out)
            pretty += c == '/' ? std::string(", ") : std::string(1, c);
        return pretty;
    }

    void Tell(Player* p, std::string const& text) { ChatHandler(p->GetSession()).SendSysMessage(text); }

    template <typename... Args>
    void TellAll(Group* g, uint32 id, char const* fallback, Args&&... args)
    {
        for (Player* m : Online(g))
            Tell(m, Acore::StringFormat(Say(m, id, fallback), args...));
    }

    void SendAll(Group* g, std::string const& text)
    {
        for (Player* m : Online(g))
            TemucraftAddonSend(m, PREFIX, text);
    }

    // with the receiver's own tank bonus at the end, for the window's explanation
    std::string WithPercent(Player* m, std::string const& text) { return text + "~" + std::to_string(uint32(TemucraftThreatTankPercent(m))); }

    void SendAllWithPercent(Group* g, std::string const& text)
    {
        for (Player* m : Online(g))
            TemucraftAddonSend(m, PREFIX, WithPercent(m, text));
    }

    std::vector<uint32> Tanks(Group* g)
    {
        std::vector<uint32> out;
        for (Player* m : Online(g))
            if (TemucraftThreatIsTankSpec(m))
                out.push_back(Id(m));
        return out;
    }

    bool AnyInCombat(Group* g)
    {
        for (Player* m : Online(g))
            if (m->IsInCombat())
                return true;
        return false;
    }

    bool InInstance(Group* g)
    {
        for (Player* m : Online(g))
            if (Map* map = m->GetMap())
                if (map->IsDungeon() || map->IsRaid())
                    return true;
        return false;
    }

    void RefreshAll(Group* g)
    {
        for (Player* m : Online(g))
            TemucraftThreatRefresh(m);
    }

    void EndPhase(Group* g, GroupRoles& r)
    {
        r.phase = Phase::None;
        r.claims.clear();
        r.answered.clear();
        r.candidates.clear();
        r.votes.clear();
        SendAll(g, "END");
    }

    void StartVote(Group* g, GroupRoles& r, std::vector<uint32> candidates, bool standIn)
    {
        r.phase = Phase::Vote;
        r.until = Now() + settings.voteSeconds;
        r.voteForStandIn = standIn;
        r.candidates = std::move(candidates);
        r.votes.clear();
        r.answered.clear();
        uint32 const places = Places(g);
        SendAllWithPercent(g, Acore::StringFormat("VOTE~{}~{}~{}~{}", settings.voteSeconds, places, Names(g, r.candidates), standIn ? "standin" : "tank"));
        TellAll(g, T_VOTE, "|cffc69b6dTank:|r more players want to tank than there are places ({}). Vote with .tankrole vote <name>: {}.",
            places, NameList(g, r.candidates));
    }

    void SetStandIns(Group* g, GroupRoles& r, std::vector<uint32> const& ids)
    {
        r.standIns.clear();
        for (uint32 id : ids)
            r.standIns.insert(id);
        r.nextNoTank = Now() + settings.cooldown;
        SendAll(g, "RESULT~standin~" + Names(g, ids));
        TellAll(g, T_STAND_IN, "|cffc69b6dTank:|r {} stands in as tank (+threat and Taunt, 10% less health, armor and damage).", NameList(g, ids));
        RefreshAll(g);
    }

    void SetChosen(Group* g, GroupRoles& r, std::vector<uint32> const& ids)
    {
        r.chosen.clear();
        for (uint32 id : ids)
            r.chosen.insert(id);
        r.decided = true;
        SendAll(g, "RESULT~tank~" + Names(g, ids));
        TellAll(g, T_CHOSEN, "|cffc69b6dTank:|r {} is the group's tank. Other tanks play without the tank threat bonus.", NameList(g, ids));
        RefreshAll(g);
    }

    void Resolve(Group* g, GroupRoles& r)
    {
        uint32 const places = Places(g);
        Phase const phase = r.phase;
        std::vector<uint32> claims = r.claims;
        if (phase == Phase::Claim)
        {
            std::vector<uint32> const tanks = Tanks(g);
            claims.erase(std::remove_if(claims.begin(), claims.end(), [&](uint32 id) { return std::find(tanks.begin(), tanks.end(), id) == tanks.end(); }), claims.end());
            EndPhase(g, r);
            if (claims.empty())
            {
                r.claimSuppressed = true;
                r.decided = false;
                r.chosen.clear();
                TellAll(g, T_NOBODY_CLAIMED, "|cffc69b6dTank:|r nobody claimed the tank place: every tank keeps the bonus for now (.tankrole claim).");
                RefreshAll(g);
            }
            else if (claims.size() <= places)
                SetChosen(g, r, claims);
            else
                StartVote(g, r, claims, false);
        }
        else if (phase == Phase::NoTank)
        {
            EndPhase(g, r);
            if (claims.empty())
            {
                r.nextNoTank = Now() + settings.cooldown;
                TellAll(g, T_NOBODY_STANDS_IN, "|cffc69b6dTank:|r nobody stands in as tank. Anyone can still: .tankrole volunteer");
            }
            else if (claims.size() <= places)
                SetStandIns(g, r, claims);
            else
                StartVote(g, r, claims, true);
        }
        else if (phase == Phase::Vote)
        {
            std::unordered_map<uint32, uint32> count;
            uint32 leaderVote = 0;
            for (auto const& [voter, candidate] : r.votes)
            {
                ++count[candidate];
                if (voter == g->GetLeaderGUID().GetCounter())
                    leaderVote = candidate;
            }
            std::vector<uint32> order = r.candidates;
            std::stable_sort(order.begin(), order.end(), [&](uint32 a, uint32 b)
            {
                if (count[a] != count[b])
                    return count[a] > count[b];
                return (a == leaderVote) > (b == leaderVote);
            });
            order.resize(std::min<std::size_t>(order.size(), places));
            bool const standIn = r.voteForStandIn;
            EndPhase(g, r);
            if (standIn)
                SetStandIns(g, r, order);
            else
                SetChosen(g, r, order);
        }
    }

    void StartClaim(Group* g, GroupRoles& r, std::vector<uint32> const& tanks)
    {
        r.phase = Phase::Claim;
        r.until = Now() + settings.claimSeconds;
        r.claims.clear();
        r.answered.clear();
        uint32 const places = Places(g);
        for (Player* m : Online(g))
        {
            bool const tank = std::find(tanks.begin(), tanks.end(), Id(m)) != tanks.end();
            TemucraftAddonSend(m, PREFIX, WithPercent(m, Acore::StringFormat("CLAIM~{}~{}~{}~{}", settings.claimSeconds, places, Names(g, tanks), tank ? 1 : 0)));
            if (tank)
                Tell(m, Acore::StringFormat(Say(m, T_CLAIM, "|cffc69b6dTank:|r {} tanks in the group and {} place(s). Want it? .tankrole claim or .tankrole pass"),
                    tanks.size(), places));
        }
    }

    void StartNoTank(Group* g, GroupRoles& r)
    {
        r.phase = Phase::NoTank;
        r.until = Now() + settings.noTankSeconds;
        r.claims.clear();
        r.answered.clear();
        std::vector<uint32> all;
        for (Player* m : Online(g))
            all.push_back(Id(m));
        SendAllWithPercent(g, Acore::StringFormat("NOTANK~{}~{}~{}", settings.noTankSeconds, Places(g), Names(g, all)));
        TellAll(g, T_NO_TANK, "|cffc69b6dTank:|r nobody in the group plays a tank spec. Stand in? .tankrole volunteer, or .tankrole pick <name>.");
    }

    // a volunteer (or someone who accepted a pick) while nobody tanks
    void Volunteer(Group* g, GroupRoles& r, Player* p)
    {
        uint32 const id = Id(p);
        if (r.phase == Phase::NoTank)
        {
            if (std::find(r.claims.begin(), r.claims.end(), id) == r.claims.end())
                r.claims.push_back(id);
            r.answered.insert(id);
            return;
        }
        if (r.phase != Phase::None || !Tanks(g).empty())
        {
            Tell(p, Say(p, T_NOT_NOW, "|cffc69b6dTank:|r not now (the group has a tank, or a question is open)."));
            return;
        }
        if (r.standIns.size() >= Places(g))
        {
            Tell(p, Say(p, T_PLACES_FULL, "|cffc69b6dTank:|r the stand-in places are taken."));
            return;
        }
        std::vector<uint32> ids(r.standIns.begin(), r.standIns.end());
        ids.push_back(id);
        SetStandIns(g, r, ids);
    }

    void Pick(Group* g, GroupRoles& r, Player* p, Player* target)
    {
        if (target == p)
        {
            Volunteer(g, r, p);
            return;
        }
        r.picks[Id(target)] = { Id(p), Now() + settings.pickSeconds };
        if (r.phase == Phase::NoTank)
            r.answered.insert(Id(p));
        TemucraftAddonSend(target, PREFIX, WithPercent(target, Acore::StringFormat("PICKED~{}~{}", TemucraftAddonClean(p->GetName()), settings.pickSeconds)));
        Tell(target, Acore::StringFormat(Say(target, T_PICKED, "|cffc69b6dTank:|r {} picked you to stand in as tank. .tankrole accept or .tankrole decline"), p->GetName()));
        Tell(p, Acore::StringFormat(Say(p, T_PICK_SENT, "|cffc69b6dTank:|r asked {}."), target->GetName()));
    }

    bool AllAnswered(Group* g, GroupRoles& r, std::vector<uint32> const& who)
    {
        for (uint32 id : who)
            if (!r.answered.count(id) && Find(g, id))
                return false;
        return r.picks.empty();
    }

    void Evaluate(Group* g)
    {
        GroupRoles& r = groups[Key(g)];
        int64 const now = Now();

        // who's in the group: changes start over
        std::vector<uint32> members;
        for (auto const& slot : g->GetMemberSlots())
            members.push_back(slot.guid.GetCounter());
        std::sort(members.begin(), members.end());
        if (members != r.members)
        {
            bool joined = false;
            for (uint32 id : members)
                if (!std::binary_search(r.members.begin(), r.members.end(), id))
                    joined = true;
            r.members = members;
            auto gone = [&](uint32 id) { return !std::binary_search(members.begin(), members.end(), id); };
            for (auto it = r.chosen.begin(); it != r.chosen.end();)
                it = gone(*it) ? (r.decided = false, r.chosen.erase(it)) : std::next(it);
            for (auto it = r.standIns.begin(); it != r.standIns.end();)
                it = gone(*it) ? r.standIns.erase(it) : std::next(it);
            if (joined)
            {
                r.nextNoTank = 0;
                r.claimSuppressed = false;
                r.decided = false;
            }
            if (r.phase != Phase::None)
                EndPhase(g, r);
            RefreshAll(g);
        }

        // picks that ran out
        for (auto it = r.picks.begin(); it != r.picks.end();)
        {
            if (it->second.until > now)
            {
                ++it;
                continue;
            }
            if (Player* nominator = Find(g, it->second.nominator))
                Tell(nominator, Say(nominator, T_PICK_TIMEOUT, "|cffc69b6dTank:|r your pick didn't answer in time."));
            it = r.picks.erase(it);
        }

        std::vector<uint32> const tanks = Tanks(g);
        uint32 const places = Places(g);

        // a real tank: stand-ins end
        if (!tanks.empty() && !r.standIns.empty())
        {
            r.standIns.clear();
            TellAll(g, T_STAND_IN_ENDS, "|cffc69b6dTank:|r a tank is in the group: the stand-in tank ends.");
            RefreshAll(g);
        }

        if (r.phase != Phase::None)
        {
            std::vector<uint32> who;
            if (r.phase == Phase::Claim)
                who = tanks;
            else
                for (Player* m : Online(g))
                    who.push_back(Id(m));
            if (now >= r.until || AllAnswered(g, r, who))
                Resolve(g, r);
            return;
        }

        if (tanks.size() > places)
        {
            bool valid = r.decided && !r.chosen.empty();
            for (uint32 id : r.chosen)
                if (std::find(tanks.begin(), tanks.end(), id) == tanks.end())
                    valid = false;
            if (!valid && r.decided)
            {
                r.decided = false;
                r.chosen.clear();
                RefreshAll(g);
            }
            if (!valid && !r.claimSuppressed && !AnyInCombat(g))
                StartClaim(g, r, tanks);
            return;
        }
        if (r.decided)
        {
            r.decided = false;
            r.chosen.clear();
            RefreshAll(g);
        }

        if (tanks.empty() && r.standIns.empty() && settings.standIn && now >= r.nextNoTank && !AnyInCombat(g))
        {
            bool const full = !g->isRaidGroup() && g->GetMemberSlots().size() >= 5;
            if ((settings.promptFull && full) || (settings.promptInstance && InInstance(g)))
                StartNoTank(g, r);
        }
    }

    GroupRoles* RolesOf(Player* p)
    {
        std::lock_guard<std::recursive_mutex> guard(stateLock);
        Group* g = p->GetGroup();
        if (!Managed(g) || g->isLFGGroup())
            return nullptr;
        auto it = groups.find(Key(g));
        return it == groups.end() ? nullptr : &it->second;
    }

    // the group member who drives the group's questions: the first one online
    bool Drives(Player* p, Group* g)
    {
        for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
            if (Player* m = ref->GetSource())
                if (m->IsInWorld())
                    return m == p;
        return false;
    }

    void Answer(Player* p, std::string const& what, std::string const& arg)
    {
        std::lock_guard<std::recursive_mutex> guard(stateLock);
        Group* g = p->GetGroup();
        if (!settings.enable || !Managed(g) || g->isLFGGroup())
        {
            Tell(p, Say(p, T_NO_GROUP, "|cffc69b6dTank:|r tank roles are for walk-in groups (the Dungeon Finder sets its own)."));
            return;
        }
        GroupRoles& r = groups[Key(g)];
        uint32 const id = Id(p);
        if (what == "CLAIM")
        {
            if (r.phase == Phase::Claim && TemucraftThreatIsTankSpec(p))
            {
                if (std::find(r.claims.begin(), r.claims.end(), id) == r.claims.end())
                    r.claims.push_back(id);
                r.answered.insert(id);
            }
            else if (r.phase == Phase::None && TemucraftThreatIsTankSpec(p) && Tanks(g).size() > Places(g))
            {
                r.claimSuppressed = false;
                StartClaim(g, r, Tanks(g));
                r.claims.push_back(id);
                r.answered.insert(id);
            }
            else
                Tell(p, Say(p, T_NOT_NOW, "|cffc69b6dTank:|r not now (the group has a tank, or a question is open)."));
        }
        else if (what == "PASS")
            r.answered.insert(id);
        else if (what == "VOTE")
        {
            Player* target = FindByName(g, arg);
            if (r.phase != Phase::Vote || !target || std::find(r.candidates.begin(), r.candidates.end(), Id(target)) == r.candidates.end())
            {
                Tell(p, Say(p, T_UNKNOWN_NAME, "|cffc69b6dTank:|r no vote open for that name."));
                return;
            }
            r.votes[id] = Id(target);
            r.answered.insert(id);
        }
        else if (what == "VOLUNTEER")
            Volunteer(g, r, p);
        else if (what == "NOTHANKS")
            r.answered.insert(id);
        else if (what == "PICK")
        {
            Player* target = FindByName(g, arg);
            if (!target || !Tanks(g).empty())
            {
                Tell(p, Say(p, !target ? T_UNKNOWN_NAME : T_NOT_NOW, !target ? "|cffc69b6dTank:|r nobody of that name in your group." :
                    "|cffc69b6dTank:|r not now (the group has a tank, or a question is open)."));
                return;
            }
            Pick(g, r, p, target);
        }
        else if (what == "ACCEPT" || what == "DECLINE")
        {
            auto it = r.picks.find(id);
            if (it == r.picks.end())
            {
                Tell(p, Say(p, T_NOT_NOW, "|cffc69b6dTank:|r not now (the group has a tank, or a question is open)."));
                return;
            }
            uint32 const nominator = it->second.nominator;
            r.picks.erase(it);
            if (what == "ACCEPT")
                Volunteer(g, r, p);
            else
            {
                r.answered.insert(id);
                if (Player* n = Find(g, nominator))
                    Tell(n, Acore::StringFormat(Say(n, T_PICK_DECLINED, "|cffc69b6dTank:|r {} declined."), p->GetName()));
            }
        }
        else if (what == "STEPDOWN")
        {
            if (r.standIns.erase(id))
            {
                r.nextNoTank = Now() + settings.cooldown;
                TellAll(g, T_STEP_DOWN, "|cffc69b6dTank:|r {} steps down as tank.", p->GetName());
                RefreshAll(g);
            }
            else if (r.chosen.erase(id))
            {
                std::vector<uint32> rest;
                for (uint32 t : Tanks(g))
                    if (t != id && !r.chosen.count(t))
                        rest.push_back(t);
                TellAll(g, T_STEP_DOWN, "|cffc69b6dTank:|r {} steps down as tank.", p->GetName());
                if (!rest.empty())
                    r.chosen.insert(rest.front());
                SetChosen(g, r, std::vector<uint32>(r.chosen.begin(), r.chosen.end()));
            }
        }
        else if (what == "ASK")
        {
            if (r.phase != Phase::None)
                return;
            r.claimSuppressed = false;
            r.nextNoTank = 0;
            std::vector<uint32> const tanks = Tanks(g);
            if (tanks.size() > Places(g))
                StartClaim(g, r, tanks);
            else if (tanks.empty() && r.standIns.empty())
                StartNoTank(g, r);
            else
                Tell(p, Say(p, T_NOT_NOW, "|cffc69b6dTank:|r not now (the group has a tank, or a question is open)."));
        }
    }
}

TemucraftTankRole TemucraftTankRoleOf(Player* p)
{
    std::lock_guard<std::recursive_mutex> guard(stateLock);
    if (!settings.enable || !p)
        return TemucraftTankRole::Normal;
    Group* g = p->GetGroup();
    if (!Managed(g))
        return TemucraftTankRole::Normal;
    if (g->isLFGGroup())
    {
        bool const tankRole = sLFGMgr->GetRoles(p->GetGUID()) & lfg::PLAYER_ROLE_TANK;
        bool const tankSpec = TemucraftThreatIsTankSpec(p);
        if (tankRole && !tankSpec && settings.offSpec)
            return TemucraftTankRole::OffSpec;
        if (!tankRole && tankSpec)
            return TemucraftTankRole::NotChosen;
        return TemucraftTankRole::Normal;
    }
    auto it = groups.find(g->GetGUID().GetCounter());
    if (it == groups.end())
        return TemucraftTankRole::Normal;
    uint32 const id = p->GetGUID().GetCounter();
    if (it->second.standIns.count(id))
        return TemucraftTankRole::StandIn;
    if (it->second.decided && !it->second.chosen.count(id) && TemucraftThreatIsTankSpec(p))
        return TemucraftTankRole::NotChosen;
    return TemucraftTankRole::Normal;
}

float TemucraftTankRoleOffSpecThreat() { return settings.offSpecThreat; }

bool TemucraftTankRoleWantsTaunt(Player* p)
{
    return settings.standInTaunt && TemucraftTankRoleOf(p) == TemucraftTankRole::StandIn;
}

void TemucraftTankRoleAuras(Player* p)
{
    TemucraftTankRole const role = TemucraftTankRoleOf(p);
    auto set = [p](uint32 spell, bool on)
    {
        if (on && !p->HasAura(spell) && sSpellMgr->GetSpellInfo(spell))
            p->AddAura(spell, p);
        else if (!on && p->HasAura(spell))
            p->RemoveAurasDueToSpell(spell);
    };
    set(SPELL_TEMUCRAFT_OFF_SPEC, role == TemucraftTankRole::OffSpec);
    set(SPELL_TEMUCRAFT_OFF_SPEC_POWER, role == TemucraftTankRole::OffSpec);
    set(SPELL_TEMUCRAFT_STAND_IN, role == TemucraftTankRole::StandIn);
    set(SPELL_TEMUCRAFT_STAND_IN_POWER, role == TemucraftTankRole::StandIn);
}

class TemucraftTankRolesWorld : public WorldScript
{
public:
    TemucraftTankRolesWorld() : WorldScript("TemucraftTankRolesWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }
    void OnAfterConfigLoad(bool /*reload*/) override { LoadSettings(); }
};

class TemucraftTankRolesGroup : public GroupScript
{
public:
    TemucraftTankRolesGroup() : GroupScript("TemucraftTankRolesGroup", { GROUPHOOK_ON_DISBAND }) { }
    void OnDisband(Group* g) override
    {
        std::lock_guard<std::recursive_mutex> guard(stateLock);
        groups.erase(g->GetGUID().GetCounter());
    }
};

class TemucraftTankRolesPlayer : public PlayerScript
{
public:
    TemucraftTankRolesPlayer() : PlayerScript("TemucraftTankRolesPlayer",
        { PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

    void OnPlayerUpdate(Player* p, uint32 diff) override
    {
        if (!settings.enable)
            return;
        std::lock_guard<std::recursive_mutex> guard(stateLock);
        uint32& t = timers[p->GetGUID().GetCounter()];
        t += diff;
        if (t < 1000)
            return;
        t = 0;
        Group* g = p->GetGroup();
        if (Managed(g) && !g->isLFGGroup() && Drives(p, g))
            Evaluate(g);
    }

    void OnPlayerLogout(Player* p) override
    {
        std::lock_guard<std::recursive_mutex> guard(stateLock);
        timers.erase(p->GetGUID().GetCounter());
    }

    using PlayerScript::OnPlayerCanUseChat;
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override
    {
        std::string body;
        if (!TemucraftAddonParse(player, type, language, msg, receiver, PREFIX, body))
            return true;
        std::vector<std::string> f = TemucraftAddonFields(body);
        if (!f.empty())
            Answer(player, f[0], f.size() > 1 ? f[1] : "");
        return false;
    }
};

class TemucraftTankRolesCommands : public CommandScript
{
public:
    TemucraftTankRolesCommands() : CommandScript("TemucraftTankRolesCommands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable table = { { "tankrole", HandleTankRole, SEC_PLAYER, Console::No } };
        return table;
    }

    static bool HandleTankRole(ChatHandler* handler, Optional<std::string> what, Optional<std::string> name)
    {
        Player* p = handler->GetPlayer();
        if (!p)
            return false;
        if (!what)
        {
            switch (TemucraftTankRoleOf(p))
            {
                case TemucraftTankRole::OffSpec:
                    handler->SendSysMessage(Say(p, T_OFF_SPEC, "|cffc69b6dTank:|r you took the tank role without a tank spec (Not Playing Your Spec)."));
                    break;
                case TemucraftTankRole::StandIn:
                    handler->SendSysMessage(Say(p, T_STATUS_STAND_IN, "|cffc69b6dTank:|r you stand in as your group's tank (.tankrole stepdown to stop)."));
                    break;
                case TemucraftTankRole::NotChosen:
                    handler->SendSysMessage(Say(p, T_STATUS_CHOSEN, "|cffc69b6dTank:|r another player is your group's tank."));
                    break;
                default:
                    if (p->GetGroup() && p->GetGroup()->isLFGGroup())
                        handler->SendSysMessage(Say(p, T_STATUS_LFG, "|cffc69b6dTank:|r the Dungeon Finder set your role."));
                    else
                        handler->SendSysMessage(Say(p, T_STATUS_NONE, "|cffc69b6dTank:|r .tankrole claim | pass | vote <name> | volunteer | pick <name> | accept | decline | stepdown | ask"));
                    break;
            }
            return true;
        }
        std::string w = *what;
        std::transform(w.begin(), w.end(), w.begin(), ::toupper);
        Answer(p, w, name ? *name : "");
        if (w == "CLAIM" || w == "PASS" || w == "VOTE" || w == "NOTHANKS" || w == "DECLINE")
            handler->SendSysMessage(Say(p, T_DONE, "|cffc69b6dTank:|r noted."));
        return true;
    }
};

void AddTemucraftTankRolesScripts()
{
    new TemucraftTankRolesWorld();
    new TemucraftTankRolesGroup();
    new TemucraftTankRolesPlayer();
    new TemucraftTankRolesCommands();
}
