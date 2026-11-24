# Task for agy: wire CoPP, then QoS/buffers, into the SAI (one session, in this order)

Do the two tasks below one after the other in this session:
1. `notes/agy_task_copp_wiring.md` — finish it completely (build passes, `notes/agy_report_copp.md` written).
2. `notes/agy_task_qos_wiring.md` — on top of the CoPP result (build passes, `notes/agy_report_qos.md` written).

Read both task files fully first; all their rules apply (no switch access, no install, no sync_from_sairedis.sh,
no git commit/push). Keep the ACL and stats wiring that is already in SwitchBCM56860HW.h/.cpp intact.
The final build must end with `==> ALL CHECKS PASSED`; put its sha256 at the end of `notes/agy_report_qos.md`.
