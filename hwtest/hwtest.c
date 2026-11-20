/*
 * hwtest.c — standalone exerciser for the hwbcm C layer on the real
 * Trident2+ box. No SAI, no C++. Usage: hwtest <config.bcm> [steps]
 * steps: comma separated list of step numbers (default: all).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include "hwbcm.h"

static int g_steps[16];
static int g_nsteps = 0;

static int step_wanted(int n)
{
    int i;
    if (g_nsteps == 0) {
        return 1;
    }
    for (i = 0; i < g_nsteps; i++) {
        if (g_steps[i] == n) {
            return 1;
        }
    }
    return 0;
}

#define STEP(n, name, rc)                                                   \
    do {                                                                    \
        printf("STEP %d: %s -> rc=%d\n", (n), (name), (rc));                \
        fflush(stdout);                                                     \
    } while (0)

static void link_cb(int port, int up, void *cookie)
{
    (void)cookie;
    printf("LINK port=%d up=%d\n", port, up);
    fflush(stdout);
}

static void l2_cb(const uint8_t mac[6], int vid, int port, int add, void *cookie)
{
    char m[32];
    (void)cookie;
    snprintf(m, sizeof(m), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (HWBCM_L2_CB_IS_TRUNK(port)) {
        printf("L2 %s vid=%d trunk=%d add=%d\n", m, vid, HWBCM_L2_CB_TRUNK_ID(port), add);
    } else {
        printf("L2 %s vid=%d port=%d add=%d\n", m, vid, port, add);
    }
    fflush(stdout);
}

static void print_stats(int port)
{
    hwbcm_port_stats_t s;
    int rc = hwbcm_port_stats_get(port, &s);
    if (rc < 0) {
        printf("  stats port %d: rc=%d\n", port, rc);
        return;
    }
    printf("  stats port %d: rx pkts=%llu bytes=%llu uc=%llu mc=%llu bc=%llu err=%llu disc=%llu |"
           " tx pkts=%llu bytes=%llu uc=%llu mc=%llu bc=%llu err=%llu disc=%llu\n",
           port,
           (unsigned long long)s.rx_packets, (unsigned long long)s.rx_bytes,
           (unsigned long long)s.rx_ucast, (unsigned long long)s.rx_mcast,
           (unsigned long long)s.rx_bcast, (unsigned long long)s.rx_errors,
           (unsigned long long)s.rx_discards,
           (unsigned long long)s.tx_packets, (unsigned long long)s.tx_bytes,
           (unsigned long long)s.tx_ucast, (unsigned long long)s.tx_mcast,
           (unsigned long long)s.tx_bcast, (unsigned long long)s.tx_errors,
           (unsigned long long)s.tx_discards);
}

int main(int argc, char **argv)
{
    static const int test_ports[3] = { 2, 3, 36 };
    static const uint8_t mac_eth4[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x02 };
    static const uint8_t mac_switch[6] = { 0xa8, 0x2b, 0xb5, 0x3a, 0x3e, 0x8e };
    const char *cfg;
    int rc, i;
    int netif_eth4 = -1, netif_cpu = -1;
    int failures = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <config.bcm> [steps e.g. 1,2,3]\n", argv[0]);
        return 2;
    }
    cfg = argv[1];
    if (argc >= 3) {
        char *s = argv[2];
        char *tok;
        for (tok = strtok(s, ","); tok != NULL && g_nsteps < 16; tok = strtok(NULL, ",")) {
            g_steps[g_nsteps++] = atoi(tok);
        }
    }

    hwbcm_log_level_set(HWBCM_LOG_DEBUG);

    /* ---- 1: init, chip info, port map --------------------------------- */
    {
        uint16_t dev = 0;
        uint8_t rev = 0;
        char name[64] = "";
        int logical[HWBCM_MAX_PORTS], phys[HWBCM_MAX_PORTS];
        int n;

        rc = hwbcm_init(cfg);
        STEP(1, "hwbcm_init", rc);
        if (rc < 0) {
            printf("init failed, aborting\n");
            return 1;
        }
        rc = hwbcm_chip_info(&dev, &rev, name, sizeof(name));
        printf("  chip: dev=0x%04x rev=0x%02x name=%s (rc=%d)\n", dev, rev, name, rc);
        n = hwbcm_port_map_get(logical, phys, HWBCM_MAX_PORTS);
        printf("  port map: %d front panel ports\n", n);
        for (i = 0; i < n && i < 10; i++) {
            char pn[16] = "";
            (void)hwbcm_port_name(logical[i], pn, sizeof(pn));
            printf("    logical %3d -> phys %3d (%s)\n", logical[i], phys[i], pn);
        }
        for (i = 0; i < 3; i++) {
            int p = test_ports[i];
            char pn[16] = "";
            (void)hwbcm_port_name(p, pn, sizeof(pn));
            printf("  test port %d = %s, phys lane via reverse map: ", p, pn);
            {
                int k, found = -1;
                for (k = 0; k < n; k++) {
                    if (logical[k] == p) {
                        found = phys[k];
                    }
                }
                printf("%d -> port_from_phys=%d\n", found, found >= 0 ? hwbcm_port_from_phys(found) : -1);
            }
        }
    }

    /* ---- 2: link callback ---------------------------------------------- */
    if (step_wanted(2)) {
        rc = hwbcm_link_callback_register(link_cb, NULL);
        STEP(2, "hwbcm_link_callback_register", rc);
        if (rc < 0) failures++;
    }

    /* ---- 8 (early, so learn events during the wait are visible) -------- */
    if (step_wanted(8)) {
        rc = hwbcm_l2_callback_register(l2_cb, NULL);
        STEP(8, "hwbcm_l2_callback_register", rc);
        if (rc < 0) failures++;
    }

    /* ---- 3: ports 2,3,36 at 10G autoneg on, admin up ------------------- */
    if (step_wanted(3)) {
        for (i = 0; i < 3; i++) {
            int p = test_ports[i];
            char what[64];
            rc = hwbcm_port_speed_set(p, 10000, 1);
            snprintf(what, sizeof(what), "hwbcm_port_speed_set(%d, 10000, an=1)", p);
            STEP(3, what, rc);
            if (rc < 0) failures++;
            rc = hwbcm_port_enable_set(p, 1);
            snprintf(what, sizeof(what), "hwbcm_port_enable_set(%d, 1)", p);
            STEP(3, what, rc);
            if (rc < 0) failures++;
            {
                int sp = 0, an = 0, en = 0, mtu = 0;
                (void)hwbcm_port_speed_get(p, &sp);
                (void)hwbcm_port_autoneg_get(p, &an);
                (void)hwbcm_port_enable_get(p, &en);
                (void)hwbcm_port_mtu_get(p, &mtu);
                printf("  port %d: speed=%d autoneg=%d enable=%d maxframe=%d\n", p, sp, an, en, mtu);
            }
        }
    }

    /* ---- 4: vlan 100 with ports 2,3 untagged, pvid --------------------- */
    if (step_wanted(4)) {
        rc = hwbcm_vlan_create(100);
        STEP(4, "hwbcm_vlan_create(100)", rc);
        if (rc < 0) failures++;
        for (i = 0; i < 2; i++) {
            int p = test_ports[i];
            char what[64];
            rc = hwbcm_vlan_port_add(100, p, 0);
            snprintf(what, sizeof(what), "hwbcm_vlan_port_add(100, %d, untagged)", p);
            STEP(4, what, rc);
            if (rc < 0) failures++;
            rc = hwbcm_port_pvid_set(p, 100);
            snprintf(what, sizeof(what), "hwbcm_port_pvid_set(%d, 100)", p);
            STEP(4, what, rc);
            if (rc < 0) failures++;
        }
        rc = hwbcm_vlan_learn_set(100, 1);
        STEP(4, "hwbcm_vlan_learn_set(100, 1)", rc);
        rc = hwbcm_vlan_flood_set(100, 1, 1, 1);
        STEP(4, "hwbcm_vlan_flood_set(100, 1,1,1)", rc);
    }

    /* ---- 5: KNET netifs ------------------------------------------------ */
    if (step_wanted(5)) {
        rc = hwbcm_switch_mac_set(mac_switch);
        STEP(5, "hwbcm_switch_mac_set", rc);
        rc = hwbcm_knet_netif_create(2, "Ethernet4", mac_eth4, &netif_eth4);
        STEP(5, "hwbcm_knet_netif_create(port 2, Ethernet4)", rc);
        if (rc < 0) failures++;
        printf("  netif Ethernet4 id=%d\n", netif_eth4);
        rc = hwbcm_knet_cpu_netif_create("cpu0", mac_switch, &netif_cpu);
        STEP(5, "hwbcm_knet_cpu_netif_create(cpu0)", rc);
        if (rc < 0) failures++;
        printf("  netif cpu0 id=%d\n", netif_cpu);
        fflush(stdout);
        rc = system("ip -br link show Ethernet4; ip -br link show cpu0");
        printf("  ip link show -> %d\n", rc);
    }

    /* ---- 6: traps ------------------------------------------------------ */
    if (step_wanted(6)) {
        rc = hwbcm_trap_set(HWBCM_TRAP_ARP, 2, -1);
        STEP(6, "hwbcm_trap_set(ARP, trap)", rc);
        if (rc < 0) failures++;
        rc = hwbcm_trap_set(HWBCM_TRAP_LLDP, 2, -1);
        STEP(6, "hwbcm_trap_set(LLDP, trap)", rc);
        if (rc < 0) failures++;
        rc = hwbcm_trap_set(HWBCM_TRAP_IP2ME, 2, -1);
        STEP(6, "hwbcm_trap_set(IP2ME, trap)", rc);
        if (rc < 0) failures++;
    }

    /* ---- 7: watch links for 30 s --------------------------------------- */
    if (step_wanted(7)) {
        int t;
        STEP(7, "watch 30s", 0);
        for (t = 0; t < 30; t += 5) {
            printf("  t=%2ds:", t);
            for (i = 0; i < 3; i++) {
                int up = -1, sp = 0;
                int p = test_ports[i];
                rc = hwbcm_port_link_get(p, &up);
                (void)hwbcm_port_speed_get(p, &sp);
                printf(" port%d=%s(%dM,rc=%d)", p, up == 1 ? "UP" : up == 0 ? "down" : "?", sp, rc);
            }
            printf("\n");
            print_stats(2);
            fflush(stdout);
            sleep(5);
        }
    }

    /* ---- 9: teardown --------------------------------------------------- */
    if (step_wanted(9)) {
        if (netif_eth4 > 0) {
            rc = hwbcm_knet_netif_destroy(netif_eth4);
            STEP(9, "hwbcm_knet_netif_destroy(Ethernet4)", rc);
        }
        if (netif_cpu > 0) {
            rc = hwbcm_knet_netif_destroy(netif_cpu);
            STEP(9, "hwbcm_knet_netif_destroy(cpu0)", rc);
        }
        rc = hwbcm_deinit();
        STEP(9, "hwbcm_deinit", rc);
    }

    printf("DONE failures=%d\n", failures);
    return 0;
}
