/*
 * hwbcm_qos.h — QoS / buffer section of the hwbcm C wrapper (Trident2+,
 * BCM56864, OpenBCM sdk-6.5.27). Implemented in hwbcm_qos.c.
 *
 * Same rules as hwbcm.h: no SDK headers here (included from C++), 0 on
 * success, negative BCM_E_* on failure, unit is HWBCM_UNIT.
 *
 * Model (see notes/integration_qos.md for the why):
 *  - The SDK stays in its default "non-ETS" scheduler mode: the per-port LLS
 *    tree built by "init mmu"/bcm init (root -> L0.0 -> L1.0..L1.9, each L1
 *    with one unicast and one multicast queue, src/soc/esw/trident2/cosq.c:1228)
 *    is kept and addressed with (local port, cos) instead of cosq gports.
 *    bcm_gport_add() would switch the whole chip to ETS mode, reset every
 *    port's LLS including the CPU port (src/bcm/esw/trident2/cosq.c:4341) and
 *    we would have to rebuild the CPU/KNET tree ourselves.
 *  - In that mode the bcm_cosq API reaches cos 0..HWBCM_QOS_NUM_COS-1 only:
 *    bcm_td2_cosq_init clamps bcm_num_cos (10 in config.bcm) to 8
 *    (src/bcm/esw/trident2/cosq.c:8046-8052). Queues 8/9 keep SDK defaults.
 *  - "cos" below is the L1 scheduler node / queue index of a port. One cos
 *    owns one UC queue and one MC queue; scheduling and shaping act on the L1
 *    node (both queues), WRED on the UC queue, buffers on either queue.
 */
#ifndef HWBCM_QOS_H
#define HWBCM_QOS_H

#include "hwbcm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HWBCM_QOS_NUM_TC        16  /* internal priorities */
#define HWBCM_QOS_NUM_COS       8   /* cos reachable through the non-ETS API */
#define HWBCM_QOS_NUM_HW_QUEUES 10  /* UC (and MC) queues per front port in HW */
#define HWBCM_QOS_NUM_PG        8
#define HWBCM_QOS_NUM_PFC       8
#define HWBCM_QOS_NUM_POOL      4   /* service pools per direction */
#define HWBCM_QOS_CELL_BYTES    208 /* MMU cell (_TD2_MMU_BYTES_PER_CELL) */

#define HWBCM_QOS_COLOR_GREEN   0
#define HWBCM_QOS_COLOR_YELLOW  1
#define HWBCM_QOS_COLOR_RED     2
#define HWBCM_QOS_NUM_COLOR     3

/* One-time setup (idempotent): logs the MMU mode and snapshots nothing yet
 * (snapshots are taken lazily at first touch of a PG/queue). */
int hwbcm_qos_init(void);

/* ------------------------------------------------------------------------ */
/* classification maps                                                      */
/* ------------------------------------------------------------------------ */

/* Ingress DSCP -> (internal priority, color). Both arrays have 64 entries;
 * tc 0..15, color HWBCM_QOS_COLOR_*. Creates a bcm L3 ingress qos map
 * (DSCP_TABLE profile). *map_id is the bcm map id. */
int hwbcm_qos_dscp_map_create(const uint8_t tc[64], const uint8_t color[64], int *map_id);

/* Ingress 802.1p (PCP, both CFI values) -> (internal priority, color). Eight
 * entries each. bcm L2 ingress map (ING_PRI_CNG_MAP profile). */
int hwbcm_qos_dot1p_map_create(const uint8_t tc[8], const uint8_t color[8], int *map_id);

/* Egress (internal priority, color) -> DSCP remark, 48 entries indexed
 * tc * 3 + color (tc 0..15). bcm L3 egress map (EGR_DSCP_TABLE). */
int hwbcm_qos_egr_dscp_map_create(const uint8_t dscp[HWBCM_QOS_NUM_TC * HWBCM_QOS_NUM_COLOR], int *map_id);

int hwbcm_qos_map_destroy(int map_id);

/* Bind the ingress maps of a port. A negative id means "no map of that kind"
 * (the port then trusts nothing of that kind). bcm_qos_port_map_set clears
 * both trust pointers when given 0, so this always clears and rebinds
 * (src/bcm/esw/triumph2/qos.c:5624-5666). */
int hwbcm_qos_port_ingress_maps_set(int port, int dscp_map, int dot1p_map);

/* Bind the egress DSCP remark map of a port (<0: none). */
int hwbcm_qos_port_egress_map_set(int port, int egr_dscp_map);

/* Internal priority for packets no map classifies (PORT_TAB.PORT_PRI). */
int hwbcm_qos_port_default_tc_set(int port, int tc);

/* Internal priority -> cos for packets entering this port (TD2+ resolves the
 * queue at ingress: COS_MAP_SEL is per ingress port). cos[i] >= 8 is clamped
 * to 7 with a warning. Unicast and multicast queues follow the same map. */
int hwbcm_qos_port_tc_to_cos_set(int port, const uint8_t cos[HWBCM_QOS_NUM_TC]);

/* Internal priority -> ingress priority group (THDI_PORT_PRI_GRP0/1). Before
 * a priority moves to a PG whose admission thresholds are still the init-mmu
 * "unusable" ones (min 0, static shared 0), the PG inherits PG7's shared
 * threshold so traffic is not dropped until its buffer profile arrives. */
int hwbcm_qos_port_tc_to_pg_set(int port, const uint8_t pg[HWBCM_QOS_NUM_TC]);

/* Priority flow control. tx_pgs: PGs (== PFC priorities on TD2+) that send
 * PFC when they cross their xoff threshold. rx_prios: received PFC classes
 * that are honoured; class p pauses cos prio_to_cos[p] (L1 node, both
 * queues). prio_to_cos may be NULL (identity). 0/0 disables PFC on the MAC. */
int hwbcm_qos_port_pfc_set(int port, uint8_t tx_pgs, uint8_t rx_prios, const uint8_t *prio_to_cos);

/* ------------------------------------------------------------------------ */
/* scheduling / shaping                                                     */
/* ------------------------------------------------------------------------ */

#define HWBCM_QOS_SCHED_STRICT 0
#define HWBCM_QOS_SCHED_WRR    1
#define HWBCM_QOS_SCHED_DWRR   2

typedef struct {
    int mode;            /* HWBCM_QOS_SCHED_* */
    int weight;          /* 1..127 for WRR/DWRR */
    int pps;             /* 1: rates in packets/s, bursts in packets */
    uint64_t min_rate;   /* bytes/s (or pps); 0 = no guarantee */
    uint64_t max_rate;   /* bytes/s (or pps); 0 = no shaping */
    uint64_t min_burst;  /* bytes (or packets); 0 = SDK default */
    uint64_t max_burst;  /* bytes (or packets); 0 = SDK default */
} hwbcm_qos_sched_t;

/* Scheduling discipline/weight of L1 node `cos` under L0.0 plus its min/max
 * shapers. NULL restores the SDK default (WRR weight 1, no shaping). */
int hwbcm_qos_cos_sched_set(int port, int cos, const hwbcm_qos_sched_t *s);

/* Port shaper (EGRMETERINGCONFIG via bcm_port_rate_egress_set). Only
 * max_rate/max_burst/pps are used. NULL or max_rate 0 disables it. */
int hwbcm_qos_port_shaper_set(int port, const hwbcm_qos_sched_t *s);

/* ------------------------------------------------------------------------ */
/* WRED / ECN (unicast queue of `cos`)                                      */
/* ------------------------------------------------------------------------ */

typedef struct {
    int enable[HWBCM_QOS_NUM_COLOR];        /* drop curve on for this color */
    uint32_t min_bytes[HWBCM_QOS_NUM_COLOR];
    uint32_t max_bytes[HWBCM_QOS_NUM_COLOR];
    int drop_pct[HWBCM_QOS_NUM_COLOR];      /* 0..100 at max threshold */
    int ecn;                                /* 1: mark ECT packets instead of dropping */
    int gain;                               /* 0..15 (SAI_WRED_ATTR_WEIGHT) */
} hwbcm_qos_wred_t;

/* NULL disables WRED and ECN marking on the queue. TCP and non-TCP curves
 * get the same thresholds. */
int hwbcm_qos_cos_wred_set(int port, int cos, const hwbcm_qos_wred_t *w);

/* ------------------------------------------------------------------------ */
/* buffers (THDI per PG, THDU/THDM per queue)                               */
/* ------------------------------------------------------------------------ */

typedef struct {
    int pool;                 /* service pool 0..3, -1 = leave as is */
    uint32_t min_bytes;       /* guaranteed (SAI RESERVED_BUFFER_SIZE) */
    int dynamic;              /* 1: shared limit is alpha * free pool */
    int alpha_exp;            /* dynamic: alpha = 2^alpha_exp, -7..3 */
    uint32_t static_bytes;    /* static: shared limit in bytes */
    uint32_t xoff_bytes;      /* PG only: headroom (SAI XOFF_TH) */
    uint32_t xon_bytes;       /* PG only: resume floor (SAI XON_TH) */
    uint32_t xon_offset_bytes;/* PG only: resume offset (SAI XON_OFFSET_TH) */
} hwbcm_qos_buffer_t;

/* Ingress PG thresholds. NULL restores the values the PG had at first touch
 * (init-mmu config). Min/headroom go through bcm_cosq_control_set so the SDK
 * moves the difference out of / into the service pool shared limit
 * (src/bcm/esw/trident2/cosq.c:13741-13800). That API is keyed by priority,
 * not PG: when no priority maps to `pg` yet, the other fields are applied and
 * BCM_E_NOT_FOUND is returned — call again after hwbcm_qos_port_tc_to_pg_set. */
int hwbcm_qos_pg_buffer_set(int port, int pg, const hwbcm_qos_buffer_t *b);

/* Egress queue thresholds of cos (0..7), unicast (mcast 0) or multicast
 * (mcast 1). NULL restores the first-touch values. */
int hwbcm_qos_queue_buffer_set(int port, int cos, int mcast, const hwbcm_qos_buffer_t *b);

/* Diagnostics: current ingress SP shared limit (THDI_BUFFER_CELL_LIMIT_SP) and
 * egress SP shared limit (MMU_THDM_DB_POOL_SHARED_LIMIT) in bytes. */
int hwbcm_qos_pool_shared_get(int pool, uint32_t *ingress_bytes, uint32_t *egress_bytes);

/* Read-only query of total MMU packet buffer size in KB (zero HW writes). */
int hwbcm_qos_total_buffer_kb_get(uint64_t *kb_out);

/* Warm boot helpers (Phase 3): persist/restore boot-time MMU snapshots
 * (s_pg_snap, s_q_snap) with zero HW writes, and cross-check QoS map ids. */
typedef struct {
    int port;
    int pg;
    uint32_t min, hdrm, dyn, limit, floor, offset;
    int spid;
} hwbcm_qos_pg_snap_wb_t;

typedef struct {
    int port;
    int mcast;
    int cos;
    int have_min, have_dyn, have_shared, have_limit_en, have_pool;
    int min_bytes, dyn, shared_bytes, limit_en, pool;
    uint32_t alpha;
} hwbcm_qos_q_snap_wb_t;

typedef struct {
    int port;
    int cos;
    int qgrp_min_en;
} hwbcm_qos_qgrp_wb_t;

/* Pass out == NULL or max == 0 to query total valid count. */
int  hwbcm_qos_wb_get_pg_snaps(hwbcm_qos_pg_snap_wb_t *out, int max);
int  hwbcm_qos_wb_get_q_snaps(hwbcm_qos_q_snap_wb_t *out, int max);
int  hwbcm_qos_wb_get_qgrp_snaps(hwbcm_qos_qgrp_wb_t *out, int max);
void hwbcm_qos_warm_restore(const hwbcm_qos_pg_snap_wb_t *pgs, int npgs,
                            const hwbcm_qos_q_snap_wb_t *qs, int nqs);
void hwbcm_qos_warm_restore_qgrp(const hwbcm_qos_qgrp_wb_t *qgrps, int n);
void hwbcm_qos_warm_reset(void);
/* Read-only check whether a bcm qos map id exists in the SDK (1 = yes, 0 = no). */
int  hwbcm_qos_map_exists(int map_id);

#ifdef __cplusplus
}
#endif
#endif /* HWBCM_QOS_H */
