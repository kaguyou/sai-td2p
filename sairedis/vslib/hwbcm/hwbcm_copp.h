/*
 * hwbcm_copp.h — control-plane policing (CoPP) for the Trident2+ hardware SAI.
 *
 * Model (see notes/integration_copp.md for the reasoning and SDK citations):
 *  - One dedicated ingress FP (IFP) group, priority HWBCM_COPP_FP_GROUP_PRIO,
 *    holds one or more entries per SAI hostif trap. Entry priority =
 *    SAI trap priority * 16 + rule specificity, so a higher SAI
 *    TRAP_PRIORITY wins, as SAI requires (only one entry per slice hits).
 *  - Every entry of a trap sets the CPU CoS queue (bcmFieldActionCosQCpuNew =
 *    trap group QUEUE) and, if the trap group has a policer, has that policer
 *    attached. All traps of a group share one bcm policer -> aggregate rate,
 *    like SAI. A colour whose SAI action is DROP cancels the CPU copy
 *    (switch-logic and FP copies), so "copy" traps keep forwarding the frame
 *    and "trap" traps lose it.
 *  - Traps that hwbcm_trap_set() already steers to the CPU (ARP, LLDP, LACP,
 *    STP, UDLD, DHCP, IP2ME/BGP, TTL) get classification-only entries; the
 *    switch logic keeps doing the copy. Trap types that nothing else steers
 *    (EAPOL, PVRST, DHCPv6, OSPF, VRRP, PIM, IGMP) get entries that copy to
 *    the CPU themselves (+ drop for TRAP).
 *  - "To me" traps (IP2ME, BGP, SSH, SNMP, BFD, LDP) are restricted with
 *    DstL3Egress == the CPU egress used by the ip2me routes, so transit
 *    traffic never consumes the CPU budget. They stay pending until the C++
 *    side reports that egress with hwbcm_copp_local_egress_set().
 *
 * Plain C types only (included from C++). All functions return 0 or a
 * negative BCM_E_* code. Unit is HWBCM_UNIT.
 */
#ifndef HWBCM_COPP_H
#define HWBCM_COPP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IFP group priority used by CoPP. ACL groups must stay below this value
 * (ACL is assumed to use 10..40). The group occupies one IFP slice (more if
 * the SDK picks a double-wide mode for the qualifier set). */
#define HWBCM_COPP_FP_GROUP_PRIO   100

/* BCM_E_* values the C++ side tests for (include/shared/error.h). */
#define HWBCM_COPP_E_NOT_FOUND     (-7)
#define HWBCM_COPP_E_BUSY          (-10)

/* Trap kinds understood by the CoPP classifier. The C++ side maps
 * sai_hostif_trap_type_t onto these. */
typedef enum {
    HWBCM_COPP_TRAP_NONE = 0,
    HWBCM_COPP_TRAP_ARP_REQUEST,
    HWBCM_COPP_TRAP_ARP_RESPONSE,
    HWBCM_COPP_TRAP_IPV6_ND,          /* ICMPv6 133..137 */
    HWBCM_COPP_TRAP_IPV6_NS,          /* ICMPv6 135 */
    HWBCM_COPP_TRAP_IPV6_NA,          /* ICMPv6 136 */
    HWBCM_COPP_TRAP_LLDP,
    HWBCM_COPP_TRAP_LACP,
    HWBCM_COPP_TRAP_STP,
    HWBCM_COPP_TRAP_PVRST,
    HWBCM_COPP_TRAP_UDLD,
    HWBCM_COPP_TRAP_EAPOL,
    HWBCM_COPP_TRAP_DHCP,
    HWBCM_COPP_TRAP_DHCPV6,
    HWBCM_COPP_TRAP_BGP,
    HWBCM_COPP_TRAP_BGPV6,
    HWBCM_COPP_TRAP_SSH,
    HWBCM_COPP_TRAP_SNMP,
    HWBCM_COPP_TRAP_IP2ME,
    HWBCM_COPP_TRAP_TTL_ERROR,
    HWBCM_COPP_TRAP_OSPF,
    HWBCM_COPP_TRAP_OSPFV6,
    HWBCM_COPP_TRAP_VRRP,
    HWBCM_COPP_TRAP_VRRPV6,
    HWBCM_COPP_TRAP_PIM,
    HWBCM_COPP_TRAP_IGMP,             /* all IGMP types (IP proto 2) */
    HWBCM_COPP_TRAP_BFD,
    HWBCM_COPP_TRAP_BFDV6,
    HWBCM_COPP_TRAP_LDP,
    HWBCM_COPP_TRAP_COUNT
} hwbcm_copp_trap_t;

#define HWBCM_COPP_ACT_TRAP   2      /* to CPU only (forwarding dropped) */
#define HWBCM_COPP_ACT_COPY   1      /* to CPU and forwarded */

#define HWBCM_COPP_POLICER_SR_TCM     0
#define HWBCM_COPP_POLICER_TR_TCM     1
#define HWBCM_COPP_POLICER_COMMITTED  2   /* single rate, single bucket */

typedef struct {
    int packets;        /* 1: cir/pir in packets/s, cbs/pbs in packets;
                           0: cir/pir in bytes/s, cbs/pbs in bytes (SAI units) */
    int mode;           /* HWBCM_COPP_POLICER_* */
    int color_blind;    /* 1: SAI_POLICER_COLOR_SOURCE_BLIND */
    uint64_t cir, cbs, pir, pbs;
} hwbcm_copp_policer_cfg_t;

/* SDK policer (bcm_policer_create in packet or kbit mode). */
int hwbcm_copp_policer_create(const hwbcm_copp_policer_cfg_t *cfg, int *policer_id);
/* New rates/bursts; the SDK rewrites the meter of every attached entry. */
int hwbcm_copp_policer_set(int policer_id, const hwbcm_copp_policer_cfg_t *cfg);
/* BCM_E_BUSY while any CoPP entry still uses it. */
int hwbcm_copp_policer_destroy(int policer_id);

typedef struct {
    int trap;           /* hwbcm_copp_trap_t */
    int action;         /* HWBCM_COPP_ACT_TRAP / HWBCM_COPP_ACT_COPY */
    uint32_t priority;  /* SAI TRAP_PRIORITY (higher wins) */
    int queue;          /* CPU CoS queue (trap group QUEUE); -1 = SDK default */
    int policer_id;     /* hwbcm_copp_policer_create id; -1 = no policer */
    int drop_green;     /* policer colour actions: 1 = SAI_PACKET_ACTION_DROP */
    int drop_yellow;
    int drop_red;
} hwbcm_copp_trap_cfg_t;

/*
 * Install (or re-install) the FP entries of one trap.
 * *handle: in = existing handle (re-apply) or -1 (new); out = handle.
 * New entries are installed before the old ones are removed, so the trap is
 * never unpoliced in between. A "to me" trap without a known local egress is
 * stored as pending (returns 0, hwbcm_copp_trap_entries() == 0).
 */
int hwbcm_copp_trap_apply(const hwbcm_copp_trap_cfg_t *cfg, int *handle);
int hwbcm_copp_trap_remove(int handle);
/* Number of FP entries currently installed for the trap (0 = pending). */
int hwbcm_copp_trap_entries(int handle);

/* Packets of the trap's entries, split by meter colour. Accumulated across
 * re-applies. Without a policer every packet is "not red". */
int hwbcm_copp_trap_stats_get(int handle, uint64_t *not_red_pkts, uint64_t *not_red_bytes,
                              uint64_t *red_pkts);
int hwbcm_copp_trap_stats_clear(int handle);

/* The L3 egress the ip2me routes point at (A3's shared L2TOCPU egress).
 * Re-installs every "to me" trap. -1 forgets it (to-me traps go pending). */
int hwbcm_copp_local_egress_set(int egr_id);

/* Re-install every trap (switch MAC changed: the ARP reply rule matches it). */
int hwbcm_copp_refresh(void);

/* Log the CoPP state (group, traps, entries, policers) at INFO level. */
int hwbcm_copp_dump(void);

/* Destroy all entries, policers and the group (switch remove / tests). */
int hwbcm_copp_deinit(void);

/* Warm boot helpers (Phase 3): persist/restore CoPP SW tables with zero HW writes. */
#define HWBCM_COPP_MAX_TRAPS    64
#define HWBCM_COPP_MAX_RULES    6
#define HWBCM_COPP_MAX_POLICERS 32

typedef struct {
    int group_valid;
    int group;
    int have_to_me;
    int have_icmp;
    int have_stats;
    int local_egr;
} hwbcm_copp_wb_globals_t;

typedef struct {
    int slot_idx;
    int pid;
    hwbcm_copp_policer_cfg_t cfg;
} hwbcm_copp_policer_wb_t;

typedef struct {
    int slot_idx;
    hwbcm_copp_trap_cfg_t cfg;
    int neid;
    int eid[HWBCM_COPP_MAX_RULES];
    int stat_id[HWBCM_COPP_MAX_RULES];
    uint64_t base_nr_pkts;
    uint64_t base_nr_bytes;
    uint64_t base_red_pkts;
} hwbcm_copp_slot_wb_t;

void hwbcm_copp_wb_get(hwbcm_copp_wb_globals_t *globals,
                       hwbcm_copp_policer_wb_t *pols, int max_pols, int *npols,
                       hwbcm_copp_slot_wb_t *slots, int max_slots, int *nslots);
int  hwbcm_copp_warm_restore(const hwbcm_copp_wb_globals_t *globals,
                             const hwbcm_copp_policer_wb_t *pols, int npols,
                             const hwbcm_copp_slot_wb_t *slots, int nslots);
void hwbcm_copp_warm_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* HWBCM_COPP_H */
