-- mod-temucraft-threat (pet tank): `happiness_auto`, with PetTank.Happiness.Mode = player, whether this player's hunter pet is raised to
-- the happiness minimum by itself (1) or only when they ask (0); NULL = PetTank.Happiness.AutoDefault.
-- only once, so the file can be run again
SET @s = (SELECT IF(COUNT(*) = 0,
  'ALTER TABLE `pet_tank_player` ADD COLUMN `happiness_auto` TINYINT NULL DEFAULT NULL AFTER `disabled`',
  'DO 0') FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pet_tank_player' AND COLUMN_NAME = 'happiness_auto');
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;
