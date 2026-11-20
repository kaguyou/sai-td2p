/*
 * hwbcm_l2.c — VLAN, L2 address table, L2 learn events and trunks (LAG) for
 * the Trident2+ hardware SAI (OpenBCM sdk-6.5.27).
 */

#ifdef BCM_HIDE_DISPATCHABLE
#undef BCM_HIDE_DISPATCHABLE
#endif

#include <string.h>

#include <sal/core/libc.h>
#include <soc/drv.h>

#include <bcm/types.h>
#include <bcm/error.h>
#include <bcm/vlan.h>
#include <bcm/stg.h>
#include <bcm/l2.h>
#include <bcm/stack.h>
#include <bcm/trunk.h>

#include "hwbcm.h"

#define HWBCM_LOGE(...) hwbcm_log(HWBCM_LOG_ERR, __VA_ARGS__)
#define HWBCM_LOGW(...) hwbcm_log(HWBCM_LOG_WARN, __VA_ARGS__)
#define HWBCM_LOGI(...) hwbcm_log(HWBCM_LOG_INFO, __VA_ARGS__)
#define HWBCM_LOGD(...) hwbcm_log(HWBCM_LOG_DEBUG, __VA_ARGS__)

#define HWBCM_CHECK(what, call)                                              \
    do {                                                                     \
        int __rv = (call);                                                   \
        if (BCM_FAILURE(__rv)) {                                             \
            HWBCM_LOGE("%s: %s (%d)", (what), bcm_errmsg(__rv), __rv);       \
            return __rv;                                                     \
        }                                                                    \
    } while (0)

static int vid_valid(int vid)
{
    return vid >= 1 && vid <= BCM_VLAN_MAX;
}

static int port_valid(int port)
{
    return port >= 0 && port < SOC_MAX_NUM_PORTS;
}

/* No STP object support: in the STG a VLAN belongs to, every front panel
 * port (or the one given, port >= 0) is forced to FORWARD. 6.5.27 leaves
 * TD2+ ports DISABLED otherwise, which silently drops all ingress. */
static int vlan_stg_forward(int vid, int only_port)
{
    int unit = HWBCM_UNIT;
    bcm_stg_t stg = 0;
    bcm_port_t port;
    int rv, n = 0;

    HWBCM_CHECK("bcm_vlan_stg_get", bcm_vlan_stg_get(unit, (bcm_vlan_t)vid, &stg));
    if (only_port >= 0) {
        HWBCM_CHECK("bcm_stg_stp_set", bcm_stg_stp_set(unit, stg, only_port, BCM_STG_STP_FORWARD));
        HWBCM_LOGD("vlan %d: stg %d port %d FORWARD", vid, (int)stg, only_port);
        return 0;
    }
    PBMP_PORT_ITER(unit, port) {
        if (IS_CPU_PORT(unit, port) || IS_LB_PORT(unit, port)) {
            continue;
        }
        rv = bcm_stg_stp_set(unit, stg, port, BCM_STG_STP_FORWARD);
        if (BCM_FAILURE(rv)) {
            HWBCM_LOGW("bcm_stg_stp_set(stg %d, port %d, FORWARD): %s", (int)stg, port, bcm_errmsg(rv));
        } else {
            n++;
        }
    }
    HWBCM_LOGI("vlan %d: stg %d, stp forward on %d ports", vid, (int)stg, n);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* VLAN                                                                     */
/* ------------------------------------------------------------------------ */

/* The CPU port must be a (tagged) member of every VLAN, like Broadcom's
 * SAI does: copies to CPU (IP2ME, traps) are otherwise dropped at the CPU
 * port's egress VLAN membership check (TDBGC3 ticks, nothing reaches KNET).
 * Verified live on the box (2026-11-20). */
int hwbcm_vlan_cpu_add(int vid)
{
    int unit = HWBCM_UNIT;
    bcm_pbmp_t pbmp, ubmp;
    int rv;

    if (!vid_valid(vid)) {
        return BCM_E_PARAM;
    }
    BCM_PBMP_CLEAR(pbmp);
    BCM_PBMP_CLEAR(ubmp);
    BCM_PBMP_PORT_ADD(pbmp, CMIC_PORT(unit));
    rv = bcm_vlan_port_add(unit, (bcm_vlan_t)vid, pbmp, ubmp);
    if (BCM_FAILURE(rv) && rv != BCM_E_EXISTS) {
        HWBCM_LOGE("bcm_vlan_port_add(vlan %d, cpu port %d): %s", vid, CMIC_PORT(unit), bcm_errmsg(rv));
        return rv;
    }
    HWBCM_LOGD("vlan %d: cpu port %d member (tagged)", vid, CMIC_PORT(unit));
    return 0;
}

int hwbcm_vlan_create(int vid)
{
    int rv;

    if (!vid_valid(vid)) {
        return BCM_E_PARAM;
    }
    rv = bcm_vlan_create(HWBCM_UNIT, (bcm_vlan_t)vid);
    if (rv == BCM_E_EXISTS) {
        HWBCM_LOGW("vlan %d already exists", vid);
        return 0;
    }
    if (BCM_FAILURE(rv)) {
        HWBCM_LOGE("bcm_vlan_create(%d): %s", vid, bcm_errmsg(rv));
        return rv;
    }
    HWBCM_LOGI("vlan %d created", vid);
    (void)vlan_stg_forward(vid, -1);
    (void)hwbcm_vlan_cpu_add(vid);
    return 0;
}

int hwbcm_vlan_destroy(int vid)
{
    int rv;

    if (!vid_valid(vid)) {
        return BCM_E_PARAM;
    }
    rv = bcm_vlan_destroy(HWBCM_UNIT, (bcm_vlan_t)vid);
    if (rv == BCM_E_NOT_FOUND) {
        HWBCM_LOGW("vlan %d does not exist", vid);
        return 0;
    }
    if (BCM_FAILURE(rv)) {
        HWBCM_LOGE("bcm_vlan_destroy(%d): %s", vid, bcm_errmsg(rv));
        return rv;
    }
    HWBCM_LOGI("vlan %d destroyed", vid);
    return 0;
}

int hwbcm_vlan_port_add(int vid, int port, int tagged)
{
    bcm_pbmp_t pbmp, ubmp;

    if (!vid_valid(vid) || !port_valid(port)) {
        return BCM_E_PARAM;
    }
    BCM_PBMP_CLEAR(pbmp);
    BCM_PBMP_CLEAR(ubmp);
    BCM_PBMP_PORT_ADD(pbmp, port);
    if (!tagged) {
        BCM_PBMP_PORT_ADD(ubmp, port);
    }
    HWBCM_CHECK("bcm_vlan_port_add", bcm_vlan_port_add(HWBCM_UNIT, (bcm_vlan_t)vid, pbmp, ubmp));
    HWBCM_LOGI("vlan %d: port %d added %s", vid, port, tagged ? "tagged" : "untagged");
    (void)vlan_stg_forward(vid, port);
    return 0;
}

int hwbcm_vlan_port_remove(int vid, int port)
{
    bcm_pbmp_t pbmp;

    if (!vid_valid(vid) || !port_valid(port)) {
        return BCM_E_PARAM;
    }
    if (port == CMIC_PORT(HWBCM_UNIT)) {
        HWBCM_LOGW("vlan %d: refusing to remove the cpu port %d", vid, port);
        return 0;
    }
    BCM_PBMP_CLEAR(pbmp);
    BCM_PBMP_PORT_ADD(pbmp, port);
    HWBCM_CHECK("bcm_vlan_port_remove", bcm_vlan_port_remove(HWBCM_UNIT, (bcm_vlan_t)vid, pbmp));
    HWBCM_LOGI("vlan %d: port %d removed", vid, port);
    return 0;
}

/* Read-modify-write of the per-VLAN control flags. */
static int vlan_flags_update(int vid, uint32 set, uint32 clear)
{
    bcm_vlan_control_vlan_t ctl;

    bcm_vlan_control_vlan_t_init(&ctl);
    HWBCM_CHECK("bcm_vlan_control_vlan_get", bcm_vlan_control_vlan_get(HWBCM_UNIT, (bcm_vlan_t)vid, &ctl));
    ctl.flags = (ctl.flags & ~clear) | set;
    HWBCM_CHECK("bcm_vlan_control_vlan_set", bcm_vlan_control_vlan_set(HWBCM_UNIT, (bcm_vlan_t)vid, ctl));
    return 0;
}

int hwbcm_vlan_learn_set(int vid, int enable)
{
    if (!vid_valid(vid)) {
        return BCM_E_PARAM;
    }
    if (enable) {
        HWBCM_CHECK("vlan learn on", vlan_flags_update(vid, 0, BCM_VLAN_LEARN_DISABLE));
    } else {
        HWBCM_CHECK("vlan learn off", vlan_flags_update(vid, BCM_VLAN_LEARN_DISABLE, 0));
    }
    HWBCM_LOGD("vlan %d: learning %s", vid, enable ? "on" : "off");
    return 0;
}

int hwbcm_vlan_flood_set(int vid, int unknown_ucast, int unknown_mcast, int bcast)
{
    uint32 set = 0, clear = 0;

    if (!vid_valid(vid)) {
        return BCM_E_PARAM;
    }
    if (unknown_ucast) {
        clear |= BCM_VLAN_UNKNOWN_UCAST_DROP;
    } else {
        set |= BCM_VLAN_UNKNOWN_UCAST_DROP;
    }
    /* The chip has one knob for all non-unicast (unknown multicast and
     * broadcast). Flood unless both are disabled. */
    if (unknown_mcast != bcast) {
        HWBCM_LOGW("vlan %d: unknown_mcast=%d bcast=%d differ; non-unicast flooding is one control, flooding",
                   vid, unknown_mcast, bcast);
    }
    if (unknown_mcast || bcast) {
        clear |= BCM_VLAN_NON_UCAST_DROP;
    } else {
        set |= BCM_VLAN_NON_UCAST_DROP;
    }
    HWBCM_CHECK("vlan flood", vlan_flags_update(vid, set, clear));
    HWBCM_LOGD("vlan %d: flood uucast=%d umcast=%d bcast=%d", vid, unknown_ucast, unknown_mcast, bcast);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* L2 addresses                                                             */
/* ------------------------------------------------------------------------ */

static int l2_addr_add_common(const uint8_t mac[6], int vid, int port, int trunk_id, int is_static)
{
    bcm_l2_addr_t l2;
    bcm_mac_t bmac;
    int rv;

    if (mac == NULL || !vid_valid(vid)) {
        return BCM_E_PARAM;
    }
    memcpy(bmac, mac, 6);
    bcm_l2_addr_t_init(&l2, bmac, (bcm_vlan_t)vid);
    if (is_static) {
        l2.flags |= BCM_L2_STATIC;
    }
    if (trunk_id >= 0) {
        l2.flags |= BCM_L2_TRUNK_MEMBER;
        l2.tgid = (bcm_trunk_t)trunk_id;
    } else {
        if (!port_valid(port)) {
            return BCM_E_PORT;
        }
        l2.port = port;
        (void)bcm_stk_my_modid_get(HWBCM_UNIT, &l2.modid);
    }
    rv = bcm_l2_addr_add(HWBCM_UNIT, &l2);
    if (BCM_FAILURE(rv)) {
        HWBCM_LOGE("bcm_l2_addr_add(%02x:%02x:%02x:%02x:%02x:%02x vlan %d %s %d): %s",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], vid,
                   trunk_id >= 0 ? "trunk" : "port", trunk_id >= 0 ? trunk_id : port, bcm_errmsg(rv));
        return rv;
    }
    HWBCM_LOGD("l2 add %02x:%02x:%02x:%02x:%02x:%02x vlan %d %s %d%s",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], vid,
               trunk_id >= 0 ? "trunk" : "port", trunk_id >= 0 ? trunk_id : port,
               is_static ? " static" : "");
    return 0;
}

int hwbcm_l2_addr_add(const uint8_t mac[6], int vid, int port, int is_static)
{
    return l2_addr_add_common(mac, vid, port, -1, is_static);
}

int hwbcm_l2_addr_add_trunk(const uint8_t mac[6], int vid, int trunk_id, int is_static)
{
    if (trunk_id < 0) {
        return BCM_E_PARAM;
    }
    return l2_addr_add_common(mac, vid, -1, trunk_id, is_static);
}

int hwbcm_l2_addr_delete(const uint8_t mac[6], int vid)
{
    bcm_mac_t bmac;
    int rv;

    if (mac == NULL || !vid_valid(vid)) {
        return BCM_E_PARAM;
    }
    memcpy(bmac, mac, 6);
    rv = bcm_l2_addr_delete(HWBCM_UNIT, bmac, (bcm_vlan_t)vid);
    if (BCM_FAILURE(rv)) {
        if (rv == BCM_E_NOT_FOUND) {
            HWBCM_LOGD("l2 delete %02x:%02x:%02x:%02x:%02x:%02x vlan %d: not found",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], vid);
        } else {
            HWBCM_LOGE("bcm_l2_addr_delete(%02x:%02x:%02x:%02x:%02x:%02x vlan %d): %s",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], vid, bcm_errmsg(rv));
        }
        return rv;
    }
    return 0;
}

/* Flushes remove dynamic entries only; static entries are owned by the
 * caller and removed explicitly. */
int hwbcm_l2_flush_port(int port)
{
    if (!port_valid(port)) {
        return BCM_E_PORT;
    }
    HWBCM_CHECK("bcm_l2_addr_delete_by_port", bcm_l2_addr_delete_by_port(HWBCM_UNIT, -1, port, 0));
    HWBCM_LOGI("l2 flush port %d", port);
    return 0;
}

int hwbcm_l2_flush_vlan_port(int vid, int port)
{
    if (!port_valid(port)) {
        return BCM_E_PORT;
    }
    HWBCM_CHECK("bcm_l2_addr_delete_by_vlan_port",
                bcm_l2_addr_delete_by_vlan_port(HWBCM_UNIT, (bcm_vlan_t)vid, -1, port, 0));
    HWBCM_LOGI("l2 flush vlan %d port %d", vid, port);
    return 0;
}

int hwbcm_l2_flush_vlan(int vid)
{
    if (!vid_valid(vid)) {
        return BCM_E_PARAM;
    }
    HWBCM_CHECK("bcm_l2_addr_delete_by_vlan", bcm_l2_addr_delete_by_vlan(HWBCM_UNIT, (bcm_vlan_t)vid, 0));
    HWBCM_LOGI("l2 flush vlan %d", vid);
    return 0;
}

int hwbcm_l2_flush_trunk(int trunk_id)
{
    if (trunk_id < 0) {
        return BCM_E_PARAM;
    }
    HWBCM_CHECK("bcm_l2_addr_delete_by_trunk", bcm_l2_addr_delete_by_trunk(HWBCM_UNIT, (bcm_trunk_t)trunk_id, 0));
    HWBCM_LOGI("l2 flush trunk %d", trunk_id);
    return 0;
}

int hwbcm_l2_age_set(int seconds)
{
    if (seconds < 0) {
        return BCM_E_PARAM;
    }
    HWBCM_CHECK("bcm_l2_age_timer_set", bcm_l2_age_timer_set(HWBCM_UNIT, seconds));
    HWBCM_LOGI("l2 age timer %d s", seconds);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* L2 learn / age events                                                    */
/* ------------------------------------------------------------------------ */

static hwbcm_l2_cb_t g_l2_cb = NULL;
static void *g_l2_cookie = NULL;
static int g_l2_cb_registered = 0;

static void hwbcm_l2_addr_handler(int unit, bcm_l2_addr_t *l2addr, int operation, void *userdata)
{
    hwbcm_l2_cb_t cb = g_l2_cb;
    uint8_t mac[6];
    int add, port;

    (void)unit;
    (void)userdata;
    if (cb == NULL || l2addr == NULL) {
        return;
    }
    switch (operation) {
    case BCM_L2_CALLBACK_ADD:
    case BCM_L2_CALLBACK_LEARN_EVENT:
    case BCM_L2_CALLBACK_MOVE_EVENT:
        add = 1;
        break;
    case BCM_L2_CALLBACK_DELETE:
    case BCM_L2_CALLBACK_AGE_EVENT:
        add = 0;
        break;
    default:
        return; /* REPORT / REFRESH: not a table change */
    }
    memcpy(mac, l2addr->mac, 6);
    if (l2addr->flags & BCM_L2_TRUNK_MEMBER) {
        port = HWBCM_L2_CB_TRUNK_PORT((int)l2addr->tgid);
    } else {
        port = l2addr->port;
    }
    cb(mac, (int)l2addr->vid, port, add, g_l2_cookie);
}

int hwbcm_l2_callback_register(hwbcm_l2_cb_t cb, void *cookie)
{
    int rv;

    g_l2_cookie = cookie;
    g_l2_cb = cb;
    if (!g_l2_cb_registered) {
        rv = bcm_l2_addr_register(HWBCM_UNIT, hwbcm_l2_addr_handler, NULL);
        if (BCM_FAILURE(rv)) {
            HWBCM_LOGE("bcm_l2_addr_register: %s", bcm_errmsg(rv));
            g_l2_cb = NULL;
            return rv;
        }
        g_l2_cb_registered = 1;
        HWBCM_LOGI("l2 learn callback registered");
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* trunk / LAG                                                              */
/* ------------------------------------------------------------------------ */

int hwbcm_trunk_create(int *trunk_id)
{
    bcm_trunk_t tid = BCM_TRUNK_INVALID;
    bcm_trunk_info_t info;
    int rv;

    if (trunk_id == NULL) {
        return BCM_E_PARAM;
    }
    HWBCM_CHECK("bcm_trunk_create", bcm_trunk_create(HWBCM_UNIT, 0, &tid));

    /* Program hashing now so a trunk with no members is well-defined. */
    bcm_trunk_info_t_init(&info);
    info.psc = BCM_TRUNK_PSC_PORTFLOW;
    info.dlf_index = BCM_TRUNK_UNSPEC_INDEX;
    info.mc_index = BCM_TRUNK_UNSPEC_INDEX;
    info.ipmc_index = BCM_TRUNK_UNSPEC_INDEX;
    rv = bcm_trunk_set(HWBCM_UNIT, tid, &info, 0, NULL);
    if (BCM_FAILURE(rv)) {
        HWBCM_LOGW("bcm_trunk_set(%d, empty): %s", (int)tid, bcm_errmsg(rv));
    }

    *trunk_id = (int)tid;
    HWBCM_LOGI("trunk %d created", (int)tid);
    return 0;
}

int hwbcm_trunk_destroy(int trunk_id)
{
    int rv;

    if (trunk_id < 0) {
        return BCM_E_PARAM;
    }
    rv = bcm_trunk_destroy(HWBCM_UNIT, (bcm_trunk_t)trunk_id);
    if (rv == BCM_E_NOT_FOUND) {
        HWBCM_LOGW("trunk %d does not exist", trunk_id);
        return 0;
    }
    if (BCM_FAILURE(rv)) {
        HWBCM_LOGE("bcm_trunk_destroy(%d): %s", trunk_id, bcm_errmsg(rv));
        return rv;
    }
    HWBCM_LOGI("trunk %d destroyed", trunk_id);
    return 0;
}

int hwbcm_trunk_member_set(int trunk_id, const int *ports, int nports)
{
    bcm_trunk_info_t info;
    bcm_trunk_member_t members[BCM_TRUNK_MAX_PORTCNT];
    int i;

    if (trunk_id < 0 || nports < 0 || (nports > 0 && ports == NULL)) {
        return BCM_E_PARAM;
    }
    if (nports > BCM_TRUNK_MAX_PORTCNT) {
        HWBCM_LOGE("trunk %d: %d members exceeds %d", trunk_id, nports, BCM_TRUNK_MAX_PORTCNT);
        return BCM_E_PARAM;
    }
    for (i = 0; i < nports; i++) {
        if (!port_valid(ports[i])) {
            return BCM_E_PORT;
        }
        bcm_trunk_member_t_init(&members[i]);
        BCM_GPORT_LOCAL_SET(members[i].gport, ports[i]);
    }

    bcm_trunk_info_t_init(&info);
    info.psc = BCM_TRUNK_PSC_PORTFLOW;
    info.dlf_index = BCM_TRUNK_UNSPEC_INDEX;
    info.mc_index = BCM_TRUNK_UNSPEC_INDEX;
    info.ipmc_index = BCM_TRUNK_UNSPEC_INDEX;
    HWBCM_CHECK("bcm_trunk_set",
                bcm_trunk_set(HWBCM_UNIT, (bcm_trunk_t)trunk_id, &info, nports, nports ? members : NULL));
    HWBCM_LOGI("trunk %d: %d members", trunk_id, nports);
    return 0;
}
