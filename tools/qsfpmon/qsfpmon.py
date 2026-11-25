#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""qsfpmon - AS5812-54T (SONiC / custom SAI, BCM56864) QSFP+ 6 ケージのリアルタイム監視.

読み取り専用。標準ライブラリのみ。root で実行すること (sudo qsfpmon)。
データ源: CPLD sysfs / optoe1 EEPROM (SFF-8636) / APPL_DB / syncd 診断シェル (任意)。
"""
import argparse
import collections
import functools
import datetime
import glob
import json
import math
import os
import re
import select
import signal
import subprocess
import sys
import termios
import threading
import time
import tty

CAGES = [49, 50, 51, 52, 53, 54]
# platform plugin (sfputil.py) の _port_to_i2c_mapping[cage][1] と同じ
CAGE_TO_BUS = {49: 4, 50: 6, 51: 3, 52: 5, 53: 7, 54: 2}
CPLD_DIR_GLOB = "/sys/bus/i2c/devices/*-0060"
DEVICE_GLOB = "/usr/share/sonic/device/x86_64-accton_as5812_54t-r0"
DARK_MW = 0.01            # これ未満の RX は dark
SHELL_SOCK = "/var/run/hwbcm/shell.sock"

# ---------------------------------------------------------------- 色

class C:
    on = True

    @staticmethod
    def w(code, s):
        return "\x1b[%sm%s\x1b[0m" % (code, s) if C.on else s


def red(s): return C.w("31;1", s)
def green(s): return C.w("32", s)
def yellow(s): return C.w("33;1", s)
def dim(s): return C.w("2", s)
def bold(s): return C.w("1", s)
def cyan(s): return C.w("36", s)
def chg(s): return C.w("7;33", s)      # 変化ハイライト (反転)


ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def vlen(s):
    return len(ANSI_RE.sub("", s))


def fit(s, w):
    """可視幅 w に切り詰める (ANSI を壊さない)。"""
    if vlen(s) <= w:
        return s
    out, n, i = [], 0, 0
    while i < len(s) and n < w:
        m = ANSI_RE.match(s, i)
        if m:
            out.append(m.group(0)); i = m.end(); continue
        out.append(s[i]); n += 1; i += 1
    out.append("\x1b[0m" if C.on else "")
    return "".join(out)

# ---------------------------------------------------------------- 取得

def sh(cmd, timeout=5):
    try:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                           timeout=timeout, universal_newlines=True)
        return r.stdout
    except Exception:
        return ""


def find_cpld():
    for d in sorted(glob.glob(CPLD_DIR_GLOB)):
        if os.path.exists(os.path.join(d, "module_present_49")):
            return d
    return None


def read_int(path):
    try:
        with open(path) as f:
            return int(f.read().strip(), 0)
    except Exception:
        return None


def read_eeprom(bus):
    p = "/sys/bus/i2c/devices/%d-0050/eeprom" % bus
    try:
        with open(p, "rb", buffering=0) as f:
            f.seek(0)
            b = f.read(256)
        return b if len(b) == 256 else None
    except Exception:
        return None


def s16(h, l):
    v = (h << 8) | l
    return v - 65536 if v & 0x8000 else v


def u16(b, o):
    return (b[o] << 8) | b[o + 1]


def ascii_(b, a, z):
    return bytes(b[a:z]).decode("ascii", "replace").strip().strip("\x00")


COMPLIANCE = [(7, "40G-AOC"), (6, "40GBASE-LR4"), (5, "40GBASE-SR4"), (4, "40GBASE-CR4"),
              (3, "10GBASE-SR"), (2, "10GBASE-LR"), (1, "10GBASE-LRM")]
IDENT = {0x0c: "QSFP", 0x0d: "QSFP+", 0x11: "QSFP28", 0x03: "SFP"}
CONN = {0x00: "unk", 0x07: "LC", 0x0b: "optical-pigtail", 0x0c: "MPO", 0x23: "no-sep(AOC/DAC)"}


def parse_eeprom(b, cur_flags):
    d = {}
    ident = b[128]
    d["ident"] = IDENT.get(ident, "0x%02x" % ident)
    cls = ((b[129] >> 6) & 3) + 1
    d["pclass"] = cls
    d["conn"] = CONN.get(b[130], "0x%02x" % b[130])
    d["compl"] = ",".join(n for bit, n in COMPLIANCE if b[131] & (1 << bit)) or ("0x%02x" % b[131])
    ln = b[146]
    if ln == 0 and b[143]:
        ln = b[143] * 2
    d["length"] = ln
    d["vendor"] = ascii_(b, 148, 164)
    d["pn"] = ascii_(b, 168, 184)
    d["sn"] = ascii_(b, 196, 212)
    d["temp"] = s16(b[22], b[23]) / 256.0
    d["vcc"] = u16(b, 26) / 10000.0
    los = cur_flags[0] if cur_flags else b[3]
    flt = cur_flags[1] if cur_flags else b[4]
    d["los_latched"] = b[3]
    txpwr_ok = bool(b[220] & 0x04)
    lanes = []
    for i in range(4):
        rx_mw = u16(b, 34 + 2 * i) * 0.0001
        bias = u16(b, 42 + 2 * i) * 2 / 1000.0
        tx_raw = u16(b, 50 + 2 * i)
        tx_mw = tx_raw * 0.0001
        lanes.append({
            "rx_mw": rx_mw,
            "bias": bias,
            "tx_mw": tx_mw if (txpwr_ok or tx_raw) else None,
            "rx_los": bool(los & (1 << i)),
            "tx_los": bool(los & (1 << (4 + i))),
            "tx_fault": bool(flt & (1 << i)),
            "tx_dis": bool(b[86] & (1 << i)),
        })
    d["lanes"] = lanes
    d["pwr93"] = b[93]
    return d


PORT_SPLIT = re.compile(r"\t")


def read_ports():
    """APPL_DB から PORT_TABLE を 1 回で取得 -> {cage: [port dict ...]}"""
    lua = ("local o={} for _,k in ipairs(redis.call('keys','PORT_TABLE:Ethernet*')) do "
           "local h=redis.call('hgetall',k) local s=k for i=1,#h,2 do s=s..'\\t'..h[i]..'='..h[i+1] end "
           "o[#o+1]=s end return o")
    out = sh(["sonic-db-cli", "APPL_DB", "eval", lua, "0"], 8)
    ports = {}
    for line in out.splitlines():
        parts = line.split("\t")
        if not parts or not parts[0].startswith("PORT_TABLE:"):
            continue
        p = {"name": parts[0].split(":", 1)[1]}
        for kv in parts[1:]:
            if "=" in kv:
                k, v = kv.split("=", 1)
                p[k] = v
        try:
            idx = int(p.get("index", "0"))
        except ValueError:
            continue
        if idx in CAGES:
            lanes = [int(x) for x in p.get("lanes", "").split(",") if x.strip().isdigit()]
            p["lanes_l"] = lanes
            try:
                p["spd"] = int(p.get("speed", "0")) // 1000
            except ValueError:
                p["spd"] = 0
            ports.setdefault(idx, []).append(p)
    for idx in ports:
        ports[idx].sort(key=lambda p: (p["lanes_l"][0] if p["lanes_l"] else 0, p["name"]))
    return ports


_portmap_cache = {}


def load_portmap():
    """portmap_N=<first lane>:<speed> -> {first lane: bcm port}"""
    if _portmap_cache:
        return _portmap_cache
    cands = []
    hw = sh(["sonic-db-cli", "CONFIG_DB", "hget", "DEVICE_METADATA|localhost", "hwsku"], 3).strip()
    dirs = ["/usr/share/sonic/hwsku"]
    if hw:
        dirs.append(os.path.join(DEVICE_GLOB, hw))
    dirs += glob.glob(DEVICE_GLOB + "/*/")
    for d in dirs:
        prof = os.path.join(d, "sai.profile")
        if os.path.exists(prof):
            try:
                for ln in open(prof):
                    if ln.startswith("SAI_INIT_CONFIG_FILE"):
                        cands.append(os.path.join(d, os.path.basename(ln.split("=", 1)[1].strip())))
            except Exception:
                pass
        cands += glob.glob(os.path.join(d, "*.config.bcm"))
    for f in cands:
        try:
            txt = open(f).read()
        except Exception:
            continue
        m = re.findall(r"^portmap_(\d+)=(\d+):(\d+)", txt, re.M)
        if m:
            for n, lane, _spd in m:
                _portmap_cache[int(lane)] = int(n)
            _portmap_cache["_file"] = f
            return _portmap_cache
    return _portmap_cache


_flag_hist = {}
LOS_HOLD = 4


def collect_base(cpld, ports, hold=LOS_HOLD):
    cages = {}
    for cage in CAGES:
        c = {"cage": cage, "bus": CAGE_TO_BUS[cage], "ports": ports.get(cage, [])}
        pres = read_int(os.path.join(cpld, "module_present_%d" % cage)) if cpld else None
        c["present"] = bool(pres) if pres is not None else None
        c["lp_pin"] = read_int(os.path.join(cpld, "module_lp_mode_%d" % cage)) if cpld else None
        c["reset"] = read_int(os.path.join(cpld, "module_reset_%d" % cage)) if cpld else None
        c["eeprom"] = None
        c["eeprom_err"] = False
        if c["present"]:
            b = read_eeprom(c["bus"])
            if b is None:
                c["eeprom_err"] = True
            else:
                # LOS/TX fault はクリアオンリードのラッチ。読んだ直後は 0 に戻りモジュールが
                # 次の更新周期で再アサートするため、直近 LOS_HOLD 回の OR を「現在値」とみなす。
                h = _flag_hist.setdefault(cage, collections.deque(maxlen=hold))
                h.append((b[3], b[4]))
                flags = (functools.reduce(lambda x, y: x | y, (t[0] for t in h)),
                         functools.reduce(lambda x, y: x | y, (t[1] for t in h)))
                c["eeprom"] = parse_eeprom(b, flags)
        cages[cage] = c
    return cages

# ---------------------------------------------------------------- SerDes (syncd 診断シェル)

SHELL_PY = (
    "import socket,sys,json\n"
    "cmds=json.loads(sys.argv[1]);res={}\n"
    "for c in cmds:\n"
    " try:\n"
    "  s=socket.socket(socket.AF_UNIX);s.settimeout(8);s.connect('%s');s.sendall((c+'\\n').encode());"
    "s.shutdown(socket.SHUT_WR);res[c]=s.makefile().read();s.close()\n"
    " except Exception as e:\n"
    "  res[c]='ERR '+str(e)\n"
    "sys.stdout.write(json.dumps(res))\n" % SHELL_SOCK)

DSC_ROW = re.compile(r"^\s*(\d+)\s+\(\S+\s*,\s*0x[0-9a-fA-F]+,\s*\d+,\s*\d+\)\s+(\d+)(\*?)\s+(\d+)(\*?)\s")
PS_ROW = re.compile(r"\(\s*(\d+)\)\s+(\S+)\s+(\S+)\s+(FD|HD)\s+.*?\b(XLAUI|XFI|SFI|KR4|CR4|SR4|LR4|KR|CR|SR|GMII|XGMII|SGMII|CAUI)\b")


def serdes_poll(bcm_ports):
    """bcm_ports: 現在モジュールが挿さっているケージの bcm ポート番号リスト。"""
    if not bcm_ports:
        return {"ok": True, "ts": time.time(), "ports": {}}
    cmds = []
    for p in bcm_ports:
        cmds += ["ps %d" % p, "phy diag %d dsc" % p]
    out = sh(["docker", "exec", "syncd", "python3", "-c", SHELL_PY, json.dumps(cmds)], 40)
    try:
        res = json.loads(out)
    except Exception:
        return {"ok": False, "ts": time.time(), "err": "syncd 診断シェルに接続できない (syncd 停止中?)", "ports": {}}
    ports = {}
    errs = 0
    for p in bcm_ports:
        ps = res.get("ps %d" % p, "")
        dsc = res.get("phy diag %d dsc" % p, "")
        if ps.startswith("ERR") or dsc.startswith("ERR"):
            errs += 1
        info = {"link": None, "speed": None, "intf": None, "lanes": []}
        m = PS_ROW.search(ps)
        if m:
            info["link"], info["speed"], info["intf"] = m.group(2), m.group(3), m.group(5)
        for ln in dsc.splitlines():
            m = DSC_ROW.match(ln)
            if m:
                info["lanes"].append({"sd": int(m.group(2)), "sd_chg": bool(m.group(3)),
                                      "lck": int(m.group(4)), "lck_chg": bool(m.group(5))})
        ports[p] = info
    if errs == len(bcm_ports):
        return {"ok": False, "ts": time.time(), "err": "診断シェルがエラー応答", "ports": {}}
    return {"ok": True, "ts": time.time(), "ports": ports}


class SerdesPoller(threading.Thread):
    def __init__(self, interval):
        super().__init__(daemon=True)
        self.interval = interval
        self.enabled = True
        self.targets = []
        self.result = None
        self.lock = threading.Lock()
        self.stop = False

    def run(self):
        while not self.stop:
            t0 = time.time()
            if self.enabled and self.targets:
                r = serdes_poll(list(self.targets))
                with self.lock:
                    self.result = r
            while not self.stop and time.time() - t0 < self.interval:
                time.sleep(0.2)

    def get(self):
        with self.lock:
            return self.result

# ---------------------------------------------------------------- 状態の組み立て

def build_slots(cage, pm):
    """ケージのレーン 1..4 を (port, ポート内レーン番号, bcm ポート) に割り当てる。"""
    slots = []
    for p in cage["ports"]:
        n = max(1, len(p["lanes_l"]))
        bcm = pm.get(p["lanes_l"][0]) if p["lanes_l"] else None
        for k in range(n):
            slots.append((p, k, bcm))
    return slots[:4]


def dbm(mw):
    return 10 * math.log10(mw) if mw > 0 else None


def bar(mw, width=10):
    d = dbm(mw)
    if d is None:
        return " " * width
    frac = max(0.0, min(1.0, (d + 20.0) / 23.0))   # -20..+3 dBm
    n = int(round(frac * width))
    s = "#" * n + "." * (width - n)
    return green(s) if d >= -7 else (yellow(s) if d >= -13 else red(s))

# ---------------------------------------------------------------- 描画

def oper_str(p, changed=False):
    o = p.get("oper_status", "?")
    s = green("UP  ") if o == "up" else (red("DOWN") if o == "down" else yellow(o[:4].ljust(4)))
    return chg(s) if changed else s


def render(cages, serdes, serdes_on, prev, events, width, height, interval, paused, pm, once=False):
    L = []
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    sd_txt = "off"
    if serdes_on:
        if serdes is None:
            sd_txt = "取得中..."
        elif not serdes["ok"]:
            sd_txt = red("利用不可") + dim(" (%s)" % serdes.get("err", ""))
        else:
            sd_txt = green("on") + dim(" (%.0fs前)" % (time.time() - serdes["ts"]))
    hdr = bold("qsfpmon") + "  AS5812-54T QSFP+ x6   %s   interval %.1fs   serdes %s" % (now, interval, sd_txt)
    if paused:
        hdr += "  " + yellow("[PAUSED]")
    L.append(hdr)
    if not once:
        L.append(dim("q:終了  s:serdes切替  p:一時停止  c:ログ消去  +/-:間隔  | dark = RX < %.2f mW ( %.0f dBm)" % (DARK_MW, dbm(DARK_MW))))
    L.append("")
    for cn in CAGES:
        c = cages[cn]
        pc = (prev or {}).get(cn)
        e = c["eeprom"]
        slots = build_slots(c, pm)
        if c["present"] is None:
            L.append("%s %s" % (bold("Cage %d" % cn), red("(CPLD 読めず)")))
            continue
        pres_chg = pc is not None and pc["present"] != c["present"]
        if not c["present"]:
            names = " ".join("%s/%dG:%s" % (p["name"], p["spd"], p.get("oper_status", "?")) for p in c["ports"])
            tag = chg(" EMPTY ") if pres_chg else dim("empty")
            L.append("%s  %s   %s   %s" % (bold("Cage %d" % cn), tag, dim("(i2c-%d)" % c["bus"]), dim(names)))
            continue
        # ヘッダ行
        if e is None:
            L.append("%s  %s  %s" % (bold("Cage %d" % cn), (chg(" PRESENT ") if pres_chg else green("present")),
                                     red("EEPROM 読み出し失敗 (i2c-%d)" % c["bus"])))
        else:
            lp = "LP:%s" % ("ON" if c["lp_pin"] else "off")
            if e["pwr93"] & 1:
                lp += "/sw:%s" % ("ON" if e["pwr93"] & 2 else "off")
            L.append("%s  %s  %s %s %s %sm  PN %s  SN %s" % (
                bold("Cage %d" % cn), (chg(" PRESENT ") if pres_chg else green("present")),
                e["ident"], cyan(e["vendor"]), e["compl"], e["length"], e["pn"], e["sn"]))
            L.append("    T %.1fC  Vcc %.2fV  %s  class%d  i2c-%d" % (e["temp"], e["vcc"], lp, e["pclass"], c["bus"]))
        # レーン表
        L.append(dim("    %-11s %3s %-4s %-4s %2s %7s %6s %-10s %8s %6s  RXL TXL FLT  SD  LCK" % ("Port", "Spd", "Adm", "Oper", "Ln", "RX mW", "dBm", "RX level", "TXbias", "TXpwr")))
        for i in range(4):
            ln = e["lanes"][i] if e else None
            slot = slots[i] if i < len(slots) else None
            # ポート列
            if slot and slot[1] == 0:
                p = slot[0]
                pp = None
                if pc:
                    for q in pc["ports"]:
                        if q["name"] == p["name"]:
                            pp = q
                ch = pp is not None and pp.get("oper_status") != p.get("oper_status")
                adm = p.get("admin_status", "?")
                port_col = "%-11s %3s %s %s" % (p["name"], "%dG" % p["spd"],
                                                (green("up  ") if adm == "up" else red(adm[:4].ljust(4))), oper_str(p, ch))
            elif slot:
                port_col = dim("%-11s %-3s %-4s %-4s" % ("  |", "", "", ""))
            else:
                port_col = " " * 24
            if ln is None:
                L.append("    %s %2d %s" % (port_col, i + 1, dim("-")))
                continue
            mw = ln["rx_mw"]
            d = dbm(mw)
            dark = mw < DARK_MW
            pl = pc["eeprom"]["lanes"][i] if (pc and pc["eeprom"]) else None
            dark_chg = pl is not None and (pl["rx_mw"] < DARK_MW) != dark
            if dark:
                lvl = chg(red("dark      ")) if dark_chg else red("dark      ")
                mwc = red("%7.3f" % mw)
                dbc = red("%6s" % ("-inf" if d is None else "%.1f" % d))
            else:
                lvl = bar(mw)
                if dark_chg:
                    lvl = chg(lvl)
                mwc = "%7.3f" % mw
                dbc = "%6.1f" % d
                if pl and pl["rx_mw"] > 0 and abs(dbm(mw) - dbm(pl["rx_mw"])) > 1.0:
                    mwc = chg(mwc)
            txp = "%6.3f" % ln["tx_mw"] if ln["tx_mw"] is not None else dim("   n/a")

            def flag(cur, pv, txt):
                s = red(txt) if cur else dim("-".ljust(len(txt)))
                return chg(s) if (pv is not None and pv != cur) else s
            rxl = flag(ln["rx_los"], pl["rx_los"] if pl else None, "LOS")
            txl = flag(ln["tx_los"], pl["tx_los"] if pl else None, "LOS")
            tfl = flag(ln["tx_fault"], pl["tx_fault"] if pl else None, "FLT")
            # SerDes
            sdc = lck = dim("  -")
            if serdes_on:
                if serdes is None:
                    sdc = lck = dim("  .")
                elif not serdes["ok"]:
                    sdc = lck = dim("n/a")
                elif slot and slot[2] in serdes["ports"]:
                    rows = serdes["ports"][slot[2]]["lanes"]
                    if slot[1] < len(rows):
                        r = rows[slot[1]]
                        sdc = (green(" 1") if r["sd"] else red(" 0")) + ("*" if r["sd_chg"] else " ")
                        lck = (green(" 1") if r["lck"] else red(" 0")) + ("*" if r["lck_chg"] else " ")
            L.append("    %s %2d %s %s %s %s %s  %s %s %s %s %s" % (
                port_col, i + 1, mwc, dbc, lvl, ("%5.2fmA" % ln["bias"]).rjust(8), txp, rxl, txl, tfl, sdc, lck))
        # serdes 側 link 状態 (ps)
        if serdes_on and serdes and serdes["ok"]:
            bits = []
            seen = set()
            for p, k, bcm in slots:
                if bcm in seen or bcm not in serdes["ports"]:
                    continue
                seen.add(bcm)
                si = serdes["ports"][bcm]
                bits.append("bcm%d:%s %s %s" % (bcm, si["link"] or "?", si["speed"] or "?", si["intf"] or "?"))
            if bits:
                L.append(dim("    ps: " + " | ".join(bits)))
    # イベントログ
    L.append("")
    L.append(bold("Events") + dim("  (最新が下)"))
    room = max(3, height - len(L) - 1) if height else 8
    ev = events[-room:]
    if not ev:
        L.append(dim("  (まだなし)"))
    for t, kind, msg in ev:
        col = {"down": red, "up": green, "los": red, "ok": green, "info": cyan}.get(kind, yellow)
        L.append("  %s  %s" % (dim(t), col(msg)))
    return [fit(x, width) for x in L]

# ---------------------------------------------------------------- 変化検出 (イベントログ)

def diff_events(prev, cur, prev_sd, cur_sd, pm):
    ev = []
    ts = datetime.datetime.now().strftime("%H:%M:%S")

    def add(kind, msg):
        ev.append((ts, kind, msg))
    for cn in CAGES:
        c, p = cur[cn], prev.get(cn)
        if not p:
            continue
        if c["present"] != p["present"]:
            if c["present"]:
                e = c["eeprom"]
                add("up", "Cage %d: モジュール挿入 %s" % (cn, (e["pn"] if e else "")))
            else:
                add("down", "Cage %d: モジュール抜去" % cn)
        pports = {x["name"]: x for x in p["ports"]}
        for x in c["ports"]:
            o = pports.get(x["name"])
            if o and o.get("oper_status") != x.get("oper_status"):
                add("up" if x.get("oper_status") == "up" else "down",
                    "%s (cage %d): oper %s -> %s" % (x["name"], cn, o.get("oper_status"), x.get("oper_status")))
            if o and o.get("admin_status") != x.get("admin_status"):
                add("info", "%s: admin %s -> %s" % (x["name"], o.get("admin_status"), x.get("admin_status")))
        if c["eeprom"] and p["eeprom"]:
            for i in range(4):
                a, b = p["eeprom"]["lanes"][i], c["eeprom"]["lanes"][i]
                for key, lab in (("rx_los", "RX LOS"), ("tx_los", "TX LOS"), ("tx_fault", "TX fault")):
                    if a[key] != b[key]:
                        add("los" if b[key] else "ok", "Cage %d lane%d: %s %s" % (cn, i + 1, lab, "ASSERT" if b[key] else "clear"))
                da, db = a["rx_mw"] < DARK_MW, b["rx_mw"] < DARK_MW
                if da != db:
                    add("los" if db else "ok", "Cage %d lane%d: RX %s (%.3f mW)" % (cn, i + 1, "dark" if db else "light 復帰", b["rx_mw"]))
    # SerDes SD / LCK 変化 (ポート単位で比較)
    if prev_sd and cur_sd and prev_sd["ok"] and cur_sd["ok"] and prev_sd is not cur_sd:
        for bcm, si in cur_sd["ports"].items():
            o = prev_sd["ports"].get(bcm)
            if not o:
                continue
            for k, (a, b) in enumerate(zip(o["lanes"], si["lanes"])):
                for key in ("sd", "lck"):
                    if a[key] != b[key]:
                        add("up" if b[key] else "down", "bcm%d lane%d: %s %d -> %d" % (bcm, k, "SD" if key == "sd" else "CDR LCK", a[key], b[key]))
            if o["link"] != si["link"] and si["link"]:
                add("up" if si["link"] == "up" else "down", "bcm%d: MAC link %s -> %s" % (bcm, o["link"], si["link"]))
    return ev

# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(
        prog="qsfpmon",
        description="AS5812-54T の QSFP+ 6 ケージ (49-54) をリアルタイム監視する (読み取り専用, root で実行)。")
    ap.add_argument("-i", "--interval", type=float, default=1.0, help="更新間隔 秒 (既定 1.0)")
    ap.add_argument("--once", action="store_true", help="1 回だけスナップショットを出力して終了")
    ap.add_argument("--no-serdes", action="store_true", help="SerDes(診断シェル)の取得を無効化")
    ap.add_argument("--serdes-interval", type=float, default=3.0, help="SerDes 取得間隔 秒 (既定 3, 最小 3)")
    ap.add_argument("--color", choices=["auto", "always", "never"], default="auto", help="色 (既定 auto)")
    ap.add_argument("--width", type=int, default=0, help="--once の出力幅 (既定 160)")
    args = ap.parse_args()
    if os.geteuid() != 0:
        sys.stderr.write("root 権限が必要です: sudo qsfpmon\n")
        return 2
    C.on = args.color == "always" or (args.color == "auto" and sys.stdout.isatty())
    cpld = find_cpld()
    if not cpld:
        sys.stderr.write("警告: CPLD sysfs (module_present_*) が見つかりません\n")
    interval = max(0.2, args.interval)
    pm = load_portmap()
    sd_on = not args.no_serdes
    serdes_interval = max(3.0, args.serdes_interval)

    if args.once:
        ports = read_ports()
        collect_base(cpld, ports, 2)          # 1 回目: ラッチをクリア
        time.sleep(0.6)
        collect_base(cpld, ports, 2)          # 2,3 回目の OR を現在値とする
        time.sleep(0.4)
        cages = collect_base(cpld, ports, 2)
        sd = None
        if sd_on:
            tg = sorted({pm.get(p["lanes_l"][0]) for cn in CAGES if cages[cn]["present"]
                         for p in cages[cn]["ports"] if p["lanes_l"] and pm.get(p["lanes_l"][0])})
            if not pm:
                sd = {"ok": False, "err": "portmap 不明", "ts": time.time(), "ports": {}}
            else:
                sd = serdes_poll(tg)
        for ln in render(cages, sd, sd_on, None, [], args.width or 160, 0, interval, False, pm, once=True):
            print(ln)
        return 0

    poller = SerdesPoller(serdes_interval)
    poller.enabled = sd_on
    if not pm:
        sd_on = False
    poller.start()
    events = []
    prev = None
    prev_sd = None
    paused = False
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd) if sys.stdin.isatty() else None
    resized = [True]
    signal.signal(signal.SIGWINCH, lambda *a: resized.__setitem__(0, True))
    try:
        if old:
            tty.setcbreak(fd)
        sys.stdout.write("\x1b[?1049h\x1b[?25l")
        cages = None
        last_sd_seen = None
        while True:
            t0 = time.time()
            if not paused or cages is None:
                ports = read_ports()
                cages = collect_base(cpld, ports)
                poller.enabled = sd_on
                poller.targets = sorted({pm.get(p["lanes_l"][0]) for cn in CAGES if cages[cn]["present"]
                                         for p in cages[cn]["ports"] if p["lanes_l"] and pm.get(p["lanes_l"][0])})
                sd = poller.get() if sd_on else None
                if prev is not None:
                    events.extend(diff_events(prev, cages, prev_sd, sd, pm))
                if sd is not None and sd is not last_sd_seen:
                    prev_sd = sd
                    last_sd_seen = sd
                prev_cages_for_render = prev
                prev = cages
            sz = os.get_terminal_size() if sys.stdout.isatty() else os.terminal_size((160, 50))
            sd = poller.get() if sd_on else None
            lines = render(cages, sd, sd_on, prev_cages_for_render, events, sz.columns, sz.lines, interval, paused, pm)
            lines = lines[:sz.lines - 1] if len(lines) >= sz.lines else lines
            sys.stdout.write("\x1b[H" + "\n".join(l + "\x1b[K" for l in lines) + "\x1b[J")
            sys.stdout.flush()
            # キー待ち
            wait = max(0.05, interval - (time.time() - t0))
            end = time.time() + wait
            quit_ = False
            while time.time() < end and old:
                r, _, _ = select.select([sys.stdin], [], [], max(0.0, end - time.time()))
                if r:
                    ch = sys.stdin.read(1)
                    if ch in ("q", "Q", "\x03"):
                        quit_ = True; break
                    elif ch in ("s", "S"):
                        if pm:
                            sd_on = not sd_on
                    elif ch in ("p", "P", " "):
                        paused = not paused
                    elif ch in ("c", "C"):
                        events.clear()
                    elif ch == "+":
                        interval = min(10.0, interval + 0.5)
                    elif ch == "-":
                        interval = max(0.2, interval - 0.5)
                    break
            if not old:
                time.sleep(max(0, end - time.time()))
            if quit_:
                break
    except KeyboardInterrupt:
        pass
    finally:
        poller.stop = True
        sys.stdout.write("\x1b[?25h\x1b[?1049l")
        sys.stdout.flush()
        if old:
            termios.tcsetattr(fd, termios.TCSADRAIN, old)
    return 0


if __name__ == "__main__":
    sys.exit(main())
