# agy task: SAI parity audit (what separates this SAI from a production Broadcom SAI on TD2+)

Repo: ~/homelab/sai-td2p-merge, branch `main` (4342e72). Read-only task: DO NOT change code, DO NOT build,
DO NOT push, DO NOT ssh to the switch. Write only notes/agy_report_sai_parity_audit.md and commit it on a new
branch `parity-audit`.
Sources: sairedis/vslib/SwitchBCM56860HW*.cpp|h, sairedis/vslib/hwbcm/*.c|h (our SAI), ~/homelab/sonic-swss
(orchagent = the caller), ~/homelab/sonic-sairedis (SAI headers under SAI/inc, vslib base classes
SwitchStateBase*), ~/homelab/openbcm-src/sdk-6.5.27-wb (SDK). The owner's bar is "on par with the vendor SAI".

## Deliver
1. Coverage table. For every SAI object type and attribute that SONiC 202505 orchagent/syncd can create, set,
   remove or get (grep the sai_*_api-> calls and attribute ids in sonic-swss/orchagent, including bulk APIs,
   stats, capability queries sai_query_attribute_capability / sai_object_type_get_availability, notifications):
   state for each whether our SAI (a) programs hardware, (b) only stores it in the vslib object DB without
   hardware effect (silent no-op), (c) returns an error, (d) is never reached. File:line for both the caller
   and our handler. Group by feature: port (speed/AN/FEC/MTU/breakout/admin/PFC/flow control/serdes),
   LAG, VLAN/bridge/FDB (ageing, flush, learn limits, move), STP, L3 (RIF types, neighbor incl. age-out and
   resolution changes, next hop, NHG/ECMP incl. member weight and resilient hashing, routes v4/v6, VRF, MTU
   check, uRPF), IPv6 (ND, link-local, host routes), tunnels (VXLAN L2/L3, IPinIP decap), ACL (stages, match
   fields, actions, counters, ranges, table/entry capacity), mirror (SPAN/ERSPAN), CoPP/hostif (trap types,
   policers, genetlink/sflow), QoS (maps, scheduler groups, WRED, buffers, PFC/PFC watchdog, headroom),
   counters (port/queue/PG/RIF/ACL/tunnel/watermarks, flex counter groups), switch attributes, CRM
   availability, warm boot coverage of each feature, notifications (port state, FDB event, queue deadlock,
   BFD/NAT if reachable).
2. For every (b) silent no-op and every unhandled attribute: what a vendor SAI on Trident2+ does (name the
   bcm_* API), the user-visible consequence, effort (S/M/L), and whether it can be tested with: one switch,
   two 1G copper links to one PC (one of them the switch's own mgmt port in a netns), no 40G, no second switch.
3. Robustness review of our code: unchecked bcm_* return values, error paths that leave the SAI DB and
   hardware out of sync, resource leaks on remove (egress objects, L3 interfaces, VPs, FP entries, KNET
   filters), behaviour when a hardware table is full (L3_DEFIP, L3_ENTRY, L2, FP, ECMP), thread-safety between
   the SAI API thread, link-scan / L2 callbacks and the counter thread, anything that can crash syncd
   (NULL derefs like the one fixed in dcbbfbe). File:line and severity.
4. A prioritised work list (top 25) to reach vendor parity, each item: what, why, effort, how to verify on the
   hardware above. Separate list: items that need more hardware.
Be concrete and complete; no general advice. If you could not verify something by reading code, say so.
