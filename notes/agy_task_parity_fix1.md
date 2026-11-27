# agy task: parity fix batch 1 (crash / hang / DB-HW desync findings of the parity audit)

Repo: ~/homelab/sai-td2p-merge, branch `main`. Work on a new branch `parity-fix-1` from it.
Build tree: ~/homelab/sonic-sairedis-merge (copy with `cp -a sairedis/vslib/. ../sonic-sairedis-merge/vslib/`
and check `diff -rq` shows no differing source file BEFORE building; DO NOT run sairedis/sync_from_sairedis.sh).
SDK: ~/homelab/openbcm-src/sdk-6.5.27-wb (patches tracked in deploy/sdk-wb-patches). If you change SDK source,
rebuild the SDK (`podman run --rm -v ~/homelab/openbcm-src/sdk-6.5.27-wb:/src/sdk-6.5.27-wb:z localhost/sdkbuild-img:ready make -C /src/sdk-6.5.27-wb/systems/linux/user/x86-smp_generic_64-2_6 -j12 bcm`)
and delete sonic-sairedis-merge/vslib/libsaivs.la and .libs/libsaivs.so* before build_wb.sh.
Build: `podman start saibuild; SAI_TD2P_DIR=$PWD SAIREDIS_DIR=~/homelab/sonic-sairedis-merge ./deploy/build_wb.sh --no-meta --force-archive`.
Do not push. Do not ssh to the switch and do not touch /dev/ttyUSB0, the lead tests there.

## Input
The audit reports are in notes/parity-audit/domain{1..6}_part*.md (each finding has file:line, mechanism, fix).
The audit was static: verify every finding against the code before fixing. If a finding is wrong, say so in the
report with the reason and skip it. Line numbers may be a few lines off.

## Scope of this batch (in this order, one commit per item, build must pass after each group)
A. Crash / hang
 1. domain6 5.1/6.1: self-deadlock on m_mapMutex in hwCreateRouterInterface / hwRemoveRouterInterface (LAG RIF).
    Lead confirmed it at SwitchBCM56860HW_l3.cpp:520-528. Check every other m_mapMutex holder for the same pattern.
 2. domain6 5.2/6.2: diag shell socket thread (SIGPIPE, dup2 of fd 1/2, no lock around hwbcm_vxlan_* / SDK calls,
    accept() on closed fd). Keep deploy/bsh working.
 3. domain5 HIGH 3: stack buffer overflow in VirtualSwitchSaiInterface::clearStats.
 4. domain6 6.3, 6.4 (= domain4 R4): negative vpn_id, qos_cells()/qos_kbits() overflow.
 5. domain3 HIGH 7, domain6 6.5/6.6: NULL list pointers / unchecked getAttr.
B. Error mapping and rollback
 6. domain6 4.1: one helper that maps BCM_E_* to SAI status (BCM_E_FULL -> SAI_STATUS_TABLE_FULL,
    BCM_E_RESOURCE/MEMORY -> SAI_STATUS_INSUFFICIENT_RESOURCES, BCM_E_EXISTS -> ITEM_ALREADY_EXISTS,
    BCM_E_NOT_FOUND -> ITEM_NOT_FOUND, BCM_E_PARAM -> INVALID_PARAMETER, BCM_E_UNAVAIL -> NOT_SUPPORTED), used by
    route / neighbor / next hop / NHG / FDB / VXLAN / ACL paths. The hwbcm C layer must pass the bcm rv up.
 7. domain6 2.1: SwitchBCM56860HW::set() rolls m_objectHash back when hwSet* fails.
 8. domain6 2.2, 2.3: create() rollback calls hwRemove*; remove() does not strip ACLs before hwRemove* succeeds.
 9. domain6 2.4, 2.5 (+ domain1 HIGH 6, domain2 HIGH 9, 13): shadow maps changed before the HW call.
10. domain6 2.6 (= domain2 HIGH 4): NEIGHBOR_ENTRY missing in set() dispatch.
C. Wrong forwarding
11. domain6 3.1 (= domain2 Critical 2): port-RIF VLAN pool 3000..3999 vs user VLANs.
12. domain2 Critical 3: TUNNEL_ENCAP / MPLS / SRV6 next hop returns success with no egress (return NOT_SUPPORTED
    unless it is the VXLAN L3 case that already works).
13. domain2 HIGH 5 (= domain1 HIGH 3): MAC move events dropped.
14. domain2 HIGH 11, 12: last ECMP member, resolveVrf() fallback to VRF 0.
15. domain3 HIGH 3, 5: unmapped ACL_IP_TYPE widens the rule; CoPP OSPF/VRRP/PIM/IGMP/DHCPv6 rules match transit.
16. domain4 R3, R6: false PFC_DLR_INIT capability; ECN-only WRED profile drops non-ECT.
D. Threads
17. domain6 5.3, 5.4 (= domain1 HIGH 1, 7): data races between bcmLINK / bcmL2X threads and the SAI thread.
18. domain6 1.1-1.7: unchecked returns (log + propagate where a caller can act on it).

Not in this batch (leave them, list them at the end of the report as "next"): everything Medium/Low, the
coverage-table gaps (unimplemented attributes), domain5 HIGH 1, 2, 4, domain4 R1, R2, domain2 HIGH 6, 7, 8,
domain3 HIGH 1, 2, 6, 8.

## Rules
- The cold path and the warm boot path that pass today must not change behaviour except for the fixes.
  Anything saved in .hwstate that changes format needs a version check.
- No hardware write may be added to warm init.
- If time or quota runs short, stop at a group boundary with a passing build and write the report.

## Report
notes/agy_report_parity_fix1.md: per item = fixed / not a bug (why) / deferred (why), commit hash, and for each
fixed item an exact on-box test (CONFIG_DB or CLI commands, what to expect before and after). sha256 of
deploy/build/libsaivs.so.0.0.0. Short.
