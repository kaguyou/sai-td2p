# agy task: parity fix batch 2 (scale / performance + the remaining High audit findings)

Repo: ~/homelab/sai-td2p-merge. Work on a new branch `parity-fix-2` from `parity-fix-1` (b6098eb or later).
Same build rules as notes/agy_task_parity_fix1.md (copy vslib to ~/homelab/sonic-sairedis-merge, `diff -rq`,
build_wb.sh, SDK rebuild rule). Do not push. Do not ssh to the switch and do not touch /dev/ttyUSB0.
The lead is testing parity-fix-1 on the switch meanwhile and may add fix commits on `parity-fix-1`; do not rebase,
the lead merges.

## A. Scale / performance (measured on the switch, notes/2026-11-27_scale_results.md)
 1. Neighbor create is O(N): 10,000 neighbors take 339 s (30/s), syncd 100 % CPU.
    SwitchBCM56860HW::hwCreateNeighbor loops over all of m_nhToEgr with getStoredAttr + sai_serialize_ip_address
    per entry to find next hops with the same (rif, ip). Add an index keyed by (rif oid, ip) -> next hop oids,
    kept in step with next hop create/remove (and rebuilt on warm restore, it is derived state: do not save it).
    Target: neighbor add and remove cost independent of N.
 2. Find and fix every other per-operation full scan on the hot paths (route, neighbor, next hop, NHG member,
    FDB event, VXLAN retry): at least the FDB-event paths that walk all NEIGHBOR_ENTRY objects (l3.cpp around
    1728, 1804, 1998 on main), hwRemoveNeighbor, hwCreateNextHop/hwRemoveNextHop, hwVxlanRetryPending being called
    on every neighbor / route change (make it a cheap no-op when no remote VTEP is pending or affected),
    anything that iterates m_objectHash[...] per call. List what you found with the cost before / after.
 3. hwbcm logs one INFO line per route / host / egress add and delete (hwbcm_l3.c `l3: route vrf=...` etc.), and
    it goes to syslog and to supervisord stdout; rsyslog rate-limits and drops. Make per-object success logs
    DEBUG (errors stay ERR/WARN). Check the other hwbcm_*.c files for the same per-object INFO logs.
 4. Delete rates are below add rates (IPv4 routes 800/s vs 1,100/s add; neighbors 180/s): look for the reason
    in our layer (not the SDK) and fix what is ours.
 Do not change SDK ALPM behaviour (the IPv6 slow-down near table full is the SDK's bucket merge).

## B. Remaining High findings of the audit (notes/parity-audit/, verify each against the code first)
 5. domain5 HIGH 1: dynamic FDB m_fdb_info_set lost across warm boot (aging, neighbor punt, MAC move).
 6. domain5 HIGH 2: _soc_trident2_misc_init runs on warm boot and rewrites live registers. Verify on the code
    what is really written on warm init today (the warm path was measured loss-free: 1771/1771 ping across a
    kexec warm-reboot, so be exact about which registers/memories and whether the value differs from cold).
 7. domain5 HIGH 4: hwColdFallbackReplay ordering and silent failures.
 8. domain2 HIGH 6, 7, 8: VXLAN underlay re-evaluation on route/neighbor/ECMP removal, underlay lookup ignores
    VRF, VXLAN over LAG (underlay and access).
 9. domain3 HIGH 1, 2, 6, 8: hostif trap removal / FORWARD leaves CPU steering, shared HWBCM_TRAP_* buckets,
    CoPP g_have_stats disabled forever on one failure, CoPP / policer errors swallowed.
10. domain4 R1, R2: buffer pool watermark clear-on-read, egress UC-only service pool watermarks read 0.

One commit per item. If time or quota runs short, stop at an item boundary with a passing build.

## Rules
- Cold path and warm boot path that pass today must not change behaviour except for the fixes.
- .hwstate format change needs a version check and must still load a file written by parity-fix-1.
- No hardware write may be added to warm init.
- A call that returns success during a normal SONiC boot today must not start returning an error.

## Report
notes/agy_report_parity_fix2.md: per item = fixed / not a bug (why) / deferred (why), commit, on-box test with
expected before / after (for A: which tools/scale command and the expected rate). sha256 of the library. Short.
