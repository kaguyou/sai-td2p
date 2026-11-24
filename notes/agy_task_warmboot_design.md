# Task for agy: design warm reboot support for the custom SAI (research + design only, no code changes)

Repo `/home/kaguyou/homelab/sai-td2p`: custom SAI for SONiC 202505 on Edgecore AS5812-54T (Broadcom Trident2+, OpenBCM sdk-6.5.27).
It is vslib (`/home/kaguyou/homelab/sonic-sairedis/vslib`, SwitchStateBase + WarmBootState) plus a hardware layer
`sairedis/vslib/SwitchBCM56860HW*.cpp` and `sairedis/vslib/hwbcm/*.c` that programs the ASIC through the SDK.
Today a syncd restart re-inits the chip (cold): traffic stops for the ~2-3 minutes of `init soc`/`init bcm` and re-programming.

Goal: what is needed so that SONiC `warm-reboot` / `config warm_restart` (syncd warm restart) keeps forwarding,
at parity with what Broadcom's SAI offers on XGS. Write `notes/design_warmboot.md` covering:
1. How SONiC drives it (sonic-sairedis syncd: SAI_SWITCH_ATTR_RESTART_WARM, SAI_KEY_WARM_BOOT_READ_FILE/WRITE_FILE,
   pre-shutdown, the comparison logic / ASIC_DB reconciliation; sonic-swss warm restart). Cite file:line in
   `/home/kaguyou/homelab/sonic-sairedis` and `/home/kaguyou/homelab/sonic-swss`.
2. What vslib already does for warm boot (SwitchStateBase warm_boot_initialize_objects, WarmBootState, the vs dump/restore files) and
   what our SwitchBCM56860HW does in warm_boot_initialize_objects today.
3. SDK side: OpenBCM warm boot (BCM_WARM_BOOT_SUPPORT, scache, `soc_warm_boot_set`, `bcm_switch_control_set(bcmSwitchControlSync)`,
   `stable_location`/`stable_size` config, `bcm_init` in warm mode, KNET netifs surviving) — which pieces exist in sdk-6.5.27 for
   BCM56860 (`/home/kaguyou/homelab/openbcm-src/sdk-6.5.27`), cite file:line, and whether our build has BCM_WARM_BOOT_SUPPORT compiled in
   (check `sdk_cflags.txt`).
4. Our hwbcm state that must be rebuilt after a warm start (the C static tables: VXLAN VPs, CoPP entries, ACL groups, QoS maps,
   KNET slots, hash, station ids …) and how to rebuild each (from SDK traverse APIs or from the SAI object store).
5. A concrete step-by-step implementation plan with effort estimates, risks, and a hardware test plan.
No code changes, no builds, no switch access (no ssh/scp to 192.168.52.1/192.168.0.2, no /dev/ttyUSB0), no git commit.
