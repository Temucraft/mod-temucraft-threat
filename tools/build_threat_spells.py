#!/usr/bin/env python3
"""Builds mod-temucraft-threat's spell rows (group tank roles) from the server's Spell.dbc:
- 96390 Not Playing Your Spec (visible): 10% less health, armor and damage
- 96391 its hidden part: 20% less maximum mana, rage and energy
- 96392 Stand-in Tank (visible): 10% less health, armor and damage
- 96393 its hidden part: 1% more mana, energy and runic power regeneration
(The threat changes of both come from the module's hidden threat aura, so they follow the settings.)
Writes data/sql/db-world/base/mod_temucraft_threat_spell_dbc.sql (spell_dbc, server). With --client the rows also go
into the client's Spell.dbc (patch-T.MPQ, game closed)."""
import os, sys
sys.path.insert(0, os.path.expanduser('~/tc'))
import dbctool as T

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(HERE)
OFF_SPEC, OFF_SPEC_POWER, STAND_IN, STAND_IN_POWER = 96390, 96391, 96392, 96393

A_MOD_DAMAGE_PERCENT_DONE, A_MOD_RESISTANCE_PCT, A_MOD_POWER_REGEN_PERCENT = 79, 101, 110
A_MOD_INCREASE_ENERGY_PERCENT, A_MOD_INCREASE_HEALTH_PERCENT = 132, 133
ATTR_PASSIVE, ATTR_HIDDEN, ATTR_NOT_SHAPESHIFT = 0x40, 0x80, 0x10000   # their penalties make them debuffs: not cancellable
ATTR3_DEATH_PERSISTENT = 0x100000

# Off-role / stand-in texts
TEXTS = {
    OFF_SPEC: ('Not Playing Your Spec',
               'You took the tank role without a tank spec.',
               'Took the tank role without a tank spec: 20% less threat, 10% less health, armor and damage, '
               '20% less maximum mana, rage and energy.'),
    STAND_IN: ('Stand-in Tank',
               'Standing in as the group\'s tank.',
               'Standing in as the group\'s tank: 50% more threat and 1% more mana, energy and runic power '
               'regeneration, but 10% less health, armor and damage.'),
}


def build(dbc):
    C = {name: i for i, (name, kind) in enumerate(T.table_columns('spell_dbc'))}
    mask = dbc.rows[633]                         # any spell, for the language masks

    def base(spell_id, icon_from, visible):
        s = [0] * dbc.fc
        s[0] = spell_id
        s[C['Attributes']] = ATTR_NOT_SHAPESHIFT if visible else (ATTR_PASSIVE | ATTR_HIDDEN)
        s[C['AttributesEx3']] = ATTR3_DEATH_PERSISTENT
        s[C['CastingTimeIndex']] = 1
        s[C['DurationIndex']] = 21               # until removed
        s[C['RangeIndex']] = 1                   # self
        s[C['BaseLevel']] = s[C['SpellLevel']] = 1
        s[C['SpellIconID']] = dbc.rows[icon_from][C['SpellIconID']]
        s[C['SchoolMask']] = 1
        for k in ('Name_Lang_Mask', 'NameSubtext_Lang_Mask', 'Description_Lang_Mask', 'AuraDescription_Lang_Mask'):
            s[C[k]] = mask[C[k]]
        if spell_id in TEXTS:
            name, desc, aura = TEXTS[spell_id]
            s[C['Name_Lang_enUS']] = dbc.add(name)
            s[C['Description_Lang_enUS']] = dbc.add(desc)
            s[C['AuraDescription_Lang_enUS']] = dbc.add(aura)
        else:
            s[C['Name_Lang_enUS']] = dbc.add(TEXTS[spell_id - 1][0])
        return s

    def effect(s, n, aura, amount, misc=0):
        s[C['Effect_%d' % n]] = 6                # apply aura
        s[C['EffectAura_%d' % n]] = aura
        s[C['EffectBasePoints_%d' % n]] = (amount - 1) & 0xFFFFFFFF
        s[C['EffectDieSides_%d' % n]] = 1
        s[C['EffectMiscValue_%d' % n]] = misc
        s[C['ImplicitTargetA_%d' % n]] = 1       # caster
        s[C['EffectMultipleValue_%d' % n]] = T.f2u(1.0)
        s[C['EffectBonusMultiplier_%d' % n]] = T.f2u(1.0)
        s[C['EffectChainAmplitude_%d' % n]] = T.f2u(1.0)

    rows = []
    for spell_id, icon in ((OFF_SPEC, 7386), (STAND_IN, 71)):   # Sunder Armor / Defensive Stance icons
        s = base(spell_id, icon, True)
        effect(s, 1, A_MOD_INCREASE_HEALTH_PERCENT, -10)
        effect(s, 2, A_MOD_RESISTANCE_PCT, -10, 1)               # armor
        effect(s, 3, A_MOD_DAMAGE_PERCENT_DONE, -10, 127)        # all schools
        rows.append(s)
    s = base(OFF_SPEC_POWER, 7386, False)
    for n, power in enumerate((0, 1, 3), 1):                     # mana, rage, energy
        effect(s, n, A_MOD_INCREASE_ENERGY_PERCENT, -20, power)
    rows.append(s)
    s = base(STAND_IN_POWER, 71, False)
    for n, power in enumerate((0, 3, 6), 1):                     # mana, energy, runic power
        effect(s, n, A_MOD_POWER_REGEN_PERCENT, 1, power)
    rows.append(s)
    for s in rows:
        dbc.put(s)
    return rows


base_dbc = T.DBC(T.SERVER_DBC + '/Spell.dbc')
for i in (OFF_SPEC, OFF_SPEC_POWER, STAND_IN, STAND_IN_POWER):
    assert i not in base_dbc.rows, i
rows = build(base_dbc)
out = os.path.join(MOD, 'data', 'sql', 'db-world', 'base', 'mod_temucraft_threat_spell_dbc.sql')
open(out, 'w').write(
    '-- mod-temucraft-threat: Not Playing Your Spec and Stand-in Tank (the client has the same rows in Spell.dbc).\n' +
    '-- Made by tools/build_threat_spells.py.\n' +
    'DELETE FROM `spell_dbc` WHERE `ID` BETWEEN %d AND %d;\n' % (OFF_SPEC, STAND_IN_POWER) +
    T.sql_insert('spell_dbc', rows, base_dbc) + '\n')
print('wrote', out)

if '--client' in sys.argv:
    tmp = '/tmp/Spell_client.dbc'
    assert T.client_get('DBFilesClient\\Spell.dbc', tmp)
    client = T.DBC(tmp)
    build(client)
    client.save(tmp)
    T.client_put({'DBFilesClient\\Spell.dbc': tmp})
    print('client Spell.dbc updated')
