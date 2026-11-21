-- mod-temucraft-threat (pet tank): run by hand to remove the module's tables from the characters database.
DROP TABLE IF EXISTS `pet_tank_player`;
DROP TABLE IF EXISTS `pet_tank_starting_pet_given`;
-- mod-temucraft-threat: tank mode choices.
DELETE FROM `mod_temucraft_character_setting` WHERE `setting` = 'tankmode';
