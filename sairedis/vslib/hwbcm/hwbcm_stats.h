/*
 * hwbcm_stats.h — MMU statistics for the Trident2+ hardware SAI: egress
 * queues, ingress priority groups, buffer (service) pools, and the port
 * counters that do not come from bcm_stat_get's table in hwbcm_core.c
 * (MMU drops, WRED drops, the >1518 EtherStats size buckets, IP counters).
 *
 * Rules (same as hwbcm.h): SDK-free, C types only, 0 / negative BCM_E_*,
 * unit HWBCM_UNIT. Owner: stats (hwbcm_stats.c).
 *
 * Value conventions
 *  - Packet / byte counters are cumulative 64-bit values from the SDK
 *    counter thread. "clear" (SAI READ_AND_CLEAR) never touches the
 *    hardware counter: several SAI counters share one MMU counter (a queue
 *    drop is also a port OUT_DROPPED_PKTS), so clearing is done with a
 *    software baseline per (object, id). The value returned is hw - baseline.
 *  - Buffer values (*_CELLS ids) are in MMU cells; multiply by
 *    hwbcm_stats_cell_bytes() for bytes. Watermarks come from BST (peak
 *    tracking, synced at most every HWBCM_STATS_BST_SYNC_MS); "clear" on a
 *    watermark is a BST clear-on-read of that entry. Current occupancy is a
 *    live read and ignores "clear".
 *  - A counter the chip cannot provide for that object (BCM_E_UNAVAIL /
 *    BCM_E_PARAM / BCM_E_NOT_FOUND from the SDK) reads as 0 and does not fail
 *    the call. Which ids are *advertised* is decided on the C++ side
 *    (queryStatsCapability), not here.
 */
#ifndef HWBCM_STATS_H
#define HWBCM_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TD2/TD2+ MMU cell size (sdk src/bcm/esw/trident2/cosq.c:49). */
#define HWBCM_STATS_CELL_BYTES 208

/* Minimum interval between two bcm_cosq_bst_stat_sync() of one BST resource. */
#define HWBCM_STATS_BST_SYNC_MS 500

/* Per-port queue limits used by the baseline tables. */
#define HWBCM_STATS_MAX_UC_QUEUES  10
#define HWBCM_STATS_MAX_CPU_QUEUES 48

/* One-time setup: BST peak tracking + enable (unless env HWBCM_STATS_NO_BST
 * is set). Called lazily by every getter; call it once after hwbcm_init()
 * to make the first watermark poll meaningful. Idempotent. */
int hwbcm_stats_init(void);

/* 1 if BST came up (watermarks are live), 0 otherwise. */
int hwbcm_stats_bst_enabled(void);

/* Bytes per MMU cell. */
uint32_t hwbcm_stats_cell_bytes(void);

/* bcm port number of the CPU (CMIC) port. */
int hwbcm_stats_cpu_port(void);

/* ------------------------------------------------------------------------ */
/* egress queues                                                            */
/* ------------------------------------------------------------------------ */

typedef enum {
    HWBCM_QSTAT_NONE = 0,
    HWBCM_QSTAT_PACKETS,            /* transmitted packets */
    HWBCM_QSTAT_BYTES,              /* transmitted bytes */
    HWBCM_QSTAT_DROPPED_PACKETS,    /* MMU drops of this queue */
    HWBCM_QSTAT_DROPPED_BYTES,
    HWBCM_QSTAT_CURR_CELLS,         /* min + shared, live */
    HWBCM_QSTAT_SHARED_CURR_CELLS,  /* shared, live */
    HWBCM_QSTAT_WATERMARK_CELLS,    /* BST peak (bcmBstStatIdUcast / Mcast) */
    HWBCM_QSTAT_COUNT
} hwbcm_qstat_t;

/*
 * Read n counters (ids[i] is a hwbcm_qstat_t) of one egress queue.
 *   port  - bcm logical port (front panel) or hwbcm_stats_cpu_port().
 *   queue - hardware cos queue: 0..9 for front-panel UC and MC queues
 *           (SAI MC index 10..19 -> 0..9 is done by the caller),
 *           0..47 for CPU queues.
 *   is_mc - 1 for the multicast queue of that cos (ignored for the CPU port,
 *           whose queues are all multicast/CPU queues).
 *   clear - SAI READ_AND_CLEAR semantics, see the header comment.
 */
int hwbcm_queue_stat_multi_get(int port, int queue, int is_mc, int n,
                               const int *ids, int clear, uint64_t *out);

/* ------------------------------------------------------------------------ */
/* ingress priority groups                                                  */
/* ------------------------------------------------------------------------ */

typedef enum {
    HWBCM_PGSTAT_NONE = 0,
    HWBCM_PGSTAT_CURR_CELLS,                /* min + shared + headroom, live */
    HWBCM_PGSTAT_SHARED_CURR_CELLS,         /* shared, live */
    HWBCM_PGSTAT_HEADROOM_CURR_CELLS,       /* headroom (XOFF room), live */
    HWBCM_PGSTAT_SHARED_WATERMARK_CELLS,    /* BST bcmBstStatIdPriGroupShared */
    HWBCM_PGSTAT_HEADROOM_WATERMARK_CELLS,  /* BST bcmBstStatIdPriGroupHeadroom */
    HWBCM_PGSTAT_COUNT
} hwbcm_pgstat_t;

/*
 * Read n counters of PG pg (0..7) of a front-panel port. The SDK addresses
 * PG resources by *priority* (trident2/cosq.c _bcm_td2_cosq_ingress_pg_get
 * maps priority -> PG through THDI_PORT_PRI_GRP0/1), so this looks up a
 * priority whose PG is pg; a PG no priority maps to reads as all zeros.
 */
int hwbcm_pg_stat_multi_get(int port, int pg, int n, const int *ids,
                            int clear, uint64_t *out);

/* ------------------------------------------------------------------------ */
/* buffer (service) pools                                                   */
/* ------------------------------------------------------------------------ */

typedef enum {
    HWBCM_POOLSTAT_NONE = 0,
    HWBCM_POOLSTAT_WATERMARK_CELLS, /* BST IngPool (ingress) / EgrPool (egress, UC+MC) */
    HWBCM_POOLSTAT_COUNT
} hwbcm_poolstat_t;

/*
 * Pools are addressed through one *user* of the pool, because the SAI pool
 * -> service pool number assignment belongs to the QoS code:
 *   egress = 0: (port, cosq) = (bcm port, PG index 0..7) of a PG whose buffer
 *               profile uses the pool; the service pool is THDI_PORT_PG_SPID.
 *   egress = 1: (port, cosq, is_mc) = a queue whose buffer profile uses the
 *               pool; the service pool is that queue's Q_SPID.
 * sp_out (optional) receives the resolved service pool number (0..3).
 */
int hwbcm_pool_stat_multi_get(int egress, int port, int cosq, int is_mc,
                              int n, const int *ids, int clear,
                              uint64_t *out, int *sp_out);

/* ------------------------------------------------------------------------ */
/* extra port counters                                                      */
/* ------------------------------------------------------------------------ */

typedef enum {
    HWBCM_XPSTAT_NONE = 0,
    HWBCM_XPSTAT_IN_DROPPED_PKTS,       /* MMU ingress drops (DROP_PKT_ING) */
    HWBCM_XPSTAT_OUT_DROPPED_PKTS,      /* MMU egress drops, all UC+MC queues */
    HWBCM_XPSTAT_GREEN_WRED_DROPPED_PKTS,
    HWBCM_XPSTAT_YELLOW_WRED_DROPPED_PKTS,
    HWBCM_XPSTAT_RED_WRED_DROPPED_PKTS,
    HWBCM_XPSTAT_WRED_DROPPED_PKTS,     /* green + yellow + red */
    HWBCM_XPSTAT_ETHER_PKTS_1519_TO_2047,
    HWBCM_XPSTAT_ETHER_PKTS_2048_TO_4095,
    HWBCM_XPSTAT_ETHER_PKTS_4096_TO_9216,
    HWBCM_XPSTAT_ETHER_PKTS_9217_TO_16383,
    HWBCM_XPSTAT_IP_IN_RECEIVES,
    HWBCM_XPSTAT_IP_IN_DISCARDS,
    HWBCM_XPSTAT_IPV6_IN_RECEIVES,
    HWBCM_XPSTAT_IPV6_IN_DISCARDS,
    HWBCM_XPSTAT_COUNT
} hwbcm_xpstat_t;

/* Read n extra counters of a front-panel port; ids[i] == HWBCM_XPSTAT_NONE
 * leaves out[i] untouched (so the caller can overlay these on the values
 * from hwbcm_port_stat_multi_get). */
int hwbcm_port_xstat_multi_get(int port, int n, const int *ids, int clear,
                               uint64_t *out);

/* Warm boot helpers (Phase 3): sparse persistence of non-zero READ_AND_CLEAR
 * software baselines (g_qbase / g_xbase) with zero HW writes. */
typedef struct {
    uint8_t port;
    uint8_t mc;
    uint8_t queue;
    uint8_t which;
    uint64_t value;
} hwbcm_qbase_wb_t;

typedef struct {
    uint8_t port;
    uint8_t id;
    uint64_t value;
} hwbcm_xbase_wb_t;

/* Pass out == NULL or max == 0 to query total non-zero count. */
int  hwbcm_stats_wb_get_qbase(hwbcm_qbase_wb_t *out, int max);
int  hwbcm_stats_wb_get_xbase(hwbcm_xbase_wb_t *out, int max);
void hwbcm_stats_warm_restore(const hwbcm_qbase_wb_t *qbase, int nq,
                              const hwbcm_xbase_wb_t *xbase, int nx);
void hwbcm_stats_warm_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* HWBCM_STATS_H */
