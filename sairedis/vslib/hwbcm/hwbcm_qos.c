/*
 * hwbcm_qos.c — QoS / buffer section of hwbcm (hwbcm_qos.h) for Trident2+
 * (BCM56864), OpenBCM sdk-6.5.27.
 *
 * Covers: ingress classification (DSCP / 802.1p -> internal priority + color),
 * egress DSCP remark, default TC, TC -> cos, TC -> PG, PFC (tx/rx, class ->
 * cos), L1 scheduling (SP/WRR/DWRR) and min/max shapers, port shaper,
 * WRED/ECN on unicast queues, THDI per-PG and THDU/THDM per-queue thresholds.
 *
 * Everything uses the non-ETS (port, cos) form of the bcm_cosq API; see the
 * header comment of hwbcm_qos.h and notes/integration_qos.md.
 *
 * Direct register / memory access (SDK has no non-ETS API for it) is limited
 * to: THDI_PORT_PG_CONFIG admission fields (shared limit / dynamic / reset
 * floor / offset) and the egress queue alpha fields. Guaranteed (min) and
 * headroom cells always go through bcm_cosq_control_set so the SDK keeps the
 * service-pool shared limits consistent.
 *
 * All functions return 0 or a negative BCM_E_* value. unit is HWBCM_UNIT.
 */

/* The SDK build flags carry -DBCM_HIDE_DISPATCHABLE; it hides the bcm_*
 * prototypes we call. */
#ifdef BCM_HIDE_DISPATCHABLE
#undef BCM_HIDE_DISPATCHABLE
#endif

#include "hwbcm_qos.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <soc/drv.h>
#include <soc/mem.h>
#include <soc/trident2.h>

#include <bcm/types.h>
#include <bcm/error.h>
#include <bcm/port.h>
#include <bcm/cosq.h>
#include <bcm/qos.h>

#define U HWBCM_UNIT

#define QLOGE(...) hwbcm_log(HWBCM_LOG_ERR, __VA_ARGS__)
#define QLOGW(...) hwbcm_log(HWBCM_LOG_WARN, __VA_ARGS__)
#define QLOGI(...) hwbcm_log(HWBCM_LOG_INFO, __VA_ARGS__)
#define QLOGD(...) hwbcm_log(HWBCM_LOG_DEBUG, __VA_ARGS__)

/* Evaluate an SDK call; on failure log and return. */
#define QCHECK(what, call)                                                   \
    do {                                                                     \
        int __rv = (call);                                                   \
        if (BCM_FAILURE(__rv)) {                                             \
            QLOGE("qos: %s: %s (%d)", (what), bcm_errmsg(__rv), __rv);       \
            return __rv;                                                     \
        }                                                                    \
    } while (0)

/* MC queue hw index offset used by the SDK for THDM tables
 * (src/bcm/esw/trident2/cosq.c:14706 "MC queues comes after UC queues"). */
#define QOS_TD2_MC_QUEUE_BASE 1480

/* WRED refresh time used by bcm_cosq_gport_discard_t_init() callers
 * (1..512, one shared time domain). */
#define QOS_WRED_REFRESH_TIME 8

/* "Unused" WRED curve threshold (MIN_THD/MAX_THD sentinel 0x1ffff cells,
 * src/bcm/esw/trident2/cosq.c:6100-6117). */
#define QOS_WRED_CELLS_OFF 0x1ffff

static int s_qos_ready = 0;

static int qos_port_ok(int port)
{
    if (port < 0 || port >= HWBCM_MAX_PORTS || !SOC_PORT_VALID(U, port)) {
        return 0;
    }
    if (IS_CPU_PORT(U, port) || IS_LB_PORT(U, port)) {
        return 0;
    }
    return 1;
}

static uint32 qos_cells(uint32 bytes)
{
    return (bytes + HWBCM_QOS_CELL_BYTES - 1) / HWBCM_QOS_CELL_BYTES;
}

static bcm_color_t qos_color(uint8_t c)
{
    switch (c) {
        case HWBCM_QOS_COLOR_YELLOW: return bcmColorYellow;
        case HWBCM_QOS_COLOR_RED:    return bcmColorRed;
        default:                     return bcmColorGreen;
    }
}

/* SAI SHARED_DYNAMIC_TH n means alpha = 2^n. TD2+ has 1/128 .. 8. */
static int qos_alpha_clamp(int exp)
{
    if (exp < -7) {
        return -7;
    }
    if (exp > 3) {
        return 3;
    }
    return exp;
}

/* bcm enum (used by bcmCosqControlDropLimitAlpha, ingress path). The bcm enum
 * is not ordered (1_128 = 9, include/bcm/cosq.h:4268-4288). */
static int qos_alpha_bcm(int exp)
{
    static const int tbl[11] = {
        bcmCosqControlDropLimitAlpha_1_128, bcmCosqControlDropLimitAlpha_1_64,
        bcmCosqControlDropLimitAlpha_1_32,  bcmCosqControlDropLimitAlpha_1_16,
        bcmCosqControlDropLimitAlpha_1_8,   bcmCosqControlDropLimitAlpha_1_4,
        bcmCosqControlDropLimitAlpha_1_2,   bcmCosqControlDropLimitAlpha_1,
        bcmCosqControlDropLimitAlpha_2,     bcmCosqControlDropLimitAlpha_4,
        bcmCosqControlDropLimitAlpha_8
    };
    return tbl[qos_alpha_clamp(exp) + 7];
}

/* soc encoding written to the MMU alpha fields (ordered, include/soc/trident2.h:163). */
static uint32 qos_alpha_soc(int exp)
{
    return (uint32)SOC_TD2_COSQ_DROP_LIMIT_ALPHA_1_128 + (uint32)(qos_alpha_clamp(exp) + 7);
}

static int qos_alpha_exp_of_soc(uint32 v)
{
    if (v > (uint32)SOC_TD2_COSQ_DROP_LIMIT_ALPHA_8) {
        return 0;
    }
    return (int)v - 7;
}

static uint32 qos_field_max(soc_mem_t mem, soc_field_t f)
{
    int len = soc_mem_field_length(U, mem, f);
    if (len <= 0 || len >= 32) {
        return 0xffffffffu;
    }
    return (1u << len) - 1;
}

int hwbcm_qos_init(void)
{
    if (s_qos_ready) {
        return 0;
    }
    if (!SOC_IS_TD2P_TT2P(U) && !SOC_IS_TD2_TT2(U)) {
        QLOGW("qos: unit %d is not a Trident2/2+, QoS programming may not apply", U);
    }
    QLOGI("qos: non-ETS mode, %d cos per port reachable via bcm_cosq (bcm_num_cos clamp), "
          "%d PGs, %d service pools", NUM_COS(U), HWBCM_QOS_NUM_PG, HWBCM_QOS_NUM_POOL);
    s_qos_ready = 1;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* classification maps                                                      */
/* ------------------------------------------------------------------------ */

int hwbcm_qos_dscp_map_create(const uint8_t tc[64], const uint8_t color[64], int *map_id)
{
    const uint32 flags = BCM_QOS_MAP_INGRESS | BCM_QOS_MAP_L3;
    bcm_qos_map_t m;
    int id = -1, d, rv;

    if (tc == NULL || color == NULL || map_id == NULL) {
        return BCM_E_PARAM;
    }
    hwbcm_qos_init();

    QCHECK("qos_map_create(ingress dscp)", bcm_qos_map_create(U, flags, &id));

    for (d = 0; d < 64; d++) {
        bcm_qos_map_t_init(&m);
        m.dscp = d;
        m.int_pri = tc[d] & 0xf;
        m.color = qos_color(color[d]);
        rv = bcm_qos_map_add(U, flags, &m, id);
        if (BCM_FAILURE(rv)) {
            QLOGE("qos: qos_map_add(dscp %d -> tc %d): %s", d, m.int_pri, bcm_errmsg(rv));
            (void)bcm_qos_map_destroy(U, id);
            return rv;
        }
    }

    *map_id = id;
    QLOGD("qos: dscp map 0x%x created", id);
    return 0;
}

int hwbcm_qos_dot1p_map_create(const uint8_t tc[8], const uint8_t color[8], int *map_id)
{
    const uint32 flags = BCM_QOS_MAP_INGRESS | BCM_QOS_MAP_L2;
    bcm_qos_map_t m;
    int id = -1, p, cfi, rv;

    if (tc == NULL || color == NULL || map_id == NULL) {
        return BCM_E_PARAM;
    }
    hwbcm_qos_init();

    QCHECK("qos_map_create(ingress dot1p)", bcm_qos_map_create(U, flags, &id));

    for (p = 0; p < 8; p++) {
        for (cfi = 0; cfi < 2; cfi++) {
            bcm_qos_map_t_init(&m);
            m.pkt_pri = (uint8)p;
            m.pkt_cfi = (uint8)cfi;
            m.int_pri = tc[p] & 0xf;
            m.color = qos_color(color[p]);
            rv = bcm_qos_map_add(U, flags, &m, id);
            if (BCM_FAILURE(rv)) {
                QLOGE("qos: qos_map_add(pcp %d cfi %d): %s", p, cfi, bcm_errmsg(rv));
                (void)bcm_qos_map_destroy(U, id);
                return rv;
            }
        }
    }

    *map_id = id;
    QLOGD("qos: dot1p map 0x%x created", id);
    return 0;
}

int hwbcm_qos_egr_dscp_map_create(const uint8_t dscp[HWBCM_QOS_NUM_TC * HWBCM_QOS_NUM_COLOR], int *map_id)
{
    const uint32 flags = BCM_QOS_MAP_EGRESS | BCM_QOS_MAP_L3;
    bcm_qos_map_t m;
    int id = -1, tc, c, rv;

    if (dscp == NULL || map_id == NULL) {
        return BCM_E_PARAM;
    }
    hwbcm_qos_init();

    QCHECK("qos_map_create(egress dscp)", bcm_qos_map_create(U, flags, &id));

    for (tc = 0; tc < HWBCM_QOS_NUM_TC; tc++) {
        for (c = 0; c < HWBCM_QOS_NUM_COLOR; c++) {
            bcm_qos_map_t_init(&m);
            m.int_pri = tc;
            m.color = qos_color((uint8_t)c);
            m.dscp = dscp[tc * HWBCM_QOS_NUM_COLOR + c] & 0x3f;
            rv = bcm_qos_map_add(U, flags, &m, id);
            if (BCM_FAILURE(rv)) {
                QLOGE("qos: qos_map_add(egress tc %d color %d -> dscp %d): %s",
                      tc, c, m.dscp, bcm_errmsg(rv));
                (void)bcm_qos_map_destroy(U, id);
                return rv;
            }
        }
    }

    *map_id = id;
    QLOGD("qos: egress dscp map 0x%x created", id);
    return 0;
}

int hwbcm_qos_map_destroy(int map_id)
{
    QCHECK("qos_map_destroy", bcm_qos_map_destroy(U, map_id));
    return 0;
}

int hwbcm_qos_port_ingress_maps_set(int port, int dscp_map, int dot1p_map)
{
    if (!qos_port_ok(port)) {
        return BCM_E_PORT;
    }

    /* 0 clears TRUST_DOT1P_PTR and TRUST_DSCP_* together; -1 = no change
     * (src/bcm/esw/triumph2/qos.c:5623-5666). */
    QCHECK("qos_port_map_set(clear ingress)", bcm_qos_port_map_set(U, port, 0, -1));

    if (dscp_map > 0) {
        QCHECK("qos_port_map_set(dscp)", bcm_qos_port_map_set(U, port, dscp_map, -1));
    }
    if (dot1p_map > 0) {
        QCHECK("qos_port_map_set(dot1p)", bcm_qos_port_map_set(U, port, dot1p_map, -1));
    }
    QLOGD("qos: port %d ingress maps dscp=0x%x dot1p=0x%x", port, dscp_map, dot1p_map);
    return 0;
}

int hwbcm_qos_port_egress_map_set(int port, int egr_dscp_map)
{
    if (!qos_port_ok(port)) {
        return BCM_E_PORT;
    }
    /* TD2+ (egr_qos_combo_profile): EGR_PORT.EGR_QOS_PROFILE_INDEX
     * (src/bcm/esw/triumph2/qos.c:5939-5950). */
    QCHECK("qos_port_map_set(egress dscp)",
           bcm_qos_port_map_set(U, port, -1, egr_dscp_map > 0 ? egr_dscp_map : 0));
    return 0;
}

int hwbcm_qos_port_default_tc_set(int port, int tc)
{
    if (!qos_port_ok(port)) {
        return BCM_E_PORT;
    }
    if (tc < 0 || tc >= HWBCM_QOS_NUM_TC) {
        return BCM_E_PARAM;
    }
    QCHECK("port_untagged_priority_set", bcm_port_untagged_priority_set(U, port, tc));
    return 0;
}

int hwbcm_qos_port_tc_to_cos_set(int port, const uint8_t cos[HWBCM_QOS_NUM_TC])
{
    int p, q, maxq;

    if (!qos_port_ok(port) ) {
        return BCM_E_PORT;
    }
    if (cos == NULL) {
        return BCM_E_PARAM;
    }

    maxq = NUM_COS(U);
    for (p = 0; p < HWBCM_QOS_NUM_TC; p++) {
        q = cos[p];
        if (q >= maxq) {
            QLOGW("qos: port %d tc %d -> queue %d not reachable in non-ETS mode, using %d",
                  port, p, q, maxq - 1);
            q = maxq - 1;
        }
        /* PORT_COS_MAP UC_COS1/MC_COS1, keyed by ingress port
         * (src/bcm/esw/trident2/cosq.c:8776-8800). */
        QCHECK("cosq_port_mapping_set", bcm_cosq_port_mapping_set(U, port, p, q));
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* THDI per-PG helpers                                                      */
/* ------------------------------------------------------------------------ */

typedef struct {
    int valid;
    uint32 min, hdrm, dyn, limit, floor, offset;   /* cells / raw alpha */
    int spid;
} qos_pg_snap_t;

static qos_pg_snap_t s_pg_snap[HWBCM_MAX_PORTS][HWBCM_QOS_NUM_PG];

static const soc_field_t s_pg_spid_field[HWBCM_QOS_NUM_PG] = {
    PG0_SPIDf, PG1_SPIDf, PG2_SPIDf, PG3_SPIDf,
    PG4_SPIDf, PG5_SPIDf, PG6_SPIDf, PG7_SPIDf
};

static int qos_pg_entry(int port, int pg, soc_mem_t *mem, int *idx, uint32 *entry)
{
    *mem = SOC_TD2_PMEM(U, port, THDI_PORT_PG_CONFIG_Xm, THDI_PORT_PG_CONFIG_Ym);
    *idx = (int)SOC_TD2_MMU_PIPED_MEM_INDEX(U, port, *mem, pg);
    if (*idx < 0) {
        return BCM_E_PARAM;
    }
    return soc_mem_read(U, *mem, MEM_BLOCK_ANY, *idx, entry);
}

static int qos_pg_spid_get(int port, int pg, int *spid)
{
    uint32 rval = 0;
    int rv = soc_reg32_get(U, THDI_PORT_PG_SPIDr, port, 0, &rval);
    if (rv < 0) {
        return rv;
    }
    *spid = (int)soc_reg_field_get(U, THDI_PORT_PG_SPIDr, rval, s_pg_spid_field[pg]);
    return 0;
}

static int qos_pg_snapshot(int port, int pg)
{
    uint32 entry[SOC_MAX_MEM_WORDS];
    qos_pg_snap_t *s = &s_pg_snap[port][pg];
    soc_mem_t mem;
    int idx, rv;

    if (s->valid) {
        return 0;
    }
    rv = qos_pg_entry(port, pg, &mem, &idx, entry);
    if (rv < 0) {
        return rv;
    }
    s->min = soc_mem_field32_get(U, mem, entry, PG_MIN_LIMITf);
    s->hdrm = soc_mem_field32_get(U, mem, entry, PG_HDRM_LIMITf);
    s->dyn = soc_mem_field32_get(U, mem, entry, PG_SHARED_DYNAMICf);
    s->limit = soc_mem_field32_get(U, mem, entry, PG_SHARED_LIMITf);
    s->floor = soc_mem_field32_get(U, mem, entry, PG_RESET_FLOORf);
    s->offset = soc_mem_field32_get(U, mem, entry, PG_RESET_OFFSETf);
    rv = qos_pg_spid_get(port, pg, &s->spid);
    if (rv < 0) {
        return rv;
    }
    s->valid = 1;
    QLOGD("qos: port %d pg %d boot: min %u hdrm %u dyn %u limit %u floor %u offset %u sp %d",
          port, pg, s->min, s->hdrm, s->dyn, s->limit, s->floor, s->offset, s->spid);
    return 0;
}

/* First internal priority whose THDI_PORT_PRI_GRP entry is pg, -1 if none. */
static int qos_pg_prio(int port, int pg)
{
    int p, cur;
    for (p = 0; p < HWBCM_QOS_NUM_TC; p++) {
        if (bcm_port_priority_group_mapping_get(U, port, p, &cur) == BCM_E_NONE && cur == pg) {
            return p;
        }
    }
    return -1;
}

/* Write the admission fields of a PG directly (same fields the init-mmu code
 * writes, src/soc/esw/trident2.c:15130-15160). */
static int qos_pg_admission_write(int port, int pg, uint32 dyn, uint32 limit, uint32 floor, uint32 offset)
{
    uint32 entry[SOC_MAX_MEM_WORDS];
    soc_mem_t mem;
    int idx, rv;
    uint32 lmax, fmax, omax;

    rv = qos_pg_entry(port, pg, &mem, &idx, entry);
    if (rv < 0) {
        return rv;
    }
    lmax = qos_field_max(mem, PG_SHARED_LIMITf);
    fmax = qos_field_max(mem, PG_RESET_FLOORf);
    omax = qos_field_max(mem, PG_RESET_OFFSETf);
    soc_mem_field32_set(U, mem, entry, PG_SHARED_DYNAMICf, dyn ? 1 : 0);
    soc_mem_field32_set(U, mem, entry, PG_SHARED_LIMITf, limit > lmax ? lmax : limit);
    soc_mem_field32_set(U, mem, entry, PG_RESET_FLOORf, floor > fmax ? fmax : floor);
    soc_mem_field32_set(U, mem, entry, PG_RESET_OFFSETf, offset > omax ? omax : offset);
    return soc_mem_write(U, mem, MEM_BLOCK_ALL, idx, entry);
}

/* A PG left by init mmu with min 0 and a static shared limit of 0 admits
 * nothing (src/soc/esw/trident2.c:14203-14215 for PG0..6, then 15136-15147). */
static int qos_pg_unusable(int port, int pg)
{
    uint32 entry[SOC_MAX_MEM_WORDS];
    soc_mem_t mem;
    int idx;

    if (qos_pg_entry(port, pg, &mem, &idx, entry) < 0) {
        return 0;
    }
    return soc_mem_field32_get(U, mem, entry, PG_MIN_LIMITf) == 0 &&
           soc_mem_field32_get(U, mem, entry, PG_SHARED_DYNAMICf) == 0 &&
           soc_mem_field32_get(U, mem, entry, PG_SHARED_LIMITf) == 0;
}

static int qos_pg_inherit_pg7(int port, int pg)
{
    uint32 entry[SOC_MAX_MEM_WORDS];
    soc_mem_t mem;
    int idx, rv;
    uint32 dyn, limit, offset;

    rv = qos_pg_snapshot(port, pg);
    if (rv < 0) {
        return rv;
    }
    rv = qos_pg_entry(port, 7, &mem, &idx, entry);
    if (rv < 0) {
        return rv;
    }
    dyn = soc_mem_field32_get(U, mem, entry, PG_SHARED_DYNAMICf);
    limit = soc_mem_field32_get(U, mem, entry, PG_SHARED_LIMITf);
    offset = soc_mem_field32_get(U, mem, entry, PG_RESET_OFFSETf);
    QLOGI("qos: port %d pg %d had no admission threshold, inheriting PG7 (dyn %u limit %u)",
          port, pg, dyn, limit);
    return qos_pg_admission_write(port, pg, dyn, limit, 0, offset);
}

int hwbcm_qos_port_tc_to_pg_set(int port, const uint8_t pg[HWBCM_QOS_NUM_TC])
{
    int p, cur, target;

    if (!qos_port_ok(port)) {
        return BCM_E_PORT;
    }
    if (pg == NULL) {
        return BCM_E_PARAM;
    }

    for (p = 0; p < HWBCM_QOS_NUM_TC; p++) {
        target = pg[p];
        if (target >= HWBCM_QOS_NUM_PG) {
            QLOGW("qos: port %d tc %d -> pg %d out of range, using 7", port, p, target);
            target = 7;
        }
        if (bcm_port_priority_group_mapping_get(U, port, p, &cur) == BCM_E_NONE && cur == target) {
            continue;
        }
        if (target != 7 && qos_pg_unusable(port, target)) {
            QCHECK("pg inherit", qos_pg_inherit_pg7(port, target));
        }
        QCHECK("port_priority_group_mapping_set",
               bcm_port_priority_group_mapping_set(U, port, p, target));
    }
    return 0;
}

int hwbcm_qos_pg_buffer_set(int port, int pg, const hwbcm_qos_buffer_t *b)
{
    qos_pg_snap_t *s;
    uint32 entry[SOC_MAX_MEM_WORDS];
    soc_mem_t mem;
    int idx, prio, rv, pool;
    uint32 min, hdrm, dyn, limit, floor, offset, cur_min, cur_hdrm;

    if (!qos_port_ok(port)) {
        return BCM_E_PORT;
    }
    if (pg < 0 || pg >= HWBCM_QOS_NUM_PG) {
        return BCM_E_PARAM;
    }
    QCHECK("pg snapshot", qos_pg_snapshot(port, pg));
    s = &s_pg_snap[port][pg];

    if (b == NULL) {
        min = s->min;
        hdrm = s->hdrm;
        dyn = s->dyn;
        limit = s->limit;
        floor = s->floor;
        offset = s->offset;
        pool = s->spid;
    } else {
        min = qos_cells(b->min_bytes);
        hdrm = qos_cells(b->xoff_bytes);
        dyn = b->dynamic ? 1 : 0;
        limit = b->dynamic ? qos_alpha_soc(b->alpha_exp) : qos_cells(b->static_bytes);
        floor = qos_cells(b->xon_bytes);
        offset = qos_cells(b->xon_offset_bytes);
        if (offset == 0) {
            /* SONiC rarely sets xon_offset; keep the PG's resume offset, or
             * PG7's (2 MTU in the init-mmu lossless config) if it has none. */
            offset = s->offset;
            if (offset == 0 && qos_pg_entry(port, 7, &mem, &idx, entry) == 0) {
                offset = soc_mem_field32_get(U, mem, entry, PG_RESET_OFFSETf);
            }
        }
        pool = b->pool;
    }

    if (pool >= 0 && pool < HWBCM_QOS_NUM_POOL) {
        /* THDI_PORT_PG_SPID, keyed by PG (src/bcm/esw/trident2/cosq.c:12000-12030). */
        QCHECK("cosq_control_set(IngressPool)",
               bcm_cosq_control_set(U, port, pg, bcmCosqControlIngressPool, pool));
    }

    QCHECK("pg admission write", qos_pg_admission_write(port, pg, dyn, limit, floor, offset));

    rv = qos_pg_entry(port, pg, &mem, &idx, entry);
    if (rv < 0) {
        return rv;
    }
    cur_min = soc_mem_field32_get(U, mem, entry, PG_MIN_LIMITf);
    cur_hdrm = soc_mem_field32_get(U, mem, entry, PG_HDRM_LIMITf);
    if (cur_min == min && cur_hdrm == hdrm) {
        return 0;
    }

    /* PG min / headroom are carved out of the service pool; the SDK adjusts
     * the pool shared limit by the delta, but only through this API, which
     * is keyed by input priority (src/bcm/esw/trident2/cosq.c:13651-13660). */
    prio = qos_pg_prio(port, pg);
    if (prio < 0) {
        QLOGD("qos: port %d pg %d has no priority mapped yet, min/headroom pending", port, pg);
        return BCM_E_NOT_FOUND;
    }
    if (cur_min != min) {
        QCHECK("cosq_control_set(PGMinLimitBytes)",
               bcm_cosq_control_set(U, port, prio, bcmCosqControlIngressPortPGMinLimitBytes,
                                    (int)(min * HWBCM_QOS_CELL_BYTES)));
    }
    if (cur_hdrm != hdrm) {
        QCHECK("cosq_control_set(PGHeadroomLimitBytes)",
               bcm_cosq_control_set(U, port, prio, bcmCosqControlIngressPortPGHeadroomLimitBytes,
                                    (int)(hdrm * HWBCM_QOS_CELL_BYTES)));
    }
    QLOGD("qos: port %d pg %d: min %u hdrm %u dyn %u limit %u floor %u offset %u pool %d",
          port, pg, min, hdrm, dyn, limit, floor, offset, pool);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* egress queue thresholds                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
    int valid;
    int have_min, have_dyn, have_shared, have_limit_en, have_pool;
    int min_bytes, dyn, shared_bytes, limit_en, pool;
    uint32 alpha;                    /* soc encoding, valid if dyn */
} qos_q_snap_t;

static qos_q_snap_t s_q_snap[HWBCM_MAX_PORTS][2][HWBCM_QOS_NUM_COS];

/* Queue alpha: no non-ETS API (the bcm path needs a queue gport,
 * src/bcm/esw/trident2/cosq.c:14666-14717), so mirror its memory writes. */
static int qos_queue_alpha_rw(int port, int cos, int mcast, uint32 *alpha, int write)
{
    soc_info_t *si = &SOC_INFO(U);
    uint32 entry[SOC_MAX_MEM_WORDS];
    soc_mem_t mems[2];
    soc_field_t f;
    int n, i, idx, rv;

    if (mcast) {
        if (cos >= si->port_num_cosq[port]) {
            return BCM_E_PARAM;
        }
        idx = soc_td2_l2_hw_index(U, si->port_cosq_base[port] + cos, 0) - QOS_TD2_MC_QUEUE_BASE;
        mems[0] = SOC_TD2_PMEM(U, port, MMU_THDM_DB_QUEUE_CONFIG_0m, MMU_THDM_DB_QUEUE_CONFIG_1m);
        mems[1] = SOC_TD2_PMEM(U, port, MMU_THDM_MCQE_QUEUE_CONFIG_0m, MMU_THDM_MCQE_QUEUE_CONFIG_1m);
        n = 2;
        f = Q_SHARED_ALPHAf;
    } else {
        if (cos >= si->port_num_uc_cosq[port]) {
            return BCM_E_PARAM;
        }
        idx = soc_td2_l2_hw_index(U, si->port_uc_cosq_base[port] + cos, 1);
        mems[0] = SOC_TD2_PMEM(U, port, MMU_THDU_XPIPE_CONFIG_QUEUEm, MMU_THDU_YPIPE_CONFIG_QUEUEm);
        n = 1;
        f = Q_SHARED_ALPHA_CELLf;
    }
    if (idx < 0) {
        return BCM_E_PARAM;
    }

    for (i = 0; i < n; i++) {
        rv = soc_mem_read(U, mems[i], MEM_BLOCK_ALL, idx, entry);
        if (rv < 0) {
            return rv;
        }
        if (!write) {
            *alpha = soc_mem_field32_get(U, mems[i], entry, f);
            return 0;
        }
        soc_mem_field32_set(U, mems[i], entry, f, *alpha);
        rv = soc_mem_write(U, mems[i], MEM_BLOCK_ALL, idx, entry);
        if (rv < 0) {
            return rv;
        }
    }
    return 0;
}

static void qos_q_types(int mcast, bcm_cosq_control_t *t_min, bcm_cosq_control_t *t_shared,
                        bcm_cosq_control_t *t_dyn, bcm_cosq_control_t *t_limit_en,
                        bcm_cosq_control_t *t_pool)
{
    if (mcast) {
        *t_min = bcmCosqControlEgressMCQueueMinLimitBytes;
        *t_shared = bcmCosqControlEgressMCQueueSharedLimitBytes;
        *t_dyn = bcmCosqControlEgressMCSharedDynamicEnable;
        *t_limit_en = bcmCosqControlEgressMCQueueLimitEnable;
        *t_pool = bcmCosqControlMCEgressPool;
    } else {
        *t_min = bcmCosqControlEgressUCQueueMinLimitBytes;
        *t_shared = bcmCosqControlEgressUCQueueSharedLimitBytes;
        *t_dyn = bcmCosqControlEgressUCSharedDynamicEnable;
        *t_limit_en = bcmCosqControlEgressUCQueueLimitEnable;
        *t_pool = bcmCosqControlUCEgressPool;
    }
}

static void qos_q_snapshot(int port, int cos, int mcast)
{
    qos_q_snap_t *s = &s_q_snap[port][mcast ? 1 : 0][cos];
    bcm_cosq_control_t t_min, t_shared, t_dyn, t_limit_en, t_pool;

    if (s->valid) {
        return;
    }
    qos_q_types(mcast, &t_min, &t_shared, &t_dyn, &t_limit_en, &t_pool);
    s->have_min = bcm_cosq_control_get(U, port, cos, t_min, &s->min_bytes) == BCM_E_NONE;
    s->have_dyn = bcm_cosq_control_get(U, port, cos, t_dyn, &s->dyn) == BCM_E_NONE;
    if (s->have_dyn && s->dyn) {
        s->have_shared = 0;
        if (qos_queue_alpha_rw(port, cos, mcast, &s->alpha, 0) < 0) {
            s->have_dyn = 0;
        }
    } else {
        s->have_shared = bcm_cosq_control_get(U, port, cos, t_shared, &s->shared_bytes) == BCM_E_NONE;
    }
    s->have_limit_en = bcm_cosq_control_get(U, port, cos, t_limit_en, &s->limit_en) == BCM_E_NONE;
    s->have_pool = bcm_cosq_control_get(U, port, cos, t_pool, &s->pool) == BCM_E_NONE;
    s->valid = 1;
    QLOGD("qos: port %d %s cos %d boot: min %d dyn %d shared %d limit_en %d pool %d",
          port, mcast ? "mc" : "uc", cos, s->min_bytes, s->dyn, s->shared_bytes,
          s->limit_en, s->pool);
}

static int qos_q_restore(int port, int cos, int mcast)
{
    qos_q_snap_t *s = &s_q_snap[port][mcast ? 1 : 0][cos];
    bcm_cosq_control_t t_min, t_shared, t_dyn, t_limit_en, t_pool;

    qos_q_types(mcast, &t_min, &t_shared, &t_dyn, &t_limit_en, &t_pool);
    if (s->have_pool) {
        QCHECK("restore pool", bcm_cosq_control_set(U, port, cos, t_pool, s->pool));
    }
    if (s->have_min) {
        QCHECK("restore min", bcm_cosq_control_set(U, port, cos, t_min, s->min_bytes));
    }
    if (s->have_dyn) {
        QCHECK("restore dyn", bcm_cosq_control_set(U, port, cos, t_dyn, s->dyn));
        if (s->dyn) {
            QCHECK("restore alpha", qos_queue_alpha_rw(port, cos, mcast, &s->alpha, 1));
        }
    }
    if (s->have_shared && !(s->have_dyn && s->dyn)) {
        QCHECK("restore shared", bcm_cosq_control_set(U, port, cos, t_shared, s->shared_bytes));
    }
    if (s->have_limit_en) {
        QCHECK("restore limit enable", bcm_cosq_control_set(U, port, cos, t_limit_en, s->limit_en));
    }
    return 0;
}

int hwbcm_qos_queue_buffer_set(int port, int cos, int mcast, const hwbcm_qos_buffer_t *b)
{
    bcm_cosq_control_t t_min, t_shared, t_dyn, t_limit_en, t_pool;
    uint32 alpha;
    int rv, limit_en = 1;

    if (!qos_port_ok(port)) {
        return BCM_E_PORT;
    }
    if (cos < 0 || cos >= NUM_COS(U) || cos >= HWBCM_QOS_NUM_COS) {
        return BCM_E_PARAM;
    }
    mcast = mcast ? 1 : 0;
    qos_q_snapshot(port, cos, mcast);

    if (b == NULL) {
        return qos_q_restore(port, cos, mcast);
    }

    qos_q_types(mcast, &t_min, &t_shared, &t_dyn, &t_limit_en, &t_pool);

    if (b->pool >= 0 && b->pool < HWBCM_QOS_NUM_POOL) {
        /* Q_SPID of the queue (src/bcm/esw/trident2/cosq.c:12160-12230). */
        QCHECK("cosq_control_set(EgressPool)", bcm_cosq_control_set(U, port, cos, t_pool, b->pool));
    }

    /* Min goes through the API: it re-balances the egress pool. */
    QCHECK("cosq_control_set(QueueMinLimitBytes)",
           bcm_cosq_control_set(U, port, cos, t_min,
                                (int)(qos_cells(b->min_bytes) * HWBCM_QOS_CELL_BYTES)));

    if (b->dynamic) {
        /* Enabling dynamic writes a default alpha; overwrite it with ours
         * (src/bcm/esw/trident2/cosq.c:14960-14975). */
        QCHECK("cosq_control_set(SharedDynamicEnable,1)", bcm_cosq_control_set(U, port, cos, t_dyn, 1));
        alpha = qos_alpha_soc(b->alpha_exp);
        QCHECK("queue alpha write", qos_queue_alpha_rw(port, cos, mcast, &alpha, 1));
    } else {
        QCHECK("cosq_control_set(SharedDynamicEnable,0)", bcm_cosq_control_set(U, port, cos, t_dyn, 0));
        rv = bcm_cosq_control_set(U, port, cos, t_shared,
                                  (int)(qos_cells(b->static_bytes) * HWBCM_QOS_CELL_BYTES));
        if (rv == BCM_E_PARAM) {
            /* Larger than the field: SONiC's egress_lossless_profile uses the
             * pool size as static_th. Treat it as "no queue limit". */
            QLOGI("qos: port %d %s cos %d static limit %u bytes exceeds HW field, queue limit disabled",
                  port, mcast ? "mc" : "uc", cos, b->static_bytes);
            limit_en = 0;
        } else if (BCM_FAILURE(rv)) {
            QLOGE("qos: cosq_control_set(QueueSharedLimitBytes): %s", bcm_errmsg(rv));
            return rv;
        }
    }

    QCHECK("cosq_control_set(QueueLimitEnable)", bcm_cosq_control_set(U, port, cos, t_limit_en, limit_en));

    QLOGD("qos: port %d %s cos %d: pool %d min %u %s %d limit_en %d", port, mcast ? "mc" : "uc",
          cos, b->pool, b->min_bytes, b->dynamic ? "alpha_exp" : "static",
          b->dynamic ? qos_alpha_clamp(b->alpha_exp) : (int)b->static_bytes, limit_en);
    return 0;
}

int hwbcm_qos_pool_shared_get(int pool, uint32_t *ingress_bytes, uint32_t *egress_bytes)
{
    uint32 rval = 0;

    if (pool < 0 || pool >= HWBCM_QOS_NUM_POOL) {
        return BCM_E_PARAM;
    }
    if (ingress_bytes != NULL) {
        QCHECK("read THDI_BUFFER_CELL_LIMIT_SP",
               soc_reg32_get(U, THDI_BUFFER_CELL_LIMIT_SPr, REG_PORT_ANY, pool, &rval));
        *ingress_bytes = soc_reg_field_get(U, THDI_BUFFER_CELL_LIMIT_SPr, rval, LIMITf) *
                         HWBCM_QOS_CELL_BYTES;
    }
    if (egress_bytes != NULL) {
        QCHECK("read MMU_THDM_DB_POOL_SHARED_LIMIT",
               soc_reg32_get(U, MMU_THDM_DB_POOL_SHARED_LIMITr, REG_PORT_ANY, pool, &rval));
        *egress_bytes = soc_reg_field_get(U, MMU_THDM_DB_POOL_SHARED_LIMITr, rval, SHARED_LIMITf) *
                        HWBCM_QOS_CELL_BYTES;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* PFC                                                                      */
/* ------------------------------------------------------------------------ */

int hwbcm_qos_port_pfc_set(int port, uint8_t tx_prios, uint8_t rx_prios, const uint8_t *prio_to_cos)
{
    bcm_port_priority_group_config_t pgc;
    bcm_cosq_pfc_class_mapping_t map[HWBCM_QOS_NUM_PFC];
    int c, i, cos;

    if (!qos_port_ok(port)) {
        return BCM_E_PORT;
    }

    /* TX: THDI_INPUT_PORT_XON_ENABLES.PORT_PRI_XON_ENABLE is a per-priority
     * bitmap (init writes it per priority, src/soc/esw/trident2.c:15104-15118).
     * bcm_port_priority_group_config_set indexes it by its PG argument and
     * also turns the MAC PFC transmitter on/off with the bitmap
     * (src/bcm/esw/trident2/cosq.c:18866-18880, port.c:38645-38750). Passing the
     * SAI priority as "PG" therefore writes exactly the SAI bitmap. */
    for (i = 0; i < HWBCM_QOS_NUM_PFC; i++) {
        bcm_port_priority_group_config_t_init(&pgc);
        pgc.pfc_transmit_enable = (tx_prios >> i) & 1;
        QCHECK("port_priority_group_config_set(pfc tx)",
               bcm_port_priority_group_config_set(U, port, i, &pgc));
    }

    /* RX: class -> L1 node (cos) to pause; a class with no gport pauses
     * nothing. Non-gport entries are cos numbers in non-ETS mode
     * (src/bcm/esw/trident2/cosq.c:17950-17966). */
    for (c = 0; c < HWBCM_QOS_NUM_PFC; c++) {
        map[c].class_id = c;
        for (i = 0; i < BCM_COSQ_PFC_GPORT_COUNT; i++) {
            map[c].gport_list[i] = BCM_GPORT_INVALID;
        }
        if ((rx_prios >> c) & 1) {
            cos = prio_to_cos ? prio_to_cos[c] : c;
            if (cos >= NUM_COS(U)) {
                QLOGW("qos: port %d pfc class %d -> queue %d not reachable, using %d",
                      port, c, cos, NUM_COS(U) - 1);
                cos = NUM_COS(U) - 1;
            }
            map[c].gport_list[0] = cos;
        }
    }
    QCHECK("cosq_pfc_class_mapping_set", bcm_cosq_pfc_class_mapping_set(U, port, HWBCM_QOS_NUM_PFC, map));
    QCHECK("port_control_set(PFCReceive)",
           bcm_port_control_set(U, port, bcmPortControlPFCReceive, rx_prios ? 1 : 0));

    QLOGI("qos: port %d pfc tx 0x%02x rx 0x%02x", port, tx_prios, rx_prios);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* scheduling / shaping                                                     */
/* ------------------------------------------------------------------------ */

static uint32 qos_kbits(uint64_t bytes_or_pkts, int pps)
{
    uint64_t v;

    if (pps) {
        v = bytes_or_pkts;
    } else {
        v = (bytes_or_pkts * 8 + 999) / 1000;
    }
    return v > 0xffffffffull ? 0xffffffffu : (uint32)v;
}

int hwbcm_qos_cos_sched_set(int port, int cos, const hwbcm_qos_sched_t *s)
{
    int mode, weight;
    uint32 flags = 0, kmin = 0, kmax = 0;

    if (!qos_port_ok(port)) {
        return BCM_E_PORT;
    }
    if (cos < 0 || cos >= NUM_COS(U)) {
        return BCM_E_PARAM;
    }

    if (s == NULL) {
        /* _td2_port_lls_config: every L1 is WRR weight 1 (soc/esw/trident2/cosq.c:1250ff). */
        mode = BCM_COSQ_WEIGHTED_ROUND_ROBIN;
        weight = 1;
    } else {
        switch (s->mode) {
            case HWBCM_QOS_SCHED_STRICT:
                mode = BCM_COSQ_STRICT;
                weight = 0;
                break;
            case HWBCM_QOS_SCHED_DWRR:
                mode = BCM_COSQ_DEFICIT_ROUND_ROBIN;
                weight = s->weight;
                break;
            default:
                mode = BCM_COSQ_WEIGHTED_ROUND_ROBIN;
                weight = s->weight;
                break;
        }
        if (mode != BCM_COSQ_STRICT) {
            if (weight < 1) {
                weight = 1;
            } else if (weight > 127) {
                weight = 127;
            }
        }
        if (s->pps) {
            flags |= BCM_COSQ_BW_PACKET_MODE;
        }
        kmin = qos_kbits(s->min_rate, s->pps);
        kmax = qos_kbits(s->max_rate, s->pps);
        if (kmax != 0 && kmax < kmin) {
            kmax = kmin;
        }
    }

    /* (port, cos) -> L1.cos under L0.0 (src/bcm/esw/trident2/cosq.c:6800-6860). */
    QCHECK("cosq_gport_sched_set", bcm_cosq_gport_sched_set(U, port, cos, mode, weight));
    /* L1 MTRO bucket (src/bcm/esw/trident2/cosq.c:5513ff, BUCKET style). 0 = off. */
    QCHECK("cosq_gport_bandwidth_set", bcm_cosq_gport_bandwidth_set(U, port, cos, kmin, kmax, flags));

    if (s != NULL && s->max_burst != 0 && kmax != 0) {
        QCHECK("cosq_control_set(BandwidthBurstMax)",
               bcm_cosq_control_set(U, port, cos, bcmCosqControlBandwidthBurstMax,
                                    (int)qos_kbits(s->max_burst, s->pps)));
    }
    if (s != NULL && s->min_burst != 0 && kmin != 0) {
        QCHECK("cosq_control_set(BandwidthBurstMin)",
               bcm_cosq_control_set(U, port, cos, bcmCosqControlBandwidthBurstMin,
                                    (int)qos_kbits(s->min_burst, s->pps)));
    }

    QLOGD("qos: port %d cos %d sched mode %d weight %d min %u max %u%s", port, cos, mode,
          weight, kmin, kmax, (flags & BCM_COSQ_BW_PACKET_MODE) ? " pps" : " kbps");
    return 0;
}

int hwbcm_qos_port_shaper_set(int port, const hwbcm_qos_sched_t *s)
{
    uint32 rate = 0, burst = 0;

    if (!qos_port_ok(port)) {
        return BCM_E_PORT;
    }
    if (s != NULL && s->max_rate != 0) {
        rate = qos_kbits(s->max_rate, s->pps);
        burst = s->max_burst ? qos_kbits(s->max_burst, s->pps) : 0;
        if (burst == 0) {
            /* ~ 10 ms worth, at least 64 kbits / 64 packets */
            burst = rate / 100;
            if (burst < 64) {
                burst = 64;
            }
        }
    }

    if (s != NULL && s->pps) {
        QCHECK("port_rate_egress_pps_set", bcm_port_rate_egress_pps_set(U, port, rate, burst));
    } else {
        QCHECK("port_rate_egress_set", bcm_port_rate_egress_set(U, port, rate, burst));
    }
    QLOGD("qos: port %d shaper %u %s burst %u", port, rate, (s && s->pps) ? "pps" : "kbps", burst);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* WRED / ECN                                                               */
/* ------------------------------------------------------------------------ */

int hwbcm_qos_cos_wred_set(int port, int cos, const hwbcm_qos_wred_t *w)
{
    static const uint32 color_flag[HWBCM_QOS_NUM_COLOR] = {
        BCM_COSQ_DISCARD_COLOR_GREEN, BCM_COSQ_DISCARD_COLOR_YELLOW, BCM_COSQ_DISCARD_COLOR_RED
    };
    const int off = (int)(QOS_WRED_CELLS_OFF * HWBCM_QOS_CELL_BYTES);
    const uint32 max_bytes = (uint32)off;
    bcm_cosq_gport_discard_t d;
    uint32 base;
    int c, any = 0;

    if (!qos_port_ok(port)) {
        return BCM_E_PORT;
    }
    if (cos < 0 || cos >= NUM_COS(U)) {
        return BCM_E_PARAM;
    }

    if (w != NULL) {
        for (c = 0; c < HWBCM_QOS_NUM_COLOR; c++) {
            any |= w->enable[c];
        }
        any |= w->ecn;
    }

    /* One call per color. WRED_EN / ECN_MARKING_EN / WEIGHT are per queue and
     * rewritten by every call (src/bcm/esw/trident2/cosq.c:6132-6141). */
    base = BCM_COSQ_DISCARD_BYTES | BCM_COSQ_DISCARD_TCP | BCM_COSQ_DISCARD_NONTCP;
    if (any) {
        base |= BCM_COSQ_DISCARD_ENABLE;
        if (w->ecn) {
            base |= BCM_COSQ_DISCARD_MARK_CONGESTION;
        }
    }

    for (c = 0; c < HWBCM_QOS_NUM_COLOR; c++) {
        bcm_cosq_gport_discard_t_init(&d);
        d.flags = base | color_flag[c];
        d.refresh_time = QOS_WRED_REFRESH_TIME;
        d.gain = (w != NULL) ? (w->gain < 0 ? 0 : (w->gain > 15 ? 15 : w->gain)) : 0;
        if (w != NULL && w->enable[c]) {
            uint32 mn = w->min_bytes[c] > max_bytes ? max_bytes : w->min_bytes[c];
            uint32 mx = w->max_bytes[c] > max_bytes ? max_bytes : w->max_bytes[c];
            if (mn > mx) {
                mn = mx;
            }
            d.min_thresh = (int)mn;
            d.max_thresh = (int)mx;
            d.drop_probability = w->drop_pct[c] < 0 ? 0 : (w->drop_pct[c] > 100 ? 100 : w->drop_pct[c]);
        } else {
            /* curve off: both thresholds at the "unused" sentinel */
            d.min_thresh = off;
            d.max_thresh = off;
            d.drop_probability = 0;
        }
        QCHECK("cosq_gport_discard_set", bcm_cosq_gport_discard_set(U, port, cos, &d));
    }

    QLOGD("qos: port %d cos %d wred %s ecn %d", port, cos, any ? "on" : "off", w ? w->ecn : 0);
    return 0;
}
