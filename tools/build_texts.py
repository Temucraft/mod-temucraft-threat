#!/usr/bin/env python3
"""Writes data/sql/db-world/base/mod_temucraft_threat_module_string.sql from the English texts in src/ (each
Say(..., T_X, "text") / TellAll(g, T_X, "text"), numbered by the files' enum Text)."""
import os, re
MOD = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
rows = {}
for f in ('TemucraftThreat.cpp', 'TemucraftTankRoles.cpp'):
    s = open(os.path.join(MOD, 'src', f)).read()
    names = [x.strip() for x in re.search(r'enum Text : uint32\s*\{([^}]*)\}', s, re.S).group(1).split(',') if x.strip()]
    vals, n = {}, 0
    for x in names:
        if '=' in x:
            k, v = x.split('='); k, n = k.strip(), int(v)
        else:
            k, n = x, n + 1
        vals[k] = n
    for m in re.finditer(r'(?:Say\([a-z]+, |TellAll\(g, )(T_[A-Z_]+), "((?:[^"\\]|\\.)*)"', s):
        rows.setdefault(vals[m.group(1)], m.group(2))
    for m in re.finditer(r'Say\(p, !target \? (T_\w+) : (T_\w+), !target \? "((?:[^"\\]|\\.)*)" :\s*"((?:[^"\\]|\\.)*)"', s):
        rows.setdefault(vals[m.group(1)], m.group(3)); rows.setdefault(vals[m.group(2)], m.group(4))
out = ["DELETE FROM `module_string` WHERE `module` = 'mod-temucraft-threat';",
       "INSERT INTO `module_string` (`module`, `id`, `string`) VALUES"]
out.append(",\n".join("('mod-temucraft-threat', %d, '%s')" % (k, t.replace('\\"', '"').replace("'", "''"))
                      for k, t in sorted(rows.items())) + ";")
open(os.path.join(MOD, 'data', 'sql', 'db-world', 'base', 'mod_temucraft_threat_module_string.sql'), 'w').write("\n".join(out) + "\n")
print(len(rows), 'texts')
