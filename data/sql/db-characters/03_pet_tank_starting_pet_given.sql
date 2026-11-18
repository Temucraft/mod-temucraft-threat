-- mod-temucraft-threat (pet tank): characters already looked at for a starting pet (PetTank.StartingPet), so it happens only once.
CREATE TABLE IF NOT EXISTS `pet_tank_starting_pet_given` (
    `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-temucraft-threat (pet tank): characters that had their starting pet chance';
