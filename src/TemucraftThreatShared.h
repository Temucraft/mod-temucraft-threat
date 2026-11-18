/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

// mod-temucraft-threat: what the player threat part (TemucraftThreat.cpp) and the group tank roles
// (TemucraftTankRoles.cpp) ask each other.

#ifndef TEMUCRAFT_THREAT_SHARED_H
#define TEMUCRAFT_THREAT_SHARED_H

class Player;

// spells from tools/build_threat_spells.py
constexpr unsigned int SPELL_TEMUCRAFT_OFF_SPEC = 96390;
constexpr unsigned int SPELL_TEMUCRAFT_OFF_SPEC_POWER = 96391;
constexpr unsigned int SPELL_TEMUCRAFT_STAND_IN = 96392;
constexpr unsigned int SPELL_TEMUCRAFT_STAND_IN_POWER = 96393;

enum class TemucraftTankRole
{
    Normal,      // the spec decides (walk-in group with no more tanks than places, or no group)
    OffSpec,     // took the tank role in the Dungeon Finder without a tank spec
    NotChosen,   // a tank spec, but another player is the group's tank
    StandIn,     // no tank in the group: stands in as tank
};

// TemucraftThreat.cpp
bool TemucraftThreatIsTankSpec(Player* player);    // a tank spec, or tank mode on (stances don't matter)
void TemucraftThreatRefresh(Player* player);       // threat auras and Taunt again, now
float TemucraftThreatTankPercent(Player* player);  // the tank bonus for this class

// TemucraftTankRoles.cpp
TemucraftTankRole TemucraftTankRoleOf(Player* player);
float TemucraftTankRoleOffSpecThreat();            // threat change while OffSpec (negative)
bool TemucraftTankRoleWantsTaunt(Player* player);  // stand-ins learn Taunt
void TemucraftTankRoleAuras(Player* player);       // the role's visible auras, every second

#endif
