/*
 * hwbcm_core.c — core of the thin C wrapper around OpenBCM (sdk-6.5.27) for
 * the Trident2+ (BCM56864) hardware SAI in sonic-sairedis vslib.
 *
 * Covers: logging, SDK bring-up (hwbcm_init/deinit), chip info, port map,
 * port admin/speed/link/mtu/learn/pause/pvid, counters, linkscan callback,
 * switch MAC, KNET netifs/filters, trap-to-CPU policy, diag shell access.
 *
 * Only this file and hwbcm_l2.c include SDK headers; hwbcm.h stays SDK-free.
 */

/* The SDK build flags carry -DBCM_HIDE_DISPATCHABLE (used when compiling the
 * dispatch layer itself). It hides every bcm_* prototype, which we need. */
#ifdef BCM_HIDE_DISPATCHABLE
#undef BCM_HIDE_DISPATCHABLE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <syslog.h>
/* Save syslog priority constants before SDK headers (<shared/bsl.h> via
 * <soc/scache.h>) redefine LOG_INFO / LOG_DEBUG as function-like macros. */
enum {
    HWBCM_SYSLOG_ERR   = LOG_ERR,
    HWBCM_SYSLOG_WARN  = LOG_WARNING,
    HWBCM_SYSLOG_INFO  = LOG_INFO,
    HWBCM_SYSLOG_DEBUG = LOG_DEBUG
};
#undef LOG_INFO
#undef LOG_DEBUG
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <sal/core/boot.h>
#include <sal/core/libc.h>
#include <sal/appl/sal.h>
#include <sal/appl/config.h>
#include <soc/drv.h>
#ifdef BCM_CMICM_SUPPORT
#include <soc/cmicm.h>
#endif
#include <soc/cm.h>
#include <soc/knet.h>
#include <soc/scache.h>
#include <appl/diag/system.h>
#include <appl/diag/shell.h>
#include <appl/diag/parse.h>
#include <appl/diag/sysconf.h>
#include <appl/diag/bslmgmt.h>
#include <appl/diag/cmdlist.h>
#include <appl/diag/diag.h>
#include <appl/diag/opennsa_diag.h>
#include <ibde.h>
#include <linux-bde.h>
#include <linux/kernel/modules/include/bcm-knet-kcom.h>

#include <bcm/types.h>
#include <bcm/error.h>
#include <bcm/init.h>
#include <bcm/port.h>
#include <bcm/link.h>
#include <bcm/stat.h>
#include <bcm/stg.h>
#include <bcm/stack.h>
#include <bcm/switch.h>
#include <bcm/l2.h>
#include <bcm/rx.h>
#include <bcm/tx.h>
#include <bcm/knet.h>

#include "hwbcm.h"

/* ------------------------------------------------------------------------ */
/* logging                                                                  */
/* ------------------------------------------------------------------------ */

static hwbcm_log_level_t g_log_level = HWBCM_LOG_INFO;
static int g_stderr_unbuffered = 0;

void hwbcm_log_level_set(hwbcm_log_level_t level)
{
    g_log_level = level;
}

void hwbcm_log(hwbcm_log_level_t level, const char *fmt, ...)
{
    static const char *tag[] = { "ERR", "WARN", "INFO", "DEBUG" };
    static const int prio[] = {
        HWBCM_SYSLOG_ERR, HWBCM_SYSLOG_WARN, HWBCM_SYSLOG_INFO, HWBCM_SYSLOG_DEBUG
    };
    char buf[1024];
    va_list ap;
    int idx;

    if (level > g_log_level) {
        return;
    }
    idx = (int)level;
    if (idx < 0 || idx > 3) {
        idx = 0;
    }

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* No openlog(): it would change the syslog ident of the whole host
     * process (syncd). Tag the message instead. Always write both sinks;
     * stderr is made unbuffered once so early lines are not lost. */
    if (!g_stderr_unbuffered) {
        setvbuf(stderr, NULL, _IONBF, 0);
        g_stderr_unbuffered = 1;
    }
    syslog(prio[idx], "hwbcm: %s", buf);
    fprintf(stderr, "hwbcm[%s]: %s\n", tag[idx], buf);
    fflush(stderr);
}

#define HWBCM_LOGE(...) hwbcm_log(HWBCM_LOG_ERR, __VA_ARGS__)
#define HWBCM_LOGW(...) hwbcm_log(HWBCM_LOG_WARN, __VA_ARGS__)
#define HWBCM_LOGI(...) hwbcm_log(HWBCM_LOG_INFO, __VA_ARGS__)
#define HWBCM_LOGD(...) hwbcm_log(HWBCM_LOG_DEBUG, __VA_ARGS__)

/* Evaluate an SDK call; on failure log "<what>: <errmsg>" and return rv. */
#define HWBCM_CHECK(what, call)                                              \
    do {                                                                     \
        int __rv = (call);                                                   \
        if (BCM_FAILURE(__rv)) {                                             \
            HWBCM_LOGE("%s: %s (%d)", (what), bcm_errmsg(__rv), __rv);       \
            return __rv;                                                     \
        }                                                                    \
    } while (0)

/* Same, but only warn and continue. */
#define HWBCM_WARN_ON_FAIL(what, call)                                       \
    do {                                                                     \
        int __rv = (call);                                                   \
        if (BCM_FAILURE(__rv)) {                                             \
            HWBCM_LOGW("%s: %s (%d)", (what), bcm_errmsg(__rv), __rv);       \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------------ */
/* globals the SDK application layer expects (see socdiag.c)                */
/* ------------------------------------------------------------------------ */

ibde_t *bde;

/* Build stamps referenced by libdiag.a (sh_print_version). bcm.user gets
 * these from a generated version.c; we provide them here so that anything
 * linking the diag shell library resolves. */
char *_build_release = "sdk-6.5.27-hwbcm";
char *_build_user = "hwbcm";
char *_build_host = "hwbcm";
char *_build_date = __DATE__ " " __TIME__;
char *_build_datestamp = __DATE__;
char *_build_tree = "sonic-sairedis/vslib/hwbcm";

int bde_create(void)
{
    linux_bde_bus_t bus;

    bus.be_pio = SYS_BE_PIO;
    bus.be_packet = SYS_BE_PACKET;
    bus.be_other = SYS_BE_OTHER;
    return linux_bde_create(&bus, &bde);
}

/* Defined in src/appl/diag/system.c (libdiag.a), not exported by a header. */
extern char *diag_user_var_get(char *varname);

/* Defined in linux-user-bde.c (libuser bde). */
extern int bde_icid_get(int d, uint8 *data, int len);
extern int bde_irq_mask_set(int unit, uint32 addr, uint32 mask);
extern int bde_hw_unit_get(int unit, int inverse);

static soc_chip_info_vectors_t g_chip_info_vect = {
    bde_icid_get
};

static soc_knet_vectors_t g_knet_vect = {
    {
        bcm_knet_kcom_open,
        bcm_knet_kcom_close,
        bcm_knet_kcom_msg_send,
        bcm_knet_kcom_msg_recv
    },
    bde_irq_mask_set,
    bde_hw_unit_get
};

/* ------------------------------------------------------------------------ */
/* state                                                                    */
/* ------------------------------------------------------------------------ */

/* Internal port tables are sized by the SDK's own limit, not HWBCM_MAX_PORTS,
 * because TD2+ logical ports go up to SOC_MAX_NUM_PORTS (CPU/loopback). */
#define HWBCM_PORT_TBL (SOC_MAX_NUM_PORTS)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_inited = 0;
static int g_sal_sysconf_inited = 0;
static int g_warm_boot = 0;

/* Level 2 warm-boot scache (buffer-backed with atomic file commit on sync). */
#define HWBCM_SCACHE_DEFAULT_SIZE  (32u * 1024u * 1024u)
#define HWBCM_SCACHE_MIN_SIZE      (16u * 1024u * 1024u)
#define HWBCM_SCACHE_MAX_SIZE      (128u * 1024u * 1024u)
#define HWBCM_SCACHE_NULL_PAD      64u

static pthread_mutex_t g_scache_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_scache_buf = NULL;
static size_t g_scache_cap = 0;
static size_t g_scache_read_limit = 0;
static size_t g_scache_written_end = 0;
static int g_scache_configured = 0;
static char g_scache_path[512];

/* front panel port map, built at init */
static int g_fp_count = 0;
static int g_fp_logical[HWBCM_PORT_TBL];
static int g_fp_phys[HWBCM_PORT_TBL];

/* per-port desired speed (0 = never set) for the 1G SerDes work-around */
static int g_desired_speed[HWBCM_PORT_TBL];

/* SGMII re-apply bookkeeping: the re-apply itself bounces the link, so it
 * must run once per link episode, not on every link-up event. */
#define HWBCM_SGMII_OWN_BOUNCE_SEC 3   /* a down within this of our re-apply is our own */
#define HWBCM_SGMII_RATE_LIMIT_SEC 10  /* never re-apply more often than this */
static int g_sgmii_applied[HWBCM_PORT_TBL];
static long g_sgmii_applied_at[HWBCM_PORT_TBL]; /* monotonic seconds, 0 = never */

static long mono_seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long)ts.tv_sec + 1; /* +1 so that 0 means "never" */
}

/* link callback */
static hwbcm_link_cb_t g_link_cb = NULL;
static void *g_link_cookie = NULL;

/* switch mac + IP2ME station */
static uint8_t g_switch_mac[6];
static int g_switch_mac_valid = 0;
static int g_ip2me_station_id = -1;
static int g_ip2me_vfi_station_id = -1;   /* RIOT: same MAC, any VXLAN VFI */
static int g_ip2me_wanted = 0;

/* knet netif -> filter(s) bookkeeping */
#define HWBCM_KNET_MAX 256
typedef struct {
    int netif_id;        /* 0 = free slot */
    int port;            /* bound front port, -1 for the CPU netif */
    int filter_id;       /* ingress-port filter (prio 50, tag kept), 0 = none */
    int pvid_filter_id;  /* ingress-port + PVID filter (prio 49, tag stripped), 0 = none */
    int pvid;            /* PVID the strip filter was built for */
    int vfi_filter_id;   /* RIOT: ingress-port + DCB VLAN 0 filter (prio 48, tag stripped), 0 = none */
} hwbcm_knet_slot_t;
static hwbcm_knet_slot_t g_knet[HWBCM_KNET_MAX];

static void knet_filter_destroy_quiet(int *fid)
{
    if (*fid != 0) {
        int rv = bcm_knet_filter_destroy(HWBCM_UNIT, *fid);
        if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
            HWBCM_LOGW("bcm_knet_filter_destroy(%d): %s", *fid, bcm_errmsg(rv));
        }
        *fid = 0;
    }
}

/* Filter (a): frames that ingressed on `port` carrying the port's PVID are
 * delivered with the VLAN tag stripped. The KNET driver strips only when
 * the matching filter has STRIP_TAG (bcm-knet.c), so without this the Linux
 * stack sees 802.1Q frames on a routed port and ignores them. Frames with
 * other VLANs fall through to filter (b) and keep their tag. */
static int knet_pvid_filter_create(hwbcm_knet_slot_t *slot, const char *name)
{
    bcm_knet_filter_t f;
    int rv;

    bcm_knet_filter_t_init(&f);
    f.type = BCM_KNET_FILTER_T_RX_PKT;
    f.priority = 49;
    f.flags = BCM_KNET_FILTER_F_STRIP_TAG;
    f.dest_type = BCM_KNET_DEST_T_NETIF;
    f.dest_id = slot->netif_id;
    f.match_flags = BCM_KNET_FILTER_M_INGPORT | BCM_KNET_FILTER_M_VLAN;
    f.m_ingport = slot->port;
    f.m_vlan = (bcm_vlan_t)slot->pvid;
    snprintf(f.desc, BCM_KNET_FILTER_DESC_MAX, "%s pvid", name);
    rv = bcm_knet_filter_create(HWBCM_UNIT, &f);
    if (BCM_FAILURE(rv)) {
        HWBCM_LOGE("bcm_knet_filter_create(port %d vlan %d strip -> netif %d): %s",
                   slot->port, slot->pvid, slot->netif_id, bcm_errmsg(rv));
        slot->pvid_filter_id = 0;
        return rv;
    }
    slot->pvid_filter_id = f.id;
    HWBCM_LOGI("knet: netif %d port %d pvid %d strip filter %d", slot->netif_id, slot->port, slot->pvid, f.id);
    return 0;
}

/* RIOT: frames that reach the CPU from a VXLAN VFI (routed to the IP2ME
 * host, or switched to the router MAC entry of the VFI) carry DCB VLAN 0 and
 * an in-band tag (VID 0 or the VFI index), so neither the PVID filter nor the
 * Linux bridge takes them. Strip the tag for DCB VLAN 0 on the port; the
 * bridge then classifies them to the port PVID (the access VLAN). */
static int knet_vfi_filter_create(hwbcm_knet_slot_t *slot, const char *name)
{
    bcm_knet_filter_t f;
    int rv;

    bcm_knet_filter_t_init(&f);
    f.type = BCM_KNET_FILTER_T_RX_PKT;
    f.priority = 48;
    f.flags = BCM_KNET_FILTER_F_STRIP_TAG;
    f.dest_type = BCM_KNET_DEST_T_NETIF;
    f.dest_id = slot->netif_id;
    f.match_flags = BCM_KNET_FILTER_M_INGPORT | BCM_KNET_FILTER_M_VLAN;
    f.m_ingport = slot->port;
    f.m_vlan = 0;
    snprintf(f.desc, BCM_KNET_FILTER_DESC_MAX, "%s vfi", name);
    rv = bcm_knet_filter_create(HWBCM_UNIT, &f);
    if (BCM_FAILURE(rv)) {
        HWBCM_LOGE("bcm_knet_filter_create(port %d vlan 0 strip -> netif %d): %s",
                   slot->port, slot->netif_id, bcm_errmsg(rv));
        slot->vfi_filter_id = 0;
        return rv;
    }
    slot->vfi_filter_id = f.id;
    return 0;
}

/* Leftovers of a previous syncd that died without hwbcm_deinit (docker stop
 * timeout -> SIGKILL): its netifs and netif filters stay in the kernel KNET
 * module, and the next process then fails every hostif create with
 * BCM_E_RESOURCE (seen 2026-11-24: 72 stale netifs + 72 new > 128). Remove
 * them before creating ours. Only NETIF-destination filters are ours. */
#define HWBCM_KNET_STALE_MAX 2048
typedef struct {
    int ids[HWBCM_KNET_STALE_MAX];
    int n;
} knet_id_list_t;

static int knet_netif_collect(int unit, bcm_knet_netif_t *netif, void *user_data)
{
    knet_id_list_t *l = (knet_id_list_t *)user_data;

    (void)unit;
    if (l->n < HWBCM_KNET_STALE_MAX) {
        l->ids[l->n++] = netif->id;
    }
    return BCM_E_NONE;
}

static int knet_filter_collect(int unit, bcm_knet_filter_t *filter, void *user_data)
{
    knet_id_list_t *l = (knet_id_list_t *)user_data;

    (void)unit;
    if (filter->dest_type == BCM_KNET_DEST_T_NETIF && l->n < HWBCM_KNET_STALE_MAX) {
        l->ids[l->n++] = filter->id;
    }
    return BCM_E_NONE;
}

static void knet_stale_cleanup(void)
{
    static knet_id_list_t l;
    int i, nf, nn;

    l.n = 0;
    (void)bcm_knet_filter_traverse(HWBCM_UNIT, knet_filter_collect, &l);
    for (i = 0; i < l.n; i++) {
        (void)bcm_knet_filter_destroy(HWBCM_UNIT, l.ids[i]);
    }
    nf = l.n;
    l.n = 0;
    (void)bcm_knet_netif_traverse(HWBCM_UNIT, knet_netif_collect, &l);
    for (i = 0; i < l.n; i++) {
        (void)bcm_knet_netif_destroy(HWBCM_UNIT, l.ids[i]);
    }
    nn = l.n;
    if (nf || nn) {
        HWBCM_LOGW("knet: removed %d stale filters and %d stale netifs of a previous process", nf, nn);
    }
}

int hwbcm_riot_enabled(void)
{
    if (!SOC_UNIT_VALID(HWBCM_UNIT)) {
        return 0;
    }
    return soc_feature(HWBCM_UNIT, soc_feature_riot) ? 1 : 0;
}

/* RIOT: a packet from a VFI that is routed to the CPU (IP2ME host entry, the
 * reserved L2-to-CPU next hop keeps the forwarding domain) leaves the CPU port
 * with egress VLAN 0. EGR_VLAN[0] is invalid by default, so the egress drops it
 * as "invalid VLAN" (TVLAND on cpu0, seen 2026-11-24). Make entry 0 valid for
 * the CPU only: default STG, no egress membership filter. */
static int riot_cpu_egress_vlan0_fix(void)
{
    egr_vlan_entry_t e;
    int rv;

    if (!SOC_MEM_IS_VALID(HWBCM_UNIT, EGR_VLANm)) {
        return 0;
    }
    rv = soc_mem_read(HWBCM_UNIT, EGR_VLANm, MEM_BLOCK_ANY, 0, &e);
    if (SOC_FAILURE(rv)) {
        HWBCM_LOGE("riot: EGR_VLAN[0] read: %d", rv);
        return rv;
    }
    soc_mem_field32_set(HWBCM_UNIT, EGR_VLANm, &e, VALIDf, 1);
    soc_mem_field32_set(HWBCM_UNIT, EGR_VLANm, &e, STGf, 1);
    if (soc_mem_field_valid(HWBCM_UNIT, EGR_VLANm, EN_EFILTERf)) {
        soc_mem_field32_set(HWBCM_UNIT, EGR_VLANm, &e, EN_EFILTERf, 0);
    }
    rv = soc_mem_write(HWBCM_UNIT, EGR_VLANm, MEM_BLOCK_ALL, 0, &e);
    if (SOC_FAILURE(rv)) {
        HWBCM_LOGE("riot: EGR_VLAN[0] write: %d", rv);
        return rv;
    }
    HWBCM_LOGI("riot: EGR_VLAN[0] valid for VFI -> CPU");
    return 0;
}

/* l2 cache (L2 user entry) index per trap reason, -1 = none */
static int g_trap_l2cache_idx[HWBCM_TRAP_STP + 1];

static int port_valid(int port)
{
    return port >= 0 && port < HWBCM_PORT_TBL;
}

/* ------------------------------------------------------------------------ */
/* linkscan handler                                                         */
/* ------------------------------------------------------------------------ */

static void hwbcm_linkscan_handler(int unit, bcm_port_t port, bcm_port_info_t *info)
{
    int up;
    hwbcm_link_cb_t cb;
    void *cookie;

    if (info == NULL || !port_valid(port)) {
        return;
    }
    up = info->linkstatus ? 1 : 0;

    if (up) {
        /* Known quirk (DESIGN.md): after autoneg links at <=1G the SerDes
         * stays at 10G; TX works but nothing is received until SGMII is
         * re-applied. Key off the ACTUAL negotiated speed, not the desired
         * one: SONiC asks for 10000 + autoneg and the peer may be 1G. */
        int speed = info->speed;
        if (speed <= 0) {
            if (BCM_FAILURE(bcm_port_speed_get(unit, port, &speed))) {
                speed = 0;
            }
        }
        if (speed > 0 && speed <= 1000) {
            long now = mono_seconds();
            long since = g_sgmii_applied_at[port] ? now - g_sgmii_applied_at[port] : -1;

            if (g_sgmii_applied[port]) {
                HWBCM_LOGI("port %d: link up at %d Mbps, SGMII already applied, skipping",
                           port, speed);
            } else if (since >= 0 && since < HWBCM_SGMII_RATE_LIMIT_SEC) {
                HWBCM_LOGW("port %d: link up at %d Mbps, SGMII re-apply rate limited (%lds ago), skipping",
                           port, speed, since);
            } else {
                int rv = bcm_port_interface_set(unit, port, BCM_PORT_IF_SGMII);
                g_sgmii_applied[port] = 1;
                g_sgmii_applied_at[port] = now;
                if (BCM_FAILURE(rv)) {
                    HWBCM_LOGW("port %d: link up at %d Mbps, SGMII re-apply failed: %s",
                               port, speed, bcm_errmsg(rv));
                } else {
                    HWBCM_LOGI("port %d: link up at %d Mbps, SGMII re-applied (desired %d)",
                               port, speed, g_desired_speed[port]);
                }
            }
        } else if (speed > 1000) {
            /* Linked above 1G: a later 1G episode must re-apply again. */
            g_sgmii_applied[port] = 0;
        }
    } else {
        /* Link down: our own re-apply bounces the link; do not forget the
         * episode for that. Any other down ends the episode. */
        if (g_sgmii_applied[port]) {
            long since = mono_seconds() - g_sgmii_applied_at[port];
            if (since >= 0 && since <= HWBCM_SGMII_OWN_BOUNCE_SEC) {
                HWBCM_LOGD("port %d: link down %lds after SGMII re-apply (own bounce), keeping flag",
                           port, since);
            } else {
                HWBCM_LOGD("port %d: link down, SGMII episode ended", port);
                g_sgmii_applied[port] = 0;
            }
        }
    }

    HWBCM_LOGD("link: port %d %s (speed %d)", port, up ? "up" : "down", info->speed);

    cb = g_link_cb;
    cookie = g_link_cookie;
    if (cb != NULL) {
        cb(port, up, cookie);
    }
}

int hwbcm_link_callback_register(hwbcm_link_cb_t cb, void *cookie)
{
    g_link_cookie = cookie;
    g_link_cb = cb;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* init / deinit                                                            */
/* ------------------------------------------------------------------------ */

/* Serializes every use of the diag shell (C++ callers and the debug socket).
 * sh_process_command mutates shell-global state (current unit, parse vars). */
static pthread_mutex_t g_shell_lock = PTHREAD_MUTEX_INITIALIZER;

#define HWBCM_SHELL_CMD_MAX 4096

/* Caller holds g_shell_lock. */
/* "hw ..." lines are hwbcm-level test hooks, not SDK diag commands.
 *   hw vxlan cpuaccess <vpn> <vid>   add the CPU port as an access VP of <vpn> for
 *                                    VLAN <vid> (one-cable VXLAN test: a KNET
 *                                    TX_CPU_INGRESS netif for <vid> then lands in
 *                                    the VFI and floods to the network VPs)
 *   hw vxlan vpdel <vpn> <vp>        remove a VP created above
 * Output goes to stdout (the shell socket dups it to the client). */
static int hwbcm_shell_hw_cmd(const char *s)
{
    int vpn = 0, vid = 0, vp = 0, rv;

    if (sscanf(s, "vxlan cpuaccess %i %i", &vpn, &vid) == 2) {
        rv = hwbcm_vxlan_access_port_add2(vpn, 0 /* cpu0 */, -1, vid, 1, &vp);
        printf("hw: vxlan cpuaccess vpn 0x%x vid %d -> vp 0x%x rv %d (%s)\n",
               vpn, vid, vp, rv, bcm_errmsg(rv));
        fflush(stdout);
        return rv < 0 ? BCM_E_FAIL : 0;
    }
    {
        int vlan = 0, flags = 0;
        if (sscanf(s, "vxlan vlanadd %i %i %i", &vlan, &vp, &flags) == 3) {
            rv = bcm_vlan_gport_add(HWBCM_UNIT, (bcm_vlan_t)vlan, (bcm_gport_t)vp, flags);
            printf("hw: vxlan vlanadd vlan %d vp 0x%x flags 0x%x rv %d (%s)\n", vlan, vp, flags, rv, bcm_errmsg(rv));
            fflush(stdout);
            return rv < 0 ? BCM_E_FAIL : 0;
        }
        if (sscanf(s, "vxlan vpefilter %i %i", &vp, &flags) == 2) {
            rv = bcm_port_vlan_member_set(HWBCM_UNIT, (bcm_gport_t)vp, (uint32)flags);
            printf("hw: vxlan vpefilter vp 0x%x flags 0x%x rv %d (%s)\n", vp, flags, rv, bcm_errmsg(rv));
            fflush(stdout);
            return rv < 0 ? BCM_E_FAIL : 0;
        }
        if (sscanf(s, "vxlan egrxlate %i %i %i", &vp, &vlan, &flags) == 3) {
            /* egress VLAN translation keyed by port class <vp> (physical port number
             * or a VP gport) for outer VLAN <vlan>: flags 1 = delete the outer tag,
             * 0 = remove the entry. */
            bcm_vlan_action_set_t act;
            bcm_vlan_action_set_t_init(&act);
            act.ot_outer = bcmVlanActionDelete;
            act.dt_outer = bcmVlanActionDelete;
            if (flags) {
                rv = bcm_vlan_translate_egress_action_add(HWBCM_UNIT, vp, (bcm_vlan_t)vlan, BCM_VLAN_NONE, &act);
            } else {
                rv = bcm_vlan_translate_egress_action_delete(HWBCM_UNIT, vp, (bcm_vlan_t)vlan, BCM_VLAN_NONE);
            }
            printf("hw: vxlan egrxlate class 0x%x vlan %d %s rv %d (%s)\n", vp, vlan, flags ? "delete-outer-tag" : "removed", rv, bcm_errmsg(rv));
            fflush(stdout);
            return rv < 0 ? BCM_E_FAIL : 0;
        }
        if (sscanf(s, "vxlan egrvt %i %i", &vp, &flags) == 2) {
            rv = bcm_vlan_control_port_set(HWBCM_UNIT, vp, bcmVlanTranslateEgressEnable, flags ? 1 : 0);
            printf("hw: vxlan egrvt port %d TranslateEgressEnable=%d rv %d (%s)\n", vp, flags ? 1 : 0, rv, bcm_errmsg(rv));
            fflush(stdout);
            return rv < 0 ? BCM_E_FAIL : 0;
        }
        if (sscanf(s, "vxlan vlandel %i %i", &vlan, &vp) == 2) {
            rv = bcm_vlan_gport_delete(HWBCM_UNIT, (bcm_vlan_t)vlan, (bcm_gport_t)vp);
            printf("hw: vxlan vlandel vlan %d vp 0x%x rv %d (%s)\n", vlan, vp, rv, bcm_errmsg(rv));
            fflush(stdout);
            return rv < 0 ? BCM_E_FAIL : 0;
        }
    }
    if (sscanf(s, "vxlan vpdel %i %i", &vpn, &vp) == 2) {
        rv = hwbcm_vxlan_port_delete(vpn, vp);
        printf("hw: vxlan vpdel vpn 0x%x vp 0x%x rv %d (%s)\n", vpn, vp, rv, bcm_errmsg(rv));
        fflush(stdout);
        return rv < 0 ? BCM_E_FAIL : 0;
    }
    printf("hw: unknown command \"%s\"\n", s);
    fflush(stdout);
    return BCM_E_PARAM;
}

static int hwbcm_shell_cmd(const char *cmd)
{
    char buf[HWBCM_SHELL_CMD_MAX];
    cmd_result_t cr;

    if (strncmp(cmd, "hw ", 3) == 0) {
        return hwbcm_shell_hw_cmd(cmd + 3);
    }

    sal_strncpy(buf, cmd, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    cr = sh_process_command(HWBCM_UNIT, buf);
    if (cr != CMD_OK) {
        HWBCM_LOGE("diag shell \"%s\" failed: %d", cmd, (int)cr);
        return BCM_E_FAIL;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* debug shell socket                                                       */
/*                                                                          */
/* One command per connection on a Unix stream socket. The command's        */
/* stdout/stderr (diag shell cli_out goes through the console sink on fd 1) */
/* are redirected to the client for the duration of the command.           */
/* Host usage: echo "ps xe0" | socat - UNIX-CONNECT:/var/run/hwbcm/shell.sock */
/* ------------------------------------------------------------------------ */

#define HWBCM_SHELL_SOCK_DIR  "/var/run/hwbcm"
#define HWBCM_SHELL_SOCK_PATH HWBCM_SHELL_SOCK_DIR "/shell.sock"

static int g_shell_sock_fd = -1;
static volatile int g_shell_sock_stop = 0;
static pthread_t g_shell_sock_thread;
static int g_shell_sock_started = 0;

static void shell_sock_serve(int client)
{
    char line[HWBCM_SHELL_CMD_MAX];
    size_t n = 0;
    int saved_out, saved_err;
    int rc;

    /* Read one line (or until EOF / buffer full). */
    while (n < sizeof(line) - 1) {
        ssize_t r = read(client, line + n, 1);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        if (line[n] == '\n') {
            break;
        }
        n++;
    }
    line[n] = '\0';
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) {
        line[--n] = '\0';
    }
    if (n == 0) {
        return;
    }

    /* Commands that switch the SDK shell into an interactive mode read from
     * syncd's own stdin (a tty in the container), so they would block this
     * thread for good (seen with a bare "cint" on 2026-11-24). Only allow
     * them with an argument, e.g. "cint /tmp/x.c". */
    {
        static const char *const interactive[] = { "cint", "shell", "sh", "debugshell", "bsh", NULL };
        const char *p = line;
        int i;

        while (*p == ' ' || *p == '\t') {
            p++;
        }
        for (i = 0; interactive[i] != NULL; i++) {
            size_t l = strlen(interactive[i]);
            if (strncasecmp(p, interactive[i], l) == 0 && p[l] == '\0') {
                const char msg[] = "refused: interactive command needs an argument (it would block on syncd's stdin)\n[hwbcm rc=-1]\n";
                ssize_t w = write(client, msg, sizeof(msg) - 1);
                (void)w;
                return;
            }
        }
    }

    pthread_mutex_lock(&g_shell_lock);
    if (g_shell_sock_stop || !g_inited || !SOC_UNIT_VALID(HWBCM_UNIT)) {
        pthread_mutex_unlock(&g_shell_lock);
        return;
    }
    fflush(stdout);
    fflush(stderr);
    saved_out = dup(1);
    saved_err = dup(2);
    if (saved_out < 0 || saved_err < 0) {
        pthread_mutex_unlock(&g_shell_lock);
        if (saved_out >= 0) close(saved_out);
        if (saved_err >= 0) close(saved_err);
        HWBCM_LOGE("shell socket: dup failed: %s", strerror(errno));
        return;
    }
    dup2(client, 1);
    dup2(client, 2);

    rc = hwbcm_shell_cmd(line);
    printf("[hwbcm rc=%d]\n", rc);

    fflush(stdout);
    fflush(stderr);
    dup2(saved_out, 1);
    dup2(saved_err, 2);
    close(saved_out);
    close(saved_err);
    pthread_mutex_unlock(&g_shell_lock);

    HWBCM_LOGD("shell socket: \"%s\" -> %d", line, rc);
}

static void *shell_sock_main(void *arg)
{
    (void)arg;
    while (!g_shell_sock_stop) {
        int client = accept(g_shell_sock_fd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!g_shell_sock_stop) {
                HWBCM_LOGW("shell socket: accept: %s", strerror(errno));
                usleep(200 * 1000);
            }
            continue;
        }
        shell_sock_serve(client);
        close(client);
    }
    return NULL;
}

static int shell_sock_start(void)
{
    struct sockaddr_un addr;
    int fd;
    pthread_attr_t attr;

    if (g_shell_sock_started) {
        return 0;
    }
    if (mkdir(HWBCM_SHELL_SOCK_DIR, 0750) < 0 && errno != EEXIST) {
        HWBCM_LOGW("shell socket: mkdir %s: %s", HWBCM_SHELL_SOCK_DIR, strerror(errno));
        return BCM_E_FAIL;
    }
    (void)unlink(HWBCM_SHELL_SOCK_PATH);

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        HWBCM_LOGW("shell socket: socket: %s", strerror(errno));
        return BCM_E_FAIL;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, HWBCM_SHELL_SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        HWBCM_LOGW("shell socket: bind %s: %s", HWBCM_SHELL_SOCK_PATH, strerror(errno));
        close(fd);
        return BCM_E_FAIL;
    }
    /* 0660 root; chmod after bind rather than touching the process umask. */
    (void)chmod(HWBCM_SHELL_SOCK_PATH, 0660);
    if (listen(fd, 4) < 0) {
        HWBCM_LOGW("shell socket: listen: %s", strerror(errno));
        close(fd);
        (void)unlink(HWBCM_SHELL_SOCK_PATH);
        return BCM_E_FAIL;
    }

    g_shell_sock_fd = fd;
    g_shell_sock_stop = 0;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&g_shell_sock_thread, &attr, shell_sock_main, NULL) != 0) {
        HWBCM_LOGW("shell socket: pthread_create: %s", strerror(errno));
        pthread_attr_destroy(&attr);
        close(fd);
        g_shell_sock_fd = -1;
        (void)unlink(HWBCM_SHELL_SOCK_PATH);
        return BCM_E_FAIL;
    }
    pthread_attr_destroy(&attr);
    g_shell_sock_started = 1;
    HWBCM_LOGI("shell socket ready at %s (one diag command per connection)", HWBCM_SHELL_SOCK_PATH);
    return 0;
}

static void shell_sock_stop(void)
{
    if (!g_shell_sock_started) {
        return;
    }
    g_shell_sock_stop = 1;
    if (g_shell_sock_fd >= 0) {
        shutdown(g_shell_sock_fd, SHUT_RDWR);
        close(g_shell_sock_fd);
        g_shell_sock_fd = -1;
    }
    (void)unlink(HWBCM_SHELL_SOCK_PATH);
    /* Wait for any in-flight shell_sock_serve() command to finish before
     * shutdown tears down SOC_CONTROL(unit). Once g_shell_sock_stop is 1,
     * no new command will enter hwbcm_shell_cmd(). */
    pthread_mutex_lock(&g_shell_lock);
    pthread_mutex_unlock(&g_shell_lock);
    g_shell_sock_started = 0;
}

static void hwbcm_port_map_build(void)
{
    int unit = HWBCM_UNIT;
    soc_info_t *si = &SOC_INFO(unit);
    bcm_port_t port;

    g_fp_count = 0;
    PBMP_PORT_ITER(unit, port) {
        if (IS_CPU_PORT(unit, port) || IS_LB_PORT(unit, port)) {
            continue;
        }
        if (!port_valid(port) || g_fp_count >= HWBCM_PORT_TBL) {
            continue;
        }
        g_fp_logical[g_fp_count] = port;
        g_fp_phys[g_fp_count] = si->port_l2p_mapping[port];
        g_fp_count++;
    }

    if (g_fp_count == 0) {
        /* Fallback: parse portmap_<logical>=<phys>:<speed> from the config. */
        int lp;
        HWBCM_LOGW("SOC_INFO has no front panel ports; parsing portmap_* properties");
        for (lp = 1; lp < HWBCM_PORT_TBL && g_fp_count < HWBCM_PORT_TBL; lp++) {
            char *s = soc_property_port_get_str(unit, lp, spn_PORTMAP);
            if (s == NULL) {
                continue;
            }
            g_fp_logical[g_fp_count] = lp;
            g_fp_phys[g_fp_count] = (int)strtol(s, NULL, 0);
            g_fp_count++;
        }
    }
    HWBCM_LOGI("port map: %d front panel ports", g_fp_count);
}

/* ------------------------------------------------------------------------ */
/* load-balancing hash (RTAG7)                                              */
/* ------------------------------------------------------------------------ */

/* The SDK default leaves ECMP on the legacy hash with only the source IP as
 * input (HASH_CONTROL ECMP_HASH_USE_DIP=0, USE_TCP_UDP_PORTS=0), every RTAG7
 * field bitmap at 0 (so the VXLAN entropy source port is always 0) and
 * RTAG7_HASH_SEL.USE_FLOW_SEL_ECMP=1. All three are fixed here. */

static int hash_sc(const char *what, bcm_switch_control_t ctl, int val)
{
    int rv = bcm_switch_control_set(HWBCM_UNIT, ctl, val);

    if (rv == BCM_E_UNAVAIL) {
        HWBCM_LOGW("hash: %s unavailable on this chip", what);
        return 0;
    }
    if (BCM_FAILURE(rv)) {
        HWBCM_LOGE("hash: bcm_switch_control_set(%s, 0x%x): %s", what, val, bcm_errmsg(rv));
        return rv;
    }
    return 0;
}

#define HASH_SC(ctl, val) do { int _r = hash_sc(#ctl, (ctl), (int)(val)); if (_r) return _r; } while (0)

static int hash_fields_program(int set, uint32_t f)
{
    int common = 0, ip4, ip4l4, ip6, ip6l4, l2;

    if (f & HWBCM_HASH_F_IN_PORT) {
        common |= BCM_HASH_FIELD_SRCMOD | BCM_HASH_FIELD_SRCPORT;
    }
    if (f & HWBCM_HASH_F_VLAN) {
        common |= BCM_HASH_FIELD_VLAN;
    }

    ip4 = common;
    ip6 = common;
    if (f & HWBCM_HASH_F_SRC_IP) {
        ip4 |= BCM_HASH_FIELD_IP4SRC_LO | BCM_HASH_FIELD_IP4SRC_HI;
        ip6 |= BCM_HASH_FIELD_IP6SRC_LO | BCM_HASH_FIELD_IP6SRC_HI;
    }
    if (f & HWBCM_HASH_F_DST_IP) {
        ip4 |= BCM_HASH_FIELD_IP4DST_LO | BCM_HASH_FIELD_IP4DST_HI;
        ip6 |= BCM_HASH_FIELD_IP6DST_LO | BCM_HASH_FIELD_IP6DST_HI;
    }
    if (f & HWBCM_HASH_F_PROTOCOL) {
        ip4 |= BCM_HASH_FIELD_PROTOCOL;
        ip6 |= BCM_HASH_FIELD_NXT_HDR;
    }
    ip4l4 = ip4;
    ip6l4 = ip6;
    if (f & HWBCM_HASH_F_L4_SRC) {
        ip4l4 |= BCM_HASH_FIELD_SRCL4;
        ip6l4 |= BCM_HASH_FIELD_SRCL4;
    }
    if (f & HWBCM_HASH_F_L4_DST) {
        ip4l4 |= BCM_HASH_FIELD_DSTL4;
        ip6l4 |= BCM_HASH_FIELD_DSTL4;
    }

    /* Non-IP frames: the SAI field list is IP-centric (SONiC's default has no
     * MAC fields), so unless MAC/ethertype fields were asked for explicitly,
     * hash non-IP traffic on the MACs, ethertype and VLAN instead of the
     * ingress port alone. */
    if (f & (HWBCM_HASH_F_SRC_MAC | HWBCM_HASH_F_DST_MAC | HWBCM_HASH_F_ETHERTYPE)) {
        l2 = common;
        if (f & HWBCM_HASH_F_SRC_MAC) {
            l2 |= BCM_HASH_FIELD_MACSA_LO | BCM_HASH_FIELD_MACSA_MI | BCM_HASH_FIELD_MACSA_HI;
        }
        if (f & HWBCM_HASH_F_DST_MAC) {
            l2 |= BCM_HASH_FIELD_MACDA_LO | BCM_HASH_FIELD_MACDA_MI | BCM_HASH_FIELD_MACDA_HI;
        }
        if (f & HWBCM_HASH_F_ETHERTYPE) {
            l2 |= BCM_HASH_FIELD_ETHER_TYPE;
        }
    } else {
        l2 = common | BCM_HASH_FIELD_VLAN | BCM_HASH_FIELD_ETHER_TYPE |
             BCM_HASH_FIELD_MACSA_LO | BCM_HASH_FIELD_MACSA_MI | BCM_HASH_FIELD_MACSA_HI |
             BCM_HASH_FIELD_MACDA_LO | BCM_HASH_FIELD_MACDA_MI | BCM_HASH_FIELD_MACDA_HI;
    }

    if (set == HWBCM_HASH_ECMP) {
        HASH_SC(bcmSwitchHashIP4Field0, ip4);
        HASH_SC(bcmSwitchHashIP4TcpUdpField0, ip4l4);
        HASH_SC(bcmSwitchHashIP4TcpUdpPortsEqualField0, ip4l4);
        HASH_SC(bcmSwitchHashIP6Field0, ip6);
        HASH_SC(bcmSwitchHashIP6TcpUdpField0, ip6l4);
        HASH_SC(bcmSwitchHashIP6TcpUdpPortsEqualField0, ip6l4);
        HASH_SC(bcmSwitchHashL2Field0, l2);
    } else {
        HASH_SC(bcmSwitchHashIP4Field1, ip4);
        HASH_SC(bcmSwitchHashIP4TcpUdpField1, ip4l4);
        HASH_SC(bcmSwitchHashIP4TcpUdpPortsEqualField1, ip4l4);
        HASH_SC(bcmSwitchHashIP6Field1, ip6);
        HASH_SC(bcmSwitchHashIP6TcpUdpField1, ip6l4);
        HASH_SC(bcmSwitchHashIP6TcpUdpPortsEqualField1, ip6l4);
        HASH_SC(bcmSwitchHashL2Field1, l2);
    }
    HWBCM_LOGI("hash set %d: fields 0x%x -> ip4 0x%x ip4l4 0x%x ip6 0x%x ip6l4 0x%x l2 0x%x",
               set, f, ip4, ip4l4, ip6, ip6l4, l2);
    return 0;
}

static uint32_t g_hash_fields[2] = { HWBCM_HASH_F_DEFAULT, HWBCM_HASH_F_DEFAULT };
static uint32_t g_hash_seed[2]   = { 0x5a5a5a5au, 0xa5a5a5a5u };

static int hash_init(void)
{
    int rv, ctl = 0;

    /* Hash A (ECMP, VXLAN entropy) and hash B (LAG). */
    HASH_SC(bcmSwitchHashSeed0, 0x5a5a5a5a);
    HASH_SC(bcmSwitchHashSeed1, 0xa5a5a5a5);
    g_hash_seed[HWBCM_HASH_ECMP] = 0x5a5a5a5au;
    g_hash_seed[HWBCM_HASH_LAG]  = 0xa5a5a5a5u;
    HASH_SC(bcmSwitchHashField0PreProcessEnable, 1);
    HASH_SC(bcmSwitchHashField1PreProcessEnable, 1);
    HASH_SC(bcmSwitchHashField0Config, BCM_HASH_FIELD_CONFIG_CRC32LO);
    HASH_SC(bcmSwitchHashField0Config1, BCM_HASH_FIELD_CONFIG_CRC32HI);
    HASH_SC(bcmSwitchHashField1Config, BCM_HASH_FIELD_CONFIG_CRC16CCITT);
    HASH_SC(bcmSwitchHashField1Config1, BCM_HASH_FIELD_CONFIG_CRC16XOR8);

    rv = hash_fields_program(HWBCM_HASH_ECMP, HWBCM_HASH_F_DEFAULT);
    if (rv) {
        return rv;
    }
    g_hash_fields[HWBCM_HASH_ECMP] = HWBCM_HASH_F_DEFAULT;
    rv = hash_fields_program(HWBCM_HASH_LAG, HWBCM_HASH_F_DEFAULT);
    if (rv) {
        return rv;
    }
    g_hash_fields[HWBCM_HASH_LAG] = HWBCM_HASH_F_DEFAULT;

    /* Port-based (not flow-table) hash selection, then the bit offsets into
     * {A0(16), B0(16), ...}: ECMP on A0, trunks on B0. */
    HASH_SC(bcmSwitchHashUseFlowSelEcmp, 0);
    HASH_SC(bcmSwitchHashUseFlowSelTrunkUc, 0);
    HASH_SC(bcmSwitchECMPHashSet0Offset, 0);
    HASH_SC(bcmSwitchTrunkHashSet0UnicastOffset, 16);
    HASH_SC(bcmSwitchTrunkHashSet0NonUnicastOffset, 16);

    /* Finally move ECMP and non-unicast trunk hashing onto RTAG7. The legacy
     * DIP/L4 bits are set too, in case anything still uses the legacy hash. */
    rv = bcm_switch_control_get(HWBCM_UNIT, bcmSwitchHashControl, &ctl);
    if (BCM_FAILURE(rv)) {
        HWBCM_LOGE("hash: bcm_switch_control_get(HashControl): %s", bcm_errmsg(rv));
        return rv;
    }
    ctl |= BCM_HASH_CONTROL_ECMP_ENHANCE | BCM_HASH_CONTROL_TRUNK_NUC_ENHANCE |
           BCM_HASH_CONTROL_MULTIPATH_L4PORTS | BCM_HASH_CONTROL_MULTIPATH_DIP;
    HASH_SC(bcmSwitchHashControl, ctl);
    HWBCM_LOGI("hash: RTAG7 enabled for ECMP/trunk (HashControl 0x%x)", ctl);
    return 0;
}

int hwbcm_hash_fields_set(int set, uint32_t fields)
{
    int rv;

    if (set != HWBCM_HASH_ECMP && set != HWBCM_HASH_LAG) {
        return BCM_E_PARAM;
    }
    pthread_mutex_lock(&g_lock);
    rv = g_inited ? hash_fields_program(set, fields) : BCM_E_INIT;
    if (rv == 0) {
        g_hash_fields[set] = fields;
    }
    pthread_mutex_unlock(&g_lock);
    return rv;
}

int hwbcm_hash_seed_set(int set, uint32_t seed)
{
    int rv;

    if (set != HWBCM_HASH_ECMP && set != HWBCM_HASH_LAG) {
        return BCM_E_PARAM;
    }
    pthread_mutex_lock(&g_lock);
    if (!g_inited) {
        rv = BCM_E_INIT;
    } else {
        rv = hash_sc(set == HWBCM_HASH_ECMP ? "HashSeed0" : "HashSeed1",
                     set == HWBCM_HASH_ECMP ? bcmSwitchHashSeed0 : bcmSwitchHashSeed1,
                     (int)seed);
        if (rv == 0) {
            g_hash_seed[set] = seed;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rv;
}

void hwbcm_hash_wb_get(hwbcm_hash_wb_state_t *out)
{
    if (out == NULL) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    out->fields[0] = g_hash_fields[0];
    out->fields[1] = g_hash_fields[1];
    out->seed[0]   = g_hash_seed[0];
    out->seed[1]   = g_hash_seed[1];
    pthread_mutex_unlock(&g_lock);
}

void hwbcm_hash_wb_restore(const hwbcm_hash_wb_state_t *in)
{
    int hw_s0 = 0, hw_s1 = 0, hw_ctl = 0;

    if (in == NULL) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_hash_fields[0] = in->fields[0];
    g_hash_fields[1] = in->fields[1];
    g_hash_seed[0]   = in->seed[0];
    g_hash_seed[1]   = in->seed[1];
    if (g_inited) {
        if (BCM_SUCCESS(bcm_switch_control_get(HWBCM_UNIT, bcmSwitchHashSeed0, &hw_s0)) &&
            (uint32_t)hw_s0 != g_hash_seed[0]) {
            HWBCM_LOGW("wb hash: HW HashSeed0 0x%08x != saved 0x%08x (HW left untouched)",
                       (unsigned)hw_s0, (unsigned)g_hash_seed[0]);
        }
        if (BCM_SUCCESS(bcm_switch_control_get(HWBCM_UNIT, bcmSwitchHashSeed1, &hw_s1)) &&
            (uint32_t)hw_s1 != g_hash_seed[1]) {
            HWBCM_LOGW("wb hash: HW HashSeed1 0x%08x != saved 0x%08x (HW left untouched)",
                       (unsigned)hw_s1, (unsigned)g_hash_seed[1]);
        }
        if (BCM_SUCCESS(bcm_switch_control_get(HWBCM_UNIT, bcmSwitchHashControl, &hw_ctl))) {
            HWBCM_LOGI("wb hash: restored ECMP(fields=0x%x seed=0x%08x) LAG(fields=0x%x seed=0x%08x) HW HashControl=0x%x",
                       (unsigned)g_hash_fields[0], (unsigned)g_hash_seed[0],
                       (unsigned)g_hash_fields[1], (unsigned)g_hash_seed[1],
                       (unsigned)hw_ctl);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

#undef HASH_SC

static long mono_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

#ifdef BCM_WARM_BOOT_SUPPORT

static int hwbcm_scache_read(int unit, uint8 *buf, int offset, int nbytes)
{
    int rv = SOC_E_NONE;

    if (unit != HWBCM_UNIT || buf == NULL || offset < 0 || nbytes < 0) {
        return SOC_E_PARAM;
    }
    pthread_mutex_lock(&g_scache_lock);
    if (g_scache_buf == NULL || g_scache_read_limit == 0 ||
        (size_t)offset + (size_t)nbytes > g_scache_read_limit) {
        rv = SOC_E_MEMORY;
    } else if (nbytes > 0) {
        memcpy(buf, g_scache_buf + (size_t)offset, (size_t)nbytes);
    }
    pthread_mutex_unlock(&g_scache_lock);
    return rv;
}

static int hwbcm_scache_write(int unit, uint8 *buf, int offset, int nbytes)
{
    int rv = SOC_E_NONE;

    if (unit != HWBCM_UNIT || buf == NULL || offset < 0 || nbytes < 0) {
        return SOC_E_PARAM;
    }
    pthread_mutex_lock(&g_scache_lock);
    if (g_scache_buf == NULL || (size_t)offset + (size_t)nbytes > g_scache_cap) {
        rv = SOC_E_MEMORY;
    } else if (nbytes > 0) {
        size_t end = (size_t)offset + (size_t)nbytes;
        memcpy(g_scache_buf + (size_t)offset, buf, (size_t)nbytes);
        if (end > g_scache_written_end) {
            g_scache_written_end = end;
        }
    }
    pthread_mutex_unlock(&g_scache_lock);
    return rv;
}

static void hwbcm_scache_cleanup(void)
{
    pthread_mutex_lock(&g_scache_lock);
    free(g_scache_buf);
    g_scache_buf = NULL;
    g_scache_cap = 0;
    g_scache_read_limit = 0;
    g_scache_written_end = 0;
    g_scache_configured = 0;
    g_scache_path[0] = '\0';
    pthread_mutex_unlock(&g_scache_lock);
}

static int hwbcm_scache_setup(int unit, int warm_boot, const char *scache_file)
{
    uint32 cfg_sz;
    size_t cap;
    size_t file_sz = 0;
    uint8_t *buf = NULL;
    int rv;

    hwbcm_scache_cleanup();

    cfg_sz = (uint32)soc_property_get(unit, spn_STABLE_SIZE, 0);
    cap = cfg_sz ? (size_t)cfg_sz : (size_t)HWBCM_SCACHE_DEFAULT_SIZE;
    if (cap < HWBCM_SCACHE_MIN_SIZE) {
        cap = HWBCM_SCACHE_MIN_SIZE;
    }
    if (cap > HWBCM_SCACHE_MAX_SIZE) {
        cap = HWBCM_SCACHE_MAX_SIZE;
    }

    if (warm_boot) {
        int fd;
        struct stat st;
        size_t done = 0;

        if (scache_file == NULL || scache_file[0] == '\0') {
            HWBCM_LOGE("wb scache: warm boot requested without scache file path");
            return BCM_E_PARAM;
        }
        fd = open(scache_file, O_RDONLY);
        if (fd < 0) {
            HWBCM_LOGE("wb scache: open(%s) failed: %s", scache_file, strerror(errno));
            return BCM_E_NOT_FOUND;
        }
        if (fstat(fd, &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > HWBCM_SCACHE_MAX_SIZE) {
            HWBCM_LOGE("wb scache: invalid size %lld for %s", (long long)st.st_size, scache_file);
            close(fd);
            return BCM_E_FAIL;
        }
        file_sz = (size_t)st.st_size;
        if (file_sz > cap) {
            cap = (file_sz + 4095u) & ~4095u;
            if (cap > HWBCM_SCACHE_MAX_SIZE) {
                cap = HWBCM_SCACHE_MAX_SIZE;
            }
        }
        buf = (uint8_t *)calloc(1, cap);
        if (buf == NULL) {
            close(fd);
            return BCM_E_MEMORY;
        }
        while (done < file_sz) {
            ssize_t r = read(fd, buf + done, file_sz - done);
            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }
                HWBCM_LOGE("wb scache: read(%s) failed: %s", scache_file, strerror(errno));
                free(buf);
                close(fd);
                return BCM_E_FAIL;
            }
            if (r == 0) {
                break;
            }
            done += (size_t)r;
        }
        close(fd);
        if (done != file_sz) {
            HWBCM_LOGE("wb scache: short read on %s (%zu/%zu)", scache_file, done, file_sz);
            free(buf);
            return BCM_E_FAIL;
        }
    } else {
        buf = (uint8_t *)calloc(1, cap);
        if (buf == NULL) {
            return BCM_E_MEMORY;
        }
    }

    pthread_mutex_lock(&g_scache_lock);
    g_scache_buf = buf;
    g_scache_cap = cap;
    g_scache_read_limit = warm_boot ? file_sz : 0;
    g_scache_written_end = warm_boot ? file_sz : 0;
    if (scache_file != NULL && scache_file[0] != '\0') {
        snprintf(g_scache_path, sizeof(g_scache_path), "%s", scache_file);
    } else {
        g_scache_path[0] = '\0';
    }
    pthread_mutex_unlock(&g_scache_lock);

    rv = soc_switch_stable_register(unit, hwbcm_scache_read, hwbcm_scache_write, NULL, NULL);
    if (SOC_FAILURE(rv)) {
        HWBCM_LOGE("wb scache: soc_switch_stable_register: %s (%d)", bcm_errmsg(rv), rv);
        hwbcm_scache_cleanup();
        return rv;
    }
    rv = soc_stable_set(unit, BCM_SWITCH_STABLE_APPLICATION, 0);
    if (SOC_FAILURE(rv)) {
        HWBCM_LOGE("wb scache: soc_stable_set(APPLICATION): %s (%d)", bcm_errmsg(rv), rv);
        hwbcm_scache_cleanup();
        return rv;
    }
    rv = soc_stable_size_set(unit, (int)cap);
    if (SOC_FAILURE(rv)) {
        HWBCM_LOGE("wb scache: soc_stable_size_set(%zu): %s (%d)", cap, bcm_errmsg(rv), rv);
        hwbcm_scache_cleanup();
        return rv;
    }

    g_scache_configured = 1;
    HWBCM_LOGI("wb scache: configured mode=%s cap=%zu bytes%s%s",
               warm_boot ? "warm" : "cold", cap,
               warm_boot ? " loaded=" : "",
               warm_boot ? scache_file : "");
    return 0;
}

static void fsync_parent_dir(const char *path)
{
    char dir[512];
    char *slash;
    int dfd;

    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if (slash == NULL) {
        strcpy(dir, ".");
    } else if (slash == dir) {
        dir[1] = '\0';
    } else {
        *slash = '\0';
    }
    dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        (void)fsync(dfd);
        close(dfd);
    }
}

static int hwbcm_scache_flush_file(const char *path, size_t *bytes_written)
{
    char tmp[528];
    int fd;
    int used = 0;
    size_t write_len = 0;
    size_t done = 0;

    if (path == NULL || path[0] == '\0') {
        HWBCM_LOGE("wb scache: no output path for flush");
        return BCM_E_PARAM;
    }

    (void)soc_stable_used_get(HWBCM_UNIT, &used);

    pthread_mutex_lock(&g_scache_lock);
    if (g_scache_buf == NULL || !g_scache_configured) {
        pthread_mutex_unlock(&g_scache_lock);
        HWBCM_LOGE("wb scache: not configured");
        return BCM_E_INIT;
    }
    write_len = g_scache_written_end;
    if (used > 0 && (size_t)used + HWBCM_SCACHE_NULL_PAD > write_len) {
        write_len = (size_t)used + HWBCM_SCACHE_NULL_PAD;
    }
    if (write_len > g_scache_cap) {
        write_len = g_scache_cap;
    }
    if (write_len == 0) {
        pthread_mutex_unlock(&g_scache_lock);
        HWBCM_LOGE("wb scache: nothing written to scache buffer (used=%d)", used);
        return BCM_E_EMPTY;
    }

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0640);
    if (fd < 0) {
        int e = errno;
        pthread_mutex_unlock(&g_scache_lock);
        HWBCM_LOGE("wb scache: open(%s) failed: %s", tmp, strerror(e));
        return BCM_E_FAIL;
    }
    while (done < write_len) {
        ssize_t w = write(fd, g_scache_buf + done, write_len - done);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            int e = errno;
            close(fd);
            (void)unlink(tmp);
            pthread_mutex_unlock(&g_scache_lock);
            HWBCM_LOGE("wb scache: write(%s) failed: %s", tmp, strerror(e));
            return BCM_E_FAIL;
        }
        done += (size_t)w;
    }
    if (fsync(fd) != 0) {
        int e = errno;
        close(fd);
        (void)unlink(tmp);
        pthread_mutex_unlock(&g_scache_lock);
        HWBCM_LOGE("wb scache: fsync(%s) failed: %s", tmp, strerror(e));
        return BCM_E_FAIL;
    }
    close(fd);
    if (rename(tmp, path) != 0) {
        int e = errno;
        (void)unlink(tmp);
        pthread_mutex_unlock(&g_scache_lock);
        HWBCM_LOGE("wb scache: rename(%s -> %s) failed: %s", tmp, path, strerror(e));
        return BCM_E_FAIL;
    }
    pthread_mutex_unlock(&g_scache_lock);

    fsync_parent_dir(path);
    if (bytes_written != NULL) {
        *bytes_written = write_len;
    }
    HWBCM_LOGI("wb scache: committed %zu bytes (sdk used=%d) -> %s", write_len, used, path);
    return 0;
}

/* ---- Phase 4: SONiC warm-reboot (kexec) ------------------------------------
 *
 * Sidecar files next to the scache (base = scache path minus ".scache", i.e.
 * /var/warmboot/sai-warmboot.bin = /host/warmboot/sai-warmboot.bin on the host):
 *   <base>.bootid    boot_id of the kernel that wrote the scache (hwbcm_warm_sync)
 *   <base>.dma_idle  verdict of a kexec-mode warm deinit, read by the platform
 *                    warm-reboot_plugin: "ok|busy boot=<id> ..." */
static void wb_sidecar_path(char *out, size_t n, const char *scache, const char *suffix)
{
    static const char ext[] = ".scache";
    size_t elen = sizeof(ext) - 1;
    size_t len;

    out[0] = '\0';
    if (scache == NULL || scache[0] == '\0') {
        return;
    }
    len = strlen(scache);
    if (len > elen && strcmp(scache + len - elen, ext) == 0) {
        len -= elen;
    }
    snprintf(out, n, "%.*s%s", (int)len, scache, suffix);
}

static int read_line_file(const char *path, char *buf, size_t n)
{
    FILE *f;
    size_t l;

    buf[0] = '\0';
    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    if (fgets(buf, (int)n, f) == NULL) {
        fclose(f);
        buf[0] = '\0';
        return -1;
    }
    fclose(f);
    l = strlen(buf);
    while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r' || buf[l - 1] == ' ')) {
        buf[--l] = '\0';
    }
    return (int)l;
}

static int write_file_atomic(const char *path, const char *content)
{
    char tmp[600];
    size_t len = strlen(content);
    ssize_t w;
    int fd;

    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return -1;
    }
    w = write(fd, content, len);
    if (w < 0 || (size_t)w != len || fsync(fd) != 0) {
        close(fd);
        (void)unlink(tmp);
        return -1;
    }
    close(fd);
    if (rename(tmp, path) != 0) {
        (void)unlink(tmp);
        return -1;
    }
    fsync_parent_dir(path);
    return 0;
}

static void boot_id_get(char *buf, size_t n)
{
    if (read_line_file("/proc/sys/kernel/random/boot_id", buf, n) <= 0) {
        buf[0] = '\0';
    }
}

/* Netdevs the kernel KNET module has right now, from /proc/bcm/knet/link
 * ("Software link status:" then one "  <name> up|down" line per netif).
 * -1 if the file cannot be read or has no header. Read before soc_init: the
 * KCOM channel only opens inside soc_init (soc_dma_attach -> soc_knet_init). */
static int knet_proc_netif_count(void)
{
    FILE *f = fopen("/proc/bcm/knet/link", "r");
    char line[256];
    int n = 0, header = 0;

    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "Software link status", 20) == 0) {
            header = 1;
            continue;
        }
        if (header && line[0] == ' ' && line[1] == ' ' &&
            line[2] != '\0' && line[2] != '\n' && line[2] != ' ') {
            n++;
        }
    }
    fclose(f);
    return header ? n : -1;
}

/* Kernel KNET verdict for a warm init (computed before soc_init):
 *   LIVE       netifs present, same boot, rings not aborted -> keep everything
 *              (syncd warm restart, Case A, exactly as before phase 4)
 *   DMA_RESET  netifs present but a kexec-mode deinit aborted the rings on this
 *              boot (or boot_id changed although netifs exist: inconsistent)
 *              -> KCOM HW_INIT, keep netifs and filters (Case A)
 *   FRESH      module has 0 netifs (kexec, or modules reloaded)
 *              -> cold-style KNET bring-up: HW_INIT, DefaultRxAPI (Case B)
 *   UNKNOWN    /proc/bcm/knet/link unreadable -> HW_INIT (safe for both
 *              cases), keep filters; hwWarmRecoverKnet decides on the live
 *              netif table */
static int g_knet_state = -1;
/* Set once a warm init in this process overrode the two KNET properties;
 * a cold init/deinit afterwards puts them back to the SDK default (0). */
static int g_knet_props_overridden = 0;

static void knet_props_cold_defaults(const char *who)
{
    if (g_knet_props_overridden) {
        (void)sal_config_set((char *)spn_WARMBOOT_KNET_SHUTDOWN_MODE, (char *)"0");
        (void)sal_config_set((char *)spn_KNET_FILTER_PERSIST, (char *)"0");
        g_knet_props_overridden = 0;
        HWBCM_LOGI("%s: warmboot_knet_shutdown_mode/knet_filter_persist back to 0 (cold semantics)", who);
    }
}

static const char *knet_state_name(int s)
{
    switch (s) {
    case HWBCM_KNET_LIVE:      return "live";
    case HWBCM_KNET_DMA_RESET: return "dma-reset";
    case HWBCM_KNET_FRESH:     return "fresh";
    case HWBCM_KNET_UNKNOWN:   return "unknown";
    default:                   return "none";
    }
}

int hwbcm_warm_knet_probe(const char *scache_file)
{
    char path[600];
    char cur[64], saved[64], marker[256];
    const char *mb;
    int nnetif, bootid_changed, marker_same_boot = 0, state;

    nnetif = knet_proc_netif_count();
    boot_id_get(cur, sizeof(cur));
    wb_sidecar_path(path, sizeof(path), scache_file, ".bootid");
    (void)read_line_file(path, saved, sizeof(saved));
    bootid_changed = (cur[0] != '\0' && saved[0] != '\0' && strcmp(cur, saved) != 0);

    wb_sidecar_path(path, sizeof(path), scache_file, ".dma_idle");
    if (read_line_file(path, marker, sizeof(marker)) > 0 && cur[0] != '\0' &&
        (mb = strstr(marker, "boot=")) != NULL &&
        strncmp(mb + 5, cur, strlen(cur)) == 0) {
        marker_same_boot = 1;
    }

    if (nnetif < 0) {
        state = HWBCM_KNET_UNKNOWN;
    } else if (nnetif == 0) {
        state = HWBCM_KNET_FRESH;
    } else if (bootid_changed) {
        HWBCM_LOGE("wb: boot_id changed (%s -> %s) but KNET still has %d netif(s): "
                   "stale .bootid? treating as dma-reset", saved, cur, nnetif);
        state = HWBCM_KNET_DMA_RESET;
    } else if (marker_same_boot) {
        state = HWBCM_KNET_DMA_RESET;
    } else {
        state = HWBCM_KNET_LIVE;
    }
    g_knet_state = state;
    HWBCM_LOGI("wb: knet fresh=%d state=%s (bootid %s saved %s%s, netifs %d, dma marker %s)",
               state == HWBCM_KNET_FRESH ? 1 : 0, knet_state_name(state),
               cur[0] ? cur : "?", saved[0] ? saved : "none",
               bootid_changed ? " CHANGED" : "", nnetif,
               marker[0] ? marker : "none");
    return state;
}

/* CMICm DMA engines that can write host memory (offsets from
 * include/soc/mcm/cmicm.h, per CMC +0x1000; deploy/platform/warm-reboot_plugin
 * checks the same bits through BAR0):
 *   CHy_DMA_CTRL   0x31140+4y  bit1 PKTDMA_ENABLE   (reported, not busy alone:
 *                                                    an enabled but finished
 *                                                    chain does not restart)
 *   DMA_STAT       0x31150     bit 8+y CHy ACTIVE   -> busy
 *   FIFO_CHy_RD_DMA_CFG 0x312c0+4y bit0 ENABLE      -> busy (L2 mod FIFO etc.)
 *   STAT_DMA_CFG   0x311c4     bit1 EN              (reported only, stays set)
 *   STAT_DMA_STAT  0x311dc     bit3 ACTIVE          -> busy
 *   SBUSDMA_CHy_STATUS 0x3161c+0x50y bit11 ACTIVE   -> busy */
#define WB_CMC_NUM 3
static uint32 cmic_pktdma_active_mask(int unit)
{
    uint32 m = 0;
    int cmc;

    for (cmc = 0; cmc < WB_CMC_NUM; cmc++) {
        m |= ((soc_pci_read(unit, 0x31150u + 0x1000u * (uint32)cmc) >> 8) & 0xfu) << (cmc * 4);
    }
    return m;
}

static int cmic_dma_busy(int unit, char *desc, size_t n)
{
    size_t off = 0;
    uint32 v, pkt_en = 0, st_en = 0;
    int cmc, ch, nbusy = 0;

#define WB_APPEND(...) do { \
        if (off < n) { int _w = snprintf(desc + off, n - off, __VA_ARGS__); \
                       if (_w > 0) off += (size_t)_w; } } while (0)

    desc[0] = '\0';
    for (cmc = 0; cmc < WB_CMC_NUM; cmc++) {
        uint32 base = 0x1000u * (uint32)cmc;

        v = soc_pci_read(unit, 0x31150u + base);
        for (ch = 0; ch < 4; ch++) {
            if (v & (0x100u << ch)) {
                WB_APPEND(" pktdma%d.%d", cmc, ch);
                nbusy++;
            }
            if (soc_pci_read(unit, 0x31140u + base + 4u * (uint32)ch) & 0x2u) {
                pkt_en |= 1u << (cmc * 4 + ch);
            }
            if (soc_pci_read(unit, 0x312c0u + base + 4u * (uint32)ch) & 0x1u) {
                WB_APPEND(" fifodma%d.%d", cmc, ch);
                nbusy++;
            }
        }
        if (soc_pci_read(unit, 0x311c4u + base) & 0x2u) {
            st_en |= 1u << cmc;
        }
        if (soc_pci_read(unit, 0x311dcu + base) & 0x8u) {
            WB_APPEND(" statdma%d", cmc);
            nbusy++;
        }
        for (ch = 0; ch < 3; ch++) {
            if (soc_pci_read(unit, 0x3161cu + base + 0x50u * (uint32)ch) & 0x800u) {
                WB_APPEND(" sbusdma%d.%d", cmc, ch);
                nbusy++;
            }
        }
    }
    WB_APPEND(" pktdma_en=0x%03x statdma_en=0x%x", pkt_en, st_en);
#undef WB_APPEND
    return nbusy;
}

#endif /* BCM_WARM_BOOT_SUPPORT */

int hwbcm_warm_knet_state(void)
{
#ifdef BCM_WARM_BOOT_SUPPORT
    return g_knet_state;
#else
    return -1;
#endif
}

int hwbcm_warm_knet_fresh(void)
{
    return hwbcm_warm_knet_state() == HWBCM_KNET_FRESH ? 1 : 0;
}

#ifndef BCM_WARM_BOOT_SUPPORT
int hwbcm_warm_knet_probe(const char *scache_file)
{
    (void)scache_file;
    return -1;
}
#endif

/* On warm boot, pre-populate g_sgmii_applied[] for every front port already
 * linked up at <=1G in SGMII mode BEFORE linkscan is enabled, so the linkscan
 * handler does not re-apply SGMII and bounce the 1G link. */
static void hwbcm_sgmii_warm_probe(int unit)
{
    int i, nmarked = 0;
    /* Stamp "now": a link-down right after warm init is then treated as our
     * own bounce (flag kept) and the following up is not re-applied. The old
     * now-RATE_LIMIT stamp cleared the flag on the first down and every up
     * re-applied SGMII, bouncing the copper link again (flap loop). */
    long ts = mono_seconds();
    if (ts <= 0) {
        ts = 1;
    }

    for (i = 0; i < g_fp_count; i++) {
        int port = g_fp_logical[i];
        int link = 0, speed = 0;
        bcm_port_if_t intf = BCM_PORT_IF_COUNT;

        if (!port_valid(port)) {
            continue;
        }
        if (BCM_FAILURE(bcm_port_link_status_get(unit, port, &link)) ||
            link != BCM_PORT_LINK_STATUS_UP) {
            continue;
        }
        if (BCM_FAILURE(bcm_port_speed_get(unit, port, &speed)) ||
            speed <= 0 || speed > 1000) {
            continue;
        }
        if (BCM_SUCCESS(bcm_port_interface_get(unit, port, &intf)) &&
            intf == BCM_PORT_IF_SGMII) {
            g_sgmii_applied[port] = 1;
            g_sgmii_applied_at[port] = ts;
            nmarked++;
            HWBCM_LOGI("wb sgmii: port %d already UP at %d Mbps (SGMII), marked applied",
                       port, speed);
        }
    }
    HWBCM_LOGI("wb sgmii: probed %d front ports, %d active 1G SGMII port(s) preserved",
               g_fp_count, nmarked);
}

static epc_link_bmap_entry_t g_epc_saved;
static int g_epc_saved_valid = 0;

static void epc_link_save(int unit)
{
    g_epc_saved_valid = 0;
    if (!SOC_MEM_IS_VALID(unit, EPC_LINK_BMAPm)) {
        return;
    }
    if (SOC_SUCCESS(soc_mem_read(unit, EPC_LINK_BMAPm, MEM_BLOCK_ANY, 0, &g_epc_saved))) {
        g_epc_saved_valid = 1;
    }
}

static void epc_link_restore(int unit, const char *after)
{
    epc_link_bmap_entry_t cur;
    pbmp_t want, have;

    if (!g_epc_saved_valid ||
        SOC_FAILURE(soc_mem_read(unit, EPC_LINK_BMAPm, MEM_BLOCK_ANY, 0, &cur))) {
        return;
    }
    soc_mem_pbmp_field_get(unit, EPC_LINK_BMAPm, &g_epc_saved, PORT_BITMAPf, &want);
    soc_mem_pbmp_field_get(unit, EPC_LINK_BMAPm, &cur, PORT_BITMAPf, &have);
    if (SOC_PBMP_EQ(want, have)) {
        HWBCM_LOGI("hwbcm_init_ex[warm]: EPC_LINK_BMAP unchanged after %s", after);
        return;
    }
    if (SOC_FAILURE(soc_mem_write(unit, EPC_LINK_BMAPm, MEM_BLOCK_ALL, 0, &g_epc_saved))) {
        HWBCM_LOGE("hwbcm_init_ex[warm]: EPC_LINK_BMAP restore after %s failed", after);
        return;
    }
    HWBCM_LOGW("hwbcm_init_ex[warm]: EPC_LINK_BMAP was rewritten by %s, restored (front-port egress kept)", after);
}

int hwbcm_init_ex(const char *config_bcm, int warm_boot, const char *scache_file)
{
    int unit = HWBCM_UNIT;
    int rv;
    int i;
    bcm_port_t port;
    long t0 = mono_ms();

    pthread_mutex_lock(&g_lock);
    if (g_inited) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

#ifndef BCM_WARM_BOOT_SUPPORT
    if (warm_boot) {
        HWBCM_LOGE("hwbcm_init_ex: warm_boot requested but compiled without BCM_WARM_BOOT_SUPPORT");
        pthread_mutex_unlock(&g_lock);
        return BCM_E_UNAVAIL;
    }
#endif

    for (i = 0; i < HWBCM_PORT_TBL; i++) {
        g_desired_speed[i] = 0;
        g_sgmii_applied[i] = 0;
        g_sgmii_applied_at[i] = 0;
    }
    for (i = 0; i <= HWBCM_TRAP_STP; i++) {
        g_trap_l2cache_idx[i] = -1;
    }
    memset(g_knet, 0, sizeof(g_knet));
    g_warm_boot = 0;

    if (config_bcm == NULL || config_bcm[0] == '\0') {
        HWBCM_LOGE("hwbcm_init_ex: no config.bcm path");
        pthread_mutex_unlock(&g_lock);
        return BCM_E_PARAM;
    }

    if (warm_boot) {
        struct stat st;
        if (scache_file == NULL || scache_file[0] == '\0' ||
            stat(scache_file, &st) != 0 || st.st_size <= 0) {
            HWBCM_LOGE("hwbcm_init_ex: warm boot scache file missing or empty: %s",
                       scache_file ? scache_file : "(null)");
            pthread_mutex_unlock(&g_lock);
            return BCM_E_NOT_FOUND;
        }
    }

    if (!g_sal_sysconf_inited) {
        /* 1. config file, must precede sal_core_init */
        {
            size_t len = strlen(config_bcm);
            char *tmp = (char *)malloc(len + 5);
            if (tmp == NULL) {
                pthread_mutex_unlock(&g_lock);
                return BCM_E_MEMORY;
            }
            memcpy(tmp, config_bcm, len);
            memcpy(tmp + len, ".tmp", 5);
            rv = sal_config_file_set(config_bcm, tmp);
            free(tmp);
            if (rv < 0) {
                HWBCM_LOGE("sal_config_file_set(%s): %d", config_bcm, rv);
                pthread_mutex_unlock(&g_lock);
                return BCM_E_FAIL;
            }
        }
        HWBCM_LOGI("hwbcm_init_ex: mode=%s config=%s", warm_boot ? "WARM" : "COLD", config_bcm);

        /* 2./3. SAL */
        if (sal_core_init() < 0) {
            HWBCM_LOGE("sal_core_init failed");
            pthread_mutex_unlock(&g_lock);
            return BCM_E_FAIL;
        }
        if (sal_appl_init() < 0) {
            HWBCM_LOGE("sal_appl_init failed");
            pthread_mutex_unlock(&g_lock);
            return BCM_E_FAIL;
        }

        /* 4. KNET kcom vectors (knet_kcom_config in socdiag.c) + chip info */
        soc_knet_config(&g_knet_vect);
        var_set("kcom", "bcm-knet", 0, 0);
        soc_chip_info_vect_config(&g_chip_info_vect);

        /* 4b. Diag shell library bring-up */
        parse_user_var_get = diag_user_var_get;
        bslmgmt_init();
        diag_init();

        /* 5.-7. BDE / device probe / attach */
        if (sysconf_init() < 0) {
            HWBCM_LOGE("sysconf_init failed");
            pthread_mutex_unlock(&g_lock);
            return BCM_E_FAIL;
        }
        if (sysconf_probe() < 0 && !soc_ndev) {
            HWBCM_LOGE("sysconf_probe failed (no devices)");
            pthread_mutex_unlock(&g_lock);
            return BCM_E_FAIL;
        }
        var_set_integer("units", soc_ndev, FALSE, FALSE);
        HWBCM_LOGI("hwbcm_init_ex: %d device(s) probed", soc_ndev);
        if (sysconf_attach(unit) < 0) {
            HWBCM_LOGE("sysconf_attach(%d) failed", unit);
            pthread_mutex_unlock(&g_lock);
            return BCM_E_FAIL;
        }

        /* 7b. Install the ESW command table and bind shell variables to unit 0. */
        cmdlist_init();
        command_mode_set(unit, ESW_CMD_MODE);
        sh_swap_unit_vars(unit);

        g_sal_sysconf_inited = 1;
    } else {
        HWBCM_LOGI("hwbcm_init_ex: re-initializing unit %d in mode=%s",
                   unit, warm_boot ? "WARM" : "COLD");
        if (!soc_attached(unit)) {
            if (sysconf_attach(unit) < 0) {
                HWBCM_LOGE("sysconf_attach(%d) retry failed", unit);
                pthread_mutex_unlock(&g_lock);
                return BCM_E_FAIL;
            }
        }
    }

#ifdef BCM_WARM_BOOT_SUPPORT
    if (warm_boot) {
        int loc = BCM_SWITCH_STABLE_NONE;
        uint32 sflags = 0;

        /* KNET during warm init. LIVE (syncd warm restart): keep the kernel
         * DMA rings and filters (mode=1, persist=1), as before phase 4.
         * Anything else: the kernel side needs KCOM_M_HW_INIT (DCB rings,
         * bkn_isr), which soc_dma_init only sends with mode=0; the KNET
         * channels get configured and bcm_esw_knet_init runs. FRESH also gets
         * persist=0 so the DefaultRxAPI filter is created like on a cold
         * boot (the filter table is empty). Forwarding tables stay untouched
         * (SOC_WARM_BOOT). Both go back to "1" after bcm_init. */
        int knet_state = hwbcm_warm_knet_probe(scache_file);
        g_knet_props_overridden = 1;
        (void)sal_config_set((char *)spn_WARMBOOT_KNET_SHUTDOWN_MODE,
                             (char *)(knet_state == HWBCM_KNET_LIVE ? "1" : "0"));
        (void)sal_config_set((char *)spn_KNET_FILTER_PERSIST,
                             (char *)(knet_state == HWBCM_KNET_FRESH ? "0" : "1"));
        HWBCM_LOGI("hwbcm_init_ex[warm]: knet %s -> warmboot_knet_shutdown_mode=%s knet_filter_persist=%s",
                   knet_state_name(knet_state),
                   knet_state == HWBCM_KNET_LIVE ? "1" : "0",
                   knet_state == HWBCM_KNET_FRESH ? "0" : "1");

        rv = hwbcm_scache_setup(unit, 1, scache_file);
        if (rv < 0) {
            pthread_mutex_unlock(&g_lock);
            return rv;
        }

        SOC_WARM_BOOT_START(unit);

        HWBCM_LOGI("hwbcm_init_ex[warm]: soc_init (noreset)");
        rv = soc_init(unit);
        if (SOC_FAILURE(rv)) {
            HWBCM_LOGE("hwbcm_init_ex[warm]: soc_init failed: %s (%d)", bcm_errmsg(rv), rv);
            SOC_WARM_BOOT_DONE(unit);
            hwbcm_scache_cleanup();
            pthread_mutex_unlock(&g_lock);
            return rv;
        }

        /* Verify Level 2 scache recovered cleanly (soc_do_init downgrades to
         * STABLE_NONE if soc_scache_recover fails CRC/format check). */
        rv = soc_stable_get(unit, &loc, &sflags);
        if (SOC_FAILURE(rv) || loc != BCM_SWITCH_STABLE_APPLICATION) {
            HWBCM_LOGE("hwbcm_init_ex[warm]: L2 scache recovery failed/corrupt (loc=%d rv=%d)",
                       loc, rv);
            SOC_WARM_BOOT_DONE(unit);
            hwbcm_scache_cleanup();
            pthread_mutex_unlock(&g_lock);
            return BCM_E_INTERNAL;
        }

        /* EPC_LINK_BMAP = egress "link up" ports. _soc_trident2_misc_init
         * overwrites it with CMIC only, and warm linkscan never rewrites it
         * (it assumes the links stayed up), so every front port stopped
         * egress after the first warm restart (2026-11-24). The wb SDK copy
         * guards that write; keep the live value here as well. */
        epc_link_save(unit);
        HWBCM_LOGI("hwbcm_init_ex[warm]: soc_misc_init");
        rv = soc_misc_init(unit);
        epc_link_restore(unit, "soc_misc_init");
        if (SOC_FAILURE(rv)) {
            HWBCM_LOGE("hwbcm_init_ex[warm]: soc_misc_init failed: %s (%d)", bcm_errmsg(rv), rv);
            SOC_WARM_BOOT_DONE(unit);
            hwbcm_scache_cleanup();
            pthread_mutex_unlock(&g_lock);
            return rv;
        }

        /* soc_mmu_init has no warm-boot guard on TD2+: soc_td2_lls_init resets
         * the LLS scheduler and rebuilds each port's tree with MMU TX disabled,
         * which stopped forwarding ~3 s into the warm init (2026-11-24). The
         * MMU is still programmed from the cold boot and bcm cosq reinit reads
         * it back from hardware, so leave it alone (HWBCM_WB_MMU_INIT=1 restores
         * the old behaviour for debugging). */
        if (getenv("HWBCM_WB_MMU_INIT") && getenv("HWBCM_WB_MMU_INIT")[0] == '1') {
            HWBCM_LOGI("hwbcm_init_ex[warm]: soc_mmu_init (forced by HWBCM_WB_MMU_INIT)");
            rv = soc_mmu_init(unit);
        } else {
            HWBCM_LOGI("hwbcm_init_ex[warm]: soc_mmu_init skipped (MMU/LLS kept from cold boot)");
            if (SOC_IS_TD2P_TT2P(unit)) {
                /* soc_td2_cpu_port_lls_init() (inside soc_mmu_init) is the only
                 * place that populates CPU_ARM_RSVD_QUEUE_BITMAP (bit 58 for
                 * PCI CPU L0.0, plus 59/60 for ARM0/1 if configured). Without
                 * bit 58, bcm_rx_start() -> _bcm_common_rx_queue_channel_set()
                 * after SOC_WARM_BOOT_DONE overwrites CMIC_CMC0_CH1_COS_CTRL_RX_1
                 * with 0x0000ffff (clearing 0x04000000), silencing CPU RX0. */
                SHR_BITSET(CPU_ARM_RSVD_QUEUE_BITMAP(unit, 0), 58);
                if (NUM_CPU_ARM_COSQ(unit, SOC_ARM_CMC(unit, 0)) > 0) {
                    SHR_BITSET(CPU_ARM_RSVD_QUEUE_BITMAP(unit, 1), 59);
                }
                if (NUM_CPU_ARM_COSQ(unit, SOC_ARM_CMC(unit, 1)) > 0) {
                    SHR_BITSET(CPU_ARM_RSVD_QUEUE_BITMAP(unit, 2), 60);
                }
            }
            rv = SOC_E_NONE;
        }
        if (SOC_FAILURE(rv)) {
            HWBCM_LOGE("hwbcm_init_ex[warm]: soc_mmu_init failed: %s (%d)", bcm_errmsg(rv), rv);
            SOC_WARM_BOOT_DONE(unit);
            hwbcm_scache_cleanup();
            pthread_mutex_unlock(&g_lock);
            return rv;
        }

        int wb_attached_now = 0;
        if (bcm_attach_check(unit) == BCM_E_UNIT) {
            wb_attached_now = 1;
            rv = bcm_attach(unit, NULL, NULL, unit);
            if (BCM_FAILURE(rv)) {
                HWBCM_LOGE("hwbcm_init_ex[warm]: bcm_attach failed: %s (%d)", bcm_errmsg(rv), rv);
                SOC_WARM_BOOT_DONE(unit);
                hwbcm_scache_cleanup();
                pthread_mutex_unlock(&g_lock);
                return rv;
            }
        }

        /* bcm_attach -> _bcm_esw_attach already ran _bcm_esw_init (the full
         * module init, warm). A second bcm_init re-ran every module on top of
         * live state: linkscan was stopped and re-inited and the copper link
         * on 1G ports dropped ~1 s into it (2026-11-24). */
        if (wb_attached_now) {
            HWBCM_LOGI("hwbcm_init_ex[warm]: bcm_init already done by bcm_attach, not repeated");
            rv = BCM_E_NONE;
        } else {
            HWBCM_LOGI("hwbcm_init_ex[warm]: bcm_init");
            rv = bcm_init(unit);
        }
        epc_link_restore(unit, "bcm_init");
        if (BCM_FAILURE(rv)) {
            HWBCM_LOGE("hwbcm_init_ex[warm]: bcm_init failed: %s (%d)", bcm_errmsg(rv), rv);
            SOC_WARM_BOOT_DONE(unit);
            hwbcm_scache_cleanup();
            pthread_mutex_unlock(&g_lock);
            return rv;
        }

        /* sdk-6.5.27-wb (src/soc/esw/trident2.c) allocates _fwd_ctrl_lock and
         * _soc_td2_mmu_traffic_ctrl in soc_td2_mmu_traffic_ctrl_wb_restore()
         * when soc_mmu_init() was skipped, so bcm_td2_cosq_reinit() advances
         * scache_ptr and restores NUM_COS(unit) cleanly. Guard here in SW only
         * (no HW writes) in case a pre-fix scache synced NUM_COS = 0. */
        if (NUM_COS(unit) <= 0) {
            int numq = soc_property_get(unit, spn_BCM_NUM_COS, BCM_COS_DEFAULT);
            if (numq < 1) {
                numq = 1;
            } else if (numq > 8) {
                numq = 8;
            }
            HWBCM_LOGW("hwbcm_init_ex[warm]: NUM_COS was %d after cosq reinit, restored %d (SW only)",
                       NUM_COS(unit), numq);
            NUM_COS(unit) = numq;
        } else {
            HWBCM_LOGI("hwbcm_init_ex[warm]: NUM_COS=%d (restored from scache)", NUM_COS(unit));
        }

        if (SOC_WARM_BOOT(unit)) {
            SOC_WARM_BOOT_DONE(unit);
        }

        /* Warm boot: skip port disable, VLAN1 flush, STP set, KNET cleanup,
         * hash_init, and EGR_VLAN[0] write. Re-attach userspace RX DCB chain. */
        if (!bcm_rx_active(unit)) {
            rv = bcm_rx_start(unit, NULL);
            if (BCM_FAILURE(rv)) {
                HWBCM_LOGW("hwbcm_init_ex[warm]: bcm_rx_start: %s", bcm_errmsg(rv));
            }
        }
#ifdef CMIC_CMCx_CHy_COS_CTRL_RX_0_OFFSET
        {
            int pci_cmc = SOC_PCI_CMC(unit);
            uint32 r0 = soc_pci_read(unit, CMIC_CMCx_CHy_COS_CTRL_RX_0_OFFSET(pci_cmc, 1));
            uint32 r1 = soc_pci_read(unit, CMIC_CMCx_CHy_COS_CTRL_RX_1_OFFSET(pci_cmc, 1));
            HWBCM_LOGI("hwbcm_init_ex[warm]: CMIC_CMC%d_CH1_COS_CTRL_RX_0=0x%08x RX_1=0x%08x",
                       pci_cmc, r0, r1);
        }
#endif

        /* Back to the syncd-restart defaults so later deinits keep the rings. */
        (void)sal_config_set((char *)spn_WARMBOOT_KNET_SHUTDOWN_MODE, (char *)"1");
        (void)sal_config_set((char *)spn_KNET_FILTER_PERSIST, (char *)"1");
        {
            /* The rings run again: the dma_idle verdict of the last deinit
             * is consumed (a stale same-boot marker would force HW_INIT on
             * the next syncd restart). */
            char mpath[600];
            wb_sidecar_path(mpath, sizeof(mpath), scache_file, ".dma_idle");
            if (mpath[0] != '\0' && unlink(mpath) == 0) {
                HWBCM_LOGI("hwbcm_init_ex[warm]: consumed %s", mpath);
            }
        }
        if (knet_state != HWBCM_KNET_LIVE) {
            HWBCM_LOGI("hwbcm_init_ex[warm]: KNET kernel side re-initialised (%s, rx %s)",
                       knet_state_name(knet_state), bcm_rx_active(unit) ? "active" : "INACTIVE");
        }

        hwbcm_port_map_build();

        /* Pre-populate 1G SGMII state BEFORE linkscan so active 1G links do not bounce. */
        hwbcm_sgmii_warm_probe(unit);

        /* Re-enable linkscan and register callback; only touch port mode if NONE. */
        rv = bcm_linkscan_enable_set(unit, BCM_LINKSCAN_INTERVAL_DEFAULT);
        if (BCM_FAILURE(rv)) {
            HWBCM_LOGE("hwbcm_init_ex[warm]: bcm_linkscan_enable_set: %s", bcm_errmsg(rv));
            hwbcm_scache_cleanup();
            pthread_mutex_unlock(&g_lock);
            return rv;
        }
        PBMP_PORT_ITER(unit, port) {
            int mode = BCM_LINKSCAN_MODE_NONE;
            if (IS_CPU_PORT(unit, port) || IS_LB_PORT(unit, port)) {
                continue;
            }
            if (BCM_FAILURE(bcm_linkscan_mode_get(unit, port, &mode)) ||
                mode == BCM_LINKSCAN_MODE_NONE) {
                (void)bcm_linkscan_mode_set(unit, port, BCM_LINKSCAN_MODE_SW);
            }
        }
        rv = bcm_linkscan_register(unit, hwbcm_linkscan_handler);
        if (BCM_FAILURE(rv)) {
            HWBCM_LOGE("hwbcm_init_ex[warm]: bcm_linkscan_register: %s", bcm_errmsg(rv));
            hwbcm_scache_cleanup();
            pthread_mutex_unlock(&g_lock);
            return rv;
        }

        if (hwbcm_riot_enabled()) {
            HWBCM_LOGI("hwbcm_init_ex[warm]: riot active (EGR_VLAN[0] preserved)");
        }

        g_inited = 1;
        g_warm_boot = 1;
        pthread_mutex_unlock(&g_lock);
        HWBCM_LOGI("hwbcm_init_ex: WARM init done (%s) in %ld ms",
                   soc_cm_get_name(unit), mono_ms() - t0);

        (void)shell_sock_start();
        return 0;
    }

    /* Cold boot: arm in-memory L2 scache buffer before init soc so a later
     * warm shutdown can sync cleanly. Failure is non-fatal to cold boot. */
    SOC_WARM_BOOT_DONE(unit);
    knet_props_cold_defaults("hwbcm_init_ex[cold]");
    if (hwbcm_scache_setup(unit, 0, scache_file) < 0) {
        HWBCM_LOGW("hwbcm_init_ex[cold]: L2 scache arm failed; cold boot continues without L2 WB");
    }
#endif /* BCM_WARM_BOOT_SUPPORT */

    /* 8. init soc / misc / mmu / bcm through the diag shell library (cold boot) */
    {
        static const char *steps[] = { "init soc", "init misc", "init mmu", "init bcm" };
        for (i = 0; i < 4; i++) {
            HWBCM_LOGI("hwbcm_init: %s", steps[i]);
            rv = hwbcm_shell_cmd(steps[i]);
            if (rv < 0) {
                pthread_mutex_unlock(&g_lock);
                return rv;
            }
        }
    }

    /* "init bcm" brings every front port up in VLAN 1 with learning on, and
     * a copper link partner (the PC) links in ~1 s and gets learned before
     * the SAI port objects exist and buildPortMaps() applies the SAI default
     * admin-down. That stale learn then reaches orchagent while it is still
     * removing the default bridge ports, the remove fails with OBJECT_IN_USE
     * (meta refcount) and orchagent aborts (2026-11-21, 07:47). Do what the
     * Broadcom SAI does: ports start admin-down, and whatever was learned in
     * the window is dropped from the L2 table. */
    {
        int ndis = 0;
        PBMP_PORT_ITER(unit, port) {
            if (IS_CPU_PORT(unit, port) || IS_LB_PORT(unit, port)) {
                continue;
            }
            rv = bcm_port_enable_set(unit, port, 0);
            if (BCM_FAILURE(rv)) {
                HWBCM_LOGW("bcm_port_enable_set(%d, 0): %s", port, bcm_errmsg(rv));
            } else {
                ndis++;
            }
        }
        rv = bcm_l2_addr_delete_by_vlan(unit, BCM_VLAN_DEFAULT, 0);
        if (BCM_FAILURE(rv)) {
            HWBCM_LOGW("bcm_l2_addr_delete_by_vlan(%d) after init: %s", BCM_VLAN_DEFAULT, bcm_errmsg(rv));
        }
        HWBCM_LOGI("hwbcm_init: %d ports admin down until SAI enables them, vlan %d L2 entries flushed",
                   ndis, BCM_VLAN_DEFAULT);
    }

    /* STP: 6.5.27 "init bcm" leaves every TD2+ front port in STP state
     * DISABLE in the default STG, which drops all ingress (TX works, RX=0).
     * We have no STP object support, so every port is always FORWARD. */
    {
        int nfwd = 0;
        PBMP_PORT_ITER(unit, port) {
            if (IS_CPU_PORT(unit, port) || IS_LB_PORT(unit, port)) {
                continue;
            }
            rv = bcm_port_stp_set(unit, port, BCM_STG_STP_FORWARD);
            if (BCM_FAILURE(rv)) {
                HWBCM_LOGW("bcm_port_stp_set(%d, FORWARD): %s", port, bcm_errmsg(rv));
            } else {
                nfwd++;
            }
        }
        HWBCM_LOGI("stp forward on %d ports", nfwd);
    }

    /* The default VLAN exists after init bcm; the CPU port must be in it
     * (and in every VLAN, see hwbcm_vlan_cpu_add) for copies to CPU. */
    (void)hwbcm_vlan_cpu_add(BCM_VLAN_DEFAULT);

    /* RX path: with KNET the kernel module owns the DMA channels but the
     * channel setup is still driven by rx start from user space. bcm_init
     * already ran bcm_esw_knet_init, so we do not call bcm_knet_init again
     * (it would reset the kernel-side netif table). */
    knet_stale_cleanup();

    if (!bcm_rx_active(unit)) {
        rv = bcm_rx_start(unit, NULL);
        if (BCM_FAILURE(rv)) {
            HWBCM_LOGW("bcm_rx_start: %s", bcm_errmsg(rv));
        }
    }

    /* 9. linkscan */
    rv = bcm_linkscan_enable_set(unit, BCM_LINKSCAN_INTERVAL_DEFAULT);
    if (BCM_FAILURE(rv)) {
        HWBCM_LOGE("bcm_linkscan_enable_set: %s", bcm_errmsg(rv));
        pthread_mutex_unlock(&g_lock);
        return rv;
    }
    PBMP_PORT_ITER(unit, port) {
        if (IS_CPU_PORT(unit, port) || IS_LB_PORT(unit, port)) {
            continue;
        }
        rv = bcm_linkscan_mode_set(unit, port, BCM_LINKSCAN_MODE_SW);
        if (BCM_FAILURE(rv)) {
            HWBCM_LOGW("bcm_linkscan_mode_set(%d, SW): %s", port, bcm_errmsg(rv));
        }
    }
    rv = bcm_linkscan_register(unit, hwbcm_linkscan_handler);
    if (BCM_FAILURE(rv)) {
        HWBCM_LOGE("bcm_linkscan_register: %s", bcm_errmsg(rv));
        pthread_mutex_unlock(&g_lock);
        return rv;
    }

    hwbcm_port_map_build();

    if (hwbcm_riot_enabled()) {
        HWBCM_LOGI("riot: enabled (routing in/out of VXLAN VFIs)");
        (void)riot_cpu_egress_vlan0_fix();
    }

    /* Load balancing: not fatal, the switch forwards without it. */
    rv = hash_init();
    if (rv) {
        HWBCM_LOGE("hash_init failed (%d); ECMP/LAG fall back to the SDK default hash", rv);
    }

    g_inited = 1;
    g_warm_boot = 0;
    pthread_mutex_unlock(&g_lock);
    HWBCM_LOGI("hwbcm_init: done (%s) in %ld ms", soc_cm_get_name(unit), mono_ms() - t0);

    /* Debug aid, failure is not fatal. */
    (void)shell_sock_start();
    return 0;
}

int hwbcm_init(const char *config_bcm)
{
    return hwbcm_init_ex(config_bcm, 0, NULL);
}

int hwbcm_is_warm_boot(void)
{
    return g_warm_boot;
}

int hwbcm_warm_sync(const char *scache_file, size_t *bytes_written)
{
#ifndef BCM_WARM_BOOT_SUPPORT
    (void)scache_file;
    (void)bytes_written;
    HWBCM_LOGE("hwbcm_warm_sync: compiled without BCM_WARM_BOOT_SUPPORT");
    return BCM_E_UNAVAIL;
#else
    int rv;
    const char *path;
    long t0 = mono_ms();

    pthread_mutex_lock(&g_lock);
    if (!g_inited || !SOC_UNIT_VALID(HWBCM_UNIT)) {
        pthread_mutex_unlock(&g_lock);
        return BCM_E_INIT;
    }
    if (!g_scache_configured) {
        pthread_mutex_unlock(&g_lock);
        HWBCM_LOGE("hwbcm_warm_sync: L2 scache not configured");
        return BCM_E_CONFIG;
    }

    path = (scache_file != NULL && scache_file[0] != '\0') ? scache_file : g_scache_path;
    if (path == NULL || path[0] == '\0') {
        pthread_mutex_unlock(&g_lock);
        HWBCM_LOGE("hwbcm_warm_sync: no scache file path");
        return BCM_E_PARAM;
    }
    if (scache_file != NULL && scache_file[0] != '\0' && scache_file != g_scache_path) {
        snprintf(g_scache_path, sizeof(g_scache_path), "%s", scache_file);
    }

    rv = bcm_switch_control_set(HWBCM_UNIT, bcmSwitchControlSync, 1);
    if (BCM_FAILURE(rv)) {
        HWBCM_LOGE("hwbcm_warm_sync: bcmSwitchControlSync failed: %s (%d)", bcm_errmsg(rv), rv);
        pthread_mutex_unlock(&g_lock);
        return rv;
    }

    rv = hwbcm_scache_flush_file(path, bytes_written);
    if (rv == 0) {
        /* boot_id of the kernel this scache belongs to: a different one at
         * warm init means the host kexec'd (logged by hwbcm_warm_knet_probe). */
        char bpath[600], bid[64], line[96];
        wb_sidecar_path(bpath, sizeof(bpath), path, ".bootid");
        boot_id_get(bid, sizeof(bid));
        if (bid[0] != '\0') {
            snprintf(line, sizeof(line), "%s\n", bid);
            if (write_file_atomic(bpath, line) != 0) {
                HWBCM_LOGW("hwbcm_warm_sync: writing %s failed: %s", bpath, strerror(errno));
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (rv == 0) {
        HWBCM_LOGI("hwbcm_warm_sync: completed in %ld ms", mono_ms() - t0);
    }
    return rv;
#endif
}

static void hwbcm_core_state_reset(void)
{
    int i;

    memset(g_knet, 0, sizeof(g_knet));
    memset(g_desired_speed, 0, sizeof(g_desired_speed));
    memset(g_sgmii_applied, 0, sizeof(g_sgmii_applied));
    memset(g_sgmii_applied_at, 0, sizeof(g_sgmii_applied_at));
    memset(g_switch_mac, 0, sizeof(g_switch_mac));
    g_switch_mac_valid = 0;
    g_ip2me_station_id = -1;
    g_ip2me_vfi_station_id = -1;
    g_ip2me_wanted = 0;
    for (i = 0; i <= HWBCM_TRAP_STP; i++) {
        g_trap_l2cache_idx[i] = -1;
    }
    g_hash_fields[0] = HWBCM_HASH_F_DEFAULT;
    g_hash_fields[1] = HWBCM_HASH_F_DEFAULT;
    g_hash_seed[0] = 0x5a5a5a5au;
    g_hash_seed[1] = 0xa5a5a5a5u;
    g_fp_count = 0;
    g_warm_boot = 0;
#ifdef BCM_WARM_BOOT_SUPPORT
    g_knet_state = -1;
    g_epc_saved_valid = 0;
#endif
}

int hwbcm_warm_deinit(void)
{
    return hwbcm_warm_deinit_ex(0);
}

int hwbcm_warm_deinit_ex(int for_kexec)
{
#ifndef BCM_WARM_BOOT_SUPPORT
    (void)for_kexec;
    HWBCM_LOGE("hwbcm_warm_deinit: compiled without BCM_WARM_BOOT_SUPPORT");
    return BCM_E_UNAVAIL;
#else
    int unit = HWBCM_UNIT;
    long t0 = mono_ms();
    char mpath[600];
    int rv_reset1 = 0, rv_reset2 = 0, rv_bcm, rv_soc, nbusy = 0, busy;
    int knet_mode = 0;
    char active[384];

    pthread_mutex_lock(&g_lock);
    if (!g_inited) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    /* soc_shutdown frees SOC_CONTROL(unit): SOC_KNET_MODE() after it is a
     * NULL dereference (syncd segfaulted here, 2026-11-24). */
    knet_mode = (SOC_UNIT_VALID(unit) && SOC_KNET_MODE(unit)) ? 1 : 0;
    wb_sidecar_path(mpath, sizeof(mpath), g_scache_path, ".dma_idle");
    HWBCM_LOGI("hwbcm_warm_deinit: mode=%s", for_kexec ? "KEXEC/dma-reset (abort KNET DMA)" : "live (keep KNET)");

    shell_sock_stop();

    /* Unregister software callbacks without disabling hardware linkscan or
     * tearing down KNET netifs/filters. */
    (void)bcm_linkscan_unregister(unit, hwbcm_linkscan_handler);
    g_link_cb = NULL;
    g_link_cookie = NULL;
    hwbcm_l2_warm_reset();
    hwbcm_l3_warm_reset();
    hwbcm_vxlan_warm_reset();

    /* Preserve KNET netifs and filters across shutdown (destroying 72 netifs
     * one by one takes ~30 s). In kexec/dma-reset mode the rings are aborted below. */
    (void)sal_config_set((char *)spn_WARMBOOT_KNET_SHUTDOWN_MODE, (char *)"1");
    (void)sal_config_set((char *)spn_KNET_FILTER_PERSIST, (char *)"1");

    if (for_kexec) {
        /* System warm reboot or dma-reset syncd restart: nothing unloads the
         * KNET module before kexec (opennsl-modules ExecStop is a no-op), so
         * its RX/TX channels would still point at old-kernel DCBs when the new
         * kernel's BDE probe sets Bus Master again. KCOM_M_HW_RESET with
         * channels=0 = bkn_dma_abort on every channel + bkn_clean_dcbs. Done
         * first (before the long steps) and again right before soc_shutdown. */
        rv_reset1 = knet_mode ? soc_knet_hw_reset(unit, -1) : SOC_E_UNAVAIL;
        if (SOC_FAILURE(rv_reset1)) {
            HWBCM_LOGE("hwbcm_warm_deinit[kexec]: soc_knet_hw_reset(-1) rv=%d (%s)",
                       rv_reset1, bcm_errmsg(rv_reset1));
        } else {
            HWBCM_LOGI("hwbcm_warm_deinit[kexec]: soc_knet_hw_reset(-1) rv=0 (KNET rings aborted)");
        }
    } else if (mpath[0] != '\0') {
        (void)unlink(mpath);   /* rings stay armed: no idle verdict */
    }

    HWBCM_WARN_ON_FAIL("bcm_tx_cleanup", bcm_tx_cleanup(unit));
    rv_bcm = _bcm_shutdown(unit);
    if (BCM_FAILURE(rv_bcm)) {
        if (for_kexec) {
            HWBCM_LOGE("hwbcm_warm_deinit[kexec]: _bcm_shutdown: %s (%d)", bcm_errmsg(rv_bcm), rv_bcm);
        } else {
            HWBCM_LOGW("_bcm_shutdown: %s (%d)", bcm_errmsg(rv_bcm), rv_bcm);
        }
    }
    if (for_kexec) {
        rv_reset2 = knet_mode ? soc_knet_hw_reset(unit, -1) : SOC_E_UNAVAIL;
        if (SOC_FAILURE(rv_reset2)) {
            HWBCM_LOGE("hwbcm_warm_deinit[kexec]: second soc_knet_hw_reset(-1) rv=%d", rv_reset2);
        }
    }

    if (for_kexec && knet_mode) {
        /* bkn_rxtick (every ~100 ms) refills and restarts an RX channel that
         * is not running if its token bucket is below the burst size, i.e.
         * if packets arrived in the tick before the abort (only matters with
         * use_rx_skb=1; with the default 0 the RX buffers come from the BCM
         * RX API chains, which are gone). Re-check after more than one tick
         * and abort again until the channels stay idle.
         *
         * MUST run BEFORE soc_shutdown(unit): soc_shutdown frees
         * SOC_CONTROL(unit), after which soc_knet_handle_rx() rejects every
         * KCOM reply (!SOC_UNIT_VALID(unit)) and soc_knet_hw_reset() always
         * times out with SOC_E_TIMEOUT. */
        int tries;
        uint32 act = 0;
        for (tries = 0; tries < 6; tries++) {
            usleep(250000);
            act = cmic_pktdma_active_mask(unit);
            if (act == 0) {
                break;
            }
            HWBCM_LOGW("hwbcm_warm_deinit[kexec]: packet DMA re-armed after abort (mask 0x%03x), aborting again",
                       act);
            rv_reset2 = soc_knet_hw_reset(unit, -1);
        }
        if (act == 0 && tries > 0) {
            HWBCM_LOGI("hwbcm_warm_deinit[kexec]: packet DMA idle after %d extra abort(s)", tries);
        }
    }

    rv_soc = soc_shutdown(unit);
    if (SOC_FAILURE(rv_soc)) {
        /* soc_shutdown stops the userspace DMA engines with SOC_IF_ERROR_RETURN
         * steps: an early error skips the later stops. */
        if (for_kexec) {
            HWBCM_LOGE("hwbcm_warm_deinit[kexec]: soc_shutdown: %s (%d)", bcm_errmsg(rv_soc), rv_soc);
        } else {
            HWBCM_LOGW("soc_shutdown: %s (%d)", bcm_errmsg(rv_soc), rv_soc);
        }
    }

    if (for_kexec) {
        /* Read back the CMIC DMA engines (soc_pci_read -> CMREAD uses static
         * CMVEC(unit), not SOC_CONTROL(unit), and sysconf_detach is never
         * called) and leave the verdict for the platform warm-reboot_plugin
         * and the next warm init's KNET probe. */
        char bid[64], line[640];
        nbusy = cmic_dma_busy(unit, active, sizeof(active));
        busy = nbusy > 0 || SOC_FAILURE(rv_reset1) || SOC_FAILURE(rv_reset2) ||
               BCM_FAILURE(rv_bcm) || SOC_FAILURE(rv_soc);
        boot_id_get(bid, sizeof(bid));
        snprintf(line, sizeof(line),
                 "%s boot=%s active=%d knet_reset=%d,%d bcm_shutdown=%d soc_shutdown=%d regs:%s\n",
                 busy ? "busy" : "ok", bid[0] ? bid : "?", nbusy, rv_reset1, rv_reset2,
                 rv_bcm, rv_soc, active);
        if (write_file_atomic(mpath, line) != 0) {
            HWBCM_LOGE("hwbcm_warm_deinit[kexec]: writing %s failed: %s", mpath, strerror(errno));
        }
        if (busy) {
            HWBCM_LOGE("hwbcm_warm_deinit[kexec]: dma_idle=busy (%d active engine(s):%s)", nbusy, active);
        } else {
            HWBCM_LOGI("hwbcm_warm_deinit[kexec]: dma_idle=ok (%s)", active);
        }
    } else {
        busy = 0;
    }

    hwbcm_scache_cleanup();
    hwbcm_core_state_reset();
    g_inited = 0;
    pthread_mutex_unlock(&g_lock);
    HWBCM_LOGI("hwbcm_warm_deinit: done in %ld ms (ASIC preserved, KNET %s)", mono_ms() - t0,
               for_kexec ? "rings aborted, netifs kept" : "preserved");
    return busy ? BCM_E_BUSY : 0;
#endif
}

int hwbcm_deinit(void)
{
    int unit = HWBCM_UNIT;
    int i;

    pthread_mutex_lock(&g_lock);
    if (!g_inited) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    shell_sock_stop();

    (void)bcm_linkscan_unregister(unit, hwbcm_linkscan_handler);
    (void)bcm_linkscan_enable_set(unit, 0);
    g_link_cb = NULL;
    g_link_cookie = NULL;
    hwbcm_l2_warm_reset();
    hwbcm_l3_warm_reset();
    hwbcm_vxlan_warm_reset();

    for (i = 0; i < HWBCM_KNET_MAX; i++) {
        if (g_knet[i].netif_id != 0) {
            knet_filter_destroy_quiet(&g_knet[i].pvid_filter_id);
            knet_filter_destroy_quiet(&g_knet[i].vfi_filter_id);
            knet_filter_destroy_quiet(&g_knet[i].filter_id);
            (void)bcm_knet_netif_destroy(unit, g_knet[i].netif_id);
            memset(&g_knet[i], 0, sizeof(g_knet[i]));
        }
    }

    if (bcm_rx_active(unit)) {
        (void)bcm_rx_stop(unit, NULL);
    }

#ifdef BCM_WARM_BOOT_SUPPORT
    /* After a warm init the KNET properties are "1", which would make this
     * cold detach leave the kernel DMA rings running (fast-reboot = kexec). */
    knet_props_cold_defaults("hwbcm_deinit");
#endif
    HWBCM_WARN_ON_FAIL("bcm_detach", bcm_detach(unit));
    if (sysconf_detach(unit) < 0) {
        HWBCM_LOGW("sysconf_detach(%d) failed", unit);
    }

#ifdef BCM_WARM_BOOT_SUPPORT
    hwbcm_scache_cleanup();
#endif

    hwbcm_core_state_reset();
    g_inited = 0;
    pthread_mutex_unlock(&g_lock);
    HWBCM_LOGI("hwbcm_deinit: done");
    return 0;
}

void hwbcm_core_wb_get(hwbcm_core_wb_state_t *out)
{
    int i;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->switch_mac, g_switch_mac, 6);
    out->switch_mac_valid = g_switch_mac_valid;
    out->ip2me_station_id = g_ip2me_station_id;
    out->ip2me_vfi_station_id = g_ip2me_vfi_station_id;
    out->ip2me_wanted = g_ip2me_wanted;
    for (i = 0; i <= HWBCM_TRAP_STP; i++) {
        out->trap_l2cache_idx[i] = g_trap_l2cache_idx[i];
    }
}

void hwbcm_core_wb_restore(const hwbcm_core_wb_state_t *in)
{
    int i;

    if (in == NULL) {
        return;
    }
    memcpy(g_switch_mac, in->switch_mac, 6);
    g_switch_mac_valid = in->switch_mac_valid ? 1 : 0;
    g_ip2me_station_id = in->ip2me_station_id;
    g_ip2me_vfi_station_id = in->ip2me_vfi_station_id;
    g_ip2me_wanted = in->ip2me_wanted ? 1 : 0;
    for (i = 0; i <= HWBCM_TRAP_STP; i++) {
        g_trap_l2cache_idx[i] = in->trap_l2cache_idx[i];
    }
    HWBCM_LOGI("wb core: restored mac_valid=%d ip2me_id=%d vfi_id=%d wanted=%d",
               g_switch_mac_valid, g_ip2me_station_id, g_ip2me_vfi_station_id, g_ip2me_wanted);
}

void hwbcm_port_desired_speed_restore(int port, int speed_mbps)
{
    if (port_valid(port) && speed_mbps > 0) {
        g_desired_speed[port] = speed_mbps;
    }
}

/* ------------------------------------------------------------------------ */
/* chip / port map                                                          */
/* ------------------------------------------------------------------------ */

int hwbcm_chip_info(uint16_t *dev_id, uint8_t *rev_id, char *name, size_t name_len)
{
    int unit = HWBCM_UNIT;
    uint16 d = 0;
    uint8 r = 0;
    int rv;

    if (!g_inited) {
        return BCM_E_INIT;
    }
    rv = soc_cm_get_id(unit, &d, &r);
    if (rv < 0) {
        HWBCM_LOGE("soc_cm_get_id: %d", rv);
        return BCM_E_FAIL;
    }
    if (dev_id) {
        *dev_id = (uint16_t)d;
    }
    if (rev_id) {
        *rev_id = (uint8_t)r;
    }
    if (name && name_len > 0) {
        const char *n = soc_cm_get_name(unit);
        if (n == NULL) {
            n = soc_cm_get_device_name(d, r);
        }
        if (n == NULL) {
            n = "unknown";
        }
        strncpy(name, n, name_len - 1);
        name[name_len - 1] = '\0';
    }
    return 0;
}

int hwbcm_port_map_get(int *logical, int *phys, int max)
{
    int i;

    if (!g_inited) {
        return BCM_E_INIT;
    }
    for (i = 0; i < g_fp_count && i < max; i++) {
        if (logical) {
            logical[i] = g_fp_logical[i];
        }
        if (phys) {
            phys[i] = g_fp_phys[i];
        }
    }
    return g_fp_count;
}

int hwbcm_port_from_phys(int phys)
{
    int i;

    for (i = 0; i < g_fp_count; i++) {
        if (g_fp_phys[i] == phys) {
            return g_fp_logical[i];
        }
    }
    return -1;
}

int hwbcm_port_name(int port, char *buf, size_t len)
{
    if (!g_inited) {
        return BCM_E_INIT;
    }
    if (!port_valid(port) || buf == NULL || len == 0) {
        return BCM_E_PARAM;
    }
    strncpy(buf, SOC_PORT_NAME(HWBCM_UNIT, port), len - 1);
    buf[len - 1] = '\0';
    return 0;
}

/* ------------------------------------------------------------------------ */
/* port attributes                                                          */
/* ------------------------------------------------------------------------ */

int hwbcm_port_enable_set(int port, int enable)
{
    if (!port_valid(port)) {
        return BCM_E_PORT;
    }
    HWBCM_CHECK("bcm_port_enable_set", bcm_port_enable_set(HWBCM_UNIT, port, enable ? 1 : 0));
    if (enable) {
        /* No STP support: an admin-up port always forwards in every STG. */
        int rv = bcm_port_stp_set(HWBCM_UNIT, port, BCM_STG_STP_FORWARD);
        if (BCM_FAILURE(rv)) {
            HWBCM_LOGW("port %d: bcm_port_stp_set(FORWARD): %s", port, bcm_errmsg(rv));
        }
    }
    HWBCM_LOGD("port %d: admin %s", port, enable ? "up" : "down");
    return 0;
}

int hwbcm_port_enable_get(int port, int *enable)
{
    int en = 0;

    if (!port_valid(port) || enable == NULL) {
        return BCM_E_PARAM;
    }
    HWBCM_CHECK("bcm_port_enable_get", bcm_port_enable_get(HWBCM_UNIT, port, &en));
    *enable = en ? 1 : 0;
    return 0;
}

int hwbcm_port_speed_set(int port, int speed_mbps, int autoneg)
{
    int unit = HWBCM_UNIT;
    int rv;

    if (!port_valid(port)) {
        return BCM_E_PORT;
    }
    if (speed_mbps <= 0) {
        return BCM_E_PARAM;
    }

    if (autoneg) {
        /* Autoneg is requested and the port already has AN on and link up
         * (e.g. orchagent re-sends port config after a warm restart or kexec
         * warm-reboot when g_desired_speed[port] is still 0). Re-running
         * bcm_port_autoneg_set(1) and SGMII interface_set goes all the way to
         * the BCM84858 copper PHY and drops the link, so leave a live port
         * alone, record g_desired_speed, and keep g_sgmii_applied as marked by
         * the warm probe. */
        int an = 0, link = 0;
        if (BCM_SUCCESS(bcm_port_autoneg_get(unit, port, &an)) && an &&
            BCM_SUCCESS(bcm_port_link_status_get(unit, port, &link)) &&
            link == BCM_PORT_LINK_STATUS_UP) {
            g_desired_speed[port] = speed_mbps;
            HWBCM_LOGI("port %d: speed %d Mbps autoneg 1 already active, link up, not reprogrammed",
                       port, speed_mbps);
            return 0;
        }
    }

    if (autoneg) {
        /* Autoneg path, same as the diag "port <p> an=1": just enable
         * autoneg and never call bcm_port_speed_set (the SDK rejects it
         * with AN on). 10GBASE-T has no forced mode, so 10G on the BCM84858
         * copper PHYs REQUIRES this path.
         * The advertised abilities are deliberately left at the driver
         * default (= full local ability). Masking them to the requested
         * speed (verified on the box, run 3) breaks BCM84858 negotiation:
         * autoneg=1, speed=0, no link. The diag shell does the same. */
        int max_speed = 0;

        if (bcm_port_speed_max(unit, port, &max_speed) == BCM_E_NONE &&
            max_speed > 0 && speed_mbps < max_speed) {
            HWBCM_LOGW("port %d: autoneg at %d Mbps requested but advert left at port max %d Mbps",
                       port, speed_mbps, max_speed);
        }
        HWBCM_CHECK("bcm_port_autoneg_set(1)", bcm_port_autoneg_set(unit, port, 1));

        if (speed_mbps <= 1000) {
            /* SGMII for <=1G; re-applied on link up by the linkscan handler
             * (SerDes otherwise stays at 10G). */
            rv = bcm_port_interface_set(unit, port, BCM_PORT_IF_SGMII);
            if (BCM_FAILURE(rv)) {
                HWBCM_LOGW("port %d: bcm_port_interface_set(SGMII): %s", port, bcm_errmsg(rv));
            }
        }
    } else {
        /* Forced speed. */
        HWBCM_CHECK("bcm_port_autoneg_set(0)", bcm_port_autoneg_set(unit, port, 0));
        HWBCM_CHECK("bcm_port_speed_set", bcm_port_speed_set(unit, port, speed_mbps));
        if (speed_mbps <= 1000) {
            rv = bcm_port_interface_set(unit, port, BCM_PORT_IF_SGMII);
            if (BCM_FAILURE(rv)) {
                HWBCM_LOGW("port %d: bcm_port_interface_set(SGMII): %s", port, bcm_errmsg(rv));
            }
        } else if (speed_mbps == 10000) {
            rv = bcm_port_interface_set(unit, port, BCM_PORT_IF_XFI);
            if (BCM_FAILURE(rv)) {
                HWBCM_LOGW("port %d: bcm_port_interface_set(XFI): %s", port, bcm_errmsg(rv));
            }
        }
    }

    g_desired_speed[port] = speed_mbps;
    g_sgmii_applied[port] = 0; /* explicit reconfig starts a new episode */
    HWBCM_LOGI("port %d: speed %d Mbps autoneg %d", port, speed_mbps, autoneg ? 1 : 0);
    return 0;
}

int hwbcm_port_speed_get(int port, int *speed_mbps)
{
    int sp = 0;

    if (!port_valid(port) || speed_mbps == NULL) {
        return BCM_E_PARAM;
    }
    HWBCM_CHECK("bcm_port_speed_get", bcm_port_speed_get(HWBCM_UNIT, port, &sp));
    *speed_mbps = sp;
    return 0;
}

int hwbcm_port_autoneg_get(int port, int *autoneg)
{
    int an = 0;

    if (!port_valid(port) || autoneg == NULL) {
        return BCM_E_PARAM;
    }
    HWBCM_CHECK("bcm_port_autoneg_get", bcm_port_autoneg_get(HWBCM_UNIT, port, &an));
    *autoneg = an ? 1 : 0;
    return 0;
}

int hwbcm_port_link_get(int port, int *up)
{
    int st = 0;

    if (!port_valid(port) || up == NULL) {
        return BCM_E_PARAM;
    }
    HWBCM_CHECK("bcm_port_link_status_get", bcm_port_link_status_get(HWBCM_UNIT, port, &st));
    *up = (st == BCM_PORT_LINK_STATUS_UP) ? 1 : 0;
    return 0;
}

int hwbcm_port_mtu_set(int port, int mtu)
{
    if (!port_valid(port)) {
        return BCM_E_PORT;
    }
    if (mtu <= 0) {
        return BCM_E_PARAM;
    }
    HWBCM_CHECK("bcm_port_frame_max_set", bcm_port_frame_max_set(HWBCM_UNIT, port, mtu));
    HWBCM_LOGD("port %d: max frame %d", port, mtu);
    return 0;
}

int hwbcm_port_mtu_get(int port, int *mtu)
{
    int sz = 0;

    if (!port_valid(port) || mtu == NULL) {
        return BCM_E_PARAM;
    }
    HWBCM_CHECK("bcm_port_frame_max_get", bcm_port_frame_max_get(HWBCM_UNIT, port, &sz));
    *mtu = sz;
    return 0;
}

int hwbcm_port_learn_set(int port, int learn_enable)
{
    uint32 flags;

    if (!port_valid(port)) {
        return BCM_E_PORT;
    }
    /* Always forward; learn into the ARL only when enabled. */
    flags = BCM_PORT_LEARN_FWD;
    if (learn_enable) {
        flags |= BCM_PORT_LEARN_ARL;
    }
    HWBCM_CHECK("bcm_port_learn_set", bcm_port_learn_set(HWBCM_UNIT, port, flags));
    HWBCM_LOGD("port %d: learning %s", port, learn_enable ? "on" : "off");
    return 0;
}

int hwbcm_port_pause_set(int port, int pause_tx, int pause_rx)
{
    if (!port_valid(port)) {
        return BCM_E_PORT;
    }
    HWBCM_CHECK("bcm_port_pause_set",
                bcm_port_pause_set(HWBCM_UNIT, port, pause_tx ? 1 : 0, pause_rx ? 1 : 0));
    return 0;
}

int hwbcm_port_pvid_set(int port, int vid)
{
    if (!port_valid(port)) {
        return BCM_E_PORT;
    }
    if (vid < 1 || vid > BCM_VLAN_MAX) {
        return BCM_E_PARAM;
    }
    HWBCM_CHECK("bcm_port_untagged_vlan_set",
                bcm_port_untagged_vlan_set(HWBCM_UNIT, port, (bcm_vlan_t)vid));
    HWBCM_LOGD("port %d: pvid %d", port, vid);

    /* Rebuild the tag-stripping KNET filter of any netif bound to this port. */
    {
        int i;
        for (i = 0; i < HWBCM_KNET_MAX; i++) {
            hwbcm_knet_slot_t *slot = &g_knet[i];
            if (slot->netif_id == 0 || slot->port != port || slot->pvid == vid) {
                continue;
            }
            knet_filter_destroy_quiet(&slot->pvid_filter_id);
            slot->pvid = vid;
            {
                char name[BCM_KNET_NETIF_NAME_MAX];
                snprintf(name, sizeof(name), "netif%d", slot->netif_id);
                (void)knet_pvid_filter_create(slot, name);
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* counters                                                                 */
/* ------------------------------------------------------------------------ */

static int stat_one(int port, bcm_stat_val_t type, uint64_t *out)
{
    uint64 v;
    int rv;

    COMPILER_64_ZERO(v);
    rv = bcm_stat_get(HWBCM_UNIT, port, type, &v);
    if (BCM_FAILURE(rv)) {
        if (rv == BCM_E_UNAVAIL) {
            *out = 0;
            return 0;
        }
        return rv;
    }
    *out = ((uint64_t)COMPILER_64_HI(v) << 32) | (uint64_t)COMPILER_64_LO(v);
    return 0;
}

int hwbcm_port_stats_get(int port, hwbcm_port_stats_t *st)
{
    int rv;

    if (!port_valid(port) || st == NULL) {
        return BCM_E_PARAM;
    }
    memset(st, 0, sizeof(*st));

#define S(type, field)                                                   \
    do {                                                                 \
        rv = stat_one(port, (type), &st->field);                         \
        if (BCM_FAILURE(rv)) {                                           \
            HWBCM_LOGE("bcm_stat_get(port %d, %d): %s", port, (int)(type), bcm_errmsg(rv)); \
            return rv;                                                   \
        }                                                                \
    } while (0)

    S(snmpIfHCInOctets, rx_bytes);
    S(snmpIfHCInUcastPkts, rx_ucast);
    S(snmpIfHCInMulticastPkts, rx_mcast);
    S(snmpIfHCInBroadcastPkts, rx_bcast);
    S(snmpIfInErrors, rx_errors);
    S(snmpIfInDiscards, rx_discards);
    S(snmpIfHCOutOctets, tx_bytes);
    S(snmpIfHCOutUcastPkts, tx_ucast);
    S(snmpIfHCOutMulticastPkts, tx_mcast);
    S(snmpIfHCOutBroadcastPckts, tx_bcast); /* sic: SDK spelling */
    S(snmpIfOutErrors, tx_errors);
    S(snmpIfOutDiscards, tx_discards);
#undef S

    st->rx_packets = st->rx_ucast + st->rx_mcast + st->rx_bcast;
    st->tx_packets = st->tx_ucast + st->tx_mcast + st->tx_bcast;
    return 0;
}

int hwbcm_port_stats_clear(int port)
{
    if (!port_valid(port)) {
        return BCM_E_PORT;
    }
    HWBCM_CHECK("bcm_stat_clear", bcm_stat_clear(HWBCM_UNIT, port));
    return 0;
}

/* hwbcm_pstat_t -> bcm_stat_val_t. Entries left 0 (snmpIfInOctets is 0, but
 * no id maps to it) are "no counter"; the NON_UCAST pair is summed below. */
static const bcm_stat_val_t pstat_map[HWBCM_PSTAT_COUNT] = {
    [HWBCM_PSTAT_IF_IN_OCTETS]                  = snmpIfHCInOctets,
    [HWBCM_PSTAT_IF_IN_UCAST_PKTS]              = snmpIfHCInUcastPkts,
    [HWBCM_PSTAT_IF_IN_DISCARDS]                = snmpIfInDiscards,
    [HWBCM_PSTAT_IF_IN_ERRORS]                  = snmpIfInErrors,
    [HWBCM_PSTAT_IF_IN_UNKNOWN_PROTOS]          = snmpIfInUnknownProtos,
    [HWBCM_PSTAT_IF_IN_BROADCAST_PKTS]          = snmpIfHCInBroadcastPkts,
    [HWBCM_PSTAT_IF_IN_MULTICAST_PKTS]          = snmpIfHCInMulticastPkts,
    [HWBCM_PSTAT_IF_OUT_OCTETS]                 = snmpIfHCOutOctets,
    [HWBCM_PSTAT_IF_OUT_UCAST_PKTS]             = snmpIfHCOutUcastPkts,
    [HWBCM_PSTAT_IF_OUT_DISCARDS]               = snmpIfOutDiscards,
    [HWBCM_PSTAT_IF_OUT_ERRORS]                 = snmpIfOutErrors,
    [HWBCM_PSTAT_IF_OUT_QLEN]                   = snmpIfOutQLen,
    [HWBCM_PSTAT_IF_OUT_BROADCAST_PKTS]         = snmpIfHCOutBroadcastPckts, /* sic */
    [HWBCM_PSTAT_IF_OUT_MULTICAST_PKTS]         = snmpIfHCOutMulticastPkts,
    [HWBCM_PSTAT_ETHER_STATS_DROP_EVENTS]       = snmpEtherStatsDropEvents,
    [HWBCM_PSTAT_ETHER_STATS_MULTICAST_PKTS]    = snmpEtherStatsMulticastPkts,
    [HWBCM_PSTAT_ETHER_STATS_BROADCAST_PKTS]    = snmpEtherStatsBroadcastPkts,
    [HWBCM_PSTAT_ETHER_STATS_UNDERSIZE_PKTS]    = snmpEtherStatsUndersizePkts,
    [HWBCM_PSTAT_ETHER_STATS_FRAGMENTS]         = snmpEtherStatsFragments,
    [HWBCM_PSTAT_ETHER_STATS_PKTS_64_OCTETS]    = snmpEtherStatsPkts64Octets,
    [HWBCM_PSTAT_ETHER_STATS_PKTS_65_TO_127_OCTETS]   = snmpEtherStatsPkts65to127Octets,
    [HWBCM_PSTAT_ETHER_STATS_PKTS_128_TO_255_OCTETS]  = snmpEtherStatsPkts128to255Octets,
    [HWBCM_PSTAT_ETHER_STATS_PKTS_256_TO_511_OCTETS]  = snmpEtherStatsPkts256to511Octets,
    [HWBCM_PSTAT_ETHER_STATS_PKTS_512_TO_1023_OCTETS] = snmpEtherStatsPkts512to1023Octets,
    [HWBCM_PSTAT_ETHER_STATS_PKTS_1024_TO_1518_OCTETS]= snmpEtherStatsPkts1024to1518Octets,
    [HWBCM_PSTAT_ETHER_STATS_OVERSIZE_PKTS]     = snmpEtherStatsOversizePkts,
    [HWBCM_PSTAT_ETHER_RX_OVERSIZE_PKTS]        = snmpEtherRxOversizePkts,
    [HWBCM_PSTAT_ETHER_TX_OVERSIZE_PKTS]        = snmpEtherTxOversizePkts,
    [HWBCM_PSTAT_ETHER_STATS_JABBERS]           = snmpEtherStatsJabbers,
    [HWBCM_PSTAT_ETHER_STATS_OCTETS]            = snmpEtherStatsOctets,
    [HWBCM_PSTAT_ETHER_STATS_PKTS]              = snmpEtherStatsPkts,
    [HWBCM_PSTAT_ETHER_STATS_COLLISIONS]        = snmpEtherStatsCollisions,
    [HWBCM_PSTAT_ETHER_STATS_CRC_ALIGN_ERRORS]  = snmpEtherStatsCRCAlignErrors,
    [HWBCM_PSTAT_ETHER_STATS_TX_NO_ERRORS]      = snmpEtherStatsTXNoErrors,
    [HWBCM_PSTAT_ETHER_STATS_RX_NO_ERRORS]      = snmpEtherStatsRXNoErrors,
    [HWBCM_PSTAT_DOT3_STATS_FCS_ERRORS]         = snmpDot3StatsFCSErrors,
    [HWBCM_PSTAT_PAUSE_RX_PKTS]                 = snmpDot3InPauseFrames,
    [HWBCM_PSTAT_PAUSE_TX_PKTS]                 = snmpDot3OutPauseFrames,
    [HWBCM_PSTAT_ETHER_IN_PKTS_64_OCTETS]           = snmpBcmReceivedPkts64Octets,
    [HWBCM_PSTAT_ETHER_IN_PKTS_65_TO_127_OCTETS]    = snmpBcmReceivedPkts65to127Octets,
    [HWBCM_PSTAT_ETHER_IN_PKTS_128_TO_255_OCTETS]   = snmpBcmReceivedPkts128to255Octets,
    [HWBCM_PSTAT_ETHER_IN_PKTS_256_TO_511_OCTETS]   = snmpBcmReceivedPkts256to511Octets,
    [HWBCM_PSTAT_ETHER_IN_PKTS_512_TO_1023_OCTETS]  = snmpBcmReceivedPkts512to1023Octets,
    [HWBCM_PSTAT_ETHER_IN_PKTS_1024_TO_1518_OCTETS] = snmpBcmReceivedPkts1024to1518Octets,
    [HWBCM_PSTAT_ETHER_IN_PKTS_1519_TO_2047_OCTETS] = snmpBcmReceivedPkts1519to2047Octets,
    [HWBCM_PSTAT_ETHER_IN_PKTS_2048_TO_4095_OCTETS] = snmpBcmReceivedPkts2048to4095Octets,
    [HWBCM_PSTAT_ETHER_IN_PKTS_4096_TO_9216_OCTETS] = snmpBcmReceivedPkts4095to9216Octets,
    [HWBCM_PSTAT_ETHER_IN_PKTS_9217_TO_16383_OCTETS]= snmpBcmReceivedPkts9217to16383Octets,
    [HWBCM_PSTAT_ETHER_OUT_PKTS_64_OCTETS]           = snmpBcmTransmittedPkts64Octets,
    [HWBCM_PSTAT_ETHER_OUT_PKTS_65_TO_127_OCTETS]    = snmpBcmTransmittedPkts65to127Octets,
    [HWBCM_PSTAT_ETHER_OUT_PKTS_128_TO_255_OCTETS]   = snmpBcmTransmittedPkts128to255Octets,
    [HWBCM_PSTAT_ETHER_OUT_PKTS_256_TO_511_OCTETS]   = snmpBcmTransmittedPkts256to511Octets,
    [HWBCM_PSTAT_ETHER_OUT_PKTS_512_TO_1023_OCTETS]  = snmpBcmTransmittedPkts512to1023Octets,
    [HWBCM_PSTAT_ETHER_OUT_PKTS_1024_TO_1518_OCTETS] = snmpBcmTransmittedPkts1024to1518Octets,
    [HWBCM_PSTAT_ETHER_OUT_PKTS_1519_TO_2047_OCTETS] = snmpBcmTransmittedPkts1519to2047Octets,
    [HWBCM_PSTAT_ETHER_OUT_PKTS_2048_TO_4095_OCTETS] = snmpBcmTransmittedPkts2048to4095Octets,
    [HWBCM_PSTAT_ETHER_OUT_PKTS_4096_TO_9216_OCTETS] = snmpBcmTransmittedPkts4095to9216Octets,
    [HWBCM_PSTAT_ETHER_OUT_PKTS_9217_TO_16383_OCTETS]= snmpBcmTransmittedPkts9217to16383Octets,
    [HWBCM_PSTAT_PFC_0_RX_PKTS] = snmpBcmRxPFCFramePriority0,
    [HWBCM_PSTAT_PFC_1_RX_PKTS] = snmpBcmRxPFCFramePriority1,
    [HWBCM_PSTAT_PFC_2_RX_PKTS] = snmpBcmRxPFCFramePriority2,
    [HWBCM_PSTAT_PFC_3_RX_PKTS] = snmpBcmRxPFCFramePriority3,
    [HWBCM_PSTAT_PFC_4_RX_PKTS] = snmpBcmRxPFCFramePriority4,
    [HWBCM_PSTAT_PFC_5_RX_PKTS] = snmpBcmRxPFCFramePriority5,
    [HWBCM_PSTAT_PFC_6_RX_PKTS] = snmpBcmRxPFCFramePriority6,
    [HWBCM_PSTAT_PFC_7_RX_PKTS] = snmpBcmRxPFCFramePriority7,
    [HWBCM_PSTAT_PFC_0_TX_PKTS] = snmpBcmTxPFCFramePriority0,
    [HWBCM_PSTAT_PFC_1_TX_PKTS] = snmpBcmTxPFCFramePriority1,
    [HWBCM_PSTAT_PFC_2_TX_PKTS] = snmpBcmTxPFCFramePriority2,
    [HWBCM_PSTAT_PFC_3_TX_PKTS] = snmpBcmTxPFCFramePriority3,
    [HWBCM_PSTAT_PFC_4_TX_PKTS] = snmpBcmTxPFCFramePriority4,
    [HWBCM_PSTAT_PFC_5_TX_PKTS] = snmpBcmTxPFCFramePriority5,
    [HWBCM_PSTAT_PFC_6_TX_PKTS] = snmpBcmTxPFCFramePriority6,
    [HWBCM_PSTAT_PFC_7_TX_PKTS] = snmpBcmTxPFCFramePriority7,
};

/* The bcm counters behind one id: 0, 1 or 2 (NON_UCAST = mcast + bcast). */
static int pstat_types(int id, bcm_stat_val_t types[2])
{
    if (id == HWBCM_PSTAT_IF_IN_NON_UCAST_PKTS) {
        types[0] = snmpIfHCInMulticastPkts;
        types[1] = snmpIfHCInBroadcastPkts;
        return 2;
    }
    if (id == HWBCM_PSTAT_IF_OUT_NON_UCAST_PKTS) {
        types[0] = snmpIfHCOutMulticastPkts;
        types[1] = snmpIfHCOutBroadcastPckts;
        return 2;
    }
    if (id <= HWBCM_PSTAT_NONE || id >= HWBCM_PSTAT_COUNT || pstat_map[id] == 0) {
        return 0;
    }
    types[0] = pstat_map[id];
    return 1;
}

int hwbcm_port_stat_multi_get(int port, int n, const int *ids, uint64_t *out)
{
    int i, k, nt, rv;
    bcm_stat_val_t types[2];
    uint64_t v;

    if (!port_valid(port) || n < 0 || (n > 0 && (ids == NULL || out == NULL))) {
        return BCM_E_PARAM;
    }
    for (i = 0; i < n; i++) {
        out[i] = 0;
        nt = pstat_types(ids[i], types);
        for (k = 0; k < nt; k++) {
            rv = stat_one(port, types[k], &v);
            if (BCM_FAILURE(rv)) {
                HWBCM_LOGE("bcm_stat_get(port %d, %d): %s", port, (int)types[k], bcm_errmsg(rv));
                return rv;
            }
            out[i] += v;
        }
    }
    return 0;
}

int hwbcm_port_stat_multi_clear(int port, int n, const int *ids)
{
    int i, k, nt, rv;
    bcm_stat_val_t types[2];

    if (!port_valid(port) || n < 0 || (n > 0 && ids == NULL)) {
        return BCM_E_PARAM;
    }
    for (i = 0; i < n; i++) {
        nt = pstat_types(ids[i], types);
        for (k = 0; k < nt; k++) {
            rv = bcm_stat_clear_single(HWBCM_UNIT, port, types[k]);
            if (BCM_FAILURE(rv) && rv != BCM_E_UNAVAIL) {
                HWBCM_LOGE("bcm_stat_clear_single(port %d, %d): %s", port, (int)types[k], bcm_errmsg(rv));
                return rv;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* switch MAC / IP2ME station                                               */
/* ------------------------------------------------------------------------ */

/* Add (or replace) the catch-all "my station" entry for the switch MAC on
 * every VLAN. Packets to this MAC then go through L3 lookup; a host entry
 * with a CPU egress (hwbcm_l3_egress_cpu_create) or UnknownL3DestToCpu then
 * delivers them to the CPU. */
static int ip2me_station_apply(void)
{
    int unit = HWBCM_UNIT;
    bcm_l2_station_t st;
    int rv;

    if (g_ip2me_station_id >= 0) {
        rv = bcm_l2_station_delete(unit, g_ip2me_station_id);
        if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
            HWBCM_LOGW("bcm_l2_station_delete(%d): %s", g_ip2me_station_id, bcm_errmsg(rv));
        }
        g_ip2me_station_id = -1;
    }
    if (g_ip2me_vfi_station_id >= 0) {
        rv = bcm_l2_station_delete(unit, g_ip2me_vfi_station_id);
        if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
            HWBCM_LOGW("bcm_l2_station_delete(%d): %s", g_ip2me_vfi_station_id, bcm_errmsg(rv));
        }
        g_ip2me_vfi_station_id = -1;
    }
    if (!g_ip2me_wanted || !g_switch_mac_valid) {
        return 0;
    }

    bcm_l2_station_t_init(&st);
    st.flags = BCM_L2_STATION_IPV4 | BCM_L2_STATION_IPV6 | BCM_L2_STATION_ARP_RARP;
    st.priority = 1000; /* low priority: per-RIF entries added by the SDK win */
    memcpy(st.dst_mac, g_switch_mac, 6);
    memset(st.dst_mac_mask, 0xff, 6);
    st.vlan = 0;
    st.vlan_mask = 0; /* any VLAN */
    rv = bcm_l2_station_add(unit, &g_ip2me_station_id, &st);
    if (BCM_FAILURE(rv)) {
        HWBCM_LOGE("bcm_l2_station_add(switch mac): %s", bcm_errmsg(rv));
        g_ip2me_station_id = -1;
        return rv;
    }
    /* RIOT: MY_STATION_TCAM_2 keys on the forwarding domain type, and the
     * entry above only matches the VLAN domain. A VFI-type VPN id with mask 0
     * (_BCM_VPN_VFI_IS_SET -> FORWARDING_FIELD_TYPE=1, VFI_ID_MASK=0) matches
     * every VXLAN VFI, which is what the VFI L3 gateway needs. */
    if (hwbcm_riot_enabled()) {
        bcm_l2_station_t vst;

        bcm_l2_station_t_init(&vst);
        vst.flags = BCM_L2_STATION_IPV4 | BCM_L2_STATION_IPV6 | BCM_L2_STATION_ARP_RARP;
        vst.priority = 1000;
        memcpy(vst.dst_mac, g_switch_mac, 6);
        memset(vst.dst_mac_mask, 0xff, 6);
        vst.vlan = 0x7000;      /* VPN id of VFI 0; the mask makes it "any VFI" */
        vst.vlan_mask = 0;
        rv = bcm_l2_station_add(unit, &g_ip2me_vfi_station_id, &vst);
        if (BCM_FAILURE(rv)) {
            HWBCM_LOGE("bcm_l2_station_add(switch mac, any VFI): %s", bcm_errmsg(rv));
            g_ip2me_vfi_station_id = -1;
        }
    }
    HWBCM_LOGI("IP2ME station %d (VFI %d) for %02x:%02x:%02x:%02x:%02x:%02x", g_ip2me_station_id, g_ip2me_vfi_station_id,
               g_switch_mac[0], g_switch_mac[1], g_switch_mac[2],
               g_switch_mac[3], g_switch_mac[4], g_switch_mac[5]);
    return 0;
}

int hwbcm_switch_mac_set(const uint8_t mac[6])
{
    if (mac == NULL) {
        return BCM_E_PARAM;
    }
    memcpy(g_switch_mac, mac, 6);
    g_switch_mac_valid = 1;
    HWBCM_LOGI("switch mac %02x:%02x:%02x:%02x:%02x:%02x",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (g_inited && g_ip2me_wanted) {
        return ip2me_station_apply();
    }
    return 0;
}

int hwbcm_switch_mac_get(uint8_t mac[6])
{
    if (mac == NULL) {
        return BCM_E_PARAM;
    }
    if (!g_switch_mac_valid) {
        return BCM_E_NOT_FOUND;
    }
    memcpy(mac, g_switch_mac, 6);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* KNET                                                                     */
/* ------------------------------------------------------------------------ */

static hwbcm_knet_slot_t *knet_slot_alloc(void)
{
    int i;
    for (i = 0; i < HWBCM_KNET_MAX; i++) {
        if (g_knet[i].netif_id == 0) {
            return &g_knet[i];
        }
    }
    return NULL;
}

static hwbcm_knet_slot_t *knet_slot_find(int netif_id)
{
    int i;
    for (i = 0; i < HWBCM_KNET_MAX; i++) {
        if (g_knet[i].netif_id == netif_id) {
            return &g_knet[i];
        }
    }
    return NULL;
}

int hwbcm_knet_netif_create(int port, const char *name, const uint8_t mac[6], int *netif_id)
{
    int unit = HWBCM_UNIT;
    bcm_knet_netif_t netif;
    bcm_knet_filter_t filter;
    hwbcm_knet_slot_t *slot;
    int rv;

    if (!g_inited) {
        return BCM_E_INIT;
    }
    if (!port_valid(port) || name == NULL || mac == NULL || netif_id == NULL) {
        return BCM_E_PARAM;
    }
    slot = knet_slot_alloc();
    if (slot == NULL) {
        HWBCM_LOGE("knet: no free netif slot");
        return BCM_E_RESOURCE;
    }

    bcm_knet_netif_t_init(&netif);
    /* TX_LOCAL_PORT: frames from the kernel go out this port as sent
     * (untagged from the stack = untagged on the wire, verified on the box). */
    netif.type = BCM_KNET_NETIF_T_TX_LOCAL_PORT;
    netif.port = port;
    memcpy(netif.mac_addr, mac, 6);
    sal_strncpy(netif.name, name, BCM_KNET_NETIF_NAME_MAX - 1);
    netif.name[BCM_KNET_NETIF_NAME_MAX - 1] = '\0';
    rv = bcm_knet_netif_create(unit, &netif);
    if (BCM_FAILURE(rv)) {
        HWBCM_LOGE("bcm_knet_netif_create(%s, port %d): %s", name, port, bcm_errmsg(rv));
        return rv;
    }

    memset(slot, 0, sizeof(*slot));
    slot->netif_id = netif.id;
    slot->port = port;

    /* Filter (a): PVID frames, tag stripped (prio 49). Built for the port's
     * current PVID; hwbcm_port_pvid_set rebuilds it on change. */
    {
        bcm_vlan_t pvid = BCM_VLAN_DEFAULT;
        rv = bcm_port_untagged_vlan_get(unit, port, &pvid);
        if (BCM_FAILURE(rv)) {
            HWBCM_LOGW("port %d: bcm_port_untagged_vlan_get: %s, assuming %d", port, bcm_errmsg(rv), (int)pvid);
        }
        slot->pvid = (int)pvid;
    }
    rv = knet_pvid_filter_create(slot, netif.name);
    if (BCM_FAILURE(rv)) {
        (void)bcm_knet_netif_destroy(unit, netif.id);
        memset(slot, 0, sizeof(*slot));
        return rv;
    }
    if (hwbcm_riot_enabled()) {
        (void)knet_vfi_filter_create(slot, netif.name);   /* not fatal */
    }

    /* Filter (b): everything else that ingressed on this port, tag kept
     * (tagged VLAN members for the Linux bridge / VLAN subinterfaces). */
    bcm_knet_filter_t_init(&filter);
    filter.type = BCM_KNET_FILTER_T_RX_PKT;
    filter.priority = 50;
    filter.dest_type = BCM_KNET_DEST_T_NETIF;
    filter.dest_id = netif.id;
    filter.match_flags = BCM_KNET_FILTER_M_INGPORT;
    filter.m_ingport = port;
    snprintf(filter.desc, BCM_KNET_FILTER_DESC_MAX, "%s", netif.name);
    rv = bcm_knet_filter_create(unit, &filter);
    if (BCM_FAILURE(rv)) {
        HWBCM_LOGE("bcm_knet_filter_create(port %d -> netif %d): %s", port, netif.id, bcm_errmsg(rv));
        knet_filter_destroy_quiet(&slot->pvid_filter_id);
        (void)bcm_knet_netif_destroy(unit, netif.id);
        memset(slot, 0, sizeof(*slot));
        return rv;
    }
    slot->filter_id = filter.id;

    *netif_id = netif.id;
    HWBCM_LOGI("knet: netif %d (%s) port %d filters %d (pvid %d strip) / %d (any)",
               netif.id, netif.name, port, slot->pvid_filter_id, slot->pvid, filter.id);
    return 0;
}

int hwbcm_knet_cpu_netif_create(const char *name, const uint8_t mac[6], int *netif_id)
{
    int unit = HWBCM_UNIT;
    bcm_knet_netif_t netif;
    bcm_knet_filter_t filter;
    hwbcm_knet_slot_t *slot;
    int rv;

    if (!g_inited) {
        return BCM_E_INIT;
    }
    if (name == NULL || mac == NULL || netif_id == NULL) {
        return BCM_E_PARAM;
    }
    slot = knet_slot_alloc();
    if (slot == NULL) {
        HWBCM_LOGE("knet: no free netif slot");
        return BCM_E_RESOURCE;
    }

    bcm_knet_netif_t_init(&netif);
    netif.type = BCM_KNET_NETIF_T_TX_CPU_INGRESS;
    memcpy(netif.mac_addr, mac, 6);
    sal_strncpy(netif.name, name, BCM_KNET_NETIF_NAME_MAX - 1);
    netif.name[BCM_KNET_NETIF_NAME_MAX - 1] = '\0';
    rv = bcm_knet_netif_create(unit, &netif);
    if (BCM_FAILURE(rv)) {
        HWBCM_LOGE("bcm_knet_netif_create(cpu %s): %s", name, bcm_errmsg(rv));
        return rv;
    }

    /* Low-priority catch-all: anything not claimed by a port filter. */
    bcm_knet_filter_t_init(&filter);
    filter.type = BCM_KNET_FILTER_T_RX_PKT;
    filter.priority = 100;
    filter.dest_type = BCM_KNET_DEST_T_NETIF;
    filter.dest_id = netif.id;
    filter.match_flags = 0;
    snprintf(filter.desc, BCM_KNET_FILTER_DESC_MAX, "%s", netif.name);
    rv = bcm_knet_filter_create(unit, &filter);
    if (BCM_FAILURE(rv)) {
        HWBCM_LOGW("bcm_knet_filter_create(catch-all -> netif %d): %s", netif.id, bcm_errmsg(rv));
        filter.id = 0;
    }

    memset(slot, 0, sizeof(*slot));
    slot->netif_id = netif.id;
    slot->port = -1;
    slot->filter_id = filter.id;
    *netif_id = netif.id;
    HWBCM_LOGI("knet: cpu netif %d (%s) filter %d", netif.id, netif.name, filter.id);
    return 0;
}

int hwbcm_knet_netif_destroy(int netif_id)
{
    int unit = HWBCM_UNIT;
    hwbcm_knet_slot_t *slot;
    int rv;

    if (!g_inited) {
        return BCM_E_INIT;
    }
    if (netif_id <= 0) {
        return BCM_E_PARAM;
    }
    slot = knet_slot_find(netif_id);
    if (slot != NULL) {
        knet_filter_destroy_quiet(&slot->pvid_filter_id);
        knet_filter_destroy_quiet(&slot->vfi_filter_id);
        knet_filter_destroy_quiet(&slot->filter_id);
    }
    rv = bcm_knet_netif_destroy(unit, netif_id);
    if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
        HWBCM_LOGE("bcm_knet_netif_destroy(%d): %s", netif_id, bcm_errmsg(rv));
        return rv;
    }
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
    }
    HWBCM_LOGI("knet: netif %d destroyed", netif_id);
    return 0;
}

typedef struct {
    hwbcm_knet_info_t items[HWBCM_KNET_MAX];
    int count;
    int filter_matched;
} knet_wb_recover_ctx_t;

static int knet_wb_netif_cb(int unit, bcm_knet_netif_t *netif, void *user_data)
{
    knet_wb_recover_ctx_t *ctx = (knet_wb_recover_ctx_t *)user_data;
    hwbcm_knet_slot_t *slot;
    int is_cpu;

    (void)unit;
    if (netif == NULL || ctx == NULL || netif->id <= 0) {
        return BCM_E_NONE;
    }
    slot = knet_slot_alloc();
    if (slot == NULL) {
        HWBCM_LOGW("wb knet: g_knet full while recovering netif %d (%s)", netif->id, netif->name);
        return BCM_E_NONE;
    }
    is_cpu = (netif->type == BCM_KNET_NETIF_T_TX_CPU_INGRESS) ? 1 : 0;
    memset(slot, 0, sizeof(*slot));
    slot->netif_id = netif->id;
    slot->port = is_cpu ? -1 : (int)netif->port;

    if (ctx->count < HWBCM_KNET_MAX) {
        hwbcm_knet_info_t *info = &ctx->items[ctx->count++];
        memset(info, 0, sizeof(*info));
        info->netif_id = netif->id;
        info->port = slot->port;
        info->is_cpu = is_cpu;
        info->pvid = 0;
        snprintf(info->name, sizeof(info->name), "%s", netif->name);
    }
    return BCM_E_NONE;
}

static int knet_wb_filter_cb(int unit, bcm_knet_filter_t *filter, void *user_data)
{
    knet_wb_recover_ctx_t *ctx = (knet_wb_recover_ctx_t *)user_data;
    hwbcm_knet_slot_t *slot;
    int i;

    (void)unit;
    if (filter == NULL || ctx == NULL || filter->dest_type != BCM_KNET_DEST_T_NETIF) {
        return BCM_E_NONE;
    }
    slot = knet_slot_find(filter->dest_id);
    if (slot == NULL) {
        return BCM_E_NONE;
    }
    if (filter->priority == 48) {
        slot->vfi_filter_id = filter->id;
    } else if (filter->priority == 49) {
        slot->pvid_filter_id = filter->id;
        slot->pvid = (int)filter->m_vlan;
        for (i = 0; i < ctx->count; i++) {
            if (ctx->items[i].netif_id == slot->netif_id) {
                ctx->items[i].pvid = slot->pvid;
                break;
            }
        }
    } else if (filter->priority == 50 || filter->priority == 100) {
        slot->filter_id = filter->id;
    }
    ctx->filter_matched++;
    return BCM_E_NONE;
}

int hwbcm_knet_warm_recover(hwbcm_knet_info_t *out, int max)
{
    knet_wb_recover_ctx_t ctx;
    int rv, i, ncopy;

    if (!g_inited) {
        return BCM_E_INIT;
    }
    memset(&ctx, 0, sizeof(ctx));
    memset(g_knet, 0, sizeof(g_knet));

    rv = bcm_knet_netif_traverse(HWBCM_UNIT, knet_wb_netif_cb, &ctx);
    if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
        HWBCM_LOGE("wb knet: bcm_knet_netif_traverse failed: %s (%d)", bcm_errmsg(rv), rv);
        return rv;
    }
    rv = bcm_knet_filter_traverse(HWBCM_UNIT, knet_wb_filter_cb, &ctx);
    if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
        HWBCM_LOGW("wb knet: bcm_knet_filter_traverse failed: %s (%d)", bcm_errmsg(rv), rv);
    }

    if (out != NULL && max > 0) {
        ncopy = ctx.count < max ? ctx.count : max;
        memcpy(out, ctx.items, (size_t)ncopy * sizeof(hwbcm_knet_info_t));
    }
    for (i = 0; i < ctx.count && i < 8; i++) {
        HWBCM_LOGD("wb knet: recovered netif %d (%s) port=%d cpu=%d pvid=%d",
                   ctx.items[i].netif_id, ctx.items[i].name,
                   ctx.items[i].port, ctx.items[i].is_cpu, ctx.items[i].pvid);
    }
    HWBCM_LOGI("wb knet: recovered %d netif(s) and %d filter(s) with zero mutations",
               ctx.count, ctx.filter_matched);
    return ctx.count;
}

/* ------------------------------------------------------------------------ */
/* traps                                                                    */
/* ------------------------------------------------------------------------ */

/* L2 user entry (vlan-agnostic) that sends a well-known DA to the CPU only.
 * These protocols are link-local and must never be forwarded, so "copy" and
 * "trap" both become CPU+DISCARD here. */
static int trap_l2cache_set(int reason, const uint8_t mac[6], int action, int is_bpdu)
{
    int unit = HWBCM_UNIT;
    bcm_l2_cache_addr_t a;
    int idx_used = -1;
    int rv;

    if (g_trap_l2cache_idx[reason] >= 0) {
        rv = bcm_l2_cache_delete(unit, g_trap_l2cache_idx[reason]);
        if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
            HWBCM_LOGW("bcm_l2_cache_delete(%d): %s", g_trap_l2cache_idx[reason], bcm_errmsg(rv));
        }
        g_trap_l2cache_idx[reason] = -1;
    }

    bcm_l2_cache_addr_t_init(&a);
    memcpy(a.mac, mac, 6);
    memset(a.mac_mask, 0xff, 6);
    a.vlan = 0;
    a.vlan_mask = 0; /* any VLAN */
    a.prio = -1;
    if (action == 0) {
        a.flags = BCM_L2_CACHE_DISCARD;
    } else {
        a.flags = BCM_L2_CACHE_CPU | BCM_L2_CACHE_DISCARD;
        if (is_bpdu) {
            a.flags |= BCM_L2_CACHE_BPDU;
        }
        (void)bcm_stk_my_modid_get(unit, &a.dest_modid);
        a.dest_port = CMIC_PORT(unit);
    }
    rv = bcm_l2_cache_set(unit, -1, &a, &idx_used);
    if (BCM_FAILURE(rv)) {
        HWBCM_LOGE("bcm_l2_cache_set(trap %d): %s", reason, bcm_errmsg(rv));
        return rv;
    }
    g_trap_l2cache_idx[reason] = idx_used;
    HWBCM_LOGI("trap %d: l2 cache idx %d action %d", reason, idx_used, action);
    return 0;
}

static int trap_switch_control(const char *what, bcm_switch_control_t ctl, int val)
{
    int rv = bcm_switch_control_set(HWBCM_UNIT, ctl, val);
    if (BCM_FAILURE(rv) && rv != BCM_E_UNAVAIL) {
        HWBCM_LOGE("bcm_switch_control_set(%s, %d): %s", what, val, bcm_errmsg(rv));
        return rv;
    }
    if (rv == BCM_E_UNAVAIL) {
        HWBCM_LOGW("bcm_switch_control_set(%s): unavailable on this chip", what);
    }
    return 0;
}

int hwbcm_trap_set(int reason, int action, int priority_queue)
{
    static const uint8_t mac_lldp[6] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e };
    static const uint8_t mac_lacp[6] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x02 };
    static const uint8_t mac_stp[6]  = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x00 };
    static const uint8_t mac_udld[6] = { 0x01, 0x00, 0x0c, 0xcc, 0xcc, 0xcc };
    int to_cpu, drop;

    if (!g_inited) {
        return BCM_E_INIT;
    }
    if (action < 0 || action > 2) {
        return BCM_E_PARAM;
    }
    to_cpu = (action >= 1) ? 1 : 0;   /* copy or trap */
    drop = (action != 1) ? 1 : 0;     /* drop, or trap = cpu only */
    if (priority_queue >= 0) {
        HWBCM_LOGD("trap %d: priority queue %d not mapped (CPU cos mapping left at SDK default)",
                   reason, priority_queue);
    }

    switch (reason) {
    case HWBCM_TRAP_ARP:
        HWBCM_CHECK("ArpRequestToCpu", trap_switch_control("ArpRequestToCpu", bcmSwitchArpRequestToCpu, to_cpu));
        HWBCM_CHECK("ArpReplyToCpu", trap_switch_control("ArpReplyToCpu", bcmSwitchArpReplyToCpu, to_cpu));
        HWBCM_CHECK("ArpRequestDrop", trap_switch_control("ArpRequestDrop", bcmSwitchArpRequestDrop, drop));
        HWBCM_CHECK("ArpReplyDrop", trap_switch_control("ArpReplyDrop", bcmSwitchArpReplyDrop, drop));
        HWBCM_CHECK("NdPktToCpu", trap_switch_control("NdPktToCpu", bcmSwitchNdPktToCpu, to_cpu));
        break;
    case HWBCM_TRAP_LLDP:
        return trap_l2cache_set(reason, mac_lldp, action, 0);
    case HWBCM_TRAP_LACP:
        return trap_l2cache_set(reason, mac_lacp, action, 0);
    case HWBCM_TRAP_STP:
        return trap_l2cache_set(reason, mac_stp, action, 1);
    case HWBCM_TRAP_UDLD:
        return trap_l2cache_set(reason, mac_udld, action, 0);
    case HWBCM_TRAP_IP2ME:
    case HWBCM_TRAP_BGP:
        /* BGP is just IP2ME traffic on this chip; the station entry makes
         * the switch MAC subject to L3 lookup on every VLAN. */
        g_ip2me_wanted = to_cpu;
        HWBCM_CHECK("ip2me station", ip2me_station_apply());
        break;
    case HWBCM_TRAP_DHCP:
        HWBCM_CHECK("DhcpPktToCpu", trap_switch_control("DhcpPktToCpu", bcmSwitchDhcpPktToCpu, to_cpu));
        HWBCM_CHECK("DhcpPktDrop", trap_switch_control("DhcpPktDrop", bcmSwitchDhcpPktDrop, drop));
        break;
    case HWBCM_TRAP_TTL_ERR:
        HWBCM_CHECK("L3UcastTtl1ToCpu", trap_switch_control("L3UcastTtl1ToCpu", bcmSwitchL3UcastTtl1ToCpu, to_cpu));
        HWBCM_CHECK("L3HeaderErrToCpu", trap_switch_control("L3HeaderErrToCpu", bcmSwitchL3HeaderErrToCpu, to_cpu));
        break;
    case HWBCM_TRAP_NEIGH_MISS:
        HWBCM_CHECK("UnknownL3DestToCpu", trap_switch_control("UnknownL3DestToCpu", bcmSwitchUnknownL3DestToCpu, to_cpu));
        HWBCM_CHECK("L3SlowpathToCpu", trap_switch_control("L3SlowpathToCpu", bcmSwitchL3SlowpathToCpu, to_cpu));
        break;
    default:
        HWBCM_LOGW("trap %d: unknown reason", reason);
        return BCM_E_PARAM;
    }
    HWBCM_LOGI("trap %d: action %d", reason, action);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* diag shell                                                               */
/* ------------------------------------------------------------------------ */

int hwbcm_shell(const char *cmd)
{
    int rc;

    if (cmd == NULL) {
        return BCM_E_PARAM;
    }
    pthread_mutex_lock(&g_shell_lock);
    if (!g_inited || !SOC_UNIT_VALID(HWBCM_UNIT)) {
        pthread_mutex_unlock(&g_shell_lock);
        return BCM_E_INIT;
    }
    rc = hwbcm_shell_cmd(cmd);
    pthread_mutex_unlock(&g_shell_lock);
    return rc;
}
