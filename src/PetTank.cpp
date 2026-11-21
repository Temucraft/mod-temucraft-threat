/*
 * mod-temucraft-threat: settings, where a player is, which pets count, and each player's own on/off choices.
 */

#include "PetTank.h"

#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "Pet.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace
{
    // The taunts that make a pet a tank (first ranks; every rank counts)
    constexpr uint32 TAUNT_SPELLS[] =
    {
        2649,  // Growl (hunter pets)
        3716,  // Torment (Voidwalker)
        17735, // Suffering (Voidwalker)
        33698, // Anguish (Felguard)
        36213, // Angered Earth (Greater Earth Elemental)
    };

    struct PlaceInfo
    {
        char const* key;     // .temucraft pettank and the addon
        char const* option;  // PetTank.Where.<option>
        char const* name;    // shown to players
        char const* fallback;
    };

    constexpr PlaceInfo PLACES[MAX_PET_TANK_PLACE] =
    {
        { "world",   "World",           "the open world",          "solo,party,raid" },
        { "normal",  "Dungeon.Normal",  "normal dungeons",         "solo,party,raid" },
        { "heroic",  "Dungeon.Heroic",  "heroic dungeons",         "solo,party,raid" },
        { "raid10",  "Raid10.Normal",   "10 player raids",         "solo,party,raid" },
        { "raid10h", "Raid10.Heroic",   "10 player heroic raids",  "solo,party,raid" },
        { "raid25",  "Raid25.Normal",   "25 player raids",         "solo,party,raid" },
        { "raid25h", "Raid25.Heroic",   "25 player heroic raids",  "solo,party,raid" },
        { "bg",      "Battleground",    "battlegrounds",           "none" },
        { "arena",   "Arena",           "arenas",                  "none" },
    };

    // "solo,party,raid", "all" or "none" -> GROUP_* bits
    uint8 ParseGroups(std::string text, char const* option)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
        std::replace(text.begin(), text.end(), ';', ',');
        std::replace(text.begin(), text.end(), ' ', ',');

        uint8 bits = 0;
        std::stringstream in(text);
        std::string word;
        while (std::getline(in, word, ','))
        {
            if (word.empty())
                continue;
            if (word == "all")
                bits |= GROUP_ALL;
            else if (word == "none" || word == "0")
                continue;
            else if (word == "solo")
                bits |= GROUP_SOLO;
            else if (word == "party" || word == "group")
                bits |= GROUP_PARTY;
            else if (word == "raid")
                bits |= GROUP_RAID;
            else
                LOG_ERROR("server.loading", "mod-temucraft-threat: PetTank.Where.{} has an unknown word '{}' (use solo, party, "
                    "raid, all or none).", option, word);
        }
        return bits;
    }
}

namespace
{
    // PetTank.Happiness.*: a word or a percent of the bar. `none` is what "leave it alone" means for the option:
    // as the core does (Tamed) or off (Minimum)
    int32 ParseHappiness(std::string text, char const* option, int32 none)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
        text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
        if (!text.empty() && text.back() == '%')
            text.pop_back();

        if (text.empty() || text == "core" || text == "off" || text == "default")
            return none;
        if (text == "empty")
            return 0;
        if (text == "unhappy")                 // the middle of unhappy (75% damage), where the core starts a tame
            return HAPPINESS_TAMED;
        if (text == "lowcontent")              // just content (100% damage)
            return HAPPINESS_CONTENT;
        if (text == "content")                 // the middle of content
            return (HAPPINESS_CONTENT + HAPPINESS_HAPPY) / 2;
        if (text == "lowhappy")                // just happy (125% damage)
            return HAPPINESS_HAPPY;
        if (text == "happy")                   // the middle of happy
            return (HAPPINESS_HAPPY + HAPPINESS_MAX) / 2;
        if (text == "full")
            return HAPPINESS_MAX;

        char* end = nullptr;
        double percent = std::strtod(text.c_str(), &end);
        if (end && *end == '\0' && end != text.c_str())
            return int32(std::clamp(percent, 0.0, 100.0) / 100.0 * HAPPINESS_MAX);

        LOG_ERROR("server.loading", "mod-temucraft-threat: PetTank.Happiness.{} = '{}' isn't understood (use core / off, "
            "empty, unhappy, lowcontent, content, lowhappy, happy, full, or a percent of the bar from 0 to 100).",
            option, text);
        return none;
    }
}

PetTankMgr* PetTankMgr::instance()
{
    static PetTankMgr mgr;
    return &mgr;
}

void PetTankMgr::LoadConfig()
{
    PetTankConfig c;
    c.enable = sConfigMgr->GetOption<bool>("PetTank.Enable", true);
    c.announce = sConfigMgr->GetOption<bool>("PetTank.Announce", true);
    c.pets = sConfigMgr->GetOption<uint8>("PetTank.Pets", PETS_TAUNTING) ? PETS_ALL : PETS_TAUNTING;
    for (uint8 i = 0; i < MAX_PET_TANK_PLACE; ++i)
    {
        std::string option = std::string("PetTank.Where.") + PLACES[i].option;
        c.where[i] = ParseGroups(sConfigMgr->GetOption<std::string>(option, PLACES[i].fallback), PLACES[i].option);
    }
    c.playerToggle = sConfigMgr->GetOption<bool>("PetTank.PlayerToggle", true);
    c.threatPct = std::min<uint32>(1000, sConfigMgr->GetOption<uint32>("PetTank.Threat.Percent", 100));
    c.regenHealthPct = std::clamp(sConfigMgr->GetOption<float>("PetTank.Regen.HealthPercent", 2.0f), 0.0f, 100.0f);
    c.regenPowerPct = std::clamp(sConfigMgr->GetOption<float>("PetTank.Regen.PowerPercent", 2.0f), 0.0f, 100.0f);
    c.regenWhileCasterInCombat = sConfigMgr->GetOption<bool>("PetTank.Regen.WhileOwnerInCombat", false);
    c.mendPetHealingPct = std::min<uint32>(1000, sConfigMgr->GetOption<uint32>("PetTank.MendPet.HealingPercent", 50));
    c.mendPetCostPct = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PetTank.MendPet.CostPercent", 50));
    c.funnelHealingPct = std::min<uint32>(1000,
        sConfigMgr->GetOption<uint32>("PetTank.HealthFunnel.HealingPercent", 50));
    c.funnelCostPct = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PetTank.HealthFunnel.CostPercent", 50));
    c.happinessTamed = ParseHappiness(sConfigMgr->GetOption<std::string>("PetTank.Happiness.Tamed", "full"),
        "Tamed", -1);
    c.happinessMinimum = ParseHappiness(sConfigMgr->GetOption<std::string>("PetTank.Happiness.Minimum", "off"),
        "Minimum", 0);
    if (c.happinessMinimum < 0)
        c.happinessMinimum = 0;
    {
        std::string mode = sConfigMgr->GetOption<std::string>("PetTank.Happiness.Mode", "enforce");
        std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) { return std::tolower(ch); });
        c.happinessMode = (mode == "player" || mode == "2") ? HAPPINESS_PLAYER : HAPPINESS_ENFORCE;
    }
    c.happinessAutoDefault = sConfigMgr->GetOption<bool>("PetTank.Happiness.AutoDefault", true);
    _config = c;
}

PetTankPlace PetTankMgr::PlaceOf(Player const* player)
{
    Map const* map = player->GetMap();
    if (!map || !map->Instanceable())
        return PLACE_WORLD;
    if (map->IsBattleArena())
        return PLACE_ARENA;
    if (map->IsBattleground())
        return PLACE_BATTLEGROUND;
    if (map->IsRaid())
    {
        bool heroic = map->IsHeroic();
        if (map->Is25ManRaid())
            return heroic ? PLACE_RAID_25_HEROIC : PLACE_RAID_25;
        return heroic ? PLACE_RAID_10_HEROIC : PLACE_RAID_10;
    }
    return map->IsHeroic() ? PLACE_DUNGEON_HEROIC : PLACE_DUNGEON_NORMAL;
}

PetTankGroup PetTankMgr::GroupOf(Player const* player)
{
    Group const* group = const_cast<Player*>(player)->GetGroup();
    if (!group)
        return GROUP_SOLO;
    return group->isRaidGroup() ? GROUP_RAID : GROUP_PARTY;
}

char const* PetTankMgr::PlaceName(uint8 place)
{
    return place < MAX_PET_TANK_PLACE ? PLACES[place].name : "?";
}

char const* PetTankMgr::PlaceKey(uint8 place)
{
    return place < MAX_PET_TANK_PLACE ? PLACES[place].key : "?";
}

int8 PetTankMgr::PlaceFromKey(std::string const& key)
{
    for (uint8 i = 0; i < MAX_PET_TANK_PLACE; ++i)
        if (key == PLACES[i].key)
            return int8(i);
    return -1;
}

char const* PetTankMgr::GroupName(uint8 group)
{
    switch (group)
    {
        case GROUP_SOLO:  return "solo";
        case GROUP_PARTY: return "in a party";
        case GROUP_RAID:  return "in a raid group";
        default:          return "?";
    }
}

bool PetTankMgr::ServerAllows(uint8 place, uint8 group) const
{
    return _config.enable && place < MAX_PET_TANK_PLACE && (_config.where[place] & group);
}

bool PetTankMgr::PlayerWants(Player const* player, uint8 place) const
{
    if (!_config.playerToggle)
        return true;
    return !(DisabledMask(player) & (1u << place));
}

bool PetTankMgr::Active(Player const* player) const
{
    if (!player || !_config.enable)
        return false;
    uint8 place = PlaceOf(player);
    return ServerAllows(place, GroupOf(player)) && PlayerWants(player, place);
}

bool PetTankMgr::KnowsTaunt(Unit const* unit)
{
    for (uint32 first : TAUNT_SPELLS)
    {
        SpellInfo const* rank = sSpellMgr->GetSpellInfo(first);
        for (; rank; rank = rank->GetNextRankSpell())
        {
            if (unit->IsPet())
            {
                Pet const* pet = static_cast<Pet const*>(unit);
                if (pet->HasSpell(rank->Id))
                    return true;
            }
            else if (unit->IsCreature())
            {
                Creature const* creature = static_cast<Creature const*>(unit);
                for (uint32 spell : creature->m_spells)
                    if (spell == rank->Id)
                        return true;
            }
        }
    }
    return false;
}

Player* PetTankMgr::TankOwner(Unit const* unit) const
{
    if (!unit || unit->IsPlayer() || unit->IsTotem() || !unit->IsGuardian())
        return nullptr;

    // The owner, or the owner of the totem that summoned it (the Greater Earth Elemental belongs to its totem)
    Unit* owner = unit->GetCharmerOrOwner();
    if (owner && owner->IsTotem())
        owner = owner->GetCharmerOrOwner();
    Player* player = owner ? owner->ToPlayer() : nullptr;
    if (!player || player == unit)
        return nullptr;

    if (_config.pets == PETS_TAUNTING && !KnowsTaunt(unit))
        return nullptr;
    return player;
}

std::vector<Unit*> PetTankMgr::TankPets(Player* player) const
{
    std::vector<Unit*> pets;
    auto consider = [&](Unit* unit)
    {
        if (unit && unit->IsInWorld() && std::find(pets.begin(), pets.end(), unit) == pets.end() &&
            TankOwner(unit) == player)
            pets.push_back(unit);
    };

    for (Unit* unit : player->m_Controlled)
        consider(unit);
    for (uint8 slot = SUMMON_SLOT_TOTEM_FIRE; slot < SUMMON_SLOT_TOTEM_FIRE + MAX_TOTEM_SLOT; ++slot)
        if (Creature* totem = player->GetMap()->GetCreature(player->m_SummonSlot[slot]))
            for (Unit* unit : totem->m_Controlled)
                consider(unit);
    return pets;
}

void PetTankMgr::LoadPlayer(Player* player)
{
    PlayerChoice choice;
    if (QueryResult result = CharacterDatabase.Query("SELECT `disabled`, `happiness_auto` FROM `pet_tank_player` "
        "WHERE `guid` = {}", player->GetGUID().GetCounter()))
    {
        choice.disabled = (*result)[0].Get<uint32>();
        if (!(*result)[1].IsNull())
            choice.happinessAuto = (*result)[1].Get<int8>() ? 1 : 0;
    }

    std::lock_guard lock(_lock);
    _choices[player->GetGUID().GetCounter()] = choice;
}

void PetTankMgr::ForgetPlayer(Player* player)
{
    std::lock_guard lock(_lock);
    _choices.erase(player->GetGUID().GetCounter());
}

uint32 PetTankMgr::DisabledMask(Player const* player) const
{
    std::lock_guard lock(_lock);
    auto itr = _choices.find(player->GetGUID().GetCounter());
    return itr == _choices.end() ? 0 : itr->second.disabled;
}

void PetTankMgr::SavePlayer(Player* player, PlayerChoice const& choice)
{
    ObjectGuid::LowType guid = player->GetGUID().GetCounter();
    if (!choice.disabled && choice.happinessAuto < 0)
        CharacterDatabase.Execute("DELETE FROM `pet_tank_player` WHERE `guid` = {}", guid);
    else if (choice.happinessAuto < 0)
        CharacterDatabase.Execute("REPLACE INTO `pet_tank_player` (`guid`, `disabled`, `happiness_auto`) VALUES "
            "({}, {}, NULL)", guid, choice.disabled);
    else
        CharacterDatabase.Execute("REPLACE INTO `pet_tank_player` (`guid`, `disabled`, `happiness_auto`) VALUES "
            "({}, {}, {})", guid, choice.disabled, int32(choice.happinessAuto));
}

void PetTankMgr::SetDisabledMask(Player* player, uint32 mask)
{
    PlayerChoice copy;
    {
        std::lock_guard lock(_lock);
        PlayerChoice& choice = _choices[player->GetGUID().GetCounter()];
        choice.disabled = mask & ((1u << MAX_PET_TANK_PLACE) - 1);
        copy = choice;
    }
    SavePlayer(player, copy);
}

int8 PetTankMgr::HappinessAutoChoice(Player const* player) const
{
    std::lock_guard lock(_lock);
    auto itr = _choices.find(player->GetGUID().GetCounter());
    return itr == _choices.end() ? -1 : itr->second.happinessAuto;
}

bool PetTankMgr::HappinessAuto(Player const* player) const
{
    if (_config.happinessMinimum <= 0)
        return false;
    if (_config.happinessMode == HAPPINESS_ENFORCE)
        return true;
    int8 choice = HappinessAutoChoice(player);
    return choice < 0 ? _config.happinessAutoDefault : choice == 1;
}

void PetTankMgr::SetHappinessAuto(Player* player, bool on)
{
    PlayerChoice copy;
    {
        std::lock_guard lock(_lock);
        PlayerChoice& choice = _choices[player->GetGUID().GetCounter()];
        choice.happinessAuto = on ? 1 : 0;
        copy = choice;
    }
    SavePlayer(player, copy);
}

int8 PetTankMgr::RaiseHappiness(Player* player) const
{
    if (!_config.enable || _config.happinessMinimum <= 0)
        return -1;
    Pet* pet = player->GetPet();
    if (!pet || !pet->IsInWorld() || pet->getPetType() != HUNTER_PET || !pet->IsAlive())
        return -1;
    uint32 floor = std::min<uint32>(uint32(_config.happinessMinimum), pet->GetMaxPower(POWER_HAPPINESS));
    if (pet->GetPower(POWER_HAPPINESS) >= floor)
        return 0;
    pet->SetPower(POWER_HAPPINESS, floor);
    return 1;
}
