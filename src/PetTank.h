/*
 * mod-temucraft-threat: shared state of the module (settings, where each player is, their own on/off choices).
 */

#ifndef MOD_PET_TANK_H
#define MOD_PET_TANK_H

#include "Define.h"
#include "ObjectGuid.h"

#include <array>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Player;
class Unit;

// Where a player is: each has its own setting (PetTank.Where.*) and its own on/off in the addon
enum PetTankPlace : uint8
{
    PLACE_WORLD = 0,
    PLACE_DUNGEON_NORMAL,
    PLACE_DUNGEON_HEROIC,
    PLACE_RAID_10,
    PLACE_RAID_10_HEROIC,
    PLACE_RAID_25,
    PLACE_RAID_25_HEROIC,
    PLACE_BATTLEGROUND,
    PLACE_ARENA,
    MAX_PET_TANK_PLACE
};

// Solo, in a party, or in a raid group: each place allows any of them (PetTank.Where.* lists them)
enum PetTankGroup : uint8
{
    GROUP_SOLO  = 0x1,
    GROUP_PARTY = 0x2,
    GROUP_RAID  = 0x4,
    GROUP_ALL   = 0x7
};

// How PetTank.Happiness.Minimum applies
enum PetTankHappinessMode : uint8
{
    HAPPINESS_ENFORCE = 1, // always, by itself
    HAPPINESS_PLAYER  = 2  // each player chooses: by itself, or only when they ask (.temucraft pettank happy, the addon)
};

// Which pets get the bonuses (PetTank.Pets)
enum PetTankPets : uint8
{
    PETS_TAUNTING = 0, // pets that know a taunt: Growl, Torment, Suffering, Anguish, the Greater Earth Elemental
    PETS_ALL      = 1  // every pet and guardian a player controls
};

constexpr uint32 PET_TANK_ADDON_VERSION = 1; // must match ADDON_VERSION in addon/PetTank/PetTank.lua
constexpr char const* PET_TANK_ADDON_PREFIX = "PETTANK";

struct PetTankConfig
{
    bool enable = true;
    bool announce = true;
    uint8 pets = PETS_TAUNTING;
    std::array<uint8, MAX_PET_TANK_PLACE> where{}; // GROUP_* bits allowed in each place
    bool playerToggle = true;                      // players may turn places off for themself
    uint32 threatPct = 100;                        // extra threat the pet makes, in percent
    float regenHealthPct = 2.0f;                   // out of combat: percent of max health back each second
    float regenPowerPct = 2.0f;                    // out of combat: percent of max mana / focus / energy each second
    bool regenWhileCasterInCombat = false;         // keep the regen going when only the owner is in combat
    uint32 mendPetHealingPct = 50;                 // Mend Pet heals this much more, in percent
    uint32 mendPetCostPct = 50;                    // and costs this much of its usual mana
    uint32 funnelHealingPct = 50;                  // Health Funnel heals this much more
    uint32 funnelCostPct = 50;                     // and costs this much of its usual health
    // Hunter pet happiness, in the core's points (0 to 1050000; below 333000 unhappy, from 666000 happy)
    int32 happinessTamed = -1;                     // a newly tamed pet starts with this; -1 as the core (166500)
    int32 happinessMinimum = 0;                    // hunter pets never drop below this; 0 off
    uint8 happinessMode = HAPPINESS_ENFORCE;       // how the minimum applies (PetTank.Happiness.Mode)
    bool happinessAutoDefault = true;              // HAPPINESS_PLAYER: automatic until the player says otherwise
};

// Happiness points (Pet.h): the bar is 1050000 long, unhappy below 333000, content below 666000, happy from there
constexpr int32 HAPPINESS_MAX = 1050000;
constexpr int32 HAPPINESS_CONTENT = 333000;
constexpr int32 HAPPINESS_HAPPY = 666000;
constexpr int32 HAPPINESS_TAMED = 166500;          // what the core gives a newly tamed pet

class PetTankMgr
{
public:
    static PetTankMgr* instance();

    void LoadConfig();
    PetTankConfig const& Config() const { return _config; }

    // Where this player is now, and whether they are solo / in a party / in a raid group
    static PetTankPlace PlaceOf(Player const* player);
    static PetTankGroup GroupOf(Player const* player);
    static char const* PlaceName(uint8 place);
    static char const* GroupName(uint8 group);
    // The short name used by .temucraft pettank and the addon: world, normal, heroic, raid10, raid10h, raid25, raid25h, bg, arena
    static char const* PlaceKey(uint8 place);
    static int8 PlaceFromKey(std::string const& key);

    // Whether the server allows the bonuses here (for this group state), and whether this player has them on
    bool ServerAllows(uint8 place, uint8 group) const;
    bool PlayerWants(Player const* player, uint8 place) const;
    // Both, for where the player is right now
    bool Active(Player const* player) const;

    // Whether this unit is a pet of a player that gets the bonuses (PetTank.Pets), and that player
    Player* TankOwner(Unit const* unit) const;
    static bool KnowsTaunt(Unit const* unit);
    // Every pet and guardian of this player that gets the bonuses (its own, and those its totems control, like the
    // Greater Earth Elemental)
    std::vector<Unit*> TankPets(Player* player) const;

    // The player's own choice, a mask of places turned off (pet_tank_player)
    void LoadPlayer(Player* player);
    void ForgetPlayer(Player* player);
    uint32 DisabledMask(Player const* player) const;
    void SetDisabledMask(Player* player, uint32 mask);

    // The happiness minimum: whether it applies by itself for this player, their own choice (-1 none yet), and
    // raising their hunter pet to it now. RaiseHappiness: 1 raised, 0 already there, -1 no hunter pet / no minimum
    bool HappinessAuto(Player const* player) const;
    int8 HappinessAutoChoice(Player const* player) const;
    void SetHappinessAuto(Player* player, bool on);
    int8 RaiseHappiness(Player* player) const;

private:
    struct PlayerChoice
    {
        uint32 disabled = 0;
        int8 happinessAuto = -1;
    };

    void SavePlayer(Player* player, PlayerChoice const& choice);

    PetTankConfig _config;
    mutable std::mutex _lock;
    std::unordered_map<ObjectGuid::LowType, PlayerChoice> _choices;
};

#define sPetTank PetTankMgr::instance()

// PetTankCommands.cpp: the addon's whisper protocol; true when the message was the addon's
bool HandlePetTankAddonMessage(Player* player, std::string const& msg);
void SendPetTankState(Player* player);

// PetTankStartingPet.cpp: reads PetTank.StartingPet* and the world table pet_tank_starting_pet (at startup and on
// .temucraft pettank reload)
void LoadPetTankStartingPets();

#endif
