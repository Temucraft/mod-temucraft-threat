-- mod-temucraft-threat: each character's tank mode choice.
CREATE TABLE IF NOT EXISTS `mod_temucraft_threat` (
    `guid`      INT UNSIGNED NOT NULL,
    `tank_mode` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
