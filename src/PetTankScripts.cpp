/*
 * mod-temucraft-threat: the hooks.
 *
 * - Threat: a tank pet carries "Increase Threat" (spell 25063, a hidden passive SPELL_AURA_MOD_THREAT on every
 *   school) set to PetTank.Threat.Percent while the bonuses are on for its owner, and loses it when they are off.
 * - Regen: once a second, a tank pet out of combat gets PetTank.Regen.* percent of its max health and power back.
 * - Happiness (hunter pets, everywhere, apart from PetTank.Where): a newly tamed pet starts content or happy
 *   instead of unhappy (PetTank.Happiness.Tamed), and pets can be kept from dropping below content or happy
 *   (PetTank.Happiness.Minimum). Unhappy pets do 75% damage, content 100%, happy 125%.
 * - Mend Pet and Health Funnel heal a tank pet for PetTank.*.HealingPercent more; part of what they cost comes back
 *   (PetTank.*.CostPercent is what's left to pay): Mend Pet's mana right after the cast, Health Funnel's health
 *   right after the cast and with every tick of the channel.
 */

#include "PetTank.h"

#include "Chat.h"
#include "Log.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "World.h"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr uint32 SPELL_INCREASE_THREAT = 25063;
    constexpr uint32 SPELL_MEND_PET = 136;      // first rank
    constexpr uint32 SPELL_HEALTH_FUNNEL = 755; // first rank
    constexpr uint32 TICK_MS = 1000;


    struct PlayerTick
    {
        uint32 timer = 0;
        int32 refundMana = 0;   // Mend Pet: given back on the next update, after the cast took its cost
        int32 refundHealth = 0; // Health Funnel's starting cost, the same way
        bool wasActive = false;
        bool told = false;      // said in chat where the bonuses are on, once per place
        uint8 toldPlace = 0xFF;
    };

    std::mutex sTickLock;
    std::unordered_map<ObjectGuid::LowType, PlayerTick> sTicks;

    uint32 FirstRank(SpellInfo const* spellInfo)
    {
        SpellInfo const* first = spellInfo ? spellInfo->GetFirstRankSpell() : nullptr;
        return first ? first->Id : 0;
    }

    std::vector<Unit*> TankPets(Player* player)
    {
        return sPetTank->TankPets(player);
    }

    void SetThreatAura(Unit* pet, bool on)
    {
        int32 amount = int32(sPetTank->Config().threatPct);
        Aura* aura = pet->GetAura(SPELL_INCREASE_THREAT, pet->GetGUID());
        if (!on || amount <= 0)
        {
            if (aura)
                pet->RemoveAura(aura);
            return;
        }

        if (!aura)
            aura = pet->AddAura(SPELL_INCREASE_THREAT, pet);
        if (!aura)
            return;
        if (AuraEffect* effect = aura->GetEffect(EFFECT_0))
            if (effect->GetAmount() != amount)
                effect->ChangeAmount(amount);
    }

    void Regenerate(Player* owner, Unit* pet)
    {
        PetTankConfig const& config = sPetTank->Config();
        if (!pet->IsAlive() || pet->IsInCombat() || (owner->IsInCombat() && !config.regenWhileCasterInCombat))
            return;

        if (config.regenHealthPct > 0.0f && pet->GetHealth() < pet->GetMaxHealth())
            pet->ModifyHealth(std::max<int32>(1, int32(pet->GetMaxHealth() * config.regenHealthPct / 100.0f)));

        Powers power = pet->getPowerType();
        uint32 maxPower = pet->GetMaxPower(power);
        if (config.regenPowerPct > 0.0f && maxPower > 0 && pet->GetPower(power) < maxPower)
            pet->ModifyPower(power, std::max<int32>(1, int32(maxPower * config.regenPowerPct / 100.0f)));
    }
}

class PetTankWorldScript : public WorldScript
{
public:
    PetTankWorldScript() : WorldScript("PetTankWorldScript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sPetTank->LoadConfig();
    }

    void OnStartup() override
    {
        LoadPetTankStartingPets(); // works with PetTank.Enable = 0 too
        PetTankConfig const& c = sPetTank->Config();
        if (!c.enable)
        {
            LOG_INFO("server.loading", "mod-temucraft-threat: off (PetTank.Enable = 0).");
            return;
        }
        std::string where;
        for (uint8 i = 0; i < MAX_PET_TANK_PLACE; ++i)
            if (c.where[i])
                where += (where.empty() ? "" : ", ") + std::string(PetTankMgr::PlaceKey(i));
        LOG_INFO("server.loading", "mod-temucraft-threat: {} pets, +{}% threat, regen {}%/{}% a second, Mend Pet +{}% for "
            "{}% of its cost, Health Funnel +{}% for {}%; on in: {}.", c.pets == PETS_ALL ? "all" : "taunting",
            c.threatPct, c.regenHealthPct, c.regenPowerPct, c.mendPetHealingPct, c.mendPetCostPct,
            c.funnelHealingPct, c.funnelCostPct, where.empty() ? "nowhere" : where);
    }
};

class PetTankPlayerScript : public PlayerScript
{
public:
    PetTankPlayerScript() : PlayerScript("PetTankPlayerScript", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT,
        PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_SPELL_CAST, PLAYERHOOK_ON_DELETE, PLAYERHOOK_ON_MAP_CHANGED }) { }

    void OnPlayerLogin(Player* player) override
    {
        sPetTank->LoadPlayer(player);
        std::lock_guard lock(sTickLock);
        sTicks[player->GetGUID().GetCounter()] = PlayerTick();
    }

    void OnPlayerLogout(Player* player) override
    {
        sPetTank->ForgetPlayer(player);
        std::lock_guard lock(sTickLock);
        sTicks.erase(player->GetGUID().GetCounter());
    }

    void OnPlayerDelete(ObjectGuid guid, uint32 /*accountId*/) override
    {
        CharacterDatabase.Execute("DELETE FROM `pet_tank_player` WHERE `guid` = {}", guid.GetCounter());
    }

    void OnPlayerMapChanged(Player* player) override
    {
        std::lock_guard lock(sTickLock);
        auto itr = sTicks.find(player->GetGUID().GetCounter());
        if (itr != sTicks.end())
            itr->second.timer = TICK_MS; // look again on the next update
    }

    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (!sPetTank->Config().enable || !spell)
            return;

        SpellInfo const* info = spell->GetSpellInfo();
        uint32 first = FirstRank(info);
        if (first != SPELL_MEND_PET && first != SPELL_HEALTH_FUNNEL)
            return;

        Unit* target = spell->m_targets.GetUnitTarget();
        if (!target || sPetTank->TankOwner(target) != player || !sPetTank->Active(player))
            return;

        uint32 keep = first == SPELL_MEND_PET ? sPetTank->Config().mendPetCostPct : sPetTank->Config().funnelCostPct;
        int32 refund = spell->GetPowerCost() * int32(100 - keep) / 100;
        if (refund <= 0)
            return;

        std::lock_guard lock(sTickLock);
        PlayerTick& tick = sTicks[player->GetGUID().GetCounter()];
        if (info->PowerType == POWER_HEALTH)
            tick.refundHealth += refund;
        else if (info->PowerType == POWER_MANA)
            tick.refundMana += refund;
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!sPetTank->Config().enable || !player->IsInWorld())
            return;
        KeepHappy(player);

        int32 refundMana = 0;
        int32 refundHealth = 0;
        bool tickNow = false;
        {
            std::lock_guard lock(sTickLock);
            PlayerTick& tick = sTicks[player->GetGUID().GetCounter()];
            std::swap(refundMana, tick.refundMana);
            std::swap(refundHealth, tick.refundHealth);
            tick.timer += diff;
            if (tick.timer >= TICK_MS)
            {
                tick.timer = 0;
                tickNow = true;
            }
        }

        if (refundMana > 0 && player->IsAlive())
            player->ModifyPower(POWER_MANA, refundMana);
        if (refundHealth > 0 && player->IsAlive())
            player->ModifyHealth(refundHealth);

        if (!tickNow)
            return;

        bool active = sPetTank->Active(player);
        for (Unit* pet : TankPets(player))
        {
            SetThreatAura(pet, active);
            if (active)
                Regenerate(player, pet);
        }

        // A pet that stopped counting (PetTank.Pets = 0 and it lost its taunt) or a player whose bonuses went off
        // keeps no threat aura
        if (!active || sPetTank->Config().pets == PETS_TAUNTING)
            for (Unit* unit : player->m_Controlled)
                if (unit && unit->IsInWorld() && (!active || sPetTank->TankOwner(unit) != player))
                    if (Aura* aura = unit->GetAura(SPELL_INCREASE_THREAT, unit->GetGUID()))
                        unit->RemoveAura(aura);

        Announce(player, active);
    }

private:
    // PetTank.Happiness.Minimum: a hunter pet that dropped below it goes back up to it, always (Mode = enforce) or
    // when its owner chose that (Mode = player)
    static void KeepHappy(Player* player)
    {
        if (sPetTank->HappinessAuto(player))
            sPetTank->RaiseHappiness(player);
    }

    // In chat, once each time the bonuses turn on or off for this player (place, group or their own choice)
    static void Announce(Player* player, bool active)
    {
        if (!sPetTank->Config().announce)
            return;

        uint8 place = PetTankMgr::PlaceOf(player);
        {
            std::lock_guard lock(sTickLock);
            PlayerTick& tick = sTicks[player->GetGUID().GetCounter()];
            if (tick.told && tick.wasActive == active && tick.toldPlace == place)
                return;
            bool first = !tick.told;
            tick.told = true;
            tick.wasActive = active;
            tick.toldPlace = place;
            // Nothing to say on login with the bonuses off, or for players without a pet that counts
            if ((first && !active) || TankPets(player).empty())
                return;
        }

        ChatHandler(player->GetSession()).PSendSysMessage("|cff8FD14F[Pet Tank]|r Your pet's tank bonuses are {} in "
            "{} ({}).", active ? "|cff4CFF00on|r" : "|cffFF6060off|r", PetTankMgr::PlaceName(place),
            PetTankMgr::GroupName(PetTankMgr::GroupOf(player)));
    }
};

// PetTank.Happiness.Tamed: the core starts every newly tamed pet unhappy, halfway (166500); start it anywhere from
// empty to full instead. A pet still at exactly that value when it appears has just been tamed.
class PetTankPetScript : public PetScript
{
public:
    PetTankPetScript() : PetScript("PetTankPetScript", { PETHOOK_ON_PET_ADD_TO_WORLD }) { }

    void OnPetAddToWorld(Pet* pet) override
    {
        int32 tamed = sPetTank->Config().happinessTamed;
        if (!sPetTank->Config().enable || tamed < 0 || pet->getPetType() != HUNTER_PET ||
            pet->GetPower(POWER_HAPPINESS) != uint32(HAPPINESS_TAMED))
            return;
        pet->SetPower(POWER_HAPPINESS, std::min<uint32>(uint32(tamed), pet->GetMaxPower(POWER_HAPPINESS)));
    }
};

class PetTankUnitScript : public UnitScript
{
public:
    PetTankUnitScript() : UnitScript("PetTankUnitScript", true, { UNITHOOK_MODIFY_HEAL_RECEIVED }) { }

    // Direct heals pass (healer, target) and periodic ones (target, healer): the pet is whichever of the two counts
    void ModifyHealReceived(Unit* a, Unit* b, uint32& heal, SpellInfo const* spellInfo) override
    {
        if (!sPetTank->Config().enable || !spellInfo || !heal)
            return;

        uint32 first = FirstRank(spellInfo);
        if (first != SPELL_MEND_PET && first != SPELL_HEALTH_FUNNEL)
            return;

        Unit* pet = nullptr;
        Player* owner = nullptr;
        for (Unit* unit : { a, b })
            if (Player* player = sPetTank->TankOwner(unit))
            {
                pet = unit;
                owner = player;
                break;
            }
        if (!pet || !owner || !sPetTank->Active(owner))
            return;

        PetTankConfig const& config = sPetTank->Config();
        uint32 bonus = first == SPELL_MEND_PET ? config.mendPetHealingPct : config.funnelHealingPct;
        heal = uint32(uint64(heal) * (100 + bonus) / 100);

        // Health Funnel takes health from the warlock every second of the channel: give part of it back
        if (first == SPELL_HEALTH_FUNNEL && config.funnelCostPct < 100 && owner->IsAlive())
        {
            int32 perSecond = int32(spellInfo->ManaPerSecond + spellInfo->ManaPerSecondPerLevel * owner->GetLevel());
            int32 refund = perSecond * int32(100 - config.funnelCostPct) / 100;
            if (refund > 0)
                owner->ModifyHealth(refund);
        }
    }
};

void AddPetTankScripts()
{
    new PetTankWorldScript();
    new PetTankPlayerScript();
    new PetTankPetScript();
    new PetTankUnitScript();
}
