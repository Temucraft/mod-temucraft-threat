# mod-temucraft-threat

Threat for players and pets, in one module (AzerothCore, WotLK 3.3.5a). All C++.

## Players

| Part | What it does | Settings |
|---|---|---|
| **Tank bonus** | Tank specs make +50% threat in their tanking stance, form or presence (Defensive Stance, Righteous Fury, Bear / Dire Bear Form, Frost Presence), on top of the class's own threat. Per-class percents. | `Temucraft.Threat.Tank.*` |
| **Tank mode** | `.temucraft tankmode [on/off]` for Demonology warlocks, Enhancement shamans, Survival hunters and Combat rogues: the tank classes' passive threat (+43%), the tank bonus, Taunt, 10% less damage taken, and extra threat from their signature abilities. Saved per character. | `Temucraft.Threat.TankMode.*` |
| **Healer threat** | Discipline/holy priests, holy paladins, restoration shamans and druids make 30% less threat. | `Temucraft.Threat.Healer.*` |

`.temucraft threat` tells a player what applies to them right now. Specs are read from the talent tree with the most points.
Stances and forms are checked every second; talents, spec swaps and resurrection apply at once.

The bonus and the reduction are hidden passive threat auras (25063 "Increase Threat" and 33790) whose amount the
module sets; tank mode adds 57339 (Tank Class Passive Threat).

Characters table: `mod_temucraft_character_setting` (setting `tankmode`) (tank mode choice). Texts: `module_string` rows of `mod-temucraft-threat`.

## Group tank roles

| Group | What happens |
|---|---|
| **Dungeon Finder** | Took the tank role without a tank spec: **Not Playing Your Spec** (debuff): 20% less threat instead of the +50%, 10% less health, armor and damage, 20% less maximum mana, rage and energy. A tank spec queued as damage or healer gets no tank bonus. |
| **Walk-in, enough places** | The spec decides, nothing to do. Places: 1 tank in a party, 2 in a raid of up to 10, 3 in a bigger raid. |
| **Walk-in, more tanks than places** | The tanks are asked who takes the place ("I'll tank" / "Not me"). If more claim than fit, the whole group votes (ties: the leader's vote, then who claimed first). The chosen tank keeps the +50%; the others play with normal threat (fine for off-tanking, or switch spec). Nobody claims: everyone keeps the bonus. |
| **No tank** | Out of combat, when the party is full or inside a dungeon or raid, everyone is asked: stand in yourself, pick someone (they must accept), or no thanks. **Stand-in Tank** (buff): +50% threat, Taunt, 1% more mana/energy/runic power regeneration, but 10% less health, armor and damage. A real tank joining ends it. |

The Temucraft interface (client 1.3.0, [`TemucraftTankRoles.lua`](https://github.com/Temucraft/mod-temucraft-client-version/blob/master/client/Interface/FrameXML/Temucraft/TemucraftTankRoles.lua), addon prefix `TTR`) shows each question with what
the choices mean and a countdown. In chat: `.temucraft tankrole` (status), `.temucraft tankrole claim | pass | vote <name> | volunteer |
pick <name> | accept | decline | stepdown | ask` (ask starts the question again).

Spells (spell_dbc here, Spell.dbc in the client's patch-T): 96390 Not Playing Your Spec and 96391 its hidden power
part, 96392 Stand-in Tank and 96393 its hidden power part (`tools/build_threat_spells.py [--client]`). Threat comes
from the module's own threat aura, so it follows the settings. Texts: `tools/build_texts.py` makes the module_string
SQL from the English texts in the code.

## Pets (pet tank)

The pet tank part was the separate mod-pet-tank module; its code, settings (`PetTank.*`), SQL file names, tables,
`.temucraft pettank` command and the **PetTank** client addon (`addon/PetTank`) are unchanged, so existing tables and choices
keep working.


| Bonus | What it does | Setting |
|---|---|---|
| **Threat** | the pet makes more threat, like a tank (+100% by default) | `PetTank.Threat.Percent` |
| **Regeneration** | out of combat it gets 2% of its max health and power back every second | `PetTank.Regen.*` |
| **Mend Pet** (hunter) | heals it 50% more, for half the mana | `PetTank.MendPet.*` |
| **Health Funnel** (warlock) | heals it 50% more, for half the health | `PetTank.HealthFunnel.*` |
| **Starting pet** (hunters) | new hunters start with their race's pet, optionally with the taming quests done (off by default) | `PetTank.StartingPet*` |
| **Happiness** (hunter pets) | a newly tamed pet starts happy instead of unhappy; optionally a minimum, enforced or each player's choice | `PetTank.Happiness.*` |

Happiness is still a Wrath mechanic: an unhappy pet does 75% damage, a content one 100% and a happy one 125%. The
core starts every newly tamed pet unhappy. `PetTank.Happiness.Tamed` (default `full`) sets where a tame starts and
`PetTank.Happiness.Minimum` (default `off`) how low a hunter pet may ever drop, both anywhere from the worst to the
best: `empty`, `unhappy`, `lowcontent`, `content`, `lowhappy`, `happy`, `full`, or a percent of the bar (`40`). They
work for every hunter pet, everywhere (PetTank.Where and PetTank.Pets don't apply to them).
`PetTank.Happiness.Mode` decides how the minimum applies: `enforce` (default) raises a pet back up by itself;
`player` lets each player choose, by itself or only when they ask (`.temucraft pettank happy`, or the addon's "Make it
happy" button; `.temucraft pettank happy auto on|off` or the addon's tick box switches).

The happiness settings, worst to best: `empty` (0%), `unhappy` (16%, where the core starts a tame), `lowcontent`
(32%), `content` (48%), `lowhappy` (63%), `happy` (82%), `full` (100%), or any percent of the bar.

Pets that count, with `PetTank.Pets = 0` (the default): hunter pets with Growl, the Voidwalker (Torment,
Suffering), the Felguard (Anguish) and the shaman's Greater Earth Elemental (Angered Earth). `PetTank.Pets = 1`:
every pet and guardian a player controls (not totems).

It comes with a client addon (3.3.5), **PetTank**, to choose where the bonuses are on for you.

### Where it works

Each place has its own setting, and each one lists who it's for: players on their own (`solo`), in a party
(`party`) or in a raid group (`raid`), or `all`, or `none`:

```
PetTank.Where.World = "solo,party,raid"
PetTank.Where.Dungeon.Normal = "solo,party,raid"
PetTank.Where.Dungeon.Heroic = "solo,party,raid"
PetTank.Where.Raid10.Normal = "solo,party,raid"
PetTank.Where.Raid10.Heroic = "solo,party,raid"
PetTank.Where.Raid25.Normal = "solo,party,raid"
PetTank.Where.Raid25.Heroic = "solo,party,raid"
PetTank.Where.Battleground = "none"
PetTank.Where.Arena = "none"
```

So `PetTank.Where.Dungeon.Heroic = "solo"` gives the bonuses in heroic dungeons only to players on their own, and
`PetTank.Where.Raid25.Heroic = "none"` never in 25 player heroic raids. Raids from before Wrath have one size and
count as their size's normal difficulty.

On top of that, players can turn the bonuses off for themselves in any place (`PetTank.PlayerToggle`, on by
default): with the PetTank addon, or `.temucraft pettank off <place>`. They can never turn them on where the server doesn't
allow them.

### Starting pet

With `PetTank.StartingPet = 1`, a new hunter starts with a pet on their first login; with `2`, existing hunters with
no pet at all get one on their next login too (once each). They also learn Call Pet, Dismiss Pet, Revive Pet and
Feed Pet, so the pet can be called back. With `PetTank.StartingPet.Quests = 1` (the default) their taming quests are
done as well, and they learn Tame Beast and Beast Training as if they had turned them in (no experience or items).

Which pet, and which quests, comes from the world table `pet_tank_starting_pet`, one row per race and class like
`playercreateinfo_skills`:

| raceMask | classMask | creature | quest | comment |
|---|---|---|---|---|
| 2 | 4 | 3126 | 6081 | Orc Hunter - Armored Scorpid |
| 4 | 4 | 1196 | 6086 | Dwarf Hunter - Ice Claw Bear |
| 8 | 4 | 1996 | 6103 | Night Elf Hunter - Strigid Screecher |
| 32 | 4 | 2970 | 6089 | Tauren Hunter - Swoop |
| 128 | 4 | 3126 | 6081 | Troll Hunter - Armored Scorpid |
| 512 | 4 | 16353 | 9673 | Blood Elf Hunter - Mistbat |
| 1024 | 4 | 17203 | 9675 | Draenei Hunter - Nightstalker |

- `creature`: any tameable creature (the defaults are each race's last "Taming the Beast" creature).
- `quest`: the last quest of the series ("Training the Beast", "Beast Training"), or 0 for none. Every quest before it
  (its `PrevQuestID` chain) and the quests that lead into it ("The Hunter's Path") are marked done too.
- `raceMask` / `classMask`: 0 means every race / class. When several rows fit a character, the one with the fewest
  races and classes wins. Any class can have rows (with your own spells for them, say). The pet is a hunter pet, so
  a character gets it when the core lets them use hunter pets (`IsClass(CLASS_HUNTER, CLASS_CONTEXT_PET)`: hunters,
  or any class a script lets use them); otherwise the server log says so and they get none.

Change the table and run `.temucraft pettank reload` (no restart needed). The characters table `pet_tank_starting_pet_given`
remembers who has had their chance.

### Install

1. Put this folder in your AzerothCore `modules/` folder and rebuild the server.
2. Copy `conf/mod-temucraft-threat.conf.dist` to your server's `configs/modules/` as `mod-temucraft-threat.conf` and change what you
   like (the `.dist` alone works too, with the defaults).
3. The tables `mod_temucraft_character_setting` (setting `tankmode`), `pet_tank_player` and `pet_tank_starting_pet_given` (characters database) and `pet_tank_starting_pet`
   (world database) are made by the DB updater on the next start (`data/sql/db-characters`, `data/sql/db-world`).
   Coming from mod-pet-tank: remove that folder from `modules/` and its `mod-pet-tank.conf`, and carry your
   `PetTank.*` values over to `mod-temucraft-threat.conf`.
4. Players copy `addon/PetTank` to their client's `Interface/AddOns`.

### Commands

| Command | Who | What |
|---|---|---|
| `.temucraft pettank` | everyone | where the bonuses are on, and whether they are on right here |
| `.temucraft pettank off <place>` / `.temucraft pettank on <place>` | everyone | turn them off / back on for you; places: `world`, `normal`, `heroic`, `raid10`, `raid10h`, `raid25`, `raid25h`, `bg`, `arena`, `here` or `all` |
| `.temucraft pettank happy` | everyone | raise your hunter pet to the happiness minimum now (`Mode = player`) |
| `.temucraft pettank happy auto on\|off` | everyone | raise it by itself or only when you ask (`Mode = player`) |
| `.temucraft pettank reload` | admin | read the settings and `pet_tank_starting_pet` again (after `.reload config`) |

### The PetTank addon

`/pettank` (or the minimap button) opens a window with every place: a tick box for each (on for you or not), what
the server allows there (solo, party, raid or off), and a highlight on where you are now with whether the bonuses are
on. Below, your hunter pet's happiness and the server's minimum, with a "Raise it by itself" tick box and a "Make it
happy" button when the server lets players choose. It talks to the module with addon whispers (prefix `PETTANK`, see `src/PetTankCommands.cpp`).

It only shows on servers that run the module: until the server answers, the window and the minimap button stay
hidden. If there's no answer, it asks again a few times (a server just after a restart can be slow), then stays
hidden but keeps listening, so a late answer still brings it back; `/pettank` asks once more.

### How it works

- **Threat**: a pet that counts carries "Increase Threat" (spell 25063, a hidden passive aura that changes threat on
  every school) set to `PetTank.Threat.Percent`, checked every second: it comes when the bonuses are on for its owner
  and goes when they aren't (another place, a group change, the owner's own choice).
- **Regeneration**: once a second, a pet out of combat (and its owner too, unless
  `PetTank.Regen.WhileOwnerInCombat = 1`) gets the percent of its max health and power back.
- **Happiness**: the bar is 1050000 points (unhappy below 333000, happy from 666000). The core gives a newly tamed
  pet exactly 166500; a hunter pet that appears with that value is set to `PetTank.Happiness.Tamed`.
  `PetTank.Happiness.Minimum` raises a hunter pet back to its value whenever it drops below, checked every update.
- **Mend Pet / Health Funnel**: their heals on a pet that counts are raised in the heal hook. What they cost is paid
  as usual, and the part above `PetTank.*.CostPercent` comes back right after: Mend Pet's mana after the cast,
  Health Funnel's health after the cast and with every tick of the channel.

### Drop / remove / uninstall

1. Take the module out of the build (move it out of `modules/`, or add it to `-DDISABLED_AC_MODULES`) and rebuild.
2. Run `data/sql/drop_remove_uninstall/characters.sql` on the characters database and
   `data/sql/drop_remove_uninstall/world.sql` on the world database.

Nothing else is stored: the threat auras aren't saved, and every other change happens in memory.

### Made with

Made with the help of [Claude](https://claude.ai), Anthropic's AI assistant: written and reviewed together with it,
then tested and published by [Ryan Turner (TheSCREWEDSoftware)](https://github.com/TheSCREWEDSoftware).
