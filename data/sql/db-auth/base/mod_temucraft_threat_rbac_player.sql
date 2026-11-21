-- mod-temucraft-threat: its commands (all under .temucraft) as RBAC permissions 101350-101399. Every command is admin only by
-- default: linked to the administrator roles 192 and 1014. To give one to players, link it to 195 and 1010 here (or
-- grant it per account with .rbac account grant <account> <id>). "Temucraft: use ..." permissions are not
-- commands: they stay linked to the player roles so the items keep working.
DELETE FROM `rbac_linked_permissions` WHERE `linkedId` BETWEEN 101350 AND 101399;
DELETE FROM `rbac_permissions` WHERE `id` BETWEEN 101350 AND 101399;
INSERT INTO `rbac_permissions` (`id`, `name`) VALUES
(101350, 'Command: temucraft pettank'),
(101351, 'Command: temucraft pettank on'),
(101352, 'Command: temucraft pettank off'),
(101353, 'Command: temucraft pettank happy'),
(101354, 'Command: temucraft tankrole'),
(101355, 'Command: temucraft tankmode'),
(101356, 'Command: temucraft threat');
INSERT INTO `rbac_linked_permissions` (`id`, `linkedId`) VALUES
(192, 101350),
(1014, 101350),
(192, 101351),
(1014, 101351),
(192, 101352),
(1014, 101352),
(192, 101353),
(1014, 101353),
(192, 101354),
(1014, 101354),
(192, 101355),
(1014, 101355),
(192, 101356),
(1014, 101356);
