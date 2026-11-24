# Task for agy: first hardware result of warm restart — link drop during warm init. Find and fix (isolated trees, same rules)

Test (lead, 2026-11-24 13:12-13:15, lib = deploy/build/libsaivs.so.wb-phase2, sha b4ca0027…):
- PC pinged a host behind the switch every 100 ms (PC on Ethernet44 = bcm port 45 = ge1, 1G copper via the 10GBASE-T PHY BCM84858
  in SGMII mode; host on Ethernet39 = port 40). `config warm_restart enable syncd; systemctl restart syncd` at ~13:13:02 (stop done 13:13:13).
- syncd log: "wb knet: recovered 72 netif(s) and 216 filter(s)", "wb hwstate: restored ... rifs=67 ... routes=142",
  "wb: true warm boot completed (zero ASIC resets, zero port flaps)", "wb sgmii: port 45 already UP at 1000 Mbps (SGMII), marked applied" at 13:13:58.
- Data plane: ZERO loss for ~22 s after the trigger (seq 1-270 all replied), then the PC NIC saw LINK DOWN (~13:13:24-27, i.e. during the
  warm INIT, before 13:13:58) and nothing came back; switch kept showing Ethernet44 up/up. After `nmcli con up` the PC had carrier but
  received nothing (PC tx ARP reached the switch CPU: Ethernet44 netdev rx +17; switch->PC ping 0/3). A COLD `systemctl restart swss`
  restored everything.
Hypotheses to check in the SDK (sdk-6.5.27-wb) and in hwbcm warm init: PHY firmware download / PHY reset of the BCM848xx (phy8481 / phymod)
during soc_init/bcm_init/port init even in warm mode; port probe/attach (`bcm_port_probe`, `soc_phyctrl_init`), `bcm_linkscan` init,
SerDes/XFI/SGMII re-init (`portmod` warm), KNET TX DMA channel not restarted after `bcm_tx_cleanup`/soc_shutdown (CPU TX broken).
Find the real cause with file:line evidence, fix it in sai-td2p-wb (keep cold boot identical), add warm-path logging that would prove it on hw
(e.g. log every PHY/port init step that touches hardware in warm mode), build with build_wb.sh, commit on `warmboot`, and write
`notes/agy_report_warmboot_fix1.md` with the exact retest steps. Same DO NOTs (no switch, no original trees, no push).
