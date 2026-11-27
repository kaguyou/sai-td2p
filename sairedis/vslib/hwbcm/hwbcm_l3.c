/*
 * hwbcm_l3.c — L3 section of hwbcm.h for Trident2+ (BCM56864), OpenBCM 6.5.27.
 *
 * Covers: L3 interfaces (bcm_l3_intf_*), egress objects (bcm_l3_egress_*),
 * ECMP groups (bcm_l3_egress_ecmp_*), host entries (bcm_l3_host_*), routes
 * (bcm_l3_route_*) and a software VRF id allocator.
 *
 * Model (mirrors what the diag "l3" command does on XGS):
 *  - bcmSwitchL3EgressMode = 1 is mandatory before any egress object exists.
 *    It is applied lazily by hwbcm_l3_lazy_init() from every entry point.
 *  - L3 ingress mode stays OFF: the ingress VRF comes from the VLAN table
 *    (bcm_vlan_control_vlan_t.vrf), which we program in hwbcm_l3_intf_create.
 *  - "trap to CPU" egress objects use BCM_L3_L2TOCPU: the SDK rewrites port,
 *    module and intf to the reserved L2-to-CPU next hop, so intf is irrelevant.
 *  - XGS routes that point at an ECMP group must carry BCM_L3_MULTIPATH, so we
 *    remember every ECMP id we create and set the flag automatically.
 *
 * All functions return 0 or a negative BCM_E_* value. unit is always 0.
 */
#include "hwbcm.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <bcm/types.h>
#include <bcm/error.h>
#include <bcm/l3.h>
#include <bcm/switch.h>
#include <bcm/vlan.h>

#define U HWBCM_UNIT

#define HWBCM_L3_MAX_VRF 1024
#define HWBCM_L3_DEFAULT_MTU 1514

/* ------------------------------------------------------------------------ */
/* lazy init                                                                */
/* ------------------------------------------------------------------------ */

static int s_l3_ready = 0;
static int s_l3_ingress_mode = 0;   /* 1: explicit L3 ingress objects (RIOT) */

static int hwbcm_l3_lazy_init(void)
{
    int rv;
    int mode = 0;

    if (s_l3_ready) {
        return 0;
    }

    rv = bcm_switch_control_get(U, bcmSwitchL3EgressMode, &mode);
    if (rv < 0) {
        hwbcm_log(HWBCM_LOG_ERR, "l3: switch_control_get(L3EgressMode): %s", bcm_errmsg(rv));
        return rv;
    }
    if (!mode) {
        rv = bcm_switch_control_set(U, bcmSwitchL3EgressMode, 1);
        if (rv < 0) {
            hwbcm_log(HWBCM_LOG_ERR, "l3: switch_control_set(L3EgressMode,1): %s", bcm_errmsg(rv));
            return rv;
        }
        hwbcm_log(HWBCM_LOG_INFO, "l3: egress mode enabled");
    }

    /* Without RIOT, L3 ingress mode stays off: the SDK maps VLAN v to L3_IIF v
     * and the VRF is taken from the VLAN table. With RIOT (riot_enable=1) a
     * VXLAN VFI needs an explicit L3 ingress interface, which only exists in
     * ingress mode, so every L3 interface then gets its own ingress object
     * (same id as the intf) mapped from its VLAN/VFI (hwbcm_l3_iif_bind). */
    if (hwbcm_riot_enabled()) {
        rv = bcm_switch_control_set(U, bcmSwitchL3IngressMode, 1);
        if (rv < 0) {
            hwbcm_log(HWBCM_LOG_ERR, "l3: switch_control_set(L3IngressMode,1): %s", bcm_errmsg(rv));
            return rv;
        }
        rv = bcm_switch_control_set(U, bcmSwitchL3IngressInterfaceMapSet, 1);
        if (rv < 0) {
            hwbcm_log(HWBCM_LOG_ERR, "l3: switch_control_set(L3IngressInterfaceMapSet,1): %s", bcm_errmsg(rv));
            return rv;
        }
        s_l3_ingress_mode = 1;
        hwbcm_log(HWBCM_LOG_INFO, "l3: RIOT, ingress mode enabled");
    }

    rv = bcm_l3_enable_set(U, 1);
    if (rv < 0 && rv != BCM_E_UNAVAIL) {
        hwbcm_log(HWBCM_LOG_WARN, "l3: bcm_l3_enable_set(1): %s (ignored)", bcm_errmsg(rv));
    }

    s_l3_ready = 1;
    return 0;
}

int hwbcm_l3_enable_set(int enable)
{
    int rv;

    if (enable) {
        return hwbcm_l3_lazy_init();
    }

    rv = bcm_l3_enable_set(U, 0);
    if (rv < 0 && rv != BCM_E_UNAVAIL) {
        hwbcm_log(HWBCM_LOG_ERR, "l3: bcm_l3_enable_set(0): %s", bcm_errmsg(rv));
        return rv;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* helpers                                                                  */
/* ------------------------------------------------------------------------ */

/* ECMP id bookkeeping (sorted array for O(log E) lookup in hwbcm_l3_is_ecmp). */
static int *s_ecmp_ids = NULL;
static int s_ecmp_count = 0;
static int s_ecmp_cap = 0;

static int hwbcm_l3_ecmp_lower_bound(int ecmp_id)
{
    int lo = 0, hi = s_ecmp_count;

    while (lo < hi) {
        int mid = lo + ((hi - lo) >> 1);
        if (s_ecmp_ids[mid] < ecmp_id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static int hwbcm_l3_ecmp_remember(int ecmp_id)
{
    int pos = hwbcm_l3_ecmp_lower_bound(ecmp_id);

    if (pos < s_ecmp_count && s_ecmp_ids[pos] == ecmp_id) {
        return 0;
    }
    if (s_ecmp_count == s_ecmp_cap) {
        int ncap = s_ecmp_cap ? s_ecmp_cap * 2 : 64;
        int *n = (int *)realloc(s_ecmp_ids, (size_t)ncap * sizeof(int));
        if (!n) {
            hwbcm_log(HWBCM_LOG_ERR, "l3: out of memory tracking ecmp ids");
            return BCM_E_MEMORY;
        }
        s_ecmp_ids = n;
        s_ecmp_cap = ncap;
    }
    if (pos < s_ecmp_count) {
        memmove(&s_ecmp_ids[pos + 1], &s_ecmp_ids[pos],
                (size_t)(s_ecmp_count - pos) * sizeof(int));
    }
    s_ecmp_ids[pos] = ecmp_id;
    s_ecmp_count++;
    return 0;
}

static void hwbcm_l3_ecmp_forget(int ecmp_id)
{
    int pos = hwbcm_l3_ecmp_lower_bound(ecmp_id);

    if (pos < s_ecmp_count && s_ecmp_ids[pos] == ecmp_id) {
        if (pos + 1 < s_ecmp_count) {
            memmove(&s_ecmp_ids[pos], &s_ecmp_ids[pos + 1],
                    (size_t)(s_ecmp_count - pos - 1) * sizeof(int));
        }
        s_ecmp_count--;
    }
}

int hwbcm_l3_is_ecmp(int id)
{
    int pos;

    if (s_ecmp_count == 0) {
        return 0;
    }
    pos = hwbcm_l3_ecmp_lower_bound(id);
    return (pos < s_ecmp_count && s_ecmp_ids[pos] == id) ? 1 : 0;
}

/* network-order 4 bytes -> bcm_ip_t (host order) */
static bcm_ip_t hwbcm_l3_ip4(const uint8_t *a)
{
    return ((bcm_ip_t)a[0] << 24) | ((bcm_ip_t)a[1] << 16) |
           ((bcm_ip_t)a[2] << 8) | (bcm_ip_t)a[3];
}

static void hwbcm_l3_mac_str(const uint8_t mac[6], char *buf, size_t len)
{
    if (len < 18) {
        if (len) {
            buf[0] = 0;
        }
        return;
    }
    snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void hwbcm_l3_addr_str(int af, const uint8_t *a, char *buf, size_t len)
{
    if (af == 6) {
        snprintf(buf, len,
                 "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                 a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                 a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
    } else {
        snprintf(buf, len, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    }
}

/* Program VRF + enable IPv4/IPv6 routing on a VLAN (ingress side). */
static int hwbcm_l3_vlan_routing_set(int vid, int vrf, int ingress_if)
{
    bcm_vlan_control_vlan_t vc;
    int rv;

    bcm_vlan_control_vlan_t_init(&vc);
    rv = bcm_vlan_control_vlan_get(U, (bcm_vlan_t)vid, &vc);
    if (rv < 0) {
        hwbcm_log(HWBCM_LOG_ERR, "l3: vlan_control_vlan_get(vid=%d): %s", vid, bcm_errmsg(rv));
        return rv;
    }
    vc.vrf = (bcm_vrf_t)vrf;
    if (ingress_if >= 0) {
        vc.ingress_if = (bcm_if_t)ingress_if;
    }
    vc.flags &= ~(uint32)(BCM_VLAN_IP4_DISABLE | BCM_VLAN_IP6_DISABLE);
    rv = bcm_vlan_control_vlan_set(U, (bcm_vlan_t)vid, vc);
    if (rv < 0) {
        hwbcm_log(HWBCM_LOG_ERR, "l3: vlan_control_vlan_set(vid=%d vrf=%d): %s", vid, vrf, bcm_errmsg(rv));
        return rv;
    }
    return 0;
}

/* L3 ingress interface with the same id as L3 intf intf_id (ingress mode). */
int hwbcm_l3_iif_create(int intf_id, int vrf)
{
    bcm_l3_ingress_t ing;
    bcm_if_t id = (bcm_if_t)intf_id;
    int rv;

    bcm_l3_ingress_t_init(&ing);
    ing.flags = BCM_L3_INGRESS_WITH_ID | BCM_L3_INGRESS_REPLACE;
    ing.vrf = (bcm_vrf_t)vrf;
    rv = bcm_l3_ingress_create(U, &ing, &id);
    if (rv < 0) {
        hwbcm_log(HWBCM_LOG_ERR, "l3: ingress_create(id=%d vrf=%d): %s", intf_id, vrf, bcm_errmsg(rv));
        return rv;
    }
    return 0;
}

int hwbcm_l3_ingress_mode(void)
{
    return s_l3_ingress_mode;
}

/* ------------------------------------------------------------------------ */
/* L3 interface                                                             */
/* ------------------------------------------------------------------------ */

int hwbcm_l3_intf_create(const uint8_t mac[6], int vid, int vrf, int mtu, int *intf_id)
{
    bcm_l3_intf_t intf;
    char macs[18];
    int rv;

    if (!mac || !intf_id || vid <= 0 || vid >= BCM_VLAN_INVALID) {
        return BCM_E_PARAM;
    }

    rv = hwbcm_l3_lazy_init();
    if (rv < 0) {
        return rv;
    }

    bcm_l3_intf_t_init(&intf);
    intf.l3a_flags = BCM_L3_ADD_TO_ARL;     /* router MAC into L2 table -> L3 lookup */
    memcpy(intf.l3a_mac_addr, mac, 6);
    intf.l3a_vid = (bcm_vlan_t)vid;
    intf.l3a_vrf = (bcm_vrf_t)vrf;
    intf.l3a_mtu = (mtu > 0) ? mtu : HWBCM_L3_DEFAULT_MTU;

    rv = bcm_l3_intf_create(U, &intf);
    if (rv < 0) {
        hwbcm_l3_mac_str(mac, macs, sizeof(macs));
        hwbcm_log(HWBCM_LOG_ERR, "l3: intf_create(mac=%s vid=%d vrf=%d mtu=%d): %s",
                  macs, vid, vrf, intf.l3a_mtu, bcm_errmsg(rv));
        return rv;
    }

    if (s_l3_ingress_mode) {
        rv = hwbcm_l3_iif_create((int)intf.l3a_intf_id, vrf);
        if (rv < 0) {
            (void)bcm_l3_intf_delete(U, &intf);
            return rv;
        }
    }
    rv = hwbcm_l3_vlan_routing_set(vid, vrf, s_l3_ingress_mode ? (int)intf.l3a_intf_id : -1);
    if (rv < 0) {
        if (s_l3_ingress_mode) {
            (void)bcm_l3_ingress_destroy(U, intf.l3a_intf_id);
        }
        (void)bcm_l3_intf_delete(U, &intf);
        return rv;
    }

    *intf_id = (int)intf.l3a_intf_id;
    if (hwbcm_log_enabled(HWBCM_LOG_DEBUG)) {
        hwbcm_l3_mac_str(mac, macs, sizeof(macs));
        hwbcm_log(HWBCM_LOG_DEBUG, "l3: intf %d created (mac=%s vid=%d vrf=%d mtu=%d)",
                  *intf_id, macs, vid, vrf, intf.l3a_mtu);
    }
    return 0;
}

int hwbcm_l3_intf_destroy(int intf_id)
{
    bcm_l3_intf_t intf;
    int rv;

    bcm_l3_intf_t_init(&intf);
    intf.l3a_intf_id = (bcm_if_t)intf_id;

    rv = bcm_l3_intf_delete(U, &intf);
    if (rv < 0) {
        hwbcm_log(HWBCM_LOG_ERR, "l3: intf_delete(%d): %s", intf_id, bcm_errmsg(rv));
        return rv;
    }
    if (s_l3_ingress_mode) {
        rv = bcm_l3_ingress_destroy(U, (bcm_if_t)intf_id);
        if (rv < 0 && rv != BCM_E_NOT_FOUND) {
            hwbcm_log(HWBCM_LOG_WARN, "l3: ingress_destroy(%d): %s", intf_id, bcm_errmsg(rv));
        }
    }
    hwbcm_log(HWBCM_LOG_DEBUG, "l3: intf %d destroyed", intf_id);
    return 0;
}

int hwbcm_l3_intf_mtu_set(int intf_id, int mtu)
{
    bcm_l3_intf_t intf;
    int rv;

    bcm_l3_intf_t_init(&intf);
    intf.l3a_intf_id = (bcm_if_t)intf_id;

    rv = bcm_l3_intf_get(U, &intf);
    if (rv < 0) {
        hwbcm_log(HWBCM_LOG_ERR, "l3: intf_get(%d): %s", intf_id, bcm_errmsg(rv));
        return rv;
    }

    intf.l3a_mtu = (mtu > 0) ? mtu : HWBCM_L3_DEFAULT_MTU;
    intf.l3a_flags |= BCM_L3_WITH_ID | BCM_L3_REPLACE;

    rv = bcm_l3_intf_create(U, &intf);
    if (rv < 0) {
        hwbcm_log(HWBCM_LOG_ERR, "l3: intf %d mtu=%d replace: %s", intf_id, mtu, bcm_errmsg(rv));
        return rv;
    }
    hwbcm_log(HWBCM_LOG_DEBUG, "l3: intf %d mtu=%d", intf_id, intf.l3a_mtu);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* egress objects                                                           */
/* ------------------------------------------------------------------------ */

/* Fill a bcm_l3_egress_t. to_cpu: BCM_L3_L2TOCPU; drop: BCM_L3_DST_DISCARD. */
static void hwbcm_l3_egress_fill(bcm_l3_egress_t *egr, int intf_id, const uint8_t *dst_mac,
                                 int port, int trunk_id, int to_cpu, int drop)
{
    bcm_l3_egress_t_init(egr);

    egr->intf = (intf_id >= 0) ? (bcm_if_t)intf_id : BCM_IF_INVALID;
    if (dst_mac) {
        memcpy(egr->mac_addr, dst_mac, 6);
    }

    if (to_cpu) {
        /* SDK rewrites port/module/intf to the reserved L2-to-CPU next hop. */
        egr->flags |= BCM_L3_L2TOCPU;
        egr->port = 0;                      /* CMIC */
    } else if (drop) {
        egr->flags |= BCM_L3_DST_DISCARD;
        egr->port = 0;
    } else if (trunk_id >= 0) {
        /* gport convention shared with hwbcm_core/l2: trunk gport */
        BCM_GPORT_TRUNK_SET(egr->port, trunk_id);
    } else {
        BCM_GPORT_LOCAL_SET(egr->port, port);
    }
}

static int hwbcm_l3_egress_do(uint32 flags, int intf_id, const uint8_t *dst_mac, int port,
                              int trunk_id, int to_cpu, int drop, int *egr_id)
{
    bcm_l3_egress_t egr;
    bcm_if_t id;
    char macs[18] = "-";
    int rv;

    rv = hwbcm_l3_lazy_init();
    if (rv < 0) {
        return rv;
    }

    hwbcm_l3_egress_fill(&egr, intf_id, dst_mac, port, trunk_id, to_cpu, drop);
    id = (flags & BCM_L3_WITH_ID) ? (bcm_if_t)*egr_id : 0;

    rv = bcm_l3_egress_create(U, flags, &egr, &id);
    if (rv < 0) {
        if (dst_mac) {
            hwbcm_l3_mac_str(dst_mac, macs, sizeof(macs));
        }
        hwbcm_log(HWBCM_LOG_ERR,
                  "l3: egress_create(flags=0x%x intf=%d mac=%s port=%d trunk=%d cpu=%d drop=%d): %s",
                  (unsigned)flags, intf_id, macs, port, trunk_id, to_cpu, drop, bcm_errmsg(rv));
        return rv;
    }
    *egr_id = (int)id;
    if (hwbcm_log_enabled(HWBCM_LOG_DEBUG)) {
        if (dst_mac) {
            hwbcm_l3_mac_str(dst_mac, macs, sizeof(macs));
        }
        hwbcm_log(HWBCM_LOG_DEBUG, "l3: egress %d %s (intf=%d mac=%s port=%d trunk=%d cpu=%d drop=%d)",
                  *egr_id, (flags & BCM_L3_REPLACE) ? "replaced" : "created",
                  intf_id, macs, port, trunk_id, to_cpu, drop);
    }
    return 0;
}

int hwbcm_l3_egress_create(int intf_id, const uint8_t dst_mac[6], int port, int trunk_id, int *egr_id)
{
    if (!egr_id || !dst_mac || (port < 0 && trunk_id < 0)) {
        return BCM_E_PARAM;
    }
    return hwbcm_l3_egress_do(0, intf_id, dst_mac, port, trunk_id, 0, 0, egr_id);
}

int hwbcm_l3_egress_cpu_create(int intf_id, int *egr_id)
{
    if (!egr_id) {
        return BCM_E_PARAM;
    }
    return hwbcm_l3_egress_do(0, intf_id, NULL, 0, -1, 1, 0, egr_id);
}

int hwbcm_l3_egress_drop_create(int *egr_id)
{
    if (!egr_id) {
        return BCM_E_PARAM;
    }
    return hwbcm_l3_egress_do(0, -1, NULL, 0, -1, 0, 1, egr_id);
}

int hwbcm_l3_egress_replace(int egr_id, int intf_id, const uint8_t dst_mac[6], int port, int trunk_id)
{
    int id = egr_id;

    if (egr_id < 0 || !dst_mac || (port < 0 && trunk_id < 0)) {
        return BCM_E_PARAM;
    }
    return hwbcm_l3_egress_do(BCM_L3_WITH_ID | BCM_L3_REPLACE, intf_id, dst_mac, port, trunk_id, 0, 0, &id);
}

int hwbcm_l3_egress_cpu_replace(int egr_id, int intf_id)
{
    int id = egr_id;

    if (egr_id < 0) {
        return BCM_E_PARAM;
    }
    return hwbcm_l3_egress_do(BCM_L3_WITH_ID | BCM_L3_REPLACE, intf_id, NULL, 0, -1, 1, 0, &id);
}

int hwbcm_l3_egress_drop_replace(int egr_id)
{
    int id = egr_id;

    if (egr_id < 0) {
        return BCM_E_PARAM;
    }
    return hwbcm_l3_egress_do(BCM_L3_WITH_ID | BCM_L3_REPLACE, -1, NULL, 0, -1, 0, 1, &id);
}

int hwbcm_l3_egress_destroy(int egr_id)
{
    int rv;

    rv = bcm_l3_egress_destroy(U, (bcm_if_t)egr_id);
    if (rv < 0) {
        hwbcm_log(HWBCM_LOG_ERR, "l3: egress_destroy(%d): %s", egr_id, bcm_errmsg(rv));
        return rv;
    }
    hwbcm_log(HWBCM_LOG_DEBUG, "l3: egress %d destroyed", egr_id);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* ECMP                                                                     */
/* ------------------------------------------------------------------------ */

static int hwbcm_l3_ecmp_program(uint32 flags, int ecmp_id, const int *egr_ids, int n, int *out_id)
{
    bcm_l3_egress_ecmp_t ecmp;
    bcm_if_t *arr = NULL;
    int i, rv;

    rv = hwbcm_l3_lazy_init();
    if (rv < 0) {
        return rv;
    }

    if (n < 0 || (n > 0 && !egr_ids)) {
        return BCM_E_PARAM;
    }

    if (n > 0) {
        arr = (bcm_if_t *)malloc((size_t)n * sizeof(bcm_if_t));
        if (!arr) {
            return BCM_E_MEMORY;
        }
        for (i = 0; i < n; i++) {
            arr[i] = (bcm_if_t)egr_ids[i];
        }
    }

    bcm_l3_egress_ecmp_t_init(&ecmp);
    ecmp.flags = flags;
    ecmp.max_paths = 0;                     /* chip default (l3_max_ecmp_paths) */
    if (flags & BCM_L3_WITH_ID) {
        ecmp.ecmp_intf = (bcm_if_t)ecmp_id;
    }

    rv = bcm_l3_egress_ecmp_create(U, &ecmp, n, arr);
    free(arr);
    if (rv < 0) {
        hwbcm_log(HWBCM_LOG_ERR, "l3: ecmp_create(flags=0x%x id=%d n=%d): %s",
                  (unsigned)flags, ecmp_id, n, bcm_errmsg(rv));
        return rv;
    }
    *out_id = (int)ecmp.ecmp_intf;
    return 0;
}

int hwbcm_l3_ecmp_create(const int *egr_ids, int n, int *ecmp_id)
{
    int id = 0, rv;

    if (!ecmp_id) {
        return BCM_E_PARAM;
    }
    rv = hwbcm_l3_ecmp_program(0, 0, egr_ids, n, &id);
    if (rv < 0) {
        return rv;
    }
    rv = hwbcm_l3_ecmp_remember(id);
    if (rv < 0) {
        (void)hwbcm_l3_ecmp_destroy(id);
        return rv;
    }
    *ecmp_id = id;
    hwbcm_log(HWBCM_LOG_DEBUG, "l3: ecmp %d created with %d members", id, n);
    return 0;
}

int hwbcm_l3_ecmp_set(int ecmp_id, const int *egr_ids, int n)
{
    int id = ecmp_id, rv;

    rv = hwbcm_l3_ecmp_program(BCM_L3_WITH_ID | BCM_L3_REPLACE, ecmp_id, egr_ids, n, &id);
    if (rv < 0) {
        return rv;
    }
    hwbcm_log(HWBCM_LOG_DEBUG, "l3: ecmp %d now has %d members", ecmp_id, n);
    return 0;
}

int hwbcm_l3_ecmp_destroy(int ecmp_id)
{
    bcm_l3_egress_ecmp_t ecmp;
    int rv;

    bcm_l3_egress_ecmp_t_init(&ecmp);
    ecmp.ecmp_intf = (bcm_if_t)ecmp_id;

    rv = bcm_l3_egress_ecmp_destroy(U, &ecmp);
    if (rv < 0) {
        hwbcm_log(HWBCM_LOG_ERR, "l3: ecmp_destroy(%d): %s", ecmp_id, bcm_errmsg(rv));
        return rv;
    }
    hwbcm_l3_ecmp_forget(ecmp_id);
    hwbcm_log(HWBCM_LOG_DEBUG, "l3: ecmp %d destroyed", ecmp_id);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* host entries                                                             */
/* ------------------------------------------------------------------------ */

static int hwbcm_l3_host_fill(bcm_l3_host_t *h, int vrf, int af, const uint8_t *addr)
{
    bcm_l3_host_t_init(h);
    h->l3a_vrf = (bcm_vrf_t)vrf;
    if (af == 6) {
        h->l3a_flags |= BCM_L3_IP6;
        memcpy(h->l3a_ip6_addr, addr, 16);
    } else if (af == 4) {
        h->l3a_ip_addr = hwbcm_l3_ip4(addr);
    } else {
        return BCM_E_PARAM;
    }
    return 0;
}

int hwbcm_l3_host_add(int vrf, int af, const uint8_t *addr, int egr_id)
{
    bcm_l3_host_t h;
    char as[48];
    int rv;

    if (!addr) {
        return BCM_E_PARAM;
    }
    rv = hwbcm_l3_lazy_init();
    if (rv < 0) {
        return rv;
    }
    rv = hwbcm_l3_host_fill(&h, vrf, af, addr);
    if (rv < 0) {
        return rv;
    }
    h.l3a_intf = (bcm_if_t)egr_id;
    if (hwbcm_l3_is_ecmp(egr_id)) {
        h.l3a_flags |= BCM_L3_MULTIPATH;
    }

    rv = bcm_l3_host_add(U, &h);
    if (rv == BCM_E_EXISTS) {
        h.l3a_flags |= BCM_L3_REPLACE;
        rv = bcm_l3_host_add(U, &h);
    }
    if (rv < 0) {
        hwbcm_l3_addr_str(af, addr, as, sizeof(as));
        hwbcm_log(HWBCM_LOG_ERR, "l3: host_add(vrf=%d %s -> egr %d): %s", vrf, as, egr_id, bcm_errmsg(rv));
        return rv;
    }
    if (hwbcm_log_enabled(HWBCM_LOG_DEBUG)) {
        hwbcm_l3_addr_str(af, addr, as, sizeof(as));
        hwbcm_log(HWBCM_LOG_DEBUG, "l3: host vrf=%d %s -> egr %d", vrf, as, egr_id);
    }
    return 0;
}

int hwbcm_l3_host_delete(int vrf, int af, const uint8_t *addr)
{
    bcm_l3_host_t h;
    char as[48];
    int rv;

    if (!addr) {
        return BCM_E_PARAM;
    }
    rv = hwbcm_l3_host_fill(&h, vrf, af, addr);
    if (rv < 0) {
        return rv;
    }

    rv = bcm_l3_host_delete(U, &h);
    if (rv < 0) {
        hwbcm_l3_addr_str(af, addr, as, sizeof(as));
        hwbcm_log(rv == BCM_E_NOT_FOUND ? HWBCM_LOG_WARN : HWBCM_LOG_ERR,
                  "l3: host_delete(vrf=%d %s): %s", vrf, as, bcm_errmsg(rv));
        return rv;
    }
    if (hwbcm_log_enabled(HWBCM_LOG_DEBUG)) {
        hwbcm_l3_addr_str(af, addr, as, sizeof(as));
        hwbcm_log(HWBCM_LOG_DEBUG, "l3: host vrf=%d %s deleted", vrf, as);
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* routes                                                                   */
/* ------------------------------------------------------------------------ */

static int hwbcm_l3_route_fill(bcm_l3_route_t *r, int vrf, int af, const uint8_t *prefix, int prefix_len)
{
    int i;

    bcm_l3_route_t_init(r);
    r->l3a_vrf = (bcm_vrf_t)vrf;

    if (af == 6) {
        if (prefix_len < 0 || prefix_len > 128) {
            return BCM_E_PARAM;
        }
        r->l3a_flags |= BCM_L3_IP6;
        bcm_ip6_mask_create(r->l3a_ip6_mask, prefix_len);
        for (i = 0; i < 16; i++) {
            r->l3a_ip6_net[i] = (uint8)(prefix[i] & r->l3a_ip6_mask[i]);
        }
    } else if (af == 4) {
        if (prefix_len < 0 || prefix_len > 32) {
            return BCM_E_PARAM;
        }
        r->l3a_ip_mask = bcm_ip_mask_create(prefix_len);
        r->l3a_subnet = hwbcm_l3_ip4(prefix) & r->l3a_ip_mask;
    } else {
        return BCM_E_PARAM;
    }
    return 0;
}

int hwbcm_l3_route_add(int vrf, int af, const uint8_t *prefix, int prefix_len, int egr_id)
{
    bcm_l3_route_t r;
    char as[48];
    int rv;

    if (!prefix) {
        return BCM_E_PARAM;
    }
    rv = hwbcm_l3_lazy_init();
    if (rv < 0) {
        return rv;
    }
    rv = hwbcm_l3_route_fill(&r, vrf, af, prefix, prefix_len);
    if (rv < 0) {
        return rv;
    }
    r.l3a_intf = (bcm_if_t)egr_id;
    if (hwbcm_l3_is_ecmp(egr_id)) {
        r.l3a_flags |= BCM_L3_MULTIPATH;    /* required on XGS for ECMP routes */
    }

    rv = bcm_l3_route_add(U, &r);
    if (rv == BCM_E_EXISTS) {
        r.l3a_flags |= BCM_L3_REPLACE;
        rv = bcm_l3_route_add(U, &r);
    }
    if (rv < 0) {
        hwbcm_l3_addr_str(af, prefix, as, sizeof(as));
        hwbcm_log(HWBCM_LOG_ERR, "l3: route_add(vrf=%d %s/%d -> %d%s): %s", vrf, as, prefix_len,
                  egr_id, (r.l3a_flags & BCM_L3_MULTIPATH) ? " ecmp" : "", bcm_errmsg(rv));
        return rv;
    }
    if (hwbcm_log_enabled(HWBCM_LOG_DEBUG)) {
        hwbcm_l3_addr_str(af, prefix, as, sizeof(as));
        hwbcm_log(HWBCM_LOG_DEBUG, "l3: route vrf=%d %s/%d -> %d%s", vrf, as, prefix_len,
                  egr_id, (r.l3a_flags & BCM_L3_MULTIPATH) ? " ecmp" : "");
    }
    return 0;
}

int hwbcm_l3_route_delete(int vrf, int af, const uint8_t *prefix, int prefix_len)
{
    bcm_l3_route_t r;
    char as[48];
    int rv;

    if (!prefix) {
        return BCM_E_PARAM;
    }
    rv = hwbcm_l3_route_fill(&r, vrf, af, prefix, prefix_len);
    if (rv < 0) {
        return rv;
    }

    rv = bcm_l3_route_delete(U, &r);
    if (rv < 0) {
        hwbcm_l3_addr_str(af, prefix, as, sizeof(as));
        hwbcm_log(rv == BCM_E_NOT_FOUND ? HWBCM_LOG_WARN : HWBCM_LOG_ERR,
                  "l3: route_delete(vrf=%d %s/%d): %s", vrf, as, prefix_len, bcm_errmsg(rv));
        return rv;
    }
    if (hwbcm_log_enabled(HWBCM_LOG_DEBUG)) {
        hwbcm_l3_addr_str(af, prefix, as, sizeof(as));
        hwbcm_log(HWBCM_LOG_DEBUG, "l3: route vrf=%d %s/%d deleted", vrf, as, prefix_len);
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* VRF allocator (XGS has no explicit VRF object: just an integer)          */
/* ------------------------------------------------------------------------ */

static uint8_t s_vrf_used[HWBCM_L3_MAX_VRF / 8];

int hwbcm_l3_vrf_create(int *vrf)
{
    int i;

    if (!vrf) {
        return BCM_E_PARAM;
    }
    /* vrf 0 is the default VRF and is never handed out. */
    for (i = 1; i < HWBCM_L3_MAX_VRF; i++) {
        if (!(s_vrf_used[i / 8] & (uint8_t)(1u << (i % 8)))) {
            s_vrf_used[i / 8] |= (uint8_t)(1u << (i % 8));
            *vrf = i;
            hwbcm_log(HWBCM_LOG_DEBUG, "l3: vrf %d allocated", i);
            return 0;
        }
    }
    hwbcm_log(HWBCM_LOG_ERR, "l3: no free vrf id");
    return BCM_E_FULL;
}

int hwbcm_l3_vrf_destroy(int vrf)
{
    if (vrf <= 0 || vrf >= HWBCM_L3_MAX_VRF) {
        return (vrf == 0) ? 0 : BCM_E_PARAM;
    }
    s_vrf_used[vrf / 8] &= (uint8_t)~(1u << (vrf % 8));
    hwbcm_log(HWBCM_LOG_DEBUG, "l3: vrf %d released", vrf);
    return 0;
}

void hwbcm_l3_warm_reset(void)
{
    s_l3_ready = 0;
    s_l3_ingress_mode = 0;
    s_ecmp_count = 0;
    memset(s_vrf_used, 0, sizeof(s_vrf_used));
}

int hwbcm_l3_warm_restore(const int *vrfs, int nvrfs, const int *ecmp_ids, int necmp)
{
    int mode = 0, imode = 0, rv, i;

    s_ecmp_count = 0;
    memset(s_vrf_used, 0, sizeof(s_vrf_used));

    rv = bcm_switch_control_get(U, bcmSwitchL3EgressMode, &mode);
    if (rv < 0) {
        hwbcm_log(HWBCM_LOG_WARN, "l3 wb: switch_control_get(L3EgressMode): %s", bcm_errmsg(rv));
        mode = 0;
    }
    rv = bcm_switch_control_get(U, bcmSwitchL3IngressMode, &imode);
    if (rv < 0) {
        imode = 0;
    }
    s_l3_ready = mode ? 1 : 0;
    s_l3_ingress_mode = imode ? 1 : 0;

    for (i = 0; i < nvrfs; i++) {
        int v = vrfs[i];
        if (v > 0 && v < HWBCM_L3_MAX_VRF) {
            s_vrf_used[v / 8] |= (uint8_t)(1u << (v % 8));
        }
    }
    for (i = 0; i < necmp; i++) {
        rv = hwbcm_l3_ecmp_remember(ecmp_ids[i]);
        if (rv < 0) {
            return rv;
        }
    }
    hwbcm_log(HWBCM_LOG_INFO, "l3 wb: restored egress_mode=%d ingress_mode=%d vrfs=%d ecmp=%d",
              s_l3_ready, s_l3_ingress_mode, nvrfs, necmp);
    return 0;
}
