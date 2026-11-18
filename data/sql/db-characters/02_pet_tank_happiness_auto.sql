-- mod-temucraft-threat (pet tank): `happiness_auto`, with PetTank.Happiness.Mode = player, whether this player's hunter pet is raised to
-- the happiness minimum by itself (1) or only when they ask (0); NULL = PetTank.Happiness.AutoDefault.
ALTER TABLE `pet_tank_player`
    ADD COLUMN `happiness_auto` TINYINT NULL DEFAULT NULL AFTER `disabled`;
