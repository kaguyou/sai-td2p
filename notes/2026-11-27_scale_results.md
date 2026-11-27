# Route scale and table-full behaviour, lib b7db1af7 (before parity-fix-1), 2026-11-27

Method: tools/scale/rs3.sh puts blackhole /24 routes straight into APPL_DB ROUTE_TABLE with swssconfig (no FRR),
waits until ASIC_DB stops changing, then reads the ASIC with the diag shell (`l3 info`, tools/scale/bshl.sh).
Console only (PC cable was not connected), cold boot, l3_alpm_enable=2.

| step | result |
|---|---|
| add 10,000 | 9.1 s = about 1,100 routes/s, no error |
| delete 10,000 | 8.2 s = about 1,200 routes/s |
| add 20,000 | 20.0 s = about 1,000 routes/s |
| up to 70,000 | all in the ASIC (`l3 defip show` 70,131 lines) |
| up to 130,000 | all in the ASIC (`routes: 130134`), no error |
| add 150,000 more | table full at 131,149 routes in the ASIC (first failure at index about 130,150) |
| delete everything (131,016) | 163 s = about 800 routes/s, ASIC back to 134 routes, nexthops 7 (unchanged) |
| add / delete 1,000 after that | 1,120 / 1,107 routes/s, works |

- Yesterday's 90 routes/s was vtysh/FRR static-route input, not the SAI.
- Table full today: the SAI returns SAI_STATUS_FAILURE, orchagent logs `EntityBulker.flush create entries failed ...
  SAI_STATUS_FAILURE` + `handleSaiFailure` for every route (6,400 ERR lines). syncd and orchagent stay up, nothing
  crashes, deleting recovers. Expected after parity-fix-1 item 6: SAI_STATUS_TABLE_FULL.
- `l3 info` prints `routes: N / 98304`; the real limit for /24 with ALPM was 131,149.
- syncd RSS 162 MB -> 358 MB at 131k routes and stays 358 MB after delete (not re-tested for growth on a second round).
- orchagent RSS 160 MB at peak, 91 MB after delete.
- Boot log, to look at later: `meta_sai_on_port_state_change_single: data.port_id ... has unexpected type:
  SAI_OBJECT_TYPE_TUNNEL` once at boot, and create SWITCH takes 37 s (syncd watchdog warns at 30 s).

## IPv6 routes (/64, blackhole), tools/scale/rs4.sh v6
- first 10,000: all in the ASIC quickly (rate line lost, under the 5 s settle window).
- after about 29,000 the add rate falls to 36/s, then 21/s; perf: `_soc_alpm_merge_cb`, `_trie_postorder_traverse2`
  (SDK ALPM bucket merge on every insert when the table is nearly full). Full at 32,550 routes, then
  SAI_STATUS_FAILURE per route. Delete 32,550: 52.6 s = about 620/s, ASIC back to 134.
- every route add/delete writes one INFO line from hwbcm (`l3: route vrf=...`) to syslog AND to supervisord stdout;
  rsyslog starts dropping (`begin to drop messages due to rate-limiting`). Should be DEBUG.

## IPv4 neighbors on a routed port, tools/scale/rs4.sh nbr
- add 10,000 (neighbor + next hop each): 339 s = about 30 neighbors/s, syncd at 100 % CPU.
  perf: getStoredAttr, std::map<string> walks, sai_serialize_*. Cause read in the code:
  SwitchBCM56860HW::hwCreateNeighbor (SwitchBCM56860HW_l3.cpp, "reuse the one a NEXT_HOP already created") loops over
  ALL of m_nhToEgr with two getStoredAttr and two sai_serialize_ip_address per entry = O(N) per neighbor, O(N^2) total.
  The same kind of full scan is in the FDB-event paths (all neighbors per FDB event, around l3.cpp 1728, 1804, 1998).
- host table full at 14,028 of 16,384 (hash collisions), then SAI_STATUS_FAILURE per neighbor, nothing crashes.
- delete all: 157 s = about 180/s, ASIC back to hosts 3 / nexthops 7.

## Other
- CPU cores and DIMMs reach 50-54 C under these tests with the fan fixed at 20 % (board sensors 32-39 C).
- PC cable is now on Ethernet24 (moved into Vlan100 untagged, 10.0.0.48/31 removed, not saved), eth0 <-> Ethernet35.
