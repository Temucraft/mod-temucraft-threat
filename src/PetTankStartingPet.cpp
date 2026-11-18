/*
 * mod-temucraft-threat: new characters (hunters by default) start with a pet (PetTank.StartingPet), optionally with their
 * taming quests done.
 *
 * Which pet comes from the world table `pet_tank_starting_pet`, one row per race and class, like
 * `playercreateinfo_skills`: raceMask, classMask (0 = every race / class), creature (a tameable creature_template
 * entry), quest (0, or the last quest of a series to mark done) and a comment. When several rows fit a character,
 * the one with the fewest races and classes in its masks wins. Any class can have rows: the pet is a hunter pet, so
 * it's given when the core lets the character use hunter pets (IsClass(CLASS_HUNTER, CLASS_CONTEXT_PET): hunters,
 * or other classes a script lets use them).
 *
 * With PetTank.StartingPet.Quests, the row's quest and every quest before it (its PrevQuestID chain, and the quests
 * that lead into that chain, like "The Hunter's Path") are marked done, and the spells they reward are learned as if
 * they had been turned in: the hunter's series teaches Tame Beast, Call Pet and Dismiss Pet ("Taming the Beast"),
 * then Feed Pet and Revive Pet ("Training the Beast"). Call Pet, Dismiss Pet, Revive Pet and Feed Pet are taught
 * either way, so the pet can be called back after it's dismissed.
 *
 * Every character is looked at once (characters table `pet_tank_starting_pet_given`): new ones on their first
 * login, and with PetTank.StartingPet = 2 existing ones on their next login (they only get the pet when they have
 * none at all, current or stabled).
 */

#include "PetTank.h"

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <bit>
#include <set>
#include <vector>

namespace
{
    enum StartingPetMode : uint8
    {
        STARTING_PET_OFF      = 0,
        STARTING_PET_NEW      = 1, // new characters, on their first login
        STARTING_PET_EXISTING = 2, // new characters, and existing ones once, on their next login
    };

    constexpr uint32 SPELL_TAME_BEAST_EFFECT = 13481; // what the core marks a tamed pet as created by
    // What a hunter needs to keep a pet: Call Pet, Dismiss Pet, Revive Pet, Feed Pet
    constexpr uint32 PET_SPELLS[] = { 883, 2641, 982, 6991 };

    struct StartingPet
    {
        uint32 raceMask = 0;
        uint32 classMask = 0;
        uint32 creature = 0;
        uint32 quest = 0;
        std::string comment;
    };

    struct
    {
        uint8 mode = STARTING_PET_OFF;
        bool quests = true;
        std::vector<StartingPet> rows;
    } sStart;

    StartingPet const* RowFor(Player const* player)
    {
        uint32 race = player->getRaceMask();
        uint32 cls = player->getClassMask();
        StartingPet const* best = nullptr;
        int bestBits = 0;
        for (StartingPet const& row : sStart.rows)
        {
            if ((row.raceMask && !(row.raceMask & race)) || (row.classMask && !(row.classMask & cls)))
                continue;
            int bits = std::popcount(row.raceMask ? row.raceMask : ~0u) +
                std::popcount(row.classMask ? row.classMask : ~0u);
            if (!best || bits < bestBits)
            {
                best = &row;
                bestBits = bits;
            }
        }
        return best;
    }

    // The quest, every quest before it, and the quests leading into them, that this character could take
    std::vector<Quest const*> Series(Player const* player, uint32 lastQuest)
    {
        std::set<uint32> ids;
        std::vector<uint32> todo{ lastQuest };
        while (!todo.empty())
        {
            uint32 id = todo.back();
            todo.pop_back();
            Quest const* quest = sObjectMgr->GetQuestTemplate(id);
            if (!quest || !ids.insert(id).second)
                continue;
            if (int32 prev = quest->GetPrevQuestId())
                todo.push_back(uint32(std::abs(prev)));
        }
        // Quests whose "next quest" is in the series: the other starts of the chain ("The Hunter's Path")
        for (bool grew = true; grew;)
        {
            grew = false;
            for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
                if (!ids.count(id) && quest->GetNextQuestInChain() && ids.count(quest->GetNextQuestInChain()))
                    grew = ids.insert(id).second || grew;
        }

        std::vector<Quest const*> series;
        for (uint32 id : ids)
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(id))
                if (player->SatisfyQuestRace(quest, false) && player->SatisfyQuestClass(quest, false))
                    series.push_back(quest);
        return series;
    }

    // Spells a quest teaches: the spells its reward spell's "learn spell" effects name
    void LearnRewardSpells(Player* player, Quest const* quest)
    {
        int32 spellId = quest->GetRewSpellCast() > 0 ? quest->GetRewSpellCast() : int32(quest->GetRewSpell());
        SpellInfo const* spell = spellId > 0 ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!spell)
            return;
        bool taught = false;
        for (SpellEffectInfo const& effect : spell->Effects)
            if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell &&
                !player->HasSpell(effect.TriggerSpell))
            {
                player->learnSpell(effect.TriggerSpell);
                taught = true;
            }
        if (!taught && !spell->HasEffect(SPELL_EFFECT_LEARN_SPELL) && !player->HasSpell(spellId))
            player->learnSpell(spellId); // a reward spell that is itself the ability
    }

    void CompleteSeries(Player* player, uint32 lastQuest)
    {
        for (Quest const* quest : Series(player, lastQuest))
        {
            uint32 id = quest->GetQuestId();
            if (player->IsQuestRewarded(id))
                continue;
            if (player->GetQuestStatus(id) != QUEST_STATUS_NONE)
            {
                // An existing character in the middle of it: take it out of the log, with its taming rod
                player->RemoveActiveQuest(id);
                if (quest->GetSrcItemId())
                    player->DestroyItemCount(quest->GetSrcItemId(), std::max<uint32>(1, quest->GetSrcItemCount()),
                        true);
            }
            player->SetRewardedQuest(id);
            LearnRewardSpells(player, quest);
        }
    }

    bool HasAnyPet(Player* player)
    {
        if (player->GetPet() || player->IsExistPet())
            return true;
        PetStable const* stable = player->GetPetStable();
        if (!stable)
            return false;
        if (stable->CurrentPet || !stable->UnslottedPets.empty())
            return true;
        for (auto const& stabled : stable->StabledPets)
            if (stabled)
                return true;
        return false;
    }

    void MarkGiven(Player* player)
    {
        CharacterDatabase.Execute("REPLACE INTO `pet_tank_starting_pet_given` (`guid`) VALUES ({})",
            player->GetGUID().GetCounter());
    }

    // Give this character its starting pet now, if a row fits it and it has no pet yet
    void Give(Player* player)
    {
        MarkGiven(player);
        StartingPet const* row = RowFor(player);
        if (!row || HasAnyPet(player))
            return;
        // The pet is a hunter pet: the core lets a character keep and call it when it counts as a hunter for pets
        // (hunters, or any class a script or custom data lets use them)
        if (!player->IsClass(CLASS_HUNTER, CLASS_CONTEXT_PET))
        {
            LOG_ERROR("module", "mod-temucraft-threat: pet_tank_starting_pet ({}) fits {}, but this server doesn't let their "
                "class use hunter pets (IsClass(CLASS_HUNTER, CLASS_CONTEXT_PET)); no starting pet.", row->comment,
                player->GetName());
            return;
        }

        if (sStart.quests && row->quest)
            CompleteSeries(player, row->quest);
        for (uint32 spell : PET_SPELLS)
            if (!player->HasSpell(spell))
                player->learnSpell(spell);

        Pet* pet = player->CreatePet(row->creature, SPELL_TAME_BEAST_EFFECT);
        if (!pet)
        {
            LOG_ERROR("module", "mod-temucraft-threat: couldn't give {} the starting pet {} (pet_tank_starting_pet).",
                player->GetName(), row->creature);
            return;
        }
        player->SaveToDB(false, false);
        if (sPetTank->Config().announce)
            ChatHandler(player->GetSession()).PSendSysMessage("|cff8FD14F[Pet Tank]|r Your {} is with you.{}",
                pet->GetName(), sStart.quests && row->quest ? " Your taming lessons are done too." : "");
    }

    bool AlreadyGiven(Player* player)
    {
        return bool(CharacterDatabase.Query("SELECT 1 FROM `pet_tank_starting_pet_given` WHERE `guid` = {}",
            player->GetGUID().GetCounter()));
    }
}

void LoadPetTankStartingPets()
{
    sStart.mode = std::min<uint8>(sConfigMgr->GetOption<uint8>("PetTank.StartingPet", STARTING_PET_OFF),
        STARTING_PET_EXISTING);
    sStart.quests = sConfigMgr->GetOption<bool>("PetTank.StartingPet.Quests", true);

    std::vector<StartingPet> rows;
    if (QueryResult result = WorldDatabase.Query("SELECT `raceMask`, `classMask`, `creature`, `quest`, `comment` "
        "FROM `pet_tank_starting_pet`"))
    {
        do
        {
            Field* fields = result->Fetch();
            StartingPet row;
            row.raceMask = fields[0].Get<uint32>();
            row.classMask = fields[1].Get<uint32>();
            row.creature = fields[2].Get<uint32>();
            row.quest = fields[3].Get<uint32>();
            row.comment = fields[4].Get<std::string>();

            CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(row.creature);
            if (!creature || !creature->family || !creature->IsTameable(true))
            {
                LOG_ERROR("sql.sql", "mod-temucraft-threat: pet_tank_starting_pet ({}): creature {} doesn't exist or can't be "
                    "tamed, skipped.", row.comment, row.creature);
                continue;
            }
            if (row.quest && !sObjectMgr->GetQuestTemplate(row.quest))
            {
                LOG_ERROR("sql.sql", "mod-temucraft-threat: pet_tank_starting_pet ({}): quest {} doesn't exist, no quests for "
                    "this row.", row.comment, row.quest);
                row.quest = 0;
            }
            rows.push_back(row);
        } while (result->NextRow());
    }
    sStart.rows = std::move(rows);

    static char const* const modes[] = { "off", "new characters", "new and existing characters" };
    LOG_INFO("server.loading", "mod-temucraft-threat: starting pets {} ({} rows{}).", modes[sStart.mode], sStart.rows.size(),
        sStart.quests ? ", taming quests done" : "");
}

class PetTankStartingPetPlayerScript : public PlayerScript
{
public:
    PetTankStartingPetPlayerScript() : PlayerScript("PetTankStartingPetPlayerScript",
        { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_FIRST_LOGIN, PLAYERHOOK_ON_DELETE }) { }

    void OnPlayerFirstLogin(Player* player) override
    {
        if (sStart.mode != STARTING_PET_OFF)
            Give(player);
    }

    // Existing characters (not on their first login: that's above), once
    void OnPlayerLogin(Player* player) override
    {
        if (sStart.mode == STARTING_PET_EXISTING && !player->HasAtLoginFlag(AT_LOGIN_FIRST) && !AlreadyGiven(player))
            Give(player);
    }

    void OnPlayerDelete(ObjectGuid guid, uint32 /*accountId*/) override
    {
        CharacterDatabase.Execute("DELETE FROM `pet_tank_starting_pet_given` WHERE `guid` = {}", guid.GetCounter());
    }
};

void AddPetTankStartingPetScripts()
{
    new PetTankStartingPetPlayerScript();
}
