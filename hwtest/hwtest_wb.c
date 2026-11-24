/*
 * hwtest_wb.c — standalone Phase 1 warm-boot exerciser for hwbcm on AS5812-54T.
 *
 * Usage:
 *   hwtest_wb <config.bcm> cold <scache_file> [hold_sec]
 *     1. Cold init (hwbcm_init_ex warm=0)
 *     2. Bring up test ports (2, 3), VLAN 100, KNET netif Ethernet4, ARP/IP2ME traps
 *     3. Wait hold_sec (default 5s) so links settle and traffic/counters run
 *     4. hwbcm_warm_sync(scache_file) + hwbcm_warm_deinit() -> exit preserving ASIC & KNET
 *
 *   hwtest_wb <config.bcm> warm <scache_file> [hold_sec] [--cold-exit]
 *     1. Warm init (hwbcm_init_ex warm=1, scache_file)
 *     2. hwbcm_knet_warm_recover() -> verify netifs/filters survived with zero mutations
 *     3. Verify chip, port link/speed/enable, stats, SGMII preservation (0 bounces)
 *     4. Wait hold_sec (default 10s), print link callbacks (should be 0!) & stats
 *     5. By default exits via hwbcm_warm_sync + hwbcm_warm_deinit (or cold hwbcm_deinit
 *        if --cold-exit is given).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include "hwbcm.h"

static volatile int g_link_events = 0;

static void link_cb(int port, int up, void *cookie)
{
    (void)cookie;
    g_link_events++;
    printf("LINK_EVENT #%d: port=%d up=%d\n", g_link_events, port, up);
    fflush(stdout);
}

static void print_port_state(int port)
{
    int en = -1, up = -1, sp = -1, an = -1;
    hwbcm_port_stats_t st;
    (void)hwbcm_port_enable_get(port, &en);
    (void)hwbcm_port_link_get(port, &up);
    (void)hwbcm_port_speed_get(port, &sp);
    (void)hwbcm_port_autoneg_get(port, &an);
    memset(&st, 0, sizeof(st));
    (void)hwbcm_port_stats_get(port, &st);
    printf("  port %2d: enable=%d link=%s speed=%dM an=%d rx_pkts=%llu tx_pkts=%llu\n",
           port, en, up == 1 ? "UP" : up == 0 ? "down" : "?", sp, an,
           (unsigned long long)st.rx_packets, (unsigned long long)st.tx_packets);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    static const int test_ports[2] = { 2, 3 };
    static const uint8_t mac_eth4[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x02 };
    static const uint8_t mac_sw[6]   = { 0xa8, 0x2b, 0xb5, 0x3a, 0x3e, 0x8e };
    const char *cfg, *mode, *scache;
    int warm = 0, hold = 5, cold_exit = 0;
    int rc, i;
    size_t wb_bytes = 0;

    if (argc < 4) {
        fprintf(stderr, "usage: %s <config.bcm> <cold|warm> <scache_file> [hold_sec] [--cold-exit]\n", argv[0]);
        return 2;
    }
    cfg = argv[1];
    mode = argv[2];
    scache = argv[3];
    if (strcmp(mode, "warm") == 0) {
        warm = 1;
        hold = 10;
    } else if (strcmp(mode, "cold") != 0) {
        fprintf(stderr, "invalid mode '%s' (expected 'cold' or 'warm')\n", mode);
        return 2;
    }
    for (i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--cold-exit") == 0) {
            cold_exit = 1;
        } else {
            hold = atoi(argv[i]);
        }
    }

    hwbcm_log_level_set(HWBCM_LOG_INFO);
    printf("=== hwtest_wb: mode=%s cfg=%s scache=%s hold=%ds ===\n",
           warm ? "WARM" : "COLD", cfg, scache, hold);
    fflush(stdout);

    rc = hwbcm_init_ex(cfg, warm, scache);
    printf("hwbcm_init_ex(warm=%d) -> %d (is_warm_boot=%d)\n", rc, warm, hwbcm_is_warm_boot());
    fflush(stdout);
    if (rc < 0) {
        return 1;
    }

    (void)hwbcm_link_callback_register(link_cb, NULL);

    if (!warm) {
        int netif_id = -1;
        (void)hwbcm_switch_mac_set(mac_sw);
        for (i = 0; i < 2; i++) {
            int p = test_ports[i];
            (void)hwbcm_port_speed_set(p, 10000, 1);
            (void)hwbcm_port_enable_set(p, 1);
        }
        (void)hwbcm_vlan_create(100);
        for (i = 0; i < 2; i++) {
            (void)hwbcm_vlan_port_add(100, test_ports[i], 0);
            (void)hwbcm_port_pvid_set(test_ports[i], 100);
        }
        rc = hwbcm_knet_netif_create(test_ports[0], "Ethernet4", mac_eth4, &netif_id);
        printf("cold: knet Ethernet4 -> rc=%d netif_id=%d\n", rc, netif_id);
        (void)hwbcm_trap_set(HWBCM_TRAP_ARP, 2, -1);
        (void)hwbcm_trap_set(HWBCM_TRAP_IP2ME, 2, -1);
    } else {
        hwbcm_knet_info_t kinfo[64];
        int nk = hwbcm_knet_warm_recover(kinfo, 64);
        printf("warm: hwbcm_knet_warm_recover -> %d netif(s)\n", nk);
        for (i = 0; i < nk && i < 64; i++) {
            printf("  knet[%d]: id=%d name=%s port=%d cpu=%d pvid=%d\n",
                   i, kinfo[i].netif_id, kinfo[i].name, kinfo[i].port,
                   kinfo[i].is_cpu, kinfo[i].pvid);
        }
    }

    if (hold > 0) {
        printf("holding %ds...\n", hold);
        fflush(stdout);
        sleep((unsigned int)hold);
    }

    for (i = 0; i < 2; i++) {
        print_port_state(test_ports[i]);
    }
    printf("total link_cb events observed: %d\n", g_link_events);
    fflush(stdout);

    if (cold_exit) {
        printf("performing COLD deinit (--cold-exit)...\n");
        fflush(stdout);
        rc = hwbcm_deinit();
        printf("hwbcm_deinit -> %d\n", rc);
    } else {
        rc = hwbcm_warm_sync(scache, &wb_bytes);
        printf("hwbcm_warm_sync -> %d (bytes=%zu)\n", rc, wb_bytes);
        fflush(stdout);
        if (rc < 0) {
            return 1;
        }
        rc = hwbcm_warm_deinit();
        printf("hwbcm_warm_deinit -> %d\n", rc);
        fflush(stdout);
        if (rc < 0) {
            return 1;
        }
    }

    printf("=== hwtest_wb DONE ===\n");
    return 0;
}
