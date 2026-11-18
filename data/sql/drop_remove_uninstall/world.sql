-- mod-temucraft-threat (pet tank): run by hand to remove the module's table from the world database.
DROP TABLE IF EXISTS `pet_tank_starting_pet`;
DELETE FROM `module_string` WHERE `module` = 'mod-temucraft-threat';
