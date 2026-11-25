#!/usr/bin/env python3
"""Switch one or more AS5812-54T QSFP+ cages from 4x10G breakout to 1x40G.

Run on the switch as root:  python3 qsfp_to_40g.py Ethernet48 Ethernet60
Edits (backups *.bak-<tag> next to each file, made once):
  - hwsku config.bcm   portmap_<l>=<first>:10 -> :40, drops the other 3 lanes' portmaps
  - hwsku port_config.ini  <first port> gets 4 lanes / 40000, the other 3 ports are removed
  - /etc/sonic/config_db.json  same for PORT, and every key/list that names a removed port
lanemap.ini must be regenerated afterwards (gen_lanemap.py) and the config reloaded.
"""
import json, os, re, shutil, sys

HWSKU = "/usr/share/sonic/device/x86_64-accton_as5812_54t-r0/Accton-AS5812-54T"
CFG_BCM = HWSKU + "/td2-as5812t-72x10G.config.bcm"
PORT_INI = HWSKU + "/port_config.ini"
CONFIG_DB = "/etc/sonic/config_db.json"
TAG = "bak-2026-11-25-pre40g"


def backup(p):
    b = "%s.%s" % (p, TAG)
    if not os.path.exists(b):
        shutil.copy2(p, b)


def read_port_ini():
    rows = []
    for line in open(PORT_INI):
        rows.append(line)
    return rows


def main():
    firsts = sys.argv[1:]
    if not firsts:
        sys.exit("usage: qsfp_to_40g.py EthernetN [EthernetM ...]")
    for p in (CFG_BCM, PORT_INI, CONFIG_DB):
        backup(p)

    # --- port_config.ini: find the 4 ports of each cage by index column ---
    rows = read_port_ini()
    data = [(i, r.split()) for i, r in enumerate(rows) if r.strip() and not r.startswith("#")]
    by_name = {c[0]: (i, c) for i, c in data}
    plan = {}  # first -> (lanes[4], removed names[3])
    for f in firsts:
        i, c = by_name[f]
        cage = c[3]
        members = [cc for _, cc in data if cc[3] == cage]
        if len(members) != 4 or members[0][0] != f:
            sys.exit("%s: cage %s has %d ports (first %s), not a 4x10G breakout" %
                     (f, cage, len(members), members[0][0]))
        lanes = [m[1] for m in members]
        plan[f] = (lanes, [m[0] for m in members[1:]])
    removed = {n for _, rm in plan.values() for n in rm}

    out = []
    for i, r in enumerate(rows):
        cols = r.split()
        if r.strip() and not r.startswith("#"):
            if cols[0] in removed:
                continue
            if cols[0] in plan:
                cols[1] = ",".join(plan[cols[0]][0])
                cols[4] = "40000"
                r = "%-14s %-18s %-13s %-6s %-10s %s\n" % tuple(cols[:5] + [" ".join(cols[5:])])
        out.append(r)
    open(PORT_INI, "w").write("".join(out))

    # --- config.bcm portmap ---
    first_phys = {plan[f][0][0] for f in plan}
    other_phys = {l for f in plan for l in plan[f][0][1:]}
    pm = re.compile(r"^\s*portmap_(\d+)\s*=\s*(\d+)\s*:\s*(\d+)")
    out = []
    for line in open(CFG_BCM):
        m = pm.match(line)
        if m and m.group(2) in other_phys:
            continue
        if m and m.group(2) in first_phys:
            line = "portmap_%s=%s:40\n" % (m.group(1), m.group(2))
        out.append(line)
    open(CFG_BCM, "w").write("".join(out))

    # --- config_db.json ---
    db = json.load(open(CONFIG_DB))

    def strip_list(s):
        items = s.split(",")
        kept = [x for x in items if x not in removed]
        return ",".join(kept), len(kept) != len(items)

    for table in list(db):
        ents = db[table]
        if not isinstance(ents, dict):
            continue
        for key in list(ents):
            parts = key.split("|")
            newparts, drop = [], False
            for p in parts:
                if p in removed:
                    drop = True
                    break
                if "," in p:
                    p2, ch = strip_list(p)
                    if p2 == "":
                        drop = True
                        break
                    p = p2
                newparts.append(p)
            if drop:
                del ents[key]
                continue
            nk = "|".join(newparts)
            val = ents.pop(key)
            if isinstance(val, dict):
                for fld, v in list(val.items()):
                    if isinstance(v, str) and ("," in v or v in removed):
                        v2, ch = strip_list(v)
                        if ch:
                            val[fld] = v2
                    elif isinstance(v, list):
                        val[fld] = [x for x in v if x not in removed]
            ents[nk] = val
    for f, (lanes, _) in plan.items():
        db["PORT"][f]["lanes"] = ",".join(lanes)
        db["PORT"][f]["speed"] = "40000"
    json.dump(db, open(CONFIG_DB, "w"), indent=4, sort_keys=True)

    for f, (lanes, rm) in plan.items():
        print("%s -> 40G lanes %s, removed %s" % (f, ",".join(lanes), " ".join(rm)))


if __name__ == "__main__":
    main()
