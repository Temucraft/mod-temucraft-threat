-- mod-temucraft-threat (pet tank): the pet a new character starts with (PetTank.StartingPet), one row per race and class like
-- `playercreateinfo_skills`. raceMask / classMask: 0 = every race / class. creature: a tameable creature_template
-- entry. quest: 0, or the last quest of the series marked done with PetTank.StartingPet.Quests (every quest before
-- it, and the quests leading into it, too). The defaults: each race's last "Taming the Beast" creature, and its
-- "Training the Beast" / "Beast Training" quest.
CREATE TABLE IF NOT EXISTS `pet_tank_starting_pet` (
    `raceMask` INT UNSIGNED NOT NULL DEFAULT 0,
    `classMask` INT UNSIGNED NOT NULL DEFAULT 0,
    `creature` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'creature_template.entry, tameable',
    `quest` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'quest_template.ID, the last of the series; 0 none',
    `comment` VARCHAR(255) NOT NULL DEFAULT '',
    PRIMARY KEY (`raceMask`, `classMask`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-temucraft-threat (pet tank): the pet each race / class starts with';

DELETE FROM `pet_tank_starting_pet` WHERE `classMask` = 4 AND `raceMask` IN (2, 4, 8, 32, 128, 512, 1024);
INSERT INTO `pet_tank_starting_pet` (`raceMask`, `classMask`, `creature`, `quest`, `comment`) VALUES
(2,    4, 3126,  6081, 'Orc Hunter - Armored Scorpid'),
(4,    4, 1196,  6086, 'Dwarf Hunter - Ice Claw Bear'),
(8,    4, 1996,  6103, 'Night Elf Hunter - Strigid Screecher'),
(32,   4, 2970,  6089, 'Tauren Hunter - Swoop'),
(128,  4, 3126,  6081, 'Troll Hunter - Armored Scorpid'),
(512,  4, 16353, 9673, 'Blood Elf Hunter - Mistbat'),
(1024, 4, 17203, 9675, 'Draenei Hunter - Nightstalker');
