-- mod-temucraft-threat (pet tank): where each player turned the pet bonuses off for themself (the PetTank addon or .pettank).
-- `disabled` is a bit mask of places (1 world, 2 dungeon normal, 4 dungeon heroic, 8 raid 10, 16 raid 10 heroic,
-- 32 raid 25, 64 raid 25 heroic, 128 battleground, 256 arena). No row = on everywhere the server allows it.
CREATE TABLE IF NOT EXISTS `pet_tank_player` (
    `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
    `disabled` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-temucraft-threat (pet tank): places each player turned the bonuses off';
