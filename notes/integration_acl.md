# Integration notes: data-plane ACL in hardware (IFP / EFP / VFP)

Owner: ACL agent, 2026-11-24. All four code files are new. No existing file was edited.

| File | What it is |
|---|---|
| `sairedis/vslib/hwbcm/hwbcm_acl.h` | SDK-free C API. It has `hwbcm_acl_rule_t`, a plain struct for one hardware entry (qualifier and action bits plus their values). It also declares the group, entry, stat and range calls. |
| `sairedis/vslib/hwbcm/hwbcm_acl.c` | The implementation, using `bcm_field_group_config_create`, qset, `bcm_field_entry_*`, `bcm_field_stat_*` and `bcm_field_range_*` |
| `sairedis/vslib/SwitchBCM56860HW_acl.cpp` | The create/set/remove handlers for the six ACL object types, the bind hooks, the counter `get` hook and the `set_acl_capabilities` override |
| `sairedis/vslib/SwitchBCM56860HW_acl.inc.h` | Member declarations and state, to `#include` inside the class body |

Syntax checks (in `saibuild`) passed with no output:
- C: the command from the brief, with `-Werror -Wextra` appended (also `-Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers`, which the SDK headers need).
- C++: `vslib_cxx_cmd.txt` plus `-fsyntax-only`, which includes `-Werror -Wconversion -Wshadow`. It ran against a copy of the current `SwitchBCM56860HW.h` with `#include "SwitchBCM56860HW_acl.inc.h"` as the last line of the class, after the stats include. That same patched header also compiles `SwitchBCM56860HW.cpp`, `_l3.cpp`, `_vxlan.cpp` and `_stats.cpp`, so the new members collide with nothing.

---

## 1. Wiring

### 1.1 Class header (`SwitchBCM56860HW.h`)

Add this as the last line inside the class, after `#include "SwitchBCM56860HW_stats.inc.h"`:

```cpp
#include "SwitchBCM56860HW_acl.inc.h"
```

The `.inc.h` file opens with `public:` and closes with `protected:`.

Also add a `get` override to the A2 section. vslib only reads ACL counters through `get()`; see 1.5.

```cpp
            virtual sai_status_t get(
                    _In_ sai_object_type_t objectType,
                    _In_ const std::string &serializedObjectId,
                    _In_ uint32_t attr_count,
                    _Out_ sai_attribute_t *attr_list) override;
```

The existing `using SwitchStateBase::get;` keeps the oid overload visible. That overload calls the string one virtually (`SwitchStateBase.cpp:615-626`), so this one override covers both entry points.

### 1.2 `create()` dispatch (`SwitchBCM56860HW.cpp`, inside `switch (object_type)`)

```cpp
        // ACL
        case SAI_OBJECT_TYPE_ACL_TABLE:              hw = hwCreateAclTable(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_ACL_ENTRY:              hw = hwCreateAclEntry(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_ACL_COUNTER:            hw = hwCreateAclCounter(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_ACL_RANGE:              hw = hwCreateAclRange(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_ACL_TABLE_GROUP:        hw = hwCreateAclTableGroup(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER: hw = hwCreateAclTableGroupMember(oid, attr_count, attr_list); break;
```

Bind attributes given at create time are optional; SONiC always binds with `set`. For completeness, add this between the end of the switch and `if (hw != SAI_STATUS_SUCCESS)`:

```cpp
    if (hw == SAI_STATUS_SUCCESS &&
        (object_type == SAI_OBJECT_TYPE_PORT || object_type == SAI_OBJECT_TYPE_LAG ||
         object_type == SAI_OBJECT_TYPE_VLAN || object_type == SAI_OBJECT_TYPE_ROUTER_INTERFACE))
    {
        hw = hwAclBindOnCreate(object_type, oid, attr_count, attr_list);
    }
```

All four types already have a `case` that `break`s, not one that returns (`hwCreatePort`, `hwCreateLag`, `hwCreateVlan`, `hwCreateRouterInterface`), so the block runs after their own hardware create. A failure is rolled back by the existing code below it.

### 1.3 `remove()` dispatch

Inside `if (m_hwReady)`, before `switch (object_type)`:

```cpp
        if (object_type == SAI_OBJECT_TYPE_PORT || object_type == SAI_OBJECT_TYPE_LAG ||
            object_type == SAI_OBJECT_TYPE_VLAN || object_type == SAI_OBJECT_TYPE_ROUTER_INTERFACE)
        {
            hwAclObjectRemoved(oid);   // drops its bindings, reprograms affected tables
        }
```

And in the switch:

```cpp
            // ACL
            case SAI_OBJECT_TYPE_ACL_TABLE:              hw = hwRemoveAclTable(oid); break;
            case SAI_OBJECT_TYPE_ACL_ENTRY:              hw = hwRemoveAclEntry(oid); break;
            case SAI_OBJECT_TYPE_ACL_COUNTER:            hw = hwRemoveAclCounter(oid); break;
            case SAI_OBJECT_TYPE_ACL_RANGE:              hw = hwRemoveAclRange(oid); break;
            case SAI_OBJECT_TYPE_ACL_TABLE_GROUP:        hw = hwRemoveAclTableGroup(oid); break;
            case SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER: hw = hwRemoveAclTableGroupMember(oid); break;
```

### 1.4 `set()` dispatch

Bind attributes must be caught before the per-type handlers. Otherwise `SAI_SWITCH_ATTR_INGRESS_ACL` reaches `hwSetSwitch` and `SAI_PORT_ATTR_INGRESS_ACL` reaches `hwSetPort`'s INFO default. Replace the existing `switch (objectType) { ... }` with:

```cpp
    if (hwAclIsBindAttr(objectType, attr->id))
    {
        // PORT/LAG/VLAN/RIF *_INGRESS_ACL/_EGRESS_ACL, SWITCH INGRESS/EGRESS/PRE_INGRESS_ACL
        hw = hwAclBind(objectType, oid, attr);
    }
    else switch (objectType)
    {
        ... existing cases unchanged ...
        case SAI_OBJECT_TYPE_ACL_ENTRY:        hw = hwSetAclEntry(oid, attr); break;
        case SAI_OBJECT_TYPE_ACL_COUNTER:      hw = hwSetAclCounter(oid, attr); break;
        default: break;
    }
```

`set()` deserialises `oid` for the switch as well (it is an object-id type), so `hwAclBind` sees `m_switch_id`. That is the bind key. The base `set` has already stored the new value when `hwSetAclEntry` runs. This is needed, because the entry is reprogrammed from the stored attribute set.

### 1.5 `get()` override (ACL counters)

syncd reads ACL counters through the **attribute** path, not `getStatsExt`:
- orchagent registers them in the `ACL_STAT_COUNTER` flex group as `SAI_ACL_COUNTER_ATTR_PACKETS`/`BYTES` (`aclorch.cpp:513-514`, `registerFlexCounter`).
- syncd's `AttrContext::collectData` then calls `m_vendorSai->get(SAI_OBJECT_TYPE_ACL_COUNTER, ...)` (`sonic-sairedis/syncd/FlexCounter.cpp:69`, `2861-2863`, `1677-1681`).
- `SwitchStateBase::get` just returns the stored value (`SwitchStateBase.cpp:628-700`). `PACKETS` is CREATE_AND_SET, not read-only, so no `refresh_read_only` hook applies.

Add to `SwitchBCM56860HW.cpp`:

```cpp
sai_status_t SwitchBCM56860HW::get(
        _In_ sai_object_type_t objectType,
        _In_ const std::string &serializedObjectId,
        _In_ uint32_t attr_count,
        _Out_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    CHECK_STATUS(SwitchBCM56850::get(objectType, serializedObjectId, attr_count, attr_list));

    if (objectType == SAI_OBJECT_TYPE_ACL_COUNTER)
    {
        return hwAclCounterGet(serializedObjectId, attr_count, attr_list);
    }

    return SAI_STATUS_SUCCESS;
}
```

Clearing (`sonic-clear acl`, or a SAI set of `PACKETS=0`) goes through `hwSetAclCounter`. It rebases a software offset and does not write to hardware.

### 1.6 LAG membership hook

Call `hwAclLagMembersChanged(lag)` when `m_lagMembers` has changed and the trunk was applied:
- `hwCreateLagMember`: after `syncLagMemberPort(lag->value.oid, port->value.oid, true);`. Use `hwAclLagMembersChanged(lag->value.oid);`.
- `hwRemoveLagMember`: after `syncLagMemberPort(lag, port, false);`. Use `hwAclLagMembersChanged(lag);`.

This is needed because a LAG binding is expanded into the member ports: InPorts on IFP, one OutPort entry per member on EFP.

### 1.7 Mirror hook (for the mirror feature)

After the mirror owner creates the bcm mirror destination for a SAI mirror session:

```cpp
hwAclMirrorSessionUpdated(session_oid, mirror_dest_gport);   // gport from bcm_mirror_destination_create
```

Before it destroys the destination, it calls the same function with `-1`. Entries that reference the session are reprogrammed:
- `MIRROR_INGRESS` becomes `bcmFieldActionMirrorIngress(0, gport)`.
- `MIRROR_EGRESS` becomes `bcmFieldActionMirrorEgress(0, gport)` (`include/bcm/field.h:7029-7040`).

Until the hook fires, a rule naming the session is installed without the mirror action and logs a WARN. Its counters still work. MirrorOrch creates the session before aclorch adds the rules, so in normal operation the order is right.

### 1.8 Makefile.am (`sairedis/vslib.patch`)

Add to `libSaiVS_a_SOURCES`, next to the other files:

```
					  SwitchBCM56860HW_acl.cpp \
...
					  hwbcm/hwbcm_acl.c \
```

`hwbcm_acl.c` needs the same `libSaiVS_a_CFLAGS` (`SDK_CFLAGS`, `-I$(srcdir)/hwbcm`) as the other `hwbcm/*.c`. No new link libraries are needed: the field module is in the SDK static libraries already linked (`bcm_esw_field_*` ships with BCM_ESW_SUPPORT).

### 1.9 Capabilities (`set_acl_capabilities` override, automatic)

The `.inc.h` overrides `SwitchStateBase::set_acl_capabilities()`, which is virtual (`SwitchStateBase.h:95`). It is called from `initialize_default_objects` (`SwitchStateBase.cpp:1864`) and warm boot (`:2184`). vslib advertised every action in both stages. The override advertises what TD2+ programs:
- **Ingress:** PACKET_ACTION, REDIRECT, COUNTER, MIRROR_INGRESS, MIRROR_EGRESS, SET_TC, SET_PACKET_COLOR, SET_OUTER_VLAN_ID/PRI, SET_DSCP, SET_ECN.
- **Egress:** PACKET_ACTION, COUNTER, MIRROR_EGRESS, SET_OUTER_VLAN_ID/PRI, SET_DSCP, SET_ECN.

With this, aclorch rejects unsupported actions up front (`queryAclActionCapability`, `aclorch.cpp` after `initDefaultTableTypes`), and they show in STATE_DB `ACL_STAGE_CAPABILITY_TABLE`. Delete the override declaration from the `.inc.h` to get the old behaviour back.

---

## 2. Design, and why

**Object mapping.**
- **SAI ACL table → one bcm field group.** Stage INGRESS → IFP, EGRESS → EFP, PRE_INGRESS → VFP. Mode is `bcmFieldGroupModeAuto`. The qset is the table's `FIELD_*` list plus what the bind point types need:
  - IFP: `InPorts` always. It sits in the IPBM part of the global mask TCAM and costs no key width (`trident2plus/field.c:172`).
  - EFP: `OutPort` if the table binds to PORT/LAG.
  - `OuterVlanId` if the table binds to VLAN/RIF.
  - The group is created at table create, so a TCAM resource error reaches orchagent at table time (`SAI_STATUS_INSUFFICIENT_RESOURCES`). This matches Broadcom SAI.
- **SAI ACL entry → N bcm entries ("variants").** Each variant is a `hwbcm_acl_rule_t`. Binding scopes are the union of every bind point whose table or group resolves to this table, in this stage:
  - **Switch bound:** 1 entry, no port qualifier.
  - **PORT/LAG bound, IFP:** 1 entry. InPorts = bound ports ∪ LAG members, intersected with the rule's own IN_PORTS if any.
  - **PORT/LAG bound, EFP:** 1 entry per port (`OutPort = p`). EFP has no out-port bitmap.
  - **VLAN bound, or RIF bound through `m_rifToVid`:** 1 entry per VLAN with `OuterVlanId = vid`.
  - **Rule OUT_PORT(S) on IFP:** expanded with `DstPort`. EFP/VFP rule IN_PORTS is expanded with `InPort`.
  - **Not bound:** **no hardware entries.** So a rule never matches traffic it was not bound to, and an idle table costs no rows.
- **Counter.** One bcm field stat (packets + bytes) per variant. SAI value = offset + Σ live stats. Retired variants and clears fold into the offset. A stat is never shared, because IFP counters are per slice and `_field_entry_stat_attach` refuses sharing across slices (`src/bcm/esw/field_common.c:10998`). Auto-expanded groups do span slices.
- **Range → `bcm_field_range_create`** (SRCPORT/DSTPORT; 32 checkers, `FP_RANGE_CHECK` index 0..31, `allmems_f.i:499428`). An entry may carry several, ANDed, which is SAI's semantics.
- **Table groups.** These are bookkeeping only. TD2+ groups always look up in parallel, and a higher group priority wins conflicting actions. That is `SAI_ACL_TABLE_GROUP_TYPE_PARALLEL`, which is what portsorch and switchorch create (`portsorch.cpp:2584-2585`, `switchorch.cpp:275-276`). A bind point may name a table or a group; switchorch binds a table directly (`switchorch.cpp:1942`).

**Always reprogram from the store.** Create and set both re-read the full stored attribute set of the entry and rebuild the variants. There is a single code path: create, set (portsorch/PFCWD `updateAclRule` changes IN_PORTS this way), bind change, group membership change, LAG change and mirror session change all go through `aclProgramEntry`.

**Hitless updates.**
- Surviving variants are rewritten in place (`hwbcm_acl_entry_update` = `qualify_clear` + `action_remove_all` + re-qualify + `reinstall`). This keeps the TCAM row and the stat.
- New variants are installed before surplus ones are removed.
- If the counter object changes, everything is done make-before-break.
- `hwbcm_acl_entry_update` validates the rule against the group qset before touching anything. If the SDK fails half-way, it `bcm_field_entry_remove`s the row, because a cleared qualifier set matches everything. C++ then recreates the row.

**Refuse rather than widen.**
- An unsupported **match** field (inner/tunnel/UDF/user-meta/BTH, TCP_FLAGS with ECE/CWR) fails the table or entry with `SAI_STATUS_NOT_SUPPORTED`. Ignoring it would turn e.g. a DROP rule into "drop more".
- An unsupported **action** (SET_POLICER, NO_NAT, DTEL, sampling, USER_TRAP_ID, ...) is ignored with a WARN, following the DESIGN.md rule.

**Field mapping notes (TD2+).**
- DSCP/ECN/TOS → `Tos`. IFP has no DSCP qualifier.
- ICMP(v6) TYPE/CODE → `IcmpTypeCode` (type<<8|code). It overlays L4SrcPort (`trident2plus/field.c:1942-1950`).
- IPV6_NEXT_HEADER → `IpProtocol`.
- ACL_IP_TYPE → `IpType`.
- TC → `IntPriority`.
- PACKET_ACTION:
  - DROP and DENY → Drop.
  - TRAP → Drop + CopyToCpu.
  - COPY and LOG → CopyToCpu.
  - COPY_CANCEL → CopyToCpuCancel.
  - DONOTDROP → DropCancel.
  - FORWARD and TRANSIT → the entry exists with no action, so it shadows lower-priority entries of its own table.
- REDIRECT:
  - port → `RedirectPort(modid, port)`.
  - LAG → `RedirectTrunk(tid)`.
  - next hop → `L3Switch(egress id)`, using `m_nhToEgr`.
  - NHG → `L3Switch(ECMP id)`, using `m_nhgToEcmp` (ECMP handled in `trx/field.c:5570-5600`).
  - bridge port → resolved to its port or LAG.
- SET_TC → `PrioIntNew`. SET_PACKET_COLOR → `DropPrecedence`.
- Entry PRIORITY → bcm entry priority 1:1. Both are "higher wins"; vslib advertises 1..16000.

---

## 3. TCAM slice and priority plan

**Group priorities.** Each value can be used only once per stage, and a higher value wins conflicting actions (`include/bcm/field.h:6711-6717`).

| Stage | Owner | Group priority |
|---|---|---|
| IFP | **CoPP** (hostif traps) | `HWBCM_COPP_FP_GROUP_PRIO = 100` (`hwbcm_copp.h:41`) |
| IFP | **ACL tables** | **10..40, reserved for ACL.** Allocated lowest-free first per table, so a later table gets the higher priority. At most 31 IFP ACL tables. |
| EFP | ACL tables | 10..40 |
| VFP | ACL tables (PRE_INGRESS) | 10..40 |

So CoPP always outranks every ACL table on a conflicting action. Nothing else in the tree creates field groups today: only `hwbcm_copp.c` and `hwbcm_acl.c` call `bcm_field_group_*`.

**Geometry (sdk-6.5.27, BCM56860_A0).**
- **IFP:** 12 slices (`trident2/field.c:11237-11242`); `FP_TCAM` has 16384 rows (`allmems_f.i:504621-504631`). Auto mode tries single, intra-slice double, double (2 paired slices) and quad (paired + intra); triple is skipped on TD2-class chips (`field_common.c:25649-25720`). Groups auto-expand into more slices as they fill (`_FP_STAGE_AUTO_EXPANSION`).
- **EFP:** 4 slices, 1024 rows (`trident2/field.c:11256-11257`, `allmems_e.i:27837`).
- **VFP:** 4 slices (`trident2/field.c:11218-11219`).

**Expected SONiC footprint.** The key-width column is my estimate from qset size; check it on hardware with `fp show`.

| Table (aclorch type on `platform=broadcom`) | Stage | Qset | Expected width |
|---|---|---|---|
| CoPP (other agent) | IFP | its own | 1 slice |
| DATAACL `L3` + mandatory range (`aclorch.cpp:2589`) | IFP | SrcIp, DstIp, L4 ports, IpProtocol, TcpControl, EtherType, OuterVlanId, IcmpTypeCode, IpType, RangeCheck, InPorts | intra-double, 1 slice |
| `L3V6` | IFP | SrcIp6, DstIp6, + the L4 set | double or quad, 2 slices |
| EVERFLOW `MIRROR`, **combined v4+v6** on brcm (`aclorch.cpp:3519-3530`, `3869-3876`) | IFP | v4 + v6 + DSCP + ranges | quad, 2 slices |
| PFCWD ingress `DROP` (TC + IN_PORTS, `aclorch.cpp:438-448`) | IFP | IntPriority, InPorts | single, 1 slice, only while a storm is active |
| PFCWD egress `PFCWD`, one table **per lossless queue** (`pfcactionhandler.cpp:385`, "EgressTable_PfcWdAclHandler_<q>") | EFP | IntPriority, OutPort | 1 EFP slice per queue. Two lossless queues = 2 of the 4 EFP slices. |

A typical config (CoPP + DATAACL + EVERFLOW + PFCWD drop) needs about 6 of the 12 IFP slices. That leaves room for an L3V6 table and auto-expansion.

---

## 4. Parity gaps (vs Broadcom SAI on XGS) and open risks

1. **Policers:** `SET_POLICER` is ignored with a WARN. There are no FP meters yet, so ACL rate limiting is missing.
2. **Not matched, so the table or entry create fails:** inner/tunnel fields, TUNNEL_VNI, UDF, ACL_USER_META (MARK_META types), BTH/AETH, TCP flags ECE/CWR (TcpControl is 6 bits, `trident2plus/field.c:1952`), PACKET_VLAN, IP_IDENTIFICATION.
3. **Ignored actions:** SAMPLEPACKET, DTEL, NO_NAT, USER_TRAP_ID, ENDPOINT_IP, REDIRECT_LIST, FLOOD, MAC/IP rewrite. EFP CopyToCpu is not advertised; it needs an internal loopback port (`trx/field.c:10502-10520`).
4. **Table ordering:** group-member PRIORITY (SONiC always sends 100) and SEQUENTIAL groups are not honoured. Table priority is creation order. FORWARD does not `DropCancel` across tables. Same-table priority works as in SAI.
5. **EFP MIRROR_EGRESS** depends on `soc_feature_egr_mirror_true` (`trx/field.c:10459-10462`). It is advertised but unverified on TD2+. A failure shows as an install error in syslog.
6. **ACL DROP vs CoPP CopyToCpu on the same packet:** two IFP groups hit and both actions apply. Whether an IFP Drop also suppresses the CPU copy has to be checked on hardware (test 5.2). With Broadcom SAI, DATAACL deny rules are known to catch control-plane packets too.
7. **VLAN/RIF binding** relies on IFP `OuterVlanId` seeing the PVID-assigned VLAN for untagged frames. It is unverified (test 5.4). RIF binding works only for RIFs that A3 put on a VLAN (`m_rifToVid`).
8. **EFP keys:** an egress table mixing IPv4 and IPv6 address fields may not get a key (EFP KEY1/KEY2/KEY3 selection), and group create returns BCM_E_RESOURCE. aclorch's egress tables (PFCWD) do not do this.
9. **Variants multiply TCAM use:** a port-bound egress table uses one EFP row per bound port per rule. A rule with OUT_PORTS on IFP uses one row per out port. The cap is 256 variants per rule (`ACL_MAX_VARIANTS`).
10. **CRM:** `AVAILABLE_ACL_TABLE`/`ENTRY`/`COUNTER` still come from vslib's static numbers (`m_maxAclTables = 3`, `SwitchStateBase.h:753-756`), not from the TCAM. `hwbcm_acl_group_status()` (`bcm_field_group_status_get`) is there to wire a real `AVAILABLE_ACL_ENTRY` later.
11. **Warm boot:** there is no reconciliation of FP state. A cold syncd start rebuilds everything from the SAI replay.
12. **A redirect to a next hop** captures the egress id at program time. A3 rewrites egresses in place (`hwbcm_l3_egress_replace`), so neighbour changes are followed. A next hop that is removed and re-created needs the rule re-set, which aclorch does.

---

## 5. Hardware test plan

Shell commands for the switch go through `deploy/bsh "<diag cmd>"`. Wherever below says "the hwbcm shell socket", that means this.

### 5.0 Before and after the build

```
bsh "fp show"                       # empty except the CoPP group before any ACL
show acl table ; show acl rule
redis-cli -n 6 HGETALL "ACL_STAGE_CAPABILITY_TABLE|INGRESS"    # shows the narrowed action list (1.9)
```

### 5.1 Table + rule via CONFIG_DB

Use CONFIG_DB directly rather than `acl-loader`. acl-loader appends a priority-1 `DEFAULT_RULE` deny for L3 tables, and that would also cut the PC's management/BGP traffic on Ethernet44.

`/tmp/acl_test.json`:

```json
{
  "ACL_TABLE": {
    "TESTV4": { "type": "L3", "stage": "ingress", "ports": ["Ethernet44"], "policy_desc": "hw acl test" }
  },
  "ACL_RULE": {
    "TESTV4|DROP_ICMP_TO_SW": {
      "PRIORITY": "200", "IP_PROTOCOL": "1", "DST_IP": "<switch Vlan100/RIF IP>/32",
      "PACKET_ACTION": "DROP"
    },
    "TESTV4|COUNT_UDP_RANGE": {
      "PRIORITY": "150", "IP_PROTOCOL": "17", "L4_DST_PORT_RANGE": "5000-5010",
      "PACKET_ACTION": "FORWARD"
    }
  }
}
```

```
sudo sonic-cfggen -j /tmp/acl_test.json --write-to-db
show acl rule                       # Status Active
bsh "fp show"                       # new IFP group, priority 10, mode, slice
bsh "fp show entry <eid>"           # InPorts = only Ethernet44's bcm port (see the syncd log "ACL ... bind"), qualifiers, Drop, stat
```

**Prove the drop and the counter:**
1. On the PC, run `ping -c 20 <switch IP>`. Expect 100 % loss.
2. On the switch, run `aclshow -a`. `DROP_ICMP_TO_SW` should read 20 packets and 20×98 bytes.
3. Run `sonic-clear acl`, then `aclshow -a`. Expect 0. Run another `ping -c 5`; expect 5.
4. Run `nping --udp -p 5005 -c 10 <switch IP>` and `-p 6000`. `COUNT_UDP_RANGE` should count 10 only for the in-range ports, which proves the range checker.
5. Delete the rule with `sudo redis-cli -n 4 DEL "ACL_RULE|TESTV4|DROP_ICMP_TO_SW"`. Ping comes back, and `fp show` loses that entry.

### 5.2 ACL drop vs CoPP (risk 6)

Add a rule `DROP` on `IP_PROTOCOL 6, L4_DST_PORT 179` from the PC's BGP peer. Check whether the BGP session drops. Also check `bsh "show c cpu0"` / KNET rx counters: does the IFP Drop suppress the CoPP copy? Record the result in DESIGN.md.

### 5.3 Transit traffic, port binding vs LAG binding

Use the single-cable method from DESIGN.md: Ethernet48 in `lb=mac` acts as the access host. Route a test prefix to a static neighbour on Ethernet48.
- Bind `TESTV4` to Ethernet44 and drop `DST_IP <test prefix>`. Ethernet48 TX counters (`show interfaces counters`) stop, and the ACL counter equals the sent packets.
- If a PortChannel exists: `config acl` table `ports` = `PortChannel01`. `fp show entry` InPorts = all member ports. Remove a member with `config portchannel member del`: the InPorts bitmap shrinks. That proves hook 1.6 with no traffic gap; ping keeps running.

### 5.4 VLAN binding (risk 7)

`"ports": ["Vlan100"]` binds to the VLAN, so `fp show entry` shows `OuterVlanId 100`. Ping from the PC's untagged and tagged (`enp2s0.100`) interfaces. Both must hit the counter.

### 5.5 Egress (EFP)

```json
"ACL_TABLE": { "TESTEGR": { "type": "L3", "stage": "egress", "ports": ["Ethernet48"] } },
"ACL_RULE":  { "TESTEGR|DROP_DST": { "PRIORITY": "100", "DST_IP": "<test prefix>", "PACKET_ACTION": "DROP" } }
```

`fp show` shows an EFP group with `OutPort` = Ethernet48's bcm port. Transit traffic from 5.3 stops leaving Ethernet48, and the ACL counter counts it. (CPU-originated packets may bypass EFP under KNET tx, so prove EFP with transit traffic.)

### 5.6 PFCWD drop path (needs PFC enabled by the QoS feature on a lossless queue)

```
pfcwd start --action drop ports Ethernet44 detection-time 200 --restoration-time 2000
redis-cli -n 2 HSET "COUNTERS:<queue 3 oid of Ethernet44>" DEBUG_STORM enabled
show pfcwd stats ; show acl table     # IngressTable_PfcWdAclHandler (DROP, ingress) + EgressTable_PfcWdAclHandler_3 (egress)
bsh "fp show"                         # IFP group with IntPriority+InPorts, EFP group with IntPriority+OutPort
redis-cli -n 2 HDEL "COUNTERS:<oid>" DEBUG_STORM      # restoration removes the rules
```

### 5.7 Mirror (after the mirror feature has wired 1.7)

```
config mirror_session add ev0 <src ip> <dst ip> 8 64
config acl add table EVERFLOW MIRROR -p Ethernet44
```

Add a rule `MIRROR_ACTION: ev0` (the CONFIG_DB key `"MIRROR_ACTION": "ev0"`). `fp show entry` shows `MirrorIngress` with the mirror gport. Before the session has a destination, syslog shows the WARN "mirror session ... has no hardware destination yet" and the rule still counts.

### 5.8 Negative cases

All of these must fail cleanly: an error in syslog, no rule in `fp show`, and syncd stays up.
- A custom table type with `INNER_SRC_IP` returns NOT_SUPPORTED at table create.
- A rule with `TCP_FLAGS: 0x40/0x40` returns NOT_SUPPORTED.
- Creating 32 IFP ACL tables: the 32nd returns INSUFFICIENT_RESOURCES (priority range exhausted) or BCM_E_RESOURCE (slices exhausted), whichever comes first.
