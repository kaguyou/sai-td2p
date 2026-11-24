# Integration notes: CoPP (control-plane policing in hardware)

Owner: CoPP agent, 2026-11-24. All four code files are new. No existing file was edited.

| File | What it is |
|---|---|
| `sairedis/vslib/hwbcm/hwbcm_copp.h` | SDK-free C API: policers (`hwbcm_copp_policer_*`), per-trap FP classifiers (`hwbcm_copp_trap_apply/remove/entries`), trap stats, `hwbcm_copp_local_egress_set`, `hwbcm_copp_refresh`, `hwbcm_copp_dump`, `hwbcm_copp_deinit` |
| `sairedis/vslib/hwbcm/hwbcm_copp.c` | Implementation: one IFP group, the rule templates for each trap kind, FP policers, per-entry Red/NotRed counters |
| `sairedis/vslib/SwitchBCM56860HW_copp.cpp` | `hwCreate/Set/RemovePolicer`, `hwCreate/Set/RemoveHostifTrapGroup`, the hooks `hwCoppOnTrapCreate/Set/Remove`, `hwCoppOnRoute`, `hwCoppRefresh`, `hwCoppCounterStats` |
| `sairedis/vslib/SwitchBCM56860HW_copp.inc.h` | Member declarations. `#include` this inside the class body. It sets `public:`/`protected:` itself and ends at `protected:` |

Syntax checks run in `saibuild`. All passed with no output:
- C: the command from the brief with `-Wall -Wextra -Werror` added.
- C++: `vslib_cxx_cmd.txt` with `-fsyntax-only` (it includes `-Werror -Wconversion ...`). The scratch copy of `SwitchBCM56860HW.h` had the include as the last line of the class, after the stats include. `SwitchBCM56860HW_copp.cpp`, `SwitchBCM56860HW.cpp` and `SwitchBCM56860HW_l3.cpp` compiled clean against it.
- Then the wiring in section 1 was applied to a scratch copy of the current `SwitchBCM56860HW.cpp` and compiled. That passed too. The 6 edits below are the ones that were tested.

---

## 1. Wiring steps

### 1.1 `SwitchBCM56860HW.h`

Put the include last in the class body, before the closing `};`. The stats include is already there; add CoPP after it:

```cpp
#include "SwitchBCM56860HW_stats.inc.h"
#include "SwitchBCM56860HW_copp.inc.h"
    };
}
```

You can also add `SwitchBCM56860HW_copp.cpp  POLICER, HOSTIF_TRAP_GROUP, hostif trap policing (CoPP)` to the "File split" comment at the top.

### 1.2 `SwitchBCM56860HW.cpp`: `create()`

Replace the HOSTIF_TRAP line, and add two cases:

```cpp
        case SAI_OBJECT_TYPE_HOSTIF_TRAP:
            hw = hwCreateHostifTrap(oid, attr_count, attr_list);
            if (hw == SAI_STATUS_SUCCESS) hw = hwCoppOnTrapCreate(oid);
            break;
        case SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP: hw = hwCreateHostifTrapGroup(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_POLICER:         hw = hwCreatePolicer(oid, attr_count, attr_list); break;
```

Change the ROUTE_ENTRY case (A3 section) so CoPP learns the ip2me CPU egress:

```cpp
        case SAI_OBJECT_TYPE_ROUTE_ENTRY:
            hw = hwCreateRoute(serializedObjectId, attr_count, attr_list);
            if (hw == SAI_STATUS_SUCCESS) hwCoppOnRoute(serializedObjectId);
            break;
```

### 1.3 `remove()`

Add these inside the `if (m_hwReady)` switch, next to `HOSTIF`. Today HOSTIF_TRAP has no remove case at all.

```cpp
            case SAI_OBJECT_TYPE_HOSTIF_TRAP:     hw = hwCoppOnTrapRemove(oid); break;
            case SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP: hw = hwRemoveHostifTrapGroup(oid); break;
            case SAI_OBJECT_TYPE_POLICER:         hw = hwRemovePolicer(oid); break;
```

### 1.4 `set()`

```cpp
        case SAI_OBJECT_TYPE_SWITCH:
            hw = hwSetSwitch(attr);
            if (hw == SAI_STATUS_SUCCESS && attr->id == SAI_SWITCH_ATTR_SRC_MAC_ADDRESS) hwCoppRefresh();
            break;
        case SAI_OBJECT_TYPE_HOSTIF_TRAP:      hw = hwCoppOnTrapSet(oid, attr); break;
        case SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP: hw = hwSetHostifTrapGroup(oid, attr); break;
        case SAI_OBJECT_TYPE_POLICER:          hw = hwSetPolicer(oid, attr); break;
...
        case SAI_OBJECT_TYPE_ROUTE_ENTRY:
            hw = hwSetRoute(serializedObjectId, attr);
            if (hw == SAI_STATUS_SUCCESS) hwCoppOnRoute(serializedObjectId);
            break;
```

`hwCoppRefresh()` is there because the ARP-reply classifier matches the switch MAC. SONiC sets SRC_MAC before copporch runs, so in practice it only matters if the MAC changes later.

### 1.5 `getStatsExt()`: trap flow counters (optional, but needed for `show flowcnt-trap stats`)

Put this after the `hwGetObjectStats` block and before `int bcm = ...`:

```cpp
    // hostif trap flow counters (SwitchBCM56860HW_copp.cpp)
    if (object_type == SAI_OBJECT_TYPE_COUNTER && m_hwReady && !(number_of_counters & VS_COUNTERS_COUNT_MSB) &&
            hwCoppCounterStats(object_id, number_of_counters, counter_ids, mode, counters))
    {
        return SAI_STATUS_SUCCESS;
    }
```

copporch binds a generic `SAI_OBJECT_TYPE_COUNTER` to every trap through `SAI_HOSTIF_TRAP_ATTR_COUNTER_ID`. syncd polls it with `FLOW_COUNTER_ID_LIST` (`sonic-sairedis/syncd/FlexCounter.cpp:70`). `PACKETS` and `BYTES` are the not-red packets of the trap's FP entries. If syncd drops the ids because `queryStatsCapability` has nothing for `SAI_OBJECT_TYPE_COUNTER`, add `SAI_COUNTER_STAT_PACKETS/BYTES` there. That override belongs to the stats agent.

### 1.6 Makefile (`sairedis/vslib.patch`, `vslib/Makefile.am` hunk)

Add two source lines next to the others:

```
 					  SwitchBCM56860HW_vxlan.cpp \
+					  SwitchBCM56860HW_copp.cpp \
...
 					  hwbcm/hwbcm_vxlan.c \
+					  hwbcm/hwbcm_copp.c
```

(Keep the backslashes consistent with whatever is last in the list.) `hwbcm_copp.c` picks up the SDK flags from `libSaiVS_a_CFLAGS`, which already applies to every `hwbcm/*.c`. It uses only `bcm_field_*`, `bcm_policer_*` and `bcm_rx_queue_max_get`. Those are in the same static libraries that are already linked (`libbcm_esw*`/`libfield`), so the link list does not change.

---

## 2. Design (and why)

### 2.1 What SONiC asks for
copp_cfg.json (`sonic-swss/tests/mock_tests/copp_cfg.json`, the same shape as the image's `copp_cfg.j2`) uses these groups:

| Group | Policer | Queue |
|---|---|---|
| `default` | 600 pps | 0 |
| `queue4_group1` (bgp, bgpv6, lacp) | none | 4 |
| `queue4_group2` (arp_req, arp_resp, neigh_discovery; action copy) | 600 pps | 4 |
| `queue4_group3` (lldp, dhcp, dhcpv6, udld, eapol) | none | 4 |
| `queue1_group1` (ip2me) | 6000 pps | 1 |
| `queue1_group3` (neighbor_miss) | 200 pps | 1 |
| `queue2_group1` (sample_packet) | 1000 pps | 2 |

All policers are sr_tcm, packets, red=drop. Note that queue4_group1 has **no** policer: BGP and LACP are prioritised, not limited. copporch's call order is: create the group (QUEUE), create the policer, set the group's POLICER, then create the traps (TRAP_TYPE, PACKET_ACTION, TRAP_GROUP, TRAP_PRIORITY). To move a trap between groups it does remove + create. A group change can also trigger `set_hostif_trap_attribute(PACKET_ACTION / TRAP_PRIORITY)`, and policer changes go through `set_policer_attribute(CIR/CBS/...)` (`copporch.cpp:495-560, 595-655, 1297-1395`).

### 2.2 Why IFP + FP meter, not CPU-queue shaping
Groups with different rates share one queue (three groups on queue 4, only one of them policed), so a per-queue shaper (`bcm_cosq_port_pps_set` on the CMIC port) cannot express the SAI model. A per-reason `CPU_COS_MAP` cannot either: LLDP and LACP both arrive as "L2 user entry CPU", and BGP and ip2me both arrive as "L3 dest = CPU". SAI's "policer on a trap group = one shared bucket for all its traps" maps directly onto one FP policer attached to all the FP entries of the group's traps. This is also how vendor SAIs on XGS implement CoPP. `bcm_rx_cos_rate_set` is a software limit in the RX thread, and with KNET the kernel does the RX, so it is not an option.

### 2.3 What is programmed
- **One IFP group**, priority **100** (`HWBCM_COPP_FP_GROUP_PRIO`), mode auto. Qualifier set: StageIngress, EtherType, DstMac, IpProtocol, L4Src/DstPort, Ttl, MyStationHit, IcmpTypeCode, DstL3Egress. The group is created lazily on the first trap. If the slice key cannot hold the full qset, it retries without DstL3Egress and then without IcmpTypeCode, and logs it.
  **For ACL:** CoPP owns IFP group priority 100. That is outside ACL's 10..40, and group priorities must be unique per stage (`include/bcm/field.h:6711`). CoPP needs one slice (two if the SDK picks double-wide). All IFP slices are looked up in parallel and conflicting actions resolve by group priority. So CoPP's CPU-copy cancel and CPU queue win over an ACL on the same packet, but an ACL `Drop` (a different action field) still applies.
- **Per SAI trap**, 1 to 6 FP entries come from a rule template (`copp_rules_for()` in `hwbcm_copp.c`), for example:
  - ARP request = 0x0806 + broadcast DA
  - ARP reply = 0x0806 + switch MAC
  - ND = ICMPv6 133..137
  - LACP / LLDP / STP / PVRST / UDLD = their DA
  - DHCP = UDP 67/68
  - DHCPv6 = UDP 546/547
  - BGP = TCP 179 (src or dst)
  - SSH / SNMP / BFD / LDP = their ports
  - IP2ME = any IPv4/IPv6
  - TTL error = MyStationHit + TTL≤1
  - OSPF / VRRP / PIM / IGMP = IP protocol

  Entry priority = `TRAP_PRIORITY*16 + specificity`. One entry per slice hits, so a higher SAI priority wins. At equal SAI priority, specific rules beat IP2ME (4), which beats TTL error (2). With SONiC's values, BGP (prio 4) is never charged to the ip2me budget (prio 1).
- **"To me" rules** (IP2ME, BGP, SSH, SNMP, BFD, LDP) also match `DstL3Egress == CPU egress of the ip2me routes`. `hwCoppOnRoute()` takes that egress from `m_routeEgr` of a route whose `NEXT_HOP_ID` is the CPU port. That is A3's shared L2TOCPU egress (`SwitchBCM56860HW_l3.cpp:38-58, 1520-1523, 1608`). Without this, transit BGP or other routed traffic would use up the CPU budget. Until the first ip2me route exists these traps are *pending*: they are stored and installed automatically by `hwbcm_copp_local_egress_set()`. BGP cannot come up before an interface IP exists, so nothing is lost.
- **Actions on each entry:**
  - `CosQCpuNew = group QUEUE`.
  - If the group has a policer, it is attached at level 0, and each colour whose SAI action is DROP gets `{Rp,Yp,Gp}CopyToCpuCancel + {Rp,Yp,Gp}SwitchToCpuCancel`. On TD2/TD2+ that pair encodes `COPY_AND_SWITCH_TO_CPU_CANCEL`, which drops the CPU copy no matter whether the switch logic (ArpRequestToCpu, L2 user entry, L2TOCPU next hop) or an FP rule made it.
  - Forwarding is untouched. A red packet of a **copy** trap (ARP) is still bridged; a red packet of a **trap** trap is gone, because the steering already drops its forwarding copy.
- **Steering is unchanged.** `hwCreateHostifTrap`/`hwbcm_trap_set` still decide what goes to the CPU, for the trap types they handle. For types that nothing steers today (EAPOL, PVRST, DHCPv6, OSPF/OSPFv6, VRRP/VRRPv6, PIM, IGMP) the entry copies to the CPU itself with `Gp/Yp/RpCopyToCpu`, leaving out the copy for a dropped colour, plus `Drop` for TRAP. The trap still reaches the CPU only once.
- **Policers.** There is one bcm policer per SAI policer, created when a trap group first references it (FP meters are per slice, so an ACL or storm-control user of the same SAI policer would need its own meter anyway).
  - Mode mapping: SR_TCM → `bcmPolicerModeSrTcm` (pkbits_sec = CIR, pkbits_burst = PBS), TR_TCM → `TrTcm`, STORM_CONTROL → `Committed`.
  - Units: packets → `BCM_POLICER_MODE_PACKETS` (pps / packets); bytes → kbits = ⌈bytes·8/1000⌉. COLOR_SOURCE BLIND → `BCM_POLICER_COLOR_BLIND`.
  - CBS = 0 (the SAI default) would mean "everything red", so the code uses CBS = CIR instead (one second of burst). This is a deliberate deviation.
  - CIR/CBS/PIR/PBS set → `bcm_policer_set`, which rewrites the meters of all attached entries. A colour action set → the users' entries are rebuilt.
- **Make-before-break.** A re-apply (group QUEUE/POLICER change, trap PRIORITY/ACTION change) installs the new entries first and then removes the old ones. The trap is never unpoliced in between, and its counters carry over.
- **Source of truth** is the vslib object hash: every apply re-reads trap → group (or the switch's DEFAULT_TRAP_GROUP) → policer, so the order of orchagent's calls does not matter. C++ keeps only `m_coppTrapHandle` (trap → C handle), `m_coppPolicer` (SAI policer → bcm id) and `m_coppLocalEgress`.
- **Errors never fail the SAI call.** orchagent aborts on hostif/policer set failures, and the steering still works without CoPP. The one exception is `hwRemovePolicer`, which returns `OBJECT_IN_USE` if the meter is somehow still attached after one re-apply.

### 2.4 SDK facts used (sdk-6.5.27)
| Claim | Where |
|---|---|
| TD2+ IFP stage uses global meter pools; a shared policer must stay in the entries' slice, so all CoPP entries go in one group | `src/bcm/esw/trident2/field.c:11231-11234`; `src/bcm/esw/field.c:17660-17732` |
| Packet-mode meters exist on TD2+ (feature inherited 56860→56850→56840_b0→56840_a0→56634_a0) | `src/soc/common/feature.c:813` and the chain `6487→2724→2152→1992→769`; checked at `src/bcm/esw/field.c:18011-18015` |
| In packet mode ckbits_sec = pps, ckbits_burst = packets | `src/bcm/esw/firebolt/meter.c:225-239, 611-619`; TD2 granularity flag `field_common.c:30210-30222` |
| srTCM: committed + peak bucket, REFRESH_MODE=1 | `src/bcm/esw/trx/field.c:15670-15672, 16166-16245` |
| Rp CopyToCpuCancel + SwitchToCpuCancel → R_COPY_TO_CPU=6 (copy **and** switch-to-CPU cancel) on TD2_TT2 | `trx/field.c:3240-3252, 8194-8206`; values `include/bcm_int/esw/field.h:5506-5508` |
| …and the pair is allowed together; Gp/Yp/RpCopyToCpu and Drop do not conflict with it | `trx/field.c:12180-12230, 12245-12265, 12331-12345, 12417-12436` |
| TD2+ is SOC_IS_TD2_TT2 | `include/soc/drv.h:3551-3554`, `include/soc/dev_macros.h:411` |
| IFP CosQCpuNew → CHANGE_CPU_COS=1, CPU_COS=q | `trx/field.c:7806-7818` |
| TD2+ IFP has every qualifier used | `src/bcm/esw/trident2plus/field.c`: DstL3Egress 365, EtherType 891, L4 ports / IcmpTypeCode 1942-1948, Ttl 2010, DstMac 2324, MyStationHit 4776 |
| Per-entry {Red,NotRed}×{pkts,bytes} = HW counter mode 3 | `trx/field.c:15409-15420` |
| Policer destroy refuses while attached | `field_common.c:7223-7224` |
| SDK installs default CPU_COS_MAP entries (INT_PRI → cos) at the bottom of the TCAM | `src/bcm/esw/cosq.c:188-260`, `rx.c:3835-3855` |

---

## 3. Not at parity (known gaps)

1. **NEIGHBOR_MISS / L3_MTU_ERROR (glean) are not policed.** Unresolved-neighbor punts use per-next-hop CPU egresses, so there is no single DstL3Egress to match. `DstPort=CPU` would match them, but it sits on the same FPF1 selector as DstL3Egress (`trident2plus/field.c:305 vs 365`, both behind `DstFwdEntitySelect`), so one group cannot have both. Fix options: a second CoPP group with a DstPort qset (another slice), or a CPU-queue shaper on the glean queue.
2. **Whether the IFP CPU_COS wins over CPU_COS_MAP is not verified on hardware.** The SDK always programs default CPU_COS_MAP entries keyed on internal priority. If the hardware gives CPU_COS_MAP precedence, the trap-group **queue** does not take effect; the **rate limit** still does. Test 4.3 checks this. The fallback is `bcm_rx_cosq_mapping_set` entries keyed on reason `FilterMatch` + int prio (coarser).
3. **No classifier** for sample_packet (sFlow), SNAT/DNAT miss, MLD, ISIS, BFD_MICRO, custom ranges, or the L2 variants of IGMP/MLD snooping. These traps get bookkeeping only, as before.
4. **Ignored attributes:** `EXCLUDE_PORT_LIST`, `MIRROR_SESSION`, trap group `ADMIN_STATE` / `OBJECT_STAGE`, policer `ENABLE_COUNTER_PACKET_ACTION_LIST`. There are no `SAI_POLICER_STAT_*` either; they could be derived from the same FP counters.
5. **ARP request/reply are told apart by DA** (broadcast vs switch MAC), because TD2+ IFP has no opcode qualifier. Replies to other router MACs (VRRP VMAC, per-RIF MACs) are not counted against `arp_resp`.
6. **All 5 IGMP trap types share one rule** (IP proto 2). If they are in different groups, the highest priority one takes all IGMP.
7. **"To me" depends on A3 keeping one shared CPU egress for ip2me routes.** If A3 ever gives each route its own CPU egress, only the last one reported matches.
8. **Removing a trap does not restore normal forwarding** of the protocol. This is pre-existing `hwbcm_trap_set` behaviour; it has no "forward" action.
9. **Counters.** For classification-only rules, the counters include matching packets that the switch did not copy. That is rare: ARP broadcasts and ND are copied anyway, and to-me rules are exact.
10. **Warm boot:** FP state is not reconciled (the whole SAI has the same limitation).
11. **CBS=0 → CBS=CIR**, as described in 2.3.

---

## 4. Hardware test plan

Diag commands go through `deploy/bsh "<cmd>"` (the hwbcm shell socket). Never run argument-less `cint/shell/bsh` (see DESIGN.md). The PC is on Ethernet44 (enp2s0, VLAN 100) as in the earlier tests. `<SW>` is the switch's VLAN 100 IP and `<SWMAC>` the router MAC.

### 4.1 Programming check (no traffic)
1. `docker exec syncd grep 'copp:' /var/log/syslog | tail -50` should show:
   - `IFP group N created (prio 100, DstL3Egress yes, IcmpTypeCode yes)`
   - one line per trap: `arp_req: 1 entry ... policer P (drop g0 y0 r1)`, `ip2me: 2 entries ...`
   - `ip2me egress -1 -> 1000xx` once the first interface IP is configured.

   If it says `DstL3Egress not in qset`, the to-me traps will stay pending. Stop and report the qset.
2. `bsh "fp show group <N>"` and `bsh "fp show entry <eid>"` for an ARP entry and the ip2me entries. Check:
   - the qualifiers (EtherType 0x0806, DstMac ff:..; DstL3Egress = the egress from step 1)
   - the actions `CosQCpuNew 4`, `RpCopyToCpuCancel`, `RpSwitchToCpuCancel`
   - the policer (mode SrTcm, packet mode, 600/600)
3. SONiC side:
   - `show copp configuration`
   - `redis-cli -n 1 keys "ASIC_STATE:SAI_OBJECT_TYPE_POLICER:*"`
   - `hgetall` of one: CIR 600 CBS 600 METER_TYPE PACKETS
   - `redis-cli -n 1 hgetall ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP:<oid>` → QUEUE / POLICER
4. Change a rate live and confirm `copp: policer P updated: cir 300` appears in syslog:
   `sonic-db-cli CONFIG_DB hset 'COPP_GROUP|queue4_group2' cir 300 cbs 300`
   Then set it back.

### 4.2 Rate limit (the main proof)
Before each run, reset the counters: `bsh "clear c"`, and note the `ip -s link show Ethernet44` RX packets inside the switch (that is the KNET netdev, i.e. what the kernel actually received).

| Test | Flood from the PC | Expected at the switch |
|---|---|---|
| ARP (queue4_group2, 600 pps, copy) | `sudo nping --arp --arp-type ARP --arp-target-ip <SW> --rate 5000 -c 50000 enp2s0.100` (or scapy `sendp(Ether(dst="ff:ff:ff:ff:ff:ff")/ARP(pdst="<SW>"), iface="enp2s0.100", loop=1, inter=0)`) | Ethernet44 netdev RX ≈ 600/s (+600 burst once). `fp stat get StatId=<sid> type=RedPackets` ≈ offered − 600/s. `show arp` still resolves |
| ip2me (queue1_group1, 6000 pps) | `sudo hping3 --udp -p 9999 --flood <SW>` or `sudo ping -f -s 64 <SW>` from 2 shells | netdev RX ≈ 6000/s; RedPackets grows for the ip2me entries; the BGP session to the PC (FRR) stays Established for the whole flood |
| BGP not charged to ip2me | during the ip2me flood, `hping3 -S -p 179 -i u500 <SW>` (2 kpps) | the BGP entry's NotRedPackets counts all of them; ip2me RedPackets does not drop |
| TTL error (default group, 600 pps) | `sudo hping3 --udp -t 1 --flood <an IP routed through the switch>` | netdev RX ≈ 600/s; RedPackets on the ttl_error entries |
| transit is not charged | route a flood **through** the switch to a host behind it (needs the 2nd cable/USB NIC, or the xe47 MAC-loopback trick from DESIGN.md) with TCP dst port 179 | the BGP and ip2me entries' counters do not move (DstL3Egress ≠ CPU egress) |

What proves the hardware does the limiting, and not the kernel or KNET:
- the drops show up as FP **RedPackets** in the ASIC,
- the CPU queue counter (`bsh "show c"` → `MC_PERQ_PKT(<q>).cpu0`) increases at ≈ CIR, not at the offered rate,
- `top` in syncd stays idle, and `/proc/bcm/knet/stats` rx drops stay 0.

Run the ARP test once with `red_action` changed to `forward` (CONFIG_DB) to show the contrast: the netdev RX then follows the offered rate. Change it back afterwards.

### 4.3 Queue selection (gap 2)
`bsh "clear c"`, then send roughly 100 packets each of:
- ARP (expect queue 4)
- ping to `<SW>` (ip2me, expect queue 1)
- LLDP (`lldpd` traffic already present, queue 4)

Then `bsh "show c cpu0"` and read the `MC_PERQ_PKT(n).cpu0` / `MC_PERQ_BYTE` lines. **Pass** = they move on 4 / 1 / 4. **Fail** = everything lands on the CPU_COS_MAP default for int-prio 0 (queue 0). In that case dump `bsh "d chg CPU_COS_MAP"` and report back; the rate limit (4.2) is still valid.

### 4.4 Lifecycle
- Restart the SONiC CoPP config (swss restart). Afterwards, `hwbcm_copp_dump` (or the syslog lines) shows the same entry count and no leaked policers: the IFP group's used-entry count in `fp show group` is unchanged.
- Move a trap between groups (edit `COPP_TRAP|arp` → `trap_group queue4_group1`). The ARP entries are rebuilt with no policer (`policer -1`) and the old 600 pps policer stays until its group is deleted.
- `config feature state` / delete a COPP_GROUP. `copp: policer P destroyed` appears, with no `BUSY`.
- `show flowcnt-trap stats` (after `counterpoll flowcnt-trap enable`) shows ARP/BGP/ip2me packet rates that match 4.2. This needs section 1.5.
