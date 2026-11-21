-- mod-temucraft-threat: its player commands (all under .temucraft) as RBAC permissions 101350-101399. Linked to the stock player
-- role 195 and the Temucraft player role 1010, so every account has them by default. To take one away, remove its
-- link here (or deny it per account with .rbac account deny <account> <id>); to give it to staff only, link it to
-- 1011-1014 instead.
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
(195, 101350),
(1010, 101350),
(195, 101351),
(1010, 101351),
(195, 101352),
(1010, 101352),
(195, 101353),
(1010, 101353),
(195, 101354),
(1010, 101354),
(195, 101355),
(1010, 101355),
(195, 101356),
(1010, 101356);
