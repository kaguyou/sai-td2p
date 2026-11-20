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
#include <soc/cm.h>
#include <soc/knet.h>
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
    static const int prio[] = { LOG_ERR, LOG_WARNING, LOG_INFO, LOG_DEBUG };
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
static int g_ip2me_wanted = 0;

/* knet netif -> filter(s) bookkeeping */
#define HWBCM_KNET_MAX 256
typedef struct {
    int netif_id;        /* 0 = free slot */
    int port;            /* bound front port, -1 for the CPU netif */
    int filter_id;       /* ingress-port filter (prio 50, tag kept), 0 = none */
    int pvid_filter_id;  /* ingress-port + PVID filter (prio 49, tag stripped), 0 = none */
    int pvid;            /* PVID the strip filter was built for */
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
static int hwbcm_shell_cmd(const char *cmd)
{
    char buf[HWBCM_SHELL_CMD_MAX];
    cmd_result_t cr;

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

    pthread_mutex_lock(&g_shell_lock);
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

int hwbcm_init(const char *config_bcm)
{
    int unit = HWBCM_UNIT;
    int rv;
    int i;
    bcm_port_t port;

    pthread_mutex_lock(&g_lock);
    if (g_inited) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    for (i = 0; i < HWBCM_PORT_TBL; i++) {
        g_desired_speed[i] = 0;
        g_sgmii_applied[i] = 0;
        g_sgmii_applied_at[i] = 0;
    }
    for (i = 0; i <= HWBCM_TRAP_STP; i++) {
        g_trap_l2cache_idx[i] = -1;
    }
    memset(g_knet, 0, sizeof(g_knet));

    if (config_bcm == NULL || config_bcm[0] == '\0') {
        HWBCM_LOGE("hwbcm_init: no config.bcm path");
        pthread_mutex_unlock(&g_lock);
        return BCM_E_PARAM;
    }

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
    HWBCM_LOGI("hwbcm_init: config %s", config_bcm);

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

    /* 4b. Diag shell library bring-up, the minimal subset of
     * diag_sdk_init() (src/appl/diag/system.c): variable lookup hook,
     * BSL log management, symbol tables / globals. Without this the shell
     * has no per-unit command table and sh_process_command returns CMD_NFND. */
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
    HWBCM_LOGI("hwbcm_init: %d device(s) probed", soc_ndev);
    if (sysconf_attach(unit) < 0) {
        HWBCM_LOGE("sysconf_attach(%d) failed", unit);
        pthread_mutex_unlock(&g_lock);
        return BCM_E_FAIL;
    }

    /* 7b. Install the ESW command table for the unit (what diag_sdk_init
     * does via cmdlist_init after attach) and bind the shell variables
     * ($unit, chip name, ...) to unit 0. */
    cmdlist_init();
    command_mode_set(unit, ESW_CMD_MODE);
    sh_swap_unit_vars(unit);

    /* 8. init soc / misc / mmu / bcm through the diag shell library */
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

    g_inited = 1;
    pthread_mutex_unlock(&g_lock);
    HWBCM_LOGI("hwbcm_init: done (%s)", soc_cm_get_name(unit));

    /* Debug aid, failure is not fatal. */
    (void)shell_sock_start();
    return 0;
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

    for (i = 0; i < HWBCM_KNET_MAX; i++) {
        if (g_knet[i].netif_id != 0) {
            knet_filter_destroy_quiet(&g_knet[i].pvid_filter_id);
            knet_filter_destroy_quiet(&g_knet[i].filter_id);
            (void)bcm_knet_netif_destroy(unit, g_knet[i].netif_id);
            memset(&g_knet[i], 0, sizeof(g_knet[i]));
        }
    }

    if (bcm_rx_active(unit)) {
        (void)bcm_rx_stop(unit, NULL);
    }

    HWBCM_WARN_ON_FAIL("bcm_detach", bcm_detach(unit));
    if (sysconf_detach(unit) < 0) {
        HWBCM_LOGW("sysconf_detach(%d) failed", unit);
    }

    g_inited = 0;
    g_fp_count = 0;
    pthread_mutex_unlock(&g_lock);
    HWBCM_LOGI("hwbcm_deinit: done");
    return 0;
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
    HWBCM_LOGI("IP2ME station %d for %02x:%02x:%02x:%02x:%02x:%02x", g_ip2me_station_id,
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

    if (!g_inited) {
        return BCM_E_INIT;
    }
    if (cmd == NULL) {
        return BCM_E_PARAM;
    }
    pthread_mutex_lock(&g_shell_lock);
    rc = hwbcm_shell_cmd(cmd);
    pthread_mutex_unlock(&g_shell_lock);
    return rc;
}
