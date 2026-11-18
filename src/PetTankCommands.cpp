/*
 * mod-temucraft-threat: .pettank and the server side of the PetTank addon (addon/PetTank).
 *
 * The addon whispers to the player themself with the prefix PETTANK:
 *   client -> server  "HELLO\t<version>"   the state below (and the version this module expects)
 *                     "SET\t<mask>"        the places this player turns off (bits as in PetTankPlace)
 *                     "HAPPY"              raise the hunter pet to the happiness minimum now
 *                     "HAPPYAUTO\t<0|1>"   raise it by itself or only when asked (PetTank.Happiness.Mode = player)
 *   server -> client  "V\t<version>"       the addon version this module expects (on HELLO)
 *                     "S\t<module on 0/1>\t<players may choose 0/1>\t<place>\t<group bit>\t<active 0/1>\t
 *                      <off mask>\t<allowed groups per place, comma separated>\t<has a pet that counts 0/1>\t
 *                      <which pets: 0 taunting, 1 all>\t<happiness mode: 0 no minimum, 1 enforce, 2 player>\t
 *                      <automatic for this player 0/1>\t<minimum, percent>\t<pet's happiness, percent; -1 no hunter pet>"
 *                     "X\t<error>"
 * No answer at all means the server doesn't run this module: the addon then stays hidden.
 */

#include "PetTank.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldPacket.h"

#include <algorithm>
#include <cctype>

using namespace Acore::ChatCommands;

namespace
{
    void SendAddon(Player* player, std::string const& body)
    {
        std::string text = std::string(PET_TANK_ADDON_PREFIX) + "\t" + body;
        if (text.size() > 254)
            text.resize(254);
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, text);
        player->SendDirectMessage(&data);
    }

    bool HasTankPet(Player* player)
    {
        return !sPetTank->TankPets(player).empty();
    }

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
        return text;
    }

    std::string GroupsText(uint8 bits)
    {
        if (!bits)
            return "|cffFF6060off|r";
        if (bits == GROUP_ALL)
            return "solo, party and raid";
        std::string out;
        if (bits & GROUP_SOLO)
            out += "solo";
        if (bits & GROUP_PARTY)
            out += std::string(out.empty() ? "" : ", ") + "party";
        if (bits & GROUP_RAID)
            out += std::string(out.empty() ? "" : ", ") + "raid";
        return out + " only";
    }
}

void SendPetTankState(Player* player)
{
    PetTankConfig const& c = sPetTank->Config();
    std::string where;
    for (uint8 i = 0; i < MAX_PET_TANK_PLACE; ++i)
        where += (i ? "," : "") + std::to_string(c.enable ? c.where[i] : 0);

    int32 petHappiness = -1;
    if (Pet* pet = player->GetPet())
        if (pet->getPetType() == HUNTER_PET && pet->GetMaxPower(POWER_HAPPINESS))
            petHappiness = int32(pet->GetPower(POWER_HAPPINESS) * 100 / pet->GetMaxPower(POWER_HAPPINESS));

    SendAddon(player, Acore::StringFormat("S\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", c.enable ? 1 : 0,
        c.playerToggle ? 1 : 0, uint32(PetTankMgr::PlaceOf(player)), uint32(PetTankMgr::GroupOf(player)),
        sPetTank->Active(player) ? 1 : 0, sPetTank->DisabledMask(player), where, HasTankPet(player) ? 1 : 0,
        uint32(c.pets), c.happinessMinimum > 0 ? uint32(c.happinessMode) : 0, sPetTank->HappinessAuto(player) ? 1 : 0,
        c.happinessMinimum * 100 / HAPPINESS_MAX, petHappiness));
}

bool HandlePetTankAddonMessage(Player* player, std::string const& msg)
{
    std::string prefix = std::string(PET_TANK_ADDON_PREFIX) + "\t";
    if (msg.compare(0, prefix.size(), prefix) != 0)
        return false;

    std::string body = msg.substr(prefix.size());
    if (body == "HELLO" || body.compare(0, 6, "HELLO\t") == 0)
    {
        SendAddon(player, Acore::StringFormat("V\t{}", PET_TANK_ADDON_VERSION));
        SendPetTankState(player);
    }
    else if (body.compare(0, 4, "SET\t") == 0)
    {
        if (!sPetTank->Config().playerToggle)
            SendAddon(player, "X\tThis server doesn't let players turn the pet bonuses off.");
        else
            sPetTank->SetDisabledMask(player, uint32(std::strtoul(body.c_str() + 4, nullptr, 10)));
        SendPetTankState(player);
    }
    else if (body == "GET")
        SendPetTankState(player);
    else if (body == "HAPPY")
    {
        if (sPetTank->RaiseHappiness(player) < 0)
            SendAddon(player, "X\tNo hunter pet out, or no happiness minimum on this server.");
        SendPetTankState(player);
    }
    else if (body.compare(0, 10, "HAPPYAUTO\t") == 0)
    {
        if (sPetTank->Config().happinessMode != HAPPINESS_PLAYER)
            SendAddon(player, "X\tThis server decides that by itself.");
        else
            sPetTank->SetHappinessAuto(player, body.size() > 10 && body[10] == '1');
        SendPetTankState(player);
    }
    return true;
}

class PetTankCommandScript : public CommandScript
{
public:
    PetTankCommandScript() : CommandScript("PetTankCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable petTankTable =
        {
            { "",       HandleStatusCommand, SEC_PLAYER,        Console::No },
            { "on",     HandleOnCommand,     SEC_PLAYER,        Console::No },
            { "off",    HandleOffCommand,    SEC_PLAYER,        Console::No },
            { "happy",  HandleHappyCommand,  SEC_PLAYER,        Console::No },
            { "reload", HandleReloadCommand, 101301 /* SEC_ADMINISTRATOR -> RBAC */, Console::Yes },
        };
        static ChatCommandTable commandTable =
        {
            { "pettank", petTankTable },
        };
        return commandTable;
    }

    // .pettank: where the bonuses are on, and whether they are on right here
    static bool HandleStatusCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        PetTankConfig const& c = sPetTank->Config();
        if (!c.enable)
        {
            handler->SendSysMessage("|cff8FD14F[Pet Tank]|r The pet tank bonuses are off on this server.");
            return true;
        }

        uint8 here = PetTankMgr::PlaceOf(player);
        handler->PSendSysMessage("|cff8FD14F[Pet Tank]|r Here ({}, {}): {}. Pets that count: {}.",
            PetTankMgr::PlaceName(here), PetTankMgr::GroupName(PetTankMgr::GroupOf(player)),
            sPetTank->Active(player) ? "|cff4CFF00on|r" : "|cffFF6060off|r",
            c.pets == PETS_ALL ? "every pet" : "pets with a taunt");
        for (uint8 i = 0; i < MAX_PET_TANK_PLACE; ++i)
            handler->PSendSysMessage("  {} ({}): server {}{}", PetTankMgr::PlaceName(i), PetTankMgr::PlaceKey(i),
                GroupsText(c.where[i]), c.where[i] && !sPetTank->PlayerWants(player, i) ? ", |cffFF6060you turned it "
                "off|r" : "");
        if (c.playerToggle)
            handler->SendSysMessage("  .pettank off <place|all> / .pettank on <place|all> turns it off or on for you "
                "(or use the PetTank addon).");
        return true;
    }

    static bool SetFor(ChatHandler* handler, std::string const& what, bool on)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!sPetTank->Config().enable || !sPetTank->Config().playerToggle)
        {
            handler->SendSysMessage("|cff8FD14F[Pet Tank]|r This server doesn't let players change this.");
            return true;
        }

        uint32 mask = sPetTank->DisabledMask(player);
        std::string key = Lower(what);
        uint32 bits = 0;
        if (key == "all" || key.empty())
            bits = (1u << MAX_PET_TANK_PLACE) - 1;
        else if (key == "here")
            bits = 1u << PetTankMgr::PlaceOf(player);
        else
        {
            int8 place = PetTankMgr::PlaceFromKey(key);
            if (place < 0)
            {
                handler->SendSysMessage("|cff8FD14F[Pet Tank]|r Places: world, normal, heroic, raid10, raid10h, "
                    "raid25, raid25h, bg, arena, here or all.");
                return true;
            }
            bits = 1u << place;
        }

        sPetTank->SetDisabledMask(player, on ? (mask & ~bits) : (mask | bits));
        SendPetTankState(player);
        return HandleStatusCommand(handler);
    }

    static bool HandleOnCommand(ChatHandler* handler, Optional<std::string> what)
    {
        return SetFor(handler, what.value_or("all"), true);
    }

    static bool HandleOffCommand(ChatHandler* handler, Optional<std::string> what)
    {
        return SetFor(handler, what.value_or("all"), false);
    }

    // .pettank happy [auto on|off]: raise the hunter pet to the happiness minimum now, or choose whether it happens
    // by itself (PetTank.Happiness.Mode = player)
    static bool HandleHappyCommand(ChatHandler* handler, Optional<std::string> what, Optional<std::string> onOff)
    {
        Player* player = handler->GetSession()->GetPlayer();
        PetTankConfig const& c = sPetTank->Config();
        if (!c.enable || c.happinessMinimum <= 0)
        {
            handler->SendSysMessage("|cff8FD14F[Pet Tank]|r This server has no happiness minimum.");
            return true;
        }

        if (what && Lower(*what) == "auto")
        {
            if (c.happinessMode != HAPPINESS_PLAYER)
            {
                handler->SendSysMessage("|cff8FD14F[Pet Tank]|r This server keeps your pet's happiness up by itself.");
                return true;
            }
            if (onOff)
                sPetTank->SetHappinessAuto(player, Lower(*onOff) == "on" || *onOff == "1");
            handler->PSendSysMessage("|cff8FD14F[Pet Tank]|r Your hunter pet's happiness is raised to the minimum "
                "{}.", sPetTank->HappinessAuto(player) ? "by itself" : "only when you ask (.pettank happy)");
            SendPetTankState(player);
            return true;
        }

        switch (sPetTank->RaiseHappiness(player))
        {
            case 1:
                handler->SendSysMessage("|cff8FD14F[Pet Tank]|r Your pet is happier now.");
                break;
            case 0:
                handler->SendSysMessage("|cff8FD14F[Pet Tank]|r Your pet is already at least that happy.");
                break;
            default:
                handler->SendSysMessage("|cff8FD14F[Pet Tank]|r Call your hunter pet first.");
                break;
        }
        SendPetTankState(player);
        return true;
    }

    static bool HandleReloadCommand(ChatHandler* handler)
    {
        sPetTank->LoadConfig();
        LoadPetTankStartingPets();
        handler->SendSysMessage("mod-temucraft-threat: settings and pet_tank_starting_pet reloaded (run .reload config first "
            "for changes in the .conf file).");
        return true;
    }
};

class PetTankAddonScript : public PlayerScript
{
public:
    PetTankAddonScript() : PlayerScript("PetTankAddonScript", { PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

    using PlayerScript::OnPlayerCanUseChat;

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override
    {
        if (type != CHAT_MSG_WHISPER || language != LANG_ADDON || receiver != player)
            return true;
        return !HandlePetTankAddonMessage(player, msg);
    }
};

void AddPetTankCommandScripts()
{
    new PetTankCommandScript();
    new PetTankAddonScript();
}
