/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

// Threat for players (the pet side is in PetTank*.cpp):
// - Tanks make more threat: tank specs in their tanking stance, form or presence (Defensive Stance, Righteous Fury,
//   Bear / Dire Bear Form, Frost Presence) carry a hidden threat bonus (+50% by default, per class settings).
// - Tank mode for specs that were never tanks: Demonology warlock, Enhancement shaman, Survival hunter, Combat rogue.
//   .temucraft tankmode on: the tank classes' passive threat aura, the tank bonus above, Taunt, less damage taken, and their
//   signature abilities make extra threat (Earth Shock, Raptor Strike / Mongoose Bite, Sinister Strike, Searing Pain...).
// - Healers make less threat: healer specs (discipline/holy priest, holy paladin, restoration shaman and druid) carry
//   a hidden threat reduction (30% by default) on everything they do.
// - Group tank roles (TemucraftTankRoles.cpp) can change the tank bonus: off-spec in the Dungeon Finder, another
//   tank chosen, or a stand-in tank.
// - .temucraft threat: what applies to you right now.

#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemucraftModuleText.h"
#include "TemucraftThreatShared.h"
#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

using namespace Acore::ChatCommands;

namespace
{
    constexpr char const* MODULE = "mod-temucraft-threat";
    constexpr uint32 SPELL_TANK_PASSIVE = 57339;    // Tank Class Passive Threat (+43%), tank mode only
    constexpr uint32 SPELL_TANK_BONUS = 25063;      // "Increase Threat": hidden passive, amount set below
    constexpr uint32 SPELL_HEALER_THREAT = 33790;   // "Salvation": hidden passive threat reduction, amount set below
    constexpr uint32 SPELL_TAUNT = 355;
    constexpr uint32 SPELL_DEFENSIVE_STANCE = 71;
    constexpr uint32 SPELL_RIGHTEOUS_FURY = 25780;
    constexpr uint32 SPELL_FROST_PRESENCE = 48263;

    enum Text : uint32 { T_ON = 1, T_OFF, T_NOT_SPEC, T_DISABLED, T_STATUS_TANK, T_STATUS_MODE, T_STATUS_HEALER, T_STATUS_NONE, T_STATUS_TANK_IDLE,
                         T_STATUS_OFF_SPEC, T_STATUS_NOT_CHOSEN, T_STATUS_STAND_IN };
    char const* Say(Player* p, uint32 id, char const* fallback) { return TemucraftModuleText(MODULE, p, id, fallback); }

    struct Settings
    {
        bool enable = true;
        bool tankBonus = true;
        float tankPct = 50.0f;
        std::unordered_map<uint32, float> tankClassPct;  // class -> percent
        std::unordered_set<uint32> tankBonusSpecs;       // class * 10 + tree
        bool needStance = true;
        bool tank = true;
        std::unordered_set<uint32> tankSpecs;            // tank mode specs
        float tankDamageTaken = 10.0f;
        bool tankTaunt = true;
        std::unordered_map<uint32, float> threat;        // first rank -> threat multiplier
        bool healerThreat = true;
        float healerThreatPct = 30.0f;
        std::unordered_set<uint32> healerSpecs;
    } settings;

    struct State
    {
        bool tankMode = false;
        int32 spec = -1;
        uint32 timer = 0;
    };
    std::unordered_map<uint32, State> states;            // character -> state

    std::unordered_set<uint32> ParseSpecs(std::string text)
    {
        std::unordered_set<uint32> out;
        std::replace(text.begin(), text.end(), ',', ' ');
        std::istringstream in(text);
        std::string item;
        while (in >> item)
        {
            std::size_t const colon = item.find(':');
            if (colon == std::string::npos)
                continue;
            uint32 const cls = uint32(std::strtoul(item.substr(0, colon).c_str(), nullptr, 10));
            std::string trees = item.substr(colon + 1);   // "1" or "0-2"
            std::size_t const dash = trees.find('-');
            uint32 const from = uint32(std::strtoul(trees.substr(0, dash).c_str(), nullptr, 10));
            uint32 const to = dash == std::string::npos ? from : uint32(std::strtoul(trees.substr(dash + 1).c_str(), nullptr, 10));
            for (uint32 t = from; t <= to && t < 3; ++t)
                out.insert(cls * 10 + t);
        }
        return out;
    }

    std::unordered_map<uint32, float> ParsePairs(std::string text)
    {
        std::unordered_map<uint32, float> out;
        std::replace(text.begin(), text.end(), ',', ' ');
        std::istringstream in(text);
        std::string item;
        while (in >> item)
            if (std::size_t colon = item.find(':'); colon != std::string::npos)
                out[uint32(std::strtoul(item.substr(0, colon).c_str(), nullptr, 10))] = std::strtof(item.substr(colon + 1).c_str(), nullptr);
        return out;
    }

    uint32 FirstRank(uint32 spell)
    {
        uint32 const first = sSpellMgr->GetFirstSpellInChain(spell);
        return first ? first : spell;
    }

    void LoadSettings()
    {
        settings.enable = sConfigMgr->GetOption<bool>("Temucraft.Threat.Enable", true);
        settings.tankBonus = sConfigMgr->GetOption<bool>("Temucraft.Threat.Tank", true);
        settings.tankPct = std::clamp(sConfigMgr->GetOption<float>("Temucraft.Threat.Tank.Percent", 50.0f), 0.0f, 1000.0f);
        settings.tankClassPct = ParsePairs(sConfigMgr->GetOption<std::string>("Temucraft.Threat.Tank.ClassPercent", ""));
        settings.tankBonusSpecs = ParseSpecs(sConfigMgr->GetOption<std::string>("Temucraft.Threat.Tank.Specs", "1:2 2:1 11:1 6:0-2"));
        settings.needStance = sConfigMgr->GetOption<bool>("Temucraft.Threat.Tank.NeedStance", true);
        settings.tank = sConfigMgr->GetOption<bool>("Temucraft.Threat.TankMode", true);
        settings.tankSpecs = ParseSpecs(sConfigMgr->GetOption<std::string>("Temucraft.Threat.TankMode.Specs", "9:1 7:1 3:2 4:1"));
        settings.tankDamageTaken = std::clamp(sConfigMgr->GetOption<float>("Temucraft.Threat.TankMode.DamageTaken", 10.0f), 0.0f, 90.0f);
        settings.tankTaunt = sConfigMgr->GetOption<bool>("Temucraft.Threat.TankMode.Taunt", true);
        settings.healerThreat = sConfigMgr->GetOption<bool>("Temucraft.Threat.Healer", true);
        settings.healerThreatPct = std::clamp(sConfigMgr->GetOption<float>("Temucraft.Threat.Healer.Percent", 30.0f), 0.0f, 90.0f);
        settings.healerSpecs = ParseSpecs(sConfigMgr->GetOption<std::string>("Temucraft.Threat.Healer.Specs", "5:0 5:1 2:0 7:2 11:2"));
    }

    // spell lists need the spell store: read once it's loaded
    void LoadSpellLists()
    {
        settings.threat.clear();
        for (auto const& [spell, mult] : ParsePairs(sConfigMgr->GetOption<std::string>("Temucraft.Threat.TankMode.Threat",
                 "8042:3 2973:2 1495:2 1752:2 5676:1.5 686:1.5 1949:2 1120:1.5")))
            if (sSpellMgr->GetSpellInfo(spell))
                settings.threat[FirstRank(spell)] = mult;
    }

    int32 ReadSpec(Player* p)
    {
        uint8 points[3] = { 0, 0, 0 };
        p->GetTalentTreePoints(points);
        if (!points[0] && !points[1] && !points[2])
            return -1;
        return int32(p->getClass()) * 10 + p->GetMostPointsTalentTree();
    }

    State& StateOf(Player* p) { return states[p->GetGUID().GetCounter()]; }

    bool CanTankMode(Player* p) { int32 s = StateOf(p).spec; return s >= 0 && settings.tankSpecs.count(uint32(s)); }

    bool InTankMode(Player* p)
    {
        if (!settings.enable || !settings.tank)
            return false;
        auto itr = states.find(p->GetGUID().GetCounter());
        return itr != states.end() && itr->second.tankMode && itr->second.spec >= 0 && settings.tankSpecs.count(uint32(itr->second.spec));
    }

    bool InTankStance(Player* p)
    {
        switch (p->getClass())
        {
            case CLASS_WARRIOR: return p->HasAura(SPELL_DEFENSIVE_STANCE);
            case CLASS_PALADIN: return p->HasAura(SPELL_RIGHTEOUS_FURY);
            case CLASS_DRUID: return p->GetShapeshiftForm() == FORM_BEAR || p->GetShapeshiftForm() == FORM_DIREBEAR;
            case CLASS_DEATH_KNIGHT: return p->HasAura(SPELL_FROST_PRESENCE);
            default: return true;
        }
    }

    float ClassPercent(Player* p)
    {
        auto itr = settings.tankClassPct.find(p->getClass());
        return itr != settings.tankClassPct.end() ? itr->second : settings.tankPct;
    }

    // the tank bonus for this player right now (0: none, negative: less threat)
    float TankBonus(Player* p)
    {
        if (!settings.enable)
            return 0.0f;
        switch (TemucraftTankRoleOf(p))
        {
            case TemucraftTankRole::OffSpec: return TemucraftTankRoleOffSpecThreat();
            case TemucraftTankRole::NotChosen: return 0.0f;
            case TemucraftTankRole::StandIn: return ClassPercent(p);
            default: break;
        }
        if (!settings.tankBonus)
            return 0.0f;
        int32 const s = StateOf(p).spec;
        bool const spec = s >= 0 && settings.tankBonusSpecs.count(uint32(s));
        if (!InTankMode(p) && !(spec && (!settings.needStance || InTankStance(p))))
            return 0.0f;
        return ClassPercent(p);
    }

    float HealerReduction(Player* p)
    {
        int32 const s = StateOf(p).spec;
        if (!settings.enable || !settings.healerThreat || s < 0 || !settings.healerSpecs.count(uint32(s)))
            return 0.0f;
        return settings.healerThreatPct;
    }

    // a hidden passive threat aura with this amount (0: removed)
    void SetAura(Player* p, uint32 spell, int32 amount)
    {
        Aura* aura = p->GetAura(spell, p->GetGUID());
        if (!amount)
        {
            if (aura)
                p->RemoveAura(aura);
            return;
        }
        if (!aura)
            aura = p->AddAura(spell, p);
        if (aura)
            if (AuraEffect* eff = aura->GetEffect(EFFECT_0))
                if (eff->GetAmount() != amount)
                    eff->ChangeAmount(amount);
    }

    // threat auras (cheap: called every second)
    void ApplyAuras(Player* p)
    {
        if (!p->IsInWorld() || !p->IsAlive())
            return;
        SetAura(p, SPELL_TANK_BONUS, int32(TankBonus(p)));
        SetAura(p, SPELL_HEALER_THREAT, -int32(HealerReduction(p)));
        TemucraftTankRoleAuras(p);
    }

    // tank mode on or off (and talents changed): passive threat aura and Taunt
    void Apply(Player* p)
    {
        StateOf(p).spec = ReadSpec(p);
        bool const on = InTankMode(p);
        bool const tankClass = p->getClass() == CLASS_WARRIOR || p->getClass() == CLASS_PALADIN || p->getClass() == CLASS_DRUID ||
                               p->getClass() == CLASS_DEATH_KNIGHT;
        if (on && !p->HasAura(SPELL_TANK_PASSIVE))
            p->AddAura(SPELL_TANK_PASSIVE, p);
        else if (!on && !tankClass && p->HasAura(SPELL_TANK_PASSIVE))
            p->RemoveAurasDueToSpell(SPELL_TANK_PASSIVE);
        if (settings.tankTaunt && p->getClass() != CLASS_WARRIOR)
        {
            bool const taunt = on || TemucraftTankRoleWantsTaunt(p);
            if (taunt && !p->HasSpell(SPELL_TAUNT))
                p->learnSpell(SPELL_TAUNT);
            else if (!taunt && p->HasSpell(SPELL_TAUNT))
                p->removeSpell(SPELL_TAUNT, SPEC_MASK_ALL, false);
        }
        ApplyAuras(p);
    }

    void SaveTank(Player* p)
    {
        CharacterDatabase.Execute("REPLACE INTO `mod_temucraft_character_setting` (`guid`, `setting`, `value`) VALUES ({}, 'tankmode', {})",
            p->GetGUID().GetCounter(), StateOf(p).tankMode ? 1 : 0);
    }
}

bool TemucraftThreatIsTankSpec(Player* p)
{
    int32 const s = ReadSpec(p);
    StateOf(p).spec = s;
    return settings.enable && s >= 0 && (settings.tankBonusSpecs.count(uint32(s)) || InTankMode(p));
}

void TemucraftThreatRefresh(Player* p) { Apply(p); }
float TemucraftThreatTankPercent(Player* p) { return ClassPercent(p); }

class TemucraftThreatWorld : public WorldScript
{
public:
    TemucraftThreatWorld() : WorldScript("TemucraftThreatWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }
    void OnAfterConfigLoad(bool reload) override { LoadSettings(); if (reload) LoadSpellLists(); }
    void OnStartup() override { LoadSpellLists(); }
};

class TemucraftThreatUnit : public UnitScript
{
public:
    TemucraftThreatUnit() : UnitScript("TemucraftThreatUnit", true,
        { UNITHOOK_MODIFY_MELEE_DAMAGE, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN, UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK }) { }

    static void Reduce(Unit* target, uint32& damage)
    {
        if (Player* p = target ? target->ToPlayer() : nullptr)
            if (InTankMode(p))
                damage = uint32(damage * (1.0f - settings.tankDamageTaken / 100.0f));
    }

    void ModifyMeleeDamage(Unit* target, Unit* /*attacker*/, uint32& damage) override { Reduce(target, damage); }
    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* /*attacker*/, uint32& damage, SpellInfo const* /*spellInfo*/) override { Reduce(target, damage); }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (damage <= 0)
            return;
        uint32 d = uint32(damage);
        Reduce(target, d);
        damage = int32(d);
        // signature abilities in tank mode: extra threat on top of the damage's own
        Player* p = attacker ? attacker->ToPlayer() : nullptr;
        if (!p || !spellInfo || !target || target->IsPlayer() || !InTankMode(p))
            return;
        auto itr = settings.threat.find(FirstRank(spellInfo->Id));
        if (itr == settings.threat.end() || itr->second <= 1.0f)
            return;
        float const extra = float(damage) * (itr->second - 1.0f);
        ObjectGuid const targetGuid = target->GetGUID();
        p->m_Events.AddEventAtOffset([p, targetGuid, extra]()
        {
            if (Unit* t = ObjectAccessor::GetUnit(*p, targetGuid))
                if (t->IsAlive() && t->CanHaveThreatList())
                    t->AddThreat(p, extra);
        }, 1ms);
    }
};

class TemucraftThreatPlayer : public PlayerScript
{
public:
    TemucraftThreatPlayer() : PlayerScript("TemucraftThreatPlayer",
        { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_AFTER_SPEC_SLOT_CHANGED,
          PLAYERHOOK_ON_TALENTS_RESET, PLAYERHOOK_ON_DELETE, PLAYERHOOK_ON_PLAYER_RESURRECT, PLAYERHOOK_ON_PLAYER_LEARN_TALENTS }) { }

    void OnPlayerLogin(Player* p) override
    {
        uint32 const guid = p->GetGUID().GetCounter();
        State& s = states[guid];
        s = State();
        if (QueryResult r = CharacterDatabase.Query("SELECT `value` FROM `mod_temucraft_character_setting` WHERE `guid` = {} AND `setting` = 'tankmode'", guid))
            s.tankMode = r->Fetch()[0].Get<int32>() != 0;
        s.spec = ReadSpec(p);
        p->m_Events.AddEventAtOffset([p]() { Apply(p); }, 1s);
    }

    void OnPlayerLogout(Player* p) override { states.erase(p->GetGUID().GetCounter()); }
    void OnPlayerDelete(ObjectGuid guid, uint32 /*account*/) override
    {
        CharacterDatabase.Execute("DELETE FROM `mod_temucraft_character_setting` WHERE `guid` = {} AND `setting` = 'tankmode'", guid.GetCounter());
    }

    // stances, forms and presences change any time: look every second
    void OnPlayerUpdate(Player* p, uint32 diff) override
    {
        if (!settings.enable)
            return;
        auto itr = states.find(p->GetGUID().GetCounter());
        if (itr == states.end())
            return;
        itr->second.timer += diff;
        if (itr->second.timer < 1000)
            return;
        itr->second.timer = 0;
        ApplyAuras(p);
    }

    void OnPlayerAfterSpecSlotChanged(Player* p, uint8 /*slot*/) override { Apply(p); }
    void OnPlayerTalentsReset(Player* p, bool /*noCost*/) override { Apply(p); }
    void OnPlayerLearnTalents(Player* p, uint32, uint32, uint32) override { p->m_Events.AddEventAtOffset([p]() { Apply(p); }, 100ms); }
    void OnPlayerResurrect(Player* p, float /*pct*/, bool& /*sickness*/) override { p->m_Events.AddEventAtOffset([p]() { Apply(p); }, 500ms); }
};

class TemucraftThreatCommands : public CommandScript
{
public:
    TemucraftThreatCommands() : CommandScript("TemucraftThreatCommands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable table =
        {
            { "temucraft tankmode", HandleTank,   101355, Console::No },
            { "temucraft threat",   HandleThreat, 101356, Console::No },
        };
        return table;
    }

    static bool HandleTank(ChatHandler* handler, Optional<std::string> arg)
    {
        Player* p = handler->GetPlayer();
        if (!p)
            return false;
        if (!settings.enable || !settings.tank)
        {
            handler->SendSysMessage(Say(p, T_DISABLED, "Tank mode isn't available on this server."));
            return true;
        }
        StateOf(p).spec = ReadSpec(p);
        if (!CanTankMode(p))
        {
            handler->SendSysMessage(Say(p, T_NOT_SPEC, "Tank mode is for Demonology warlocks, Enhancement shamans, Survival hunters and Combat rogues."));
            return true;
        }
        bool& on = StateOf(p).tankMode;
        if (!arg)
            on = !on;
        else
            on = (*arg == "on" || *arg == "1");
        SaveTank(p);
        Apply(p);
        handler->SendSysMessage(on ? Say(p, T_ON, "|cffc69b6dTank mode|r on: more threat, Taunt and less damage taken.")
                                   : Say(p, T_OFF, "|cffc69b6dTank mode|r off."));
        return true;
    }

    static bool HandleThreat(ChatHandler* handler)
    {
        Player* p = handler->GetPlayer();
        if (!p)
            return false;
        StateOf(p).spec = ReadSpec(p);
        ApplyAuras(p);
        bool any = false;
        switch (TemucraftTankRoleOf(p))
        {
            case TemucraftTankRole::OffSpec:
                handler->PSendSysMessage(Say(p, T_STATUS_OFF_SPEC, "Tank role without a tank spec: |cffff8040{}% threat|r, 10% less health, armor and damage, 20% less maximum mana, rage and energy."),
                    int32(TemucraftTankRoleOffSpecThreat()));
                any = true;
                break;
            case TemucraftTankRole::NotChosen:
                handler->SendSysMessage(Say(p, T_STATUS_NOT_CHOSEN, "Another player is your group's tank: no tank threat bonus for you (.temucraft tankrole)."));
                any = true;
                break;
            case TemucraftTankRole::StandIn:
                handler->PSendSysMessage(Say(p, T_STATUS_STAND_IN, "Stand-in tank: |cff40ff40+{}% threat|r, Taunt, 1% more power regeneration, 10% less health, armor and damage."),
                    uint32(ClassPercent(p)));
                any = true;
                break;
            default:
                break;
        }
        float const bonus = any ? 0.0f : TankBonus(p);
        int32 const spec = StateOf(p).spec;
        if (bonus > 0.0f)
        {
            handler->PSendSysMessage(Say(p, T_STATUS_TANK, "Tank threat: |cff40ff40+{}%|r."), uint32(bonus));
            any = true;
        }
        else if (!any && settings.enable && settings.tankBonus && spec >= 0 && settings.tankBonusSpecs.count(uint32(spec)))
        {
            handler->SendSysMessage(Say(p, T_STATUS_TANK_IDLE, "Tank threat: only in Defensive Stance, Righteous Fury, Bear Form or Frost Presence."));
            any = true;
        }
        if (InTankMode(p))
        {
            handler->PSendSysMessage(Say(p, T_STATUS_MODE, "Tank mode: on (Taunt, {}% less damage taken, more threat from your signature abilities)."),
                uint32(settings.tankDamageTaken));
            any = true;
        }
        if (float const less = HealerReduction(p); less > 0.0f)
        {
            handler->PSendSysMessage(Say(p, T_STATUS_HEALER, "Healer threat: |cffff8040-{}%|r."), uint32(less));
            any = true;
        }
        if (!any)
            handler->SendSysMessage(Say(p, T_STATUS_NONE, "No threat changes for your spec."));
        return true;
    }
};

void AddTemucraftThreatScripts()
{
    new TemucraftThreatWorld();
    new TemucraftThreatUnit();
    new TemucraftThreatPlayer();
    new TemucraftThreatCommands();
}
