/*
 * hwbcm_stats.c — queue / priority-group / buffer-pool statistics and the
 * MMU-sourced port counters for the Trident2+ hardware SAI. See
 * hwbcm_stats.h for the API contract.
 *
 * SDK facts this relies on (sdk-6.5.27):
 *  - Per-queue transmit counters: bcm_cosq_stat_get(port, cos,
 *    bcmCosqStatUCOutPackets/UCOutBytes/MCOutPackets/MCOutBytes) with a
 *    plain local port read EGR_PERQ_XMT_{PKT,BYTE}[_UC] at index cos
 *    (src/bcm/esw/trident2/cosq.c:10710-10833).
 *  - Per-queue drops: the public API only has the UC+MC sum per cos
 *    (bcmCosqStatDroppedPackets, trident2/cosq.c:10316-10399). The split is
 *    read the same way the SDK does it internally for a plain port:
 *    soc_counter_get(port, SOC_COUNTER_NON_DMA_COSQ_DROP_{PKT,BYTE}_UC, cos)
 *    for UC and SOC_COUNTER_NON_DMA_COSQ_DROP_{PKT,BYTE} for MC; the per-port
 *    base index is applied by the counter code (common/counter.c:1125-1146),
 *    and _BCM_TD2_COSQ_INDEX_STYLE_EGR_PERQ_* resolve to "cos" itself for a
 *    plain port (trident2/cosq.c:2383-2407).
 *  - Current occupancy: bcmCosqStatEgress{UC,MC}Queue{Min,}BytesCurrent are
 *    live MMU_THDU/THDM table reads, already in bytes (trident2/cosq.c:
 *    11229-11414); PG: bcmCosqStatIngressPortPG{Min,Shared,Headroom}Bytes-
 *    Current (counter thread, trident2/cosq.c:10904-11008).
 *  - Watermarks: BST. bcmSwitchBstTrackingMode=peak keeps the software cache
 *    at the max of every sync (esw/bst.c:3718-3724); bcm_cosq_bst_stat_get
 *    returns the cached value in cells, BCM_COSQ_BST_STAT_CLEAR_ON_READ clears
 *    hw + cache of that entry (esw/bst.c:3090-3104). Index resolution per
 *    resource: trident2/cosq.c:7437-7620.
 */

#include "hwbcm.h"
#include "hwbcm_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include <bcm/types.h>
#include <bcm/error.h>
#include <bcm/cosq.h>
#include <bcm/switch.h>
#include <bcm/port.h>
#include <bcm/stat.h>
#include <soc/drv.h>
#include <soc/counter.h>

#define U HWBCM_UNIT

#define HWBCM_LOGE(...) hwbcm_log(HWBCM_LOG_ERR, __VA_ARGS__)
#define HWBCM_LOGW(...) hwbcm_log(HWBCM_LOG_WARN, __VA_ARGS__)
#define HWBCM_LOGI(...) hwbcm_log(HWBCM_LOG_INFO, __VA_ARGS__)
#define HWBCM_LOGD(...) hwbcm_log(HWBCM_LOG_DEBUG, __VA_ARGS__)

#define NUM_PRIO 16 /* TD2 internal priorities that map to a PG */
#define NUM_PG   8
#define NUM_SP   4  /* service pools per direction */

/* ------------------------------------------------------------------------ */
/* state                                                                    */
/* ------------------------------------------------------------------------ */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_inited;
static int g_bst_on;
static int g_num_cos = HWBCM_STATS_MAX_UC_QUEUES;
static int g_num_cpu_cos = HWBCM_STATS_MAX_CPU_QUEUES;
static int g_soft_logs;
static uint64_t g_bst_last_ms[bcmBstStatIdMaxCount];

/* Software baselines for READ_AND_CLEAR of cumulative counters. */
enum { QB_PKT = 0, QB_BYTE, QB_DPKT, QB_DBYTE, QB_COUNT };
static uint64_t g_qbase[HWBCM_MAX_PORTS][2][HWBCM_STATS_MAX_CPU_QUEUES][QB_COUNT];
static uint64_t g_xbase[HWBCM_MAX_PORTS][HWBCM_XPSTAT_COUNT];

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int port_ok(int port)
{
    return port >= 0 && port < HWBCM_MAX_PORTS;
}

static uint64_t u64_of(uint64 v)
{
    return ((uint64_t)COMPILER_64_HI(v) << 32) | (uint64_t)COMPILER_64_LO(v);
}

/* SDK "this object has no such counter" -> 0, real failures pass through. */
static int soft_rv(int rv, const char *what, int port, int idx)
{
    if (rv >= 0) {
        return 0;
    }
    if (rv == BCM_E_UNAVAIL || rv == BCM_E_PARAM || rv == BCM_E_NOT_FOUND) {
        if (g_soft_logs < 32) {
            g_soft_logs++;
            HWBCM_LOGW("stats: %s(port %d, idx %d): %s -> reported as 0%s",
                       what, port, idx, bcm_errmsg(rv),
                       g_soft_logs == 32 ? " (further messages suppressed)" : "");
        }
        return 0;
    }
    HWBCM_LOGE("stats: %s(port %d, idx %d): %s", what, port, idx, bcm_errmsg(rv));
    return rv;
}

/* hw - baseline, and move the baseline when clearing. Caller holds g_lock. */
static uint64_t baseline_apply(uint64_t *base, uint64_t raw, int clear)
{
    uint64_t v;

    if (raw < *base) {
        *base = 0;          /* counter was reset underneath us */
    }
    v = raw - *base;
    if (clear) {
        *base = raw;
    }
    return v;
}

/* ------------------------------------------------------------------------ */
/* init                                                                     */
/* ------------------------------------------------------------------------ */

int hwbcm_stats_init(void)
{
    int rv, numq = 0;

    pthread_mutex_lock(&g_lock);
    if (g_inited) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    g_inited = 1;

    if (BCM_SUCCESS(bcm_cosq_config_get(U, &numq)) && numq > 0) {
        g_num_cos = numq > HWBCM_STATS_MAX_UC_QUEUES ? HWBCM_STATS_MAX_UC_QUEUES : numq;
    }
    if (NUM_CPU_COSQ(U) > 0) {
        g_num_cpu_cos = NUM_CPU_COSQ(U) > HWBCM_STATS_MAX_CPU_QUEUES ?
                        HWBCM_STATS_MAX_CPU_QUEUES : NUM_CPU_COSQ(U);
    }

    if (getenv("HWBCM_STATS_NO_BST") != NULL) {
        HWBCM_LOGW("stats: HWBCM_STATS_NO_BST set, watermarks read as 0");
        g_bst_on = 0;
    } else if (hwbcm_is_warm_boot()) {
        int bst_en = 0;
        rv = bcm_switch_control_get(U, bcmSwitchBstEnable, &bst_en);
        g_bst_on = (BCM_SUCCESS(rv) && bst_en) ? 1 : 0;
        HWBCM_LOGI("stats: warm boot probed BST=%d (rv=%d)", g_bst_on, rv);
    } else {
        rv = bcm_switch_control_set(U, bcmSwitchBstTrackingMode, bcmSwitchBstTrackPeak);
        if (BCM_SUCCESS(rv)) {
            rv = bcm_switch_control_set(U, bcmSwitchBstEnable, 1);
        }
        g_bst_on = BCM_SUCCESS(rv) ? 1 : 0;
        if (!g_bst_on) {
            HWBCM_LOGW("stats: BST enable failed (%s), watermarks read as 0", bcm_errmsg(rv));
        }
    }
    HWBCM_LOGI("stats: init num_cos %d cpu_cos %d bst %s",
               g_num_cos, g_num_cpu_cos, g_bst_on ? "on (peak)" : "off");
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int hwbcm_stats_bst_enabled(void)
{
    int on;

    (void)hwbcm_stats_init();
    pthread_mutex_lock(&g_lock);
    on = g_bst_on;
    pthread_mutex_unlock(&g_lock);
    return on;
}

uint32_t hwbcm_stats_cell_bytes(void)
{
    return HWBCM_STATS_CELL_BYTES;
}

int hwbcm_stats_cpu_port(void)
{
    return CMIC_PORT(U);
}

/* ------------------------------------------------------------------------ */
/* BST helpers                                                              */
/* ------------------------------------------------------------------------ */

static void bst_sync(bcm_bst_stat_id_t bid)
{
    uint64_t t = now_ms();
    int rv;

    if (bid < 0 || bid >= bcmBstStatIdMaxCount) {
        return;
    }
    if (g_bst_last_ms[bid] != 0 && t - g_bst_last_ms[bid] < HWBCM_STATS_BST_SYNC_MS) {
        return;
    }
    rv = bcm_cosq_bst_stat_sync(U, bid);
    if (BCM_FAILURE(rv)) {
        (void)soft_rv(rv, "bcm_cosq_bst_stat_sync", -1, (int)bid);
    }
    g_bst_last_ms[bid] = t;
}

/* Cells; caller holds g_lock. */
static int bst_read(int port, int cosq, bcm_bst_stat_id_t bid, int clear, uint64_t *cells)
{
    uint64 v;
    int rv;

    *cells = 0;
    if (!g_bst_on) {
        return 0;
    }
    bst_sync(bid);
    COMPILER_64_ZERO(v);
    rv = bcm_cosq_bst_stat_get(U, port, cosq, bid,
                               clear ? BCM_COSQ_BST_STAT_CLEAR_ON_READ : 0, &v);
    if (BCM_FAILURE(rv)) {
        return soft_rv(rv, "bcm_cosq_bst_stat_get", port, cosq);
    }
    *cells = u64_of(v);
    return 0;
}

static int cosq_stat(int port, int cosq, bcm_cosq_stat_t stat, uint64_t *out)
{
    uint64 v;
    int rv;

    *out = 0;
    COMPILER_64_ZERO(v);
    rv = bcm_cosq_stat_get(U, port, cosq, stat, &v);
    if (BCM_FAILURE(rv)) {
        return soft_rv(rv, "bcm_cosq_stat_get", port, cosq);
    }
    *out = u64_of(v);
    return 0;
}

static int soc_ctr(int port, int idx, soc_counter_non_dma_id_t id, uint64_t *out)
{
    uint64 v;
    int rv;

    *out = 0;
    COMPILER_64_ZERO(v);
    rv = soc_counter_get(U, port, (soc_reg_t)id, idx, &v);
    if (SOC_FAILURE(rv)) {
        return soft_rv(rv, "soc_counter_get", port, idx);
    }
    *out = u64_of(v);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* queues                                                                   */
/* ------------------------------------------------------------------------ */

/* One cumulative queue counter (QB_*), raw hardware value. */
static int queue_counter_raw(int port, int cos, int mc, int cpu, int which, uint64_t *v)
{
    static const bcm_cosq_stat_t cpu_stat[QB_COUNT] = {
        bcmCosqStatOutPackets, bcmCosqStatOutBytes,
        bcmCosqStatDroppedPackets, bcmCosqStatDroppedBytes
    };

    if (cpu) {
        /* CPU port: no UC queues, the generic stats are the MC/CPU queue
         * (trident2/cosq.c:10591 skips UC for IS_CPU_PORT). */
        return cosq_stat(port, cos, cpu_stat[which], v);
    }
    switch (which) {
    case QB_PKT:
        return cosq_stat(port, cos, mc ? bcmCosqStatMCOutPackets : bcmCosqStatUCOutPackets, v);
    case QB_BYTE:
        return cosq_stat(port, cos, mc ? bcmCosqStatMCOutBytes : bcmCosqStatUCOutBytes, v);
    case QB_DPKT:
        return soc_ctr(port, cos, mc ? SOC_COUNTER_NON_DMA_COSQ_DROP_PKT :
                                       SOC_COUNTER_NON_DMA_COSQ_DROP_PKT_UC, v);
    case QB_DBYTE:
        return soc_ctr(port, cos, mc ? SOC_COUNTER_NON_DMA_COSQ_DROP_BYTE :
                                       SOC_COUNTER_NON_DMA_COSQ_DROP_BYTE_UC, v);
    default:
        *v = 0;
        return 0;
    }
}

int hwbcm_queue_stat_multi_get(int port, int queue, int is_mc, int n,
                               const int *ids, int clear, uint64_t *out)
{
    uint64_t ctr[QB_COUNT], cur_min = 0, cur_sh = 0, wm = 0, a, b;
    int have_ctr[QB_COUNT] = {0, 0, 0, 0};
    int have_cur = 0, have_wm = 0;
    int i, rv = 0, cpu, mc, maxq;

    if (!port_ok(port) || n < 0 || (n > 0 && (ids == NULL || out == NULL))) {
        return BCM_E_PARAM;
    }
    (void)hwbcm_stats_init();

    cpu = (port == CMIC_PORT(U));
    mc = cpu ? 1 : (is_mc ? 1 : 0);

    pthread_mutex_lock(&g_lock);
    maxq = cpu ? g_num_cpu_cos : g_num_cos;
    if (queue < 0 || queue >= maxq) {
        pthread_mutex_unlock(&g_lock);
        for (i = 0; i < n; i++) {
            out[i] = 0;
        }
        return 0;
    }
    for (i = 0; i < n && rv == 0; i++) {
        out[i] = 0;
        switch (ids[i]) {
        case HWBCM_QSTAT_PACKETS:
        case HWBCM_QSTAT_BYTES:
        case HWBCM_QSTAT_DROPPED_PACKETS:
        case HWBCM_QSTAT_DROPPED_BYTES: {
            int w = ids[i] - HWBCM_QSTAT_PACKETS; /* QB_* order matches */
            if (!have_ctr[w]) {
                uint64_t raw = 0;
                rv = queue_counter_raw(port, queue, mc, cpu, w, &raw);
                ctr[w] = rv == 0 ? baseline_apply(&g_qbase[port][mc][queue][w], raw, clear) : 0;
                have_ctr[w] = 1;
            }
            out[i] = ctr[w];
            break;
        }
        case HWBCM_QSTAT_CURR_CELLS:
        case HWBCM_QSTAT_SHARED_CURR_CELLS:
            if (!have_cur) {
                if (mc) {
                    rv = cosq_stat(port, queue, bcmCosqStatEgressMCQueueMinBytesCurrent, &a);
                    if (rv == 0) {
                        rv = cosq_stat(port, queue, bcmCosqStatEgressMCQueueBytesCurrent, &b);
                    }
                } else {
                    rv = cosq_stat(port, queue, bcmCosqStatEgressUCQueueMinBytesCurrent, &a);
                    if (rv == 0) {
                        rv = cosq_stat(port, queue, bcmCosqStatEgressUCQueueBytesCurrent, &b);
                    }
                }
                if (rv == 0) {
                    cur_min = a / HWBCM_STATS_CELL_BYTES;
                    cur_sh = b / HWBCM_STATS_CELL_BYTES;
                }
                have_cur = 1;
            }
            out[i] = (ids[i] == HWBCM_QSTAT_CURR_CELLS) ? cur_min + cur_sh : cur_sh;
            break;
        case HWBCM_QSTAT_WATERMARK_CELLS:
            if (!have_wm) {
                rv = bst_read(port, queue, mc ? bcmBstStatIdMcast : bcmBstStatIdUcast, clear, &wm);
                have_wm = 1;
            }
            out[i] = wm;
            break;
        default:
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rv;
}


/* ------------------------------------------------------------------------ */
/* priority groups                                                          */
/* ------------------------------------------------------------------------ */

/* Lowest priority mapped to pg on port, -1 if none. */
static int pg_priority(int port, int pg)
{
    int pri, g, rv;

    for (pri = 0; pri < NUM_PRIO; pri++) {
        g = -1;
        rv = bcm_port_priority_group_mapping_get(U, port, pri, &g);
        if (BCM_FAILURE(rv)) {
            (void)soft_rv(rv, "bcm_port_priority_group_mapping_get", port, pri);
            return -1;
        }
        if (g == pg) {
            return pri;
        }
    }
    return -1;
}

int hwbcm_pg_stat_multi_get(int port, int pg, int n, const int *ids,
                            int clear, uint64_t *out)
{
    uint64_t mn = 0, sh = 0, hd = 0, wsh = 0, whd = 0;
    int have_cur = 0, have_wsh = 0, have_whd = 0;
    int i, rv = 0, pri;

    if (!port_ok(port) || n < 0 || (n > 0 && (ids == NULL || out == NULL))) {
        return BCM_E_PARAM;
    }
    (void)hwbcm_stats_init();

    for (i = 0; i < n; i++) {
        out[i] = 0;
    }
    if (pg < 0 || pg >= NUM_PG || port == CMIC_PORT(U)) {
        return 0;
    }

    pthread_mutex_lock(&g_lock);
    pri = pg_priority(port, pg);
    if (pri < 0) {
        pthread_mutex_unlock(&g_lock);
        return 0;   /* nothing maps to this PG: it holds no buffers */
    }
    for (i = 0; i < n && rv == 0; i++) {
        switch (ids[i]) {
        case HWBCM_PGSTAT_CURR_CELLS:
        case HWBCM_PGSTAT_SHARED_CURR_CELLS:
        case HWBCM_PGSTAT_HEADROOM_CURR_CELLS:
            if (!have_cur) {
                rv = cosq_stat(port, pri, bcmCosqStatIngressPortPGMinBytesCurrent, &mn);
                if (rv == 0) {
                    rv = cosq_stat(port, pri, bcmCosqStatIngressPortPGSharedBytesCurrent, &sh);
                }
                if (rv == 0) {
                    rv = cosq_stat(port, pri, bcmCosqStatIngressPortPGHeadroomBytesCurrent, &hd);
                }
                mn /= HWBCM_STATS_CELL_BYTES;
                sh /= HWBCM_STATS_CELL_BYTES;
                hd /= HWBCM_STATS_CELL_BYTES;
                have_cur = 1;
            }
            out[i] = ids[i] == HWBCM_PGSTAT_CURR_CELLS ? mn + sh + hd :
                     ids[i] == HWBCM_PGSTAT_SHARED_CURR_CELLS ? sh : hd;
            break;
        case HWBCM_PGSTAT_SHARED_WATERMARK_CELLS:
            if (!have_wsh) {
                rv = bst_read(port, pri, bcmBstStatIdPriGroupShared, clear, &wsh);
                have_wsh = 1;
            }
            out[i] = wsh;
            break;
        case HWBCM_PGSTAT_HEADROOM_WATERMARK_CELLS:
            if (!have_whd) {
                rv = bst_read(port, pri, bcmBstStatIdPriGroupHeadroom, clear, &whd);
                have_whd = 1;
            }
            out[i] = whd;
            break;
        default:
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rv;
}


/* ------------------------------------------------------------------------ */
/* buffer pools                                                             */
/* ------------------------------------------------------------------------ */

/* Read (and optionally clear) TD2+ egress service pool watermark directly from
 * MMU_THDM_DB_POOL_MCUC_BST_STATr[sp] (sdk esw/bst.c:1553, 3530-3559).
 * Avoids bcm_cosq_bst_stat_get(bcmBstStatIdEgrPool) which in non-ETS mode only
 * resolves sp via multicast queue MMU_THDM_MCQE_QUEUE_CONFIG (cosq.c:2343-2358)
 * and therefore fails for UC-only egress service pools (SP1..SP3).
 * Caller holds g_lock. */
static int bst_read_egr_pool(int sp, int clear, uint64_t *cells)
{
    uint32 rval = 0, en_rval = 0, prev_thdo = 0;
    int rv;

    *cells = 0;
    if (!g_bst_on) {
        return 0;
    }
    if (sp < 0 || sp >= NUM_SP) {
        return BCM_E_PARAM;
    }
    if (!SOC_REG_IS_VALID(U, MMU_THDM_DB_POOL_MCUC_BST_STATr)) {
        return 0;
    }

    rv = soc_reg32_get(U, BST_TRACKING_ENABLEr, REG_PORT_ANY, 0, &en_rval);
    if (SOC_FAILURE(rv)) {
        return soft_rv(rv, "BST_TRACKING_ENABLEr get", -1, sp);
    }
    prev_thdo = soc_reg_field_get(U, BST_TRACKING_ENABLEr, en_rval, BST_TRACK_EN_THDOf);
    if (prev_thdo) {
        uint32 dis = en_rval;
        soc_reg_field_set(U, BST_TRACKING_ENABLEr, &dis, BST_TRACK_EN_THDOf, 0);
        rv = soc_reg32_set(U, BST_TRACKING_ENABLEr, REG_PORT_ANY, 0, dis);
        if (SOC_FAILURE(rv)) {
            return soft_rv(rv, "BST_TRACKING_ENABLEr disable", -1, sp);
        }
    }

    rv = soc_reg32_get(U, MMU_THDM_DB_POOL_MCUC_BST_STATr, REG_PORT_ANY, sp, &rval);
    if (SOC_SUCCESS(rv)) {
        *cells = (uint64_t)soc_reg_field_get(U, MMU_THDM_DB_POOL_MCUC_BST_STATr, rval, BST_STATf);
        if (clear) {
            soc_reg_field_set(U, MMU_THDM_DB_POOL_MCUC_BST_STATr, &rval, BST_STATf, 0);
            rv = soc_reg32_set(U, MMU_THDM_DB_POOL_MCUC_BST_STATr, REG_PORT_ANY, sp, rval);
        }
    }

    if (prev_thdo) {
        (void)soc_reg32_set(U, BST_TRACKING_ENABLEr, REG_PORT_ANY, 0, en_rval);
    }
    if (SOC_FAILURE(rv)) {
        return soft_rv(rv, "MMU_THDM_DB_POOL_MCUC_BST_STATr", -1, sp);
    }
    return 0;
}

int hwbcm_pool_stat_multi_get(int egress, int port, int cosq, int is_mc,
                              int n, const int *ids, int clear,
                              uint64_t *out, int *sp_out)
{
    uint64_t wm = 0;
    int i, rv = 0, sp = -1, have_wm = 0;

    if (!port_ok(port) || n < 0 || (n > 0 && (ids == NULL || out == NULL))) {
        return BCM_E_PARAM;
    }
    (void)hwbcm_stats_init();
    for (i = 0; i < n; i++) {
        out[i] = 0;
    }

    pthread_mutex_lock(&g_lock);
    if (egress) {
        rv = bcm_cosq_control_get(U, port, cosq,
                                  is_mc ? bcmCosqControlMCEgressPool : bcmCosqControlUCEgressPool,
                                  &sp);
    } else {
        rv = bcm_cosq_control_get(U, port, cosq, bcmCosqControlIngressPool, &sp);
    }
    if (BCM_FAILURE(rv)) {
        rv = soft_rv(rv, egress ? "egress pool resolve" : "ingress pool resolve", port, cosq);
        pthread_mutex_unlock(&g_lock);
        if (sp_out != NULL) {
            *sp_out = -1;
        }
        return rv;
    }
    if (sp_out != NULL) {
        *sp_out = sp;
    }

    for (i = 0; i < n && rv == 0; i++) {
        if (ids[i] == HWBCM_POOLSTAT_WATERMARK_CELLS) {
            if (!have_wm) {
                if (egress) {
                    rv = bst_read_egr_pool(sp, clear, &wm);
                } else {
                    /* IngPool takes (port, PG) -> THDI_PORT_PG_SPID (cosq.c:7480-
                     * 7484 -> _bcm_td2_cosq_ingress_sp_get, cosq.c:7352); the resource
                     * itself is per service pool, not per port (esw/bst.c:1572-1585). */
                    rv = bst_read(port, cosq, bcmBstStatIdIngPool, clear, &wm);
                }
                have_wm = 1;
            }
            out[i] = wm;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rv;
}


/* ------------------------------------------------------------------------ */
/* extra port counters                                                      */
/* ------------------------------------------------------------------------ */

static int bstat(int port, bcm_stat_val_t t, uint64_t *out)
{
    uint64 v;
    int rv;

    *out = 0;
    COMPILER_64_ZERO(v);
    rv = bcm_stat_get(U, port, t, &v);
    if (BCM_FAILURE(rv)) {
        return soft_rv(rv, "bcm_stat_get", port, (int)t);
    }
    *out = u64_of(v);
    return 0;
}

static int xstat_raw(int port, int id, uint64_t *v)
{
    uint64_t a = 0, b = 0, c = 0;
    int rv;

    *v = 0;
    switch (id) {
    case HWBCM_XPSTAT_IN_DROPPED_PKTS:
        /* MMU_CTR_ING_DROP_MEM (cosq.c:11415-11431, counter.c:5451-5461) */
        return cosq_stat(port, BCM_COS_INVALID, bcmCosqStatSourcePortDroppedPackets, v);
    case HWBCM_XPSTAT_OUT_DROPPED_PKTS:
        /* cos -1: sum of all UC + MC queue drops (cosq.c:10364-10397) */
        return cosq_stat(port, BCM_COS_INVALID, bcmCosqStatDroppedPackets, v);
    case HWBCM_XPSTAT_GREEN_WRED_DROPPED_PKTS:
        return cosq_stat(port, BCM_COS_INVALID, bcmCosqStatGreenDiscardDroppedPackets, v);
    case HWBCM_XPSTAT_YELLOW_WRED_DROPPED_PKTS:
        return cosq_stat(port, BCM_COS_INVALID, bcmCosqStatYellowDiscardDroppedPackets, v);
    case HWBCM_XPSTAT_RED_WRED_DROPPED_PKTS:
        return cosq_stat(port, BCM_COS_INVALID, bcmCosqStatRedDiscardDroppedPackets, v);
    case HWBCM_XPSTAT_WRED_DROPPED_PKTS:
        rv = cosq_stat(port, BCM_COS_INVALID, bcmCosqStatGreenDiscardDroppedPackets, &a);
        if (rv == 0) {
            rv = cosq_stat(port, BCM_COS_INVALID, bcmCosqStatYellowDiscardDroppedPackets, &b);
        }
        if (rv == 0) {
            rv = cosq_stat(port, BCM_COS_INVALID, bcmCosqStatRedDiscardDroppedPackets, &c);
        }
        *v = a + b + c;
        return rv;
    case HWBCM_XPSTAT_ETHER_PKTS_1519_TO_2047:
        rv = bstat(port, snmpBcmEtherStatsPkts1519to1522Octets, &a);
        if (rv == 0) {
            rv = bstat(port, snmpBcmEtherStatsPkts1522to2047Octets, &b);
        }
        *v = a + b;
        return rv;
    case HWBCM_XPSTAT_ETHER_PKTS_2048_TO_4095:
        return bstat(port, snmpBcmEtherStatsPkts2048to4095Octets, v);
    case HWBCM_XPSTAT_ETHER_PKTS_4096_TO_9216:
        return bstat(port, snmpBcmEtherStatsPkts4095to9216Octets, v);
    case HWBCM_XPSTAT_ETHER_PKTS_9217_TO_16383:
        return bstat(port, snmpBcmEtherStatsPkts9217to16383Octets, v);
    case HWBCM_XPSTAT_IP_IN_RECEIVES:
        return bstat(port, snmpIpInReceives, v);
    case HWBCM_XPSTAT_IP_IN_DISCARDS:
        return bstat(port, snmpIpInDiscards, v);
    case HWBCM_XPSTAT_IPV6_IN_RECEIVES:
        return bstat(port, snmpIpv6IfStatsInReceives, v);
    case HWBCM_XPSTAT_IPV6_IN_DISCARDS:
        return bstat(port, snmpIpv6IfStatsInDiscards, v);
    default:
        return 0;
    }
}

int hwbcm_port_xstat_multi_get(int port, int n, const int *ids, int clear,
                               uint64_t *out)
{
    uint64_t raw;
    int i, rv = 0;

    if (!port_ok(port) || n < 0 || (n > 0 && (ids == NULL || out == NULL))) {
        return BCM_E_PARAM;
    }
    (void)hwbcm_stats_init();

    pthread_mutex_lock(&g_lock);
    for (i = 0; i < n && rv == 0; i++) {
        if (ids[i] <= HWBCM_XPSTAT_NONE || ids[i] >= HWBCM_XPSTAT_COUNT) {
            continue;
        }
        raw = 0;
        rv = xstat_raw(port, ids[i], &raw);
        if (rv == 0) {
            out[i] = baseline_apply(&g_xbase[port][ids[i]], raw, clear);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rv;
}

/* ------------------------------------------------------------------------ */
/* warm boot (Phase 3)                                                      */
/* ------------------------------------------------------------------------ */

void hwbcm_stats_warm_reset(void)
{
    pthread_mutex_lock(&g_lock);
    g_inited = 0;
    g_bst_on = 0;
    g_soft_logs = 0;
    memset(g_bst_last_ms, 0, sizeof(g_bst_last_ms));
    memset(g_qbase, 0, sizeof(g_qbase));
    memset(g_xbase, 0, sizeof(g_xbase));
    pthread_mutex_unlock(&g_lock);
}

int hwbcm_stats_wb_get_qbase(hwbcm_qbase_wb_t *out, int max)
{
    int p, mc, q, w, count = 0;

    pthread_mutex_lock(&g_lock);
    for (p = 0; p < HWBCM_MAX_PORTS; p++) {
        for (mc = 0; mc < 2; mc++) {
            for (q = 0; q < HWBCM_STATS_MAX_CPU_QUEUES; q++) {
                for (w = 0; w < QB_COUNT; w++) {
                    uint64_t val = g_qbase[p][mc][q][w];
                    if (val == 0) {
                        continue;
                    }
                    if (out != NULL && count < max) {
                        out[count].port  = (uint8_t)p;
                        out[count].mc    = (uint8_t)mc;
                        out[count].queue = (uint8_t)q;
                        out[count].which = (uint8_t)w;
                        out[count].value = val;
                    }
                    count++;
                }
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    return count;
}

int hwbcm_stats_wb_get_xbase(hwbcm_xbase_wb_t *out, int max)
{
    int p, id, count = 0;

    pthread_mutex_lock(&g_lock);
    for (p = 0; p < HWBCM_MAX_PORTS; p++) {
        for (id = 0; id < HWBCM_XPSTAT_COUNT; id++) {
            uint64_t val = g_xbase[p][id];
            if (val == 0) {
                continue;
            }
            if (out != NULL && count < max) {
                out[count].port  = (uint8_t)p;
                out[count].id    = (uint8_t)id;
                out[count].value = val;
            }
            count++;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return count;
}

void hwbcm_stats_warm_restore(const hwbcm_qbase_wb_t *qbase, int nq,
                              const hwbcm_xbase_wb_t *xbase, int nx)
{
    int i, rq = 0, rx = 0;

    pthread_mutex_lock(&g_lock);
    memset(g_qbase, 0, sizeof(g_qbase));
    memset(g_xbase, 0, sizeof(g_xbase));
    memset(g_bst_last_ms, 0, sizeof(g_bst_last_ms));

    for (i = 0; i < nq && qbase != NULL; i++) {
        int p = qbase[i].port;
        int mc = qbase[i].mc;
        int q = qbase[i].queue;
        int w = qbase[i].which;
        if (p < HWBCM_MAX_PORTS && mc < 2 && q < HWBCM_STATS_MAX_CPU_QUEUES && w < QB_COUNT) {
            g_qbase[p][mc][q][w] = qbase[i].value;
            rq++;
        }
    }
    for (i = 0; i < nx && xbase != NULL; i++) {
        int p = xbase[i].port;
        int id = xbase[i].id;
        if (p < HWBCM_MAX_PORTS && id < HWBCM_XPSTAT_COUNT) {
            g_xbase[p][id] = xbase[i].value;
            rx++;
        }
    }
    pthread_mutex_unlock(&g_lock);
    HWBCM_LOGI("stats: warm restored %d queue baseline(s) and %d port xstat baseline(s)", rq, rx);
}
