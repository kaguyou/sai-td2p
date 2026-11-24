/*
 * hwbcm_acl.h — data-plane ACL (field processor) section of the hwbcm C layer
 * for Trident2+ (BCM56864), OpenBCM sdk-6.5.27.
 *
 * Same rules as hwbcm.h: included from C++ (SwitchBCM56860HW_acl.cpp), no SDK
 * header here, plain C types only, 0 on success / negative BCM_E_* on failure,
 * unit is always HWBCM_UNIT. Implementation: hwbcm_acl.c.
 *
 * Model
 *  - One SAI ACL table  = one bcm field group (bcm_field_group_config_create,
 *    mode Auto) in IFP (SAI INGRESS), EFP (SAI EGRESS) or VFP (SAI PRE_INGRESS).
 *    The group's qset is the table's match fields (HWBCM_ACL_Q_* bits) plus
 *    the stage qualifier and the qualifiers the binding needs.
 *  - One SAI ACL entry = one or more bcm field entries ("variants": one per
 *    binding scope, see SwitchBCM56860HW_acl.cpp). Each variant is described
 *    by a hwbcm_acl_rule_t; the C++ side never sees an SDK type.
 *  - Counters: every variant with a counter owns one bcm field stat
 *    (bcmFieldStatPackets + bcmFieldStatBytes). TD2+ IFP counters live in the
 *    slice of the entry and cannot be shared across slices
 *    (src/bcm/esw/field_common.c:10998 "Counters cannot be shared between
 *    slices"), so a SAI counter is the sum of its variants' stats.
 */
#ifndef HWBCM_ACL_H
#define HWBCM_ACL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* stages */
#define HWBCM_ACL_STAGE_INGRESS 0   /* IFP */
#define HWBCM_ACL_STAGE_EGRESS  1   /* EFP */
#define HWBCM_ACL_STAGE_LOOKUP  2   /* VFP (SAI_ACL_STAGE_PRE_INGRESS) */

/* qualifier bits: a group's qset and the qualifiers present in a rule */
#define HWBCM_ACL_Q_IN_PORTS   (1u << 0)   /* IFP only: InPorts bitmap */
#define HWBCM_ACL_Q_IN_PORT    (1u << 1)   /* EFP/VFP: single InPort */
#define HWBCM_ACL_Q_OUT_PORT   (1u << 2)   /* EFP: OutPort; IFP: DstPort (modid, port) */
#define HWBCM_ACL_Q_VLAN       (1u << 3)   /* OuterVlanId */
#define HWBCM_ACL_Q_ETHER_TYPE (1u << 4)
#define HWBCM_ACL_Q_IP_PROTO   (1u << 5)   /* IPv4 protocol / IPv6 next header */
#define HWBCM_ACL_Q_SRC_IP     (1u << 6)
#define HWBCM_ACL_Q_DST_IP     (1u << 7)
#define HWBCM_ACL_Q_SRC_IP6    (1u << 8)
#define HWBCM_ACL_Q_DST_IP6    (1u << 9)
#define HWBCM_ACL_Q_L4_SRC     (1u << 10)
#define HWBCM_ACL_Q_L4_DST     (1u << 11)
#define HWBCM_ACL_Q_TCP_FLAGS  (1u << 12)  /* TcpControl, 6 bits (FIN..URG) */
#define HWBCM_ACL_Q_TOS        (1u << 13)  /* DSCP<<2 | ECN */
#define HWBCM_ACL_Q_ICMP       (1u << 14)  /* IcmpTypeCode: type<<8 | code */
#define HWBCM_ACL_Q_IP_TYPE    (1u << 15)
#define HWBCM_ACL_Q_TC         (1u << 16)  /* IntPriority */
#define HWBCM_ACL_Q_RANGE      (1u << 17)  /* RangeCheck (IFP only) */
#define HWBCM_ACL_Q_SRC_MAC    (1u << 18)
#define HWBCM_ACL_Q_DST_MAC    (1u << 19)
#define HWBCM_ACL_Q_TTL        (1u << 20)
#define HWBCM_ACL_Q_IP_FRAG    (1u << 21)

/* ip_type values (mapped to bcm_field_IpType_t in hwbcm_acl.c) */
#define HWBCM_ACL_IPT_ANY        0
#define HWBCM_ACL_IPT_IP         1
#define HWBCM_ACL_IPT_NON_IP     2
#define HWBCM_ACL_IPT_IPV4       3
#define HWBCM_ACL_IPT_NON_IPV4   4
#define HWBCM_ACL_IPT_IPV6       5
#define HWBCM_ACL_IPT_NON_IPV6   6
#define HWBCM_ACL_IPT_ARP        7
#define HWBCM_ACL_IPT_ARP_REQ    8
#define HWBCM_ACL_IPT_ARP_REPLY  9

/* ip_frag values (mapped to bcm_field_IpFrag_t) */
#define HWBCM_ACL_FRAG_ANY          0   /* any fragment */
#define HWBCM_ACL_FRAG_NON          1   /* not fragmented */
#define HWBCM_ACL_FRAG_NON_OR_HEAD  2
#define HWBCM_ACL_FRAG_HEAD         3
#define HWBCM_ACL_FRAG_NON_HEAD     4

/* action bits */
#define HWBCM_ACL_A_DROP            (1u << 0)
#define HWBCM_ACL_A_DROP_CANCEL     (1u << 1)
#define HWBCM_ACL_A_COPY_CPU        (1u << 2)
#define HWBCM_ACL_A_COPY_CPU_CANCEL (1u << 3)
#define HWBCM_ACL_A_REDIRECT_PORT   (1u << 4)   /* redirect = bcm port */
#define HWBCM_ACL_A_REDIRECT_TRUNK  (1u << 5)   /* redirect = trunk id */
#define HWBCM_ACL_A_REDIRECT_L3     (1u << 6)   /* redirect = egress object or ECMP id (L3Switch) */
#define HWBCM_ACL_A_MIRROR_ING      (1u << 7)   /* mirror_ing[] = mirror destination gports */
#define HWBCM_ACL_A_MIRROR_EGR      (1u << 8)
#define HWBCM_ACL_A_SET_TC          (1u << 9)   /* PrioIntNew */
#define HWBCM_ACL_A_SET_DSCP        (1u << 10)
#define HWBCM_ACL_A_SET_ECN         (1u << 11)
#define HWBCM_ACL_A_SET_COLOR       (1u << 12)  /* DropPrecedence */
#define HWBCM_ACL_A_SET_OUTER_VLAN  (1u << 13)
#define HWBCM_ACL_A_SET_OUTER_PRI   (1u << 14)

#define HWBCM_ACL_COLOR_GREEN  0
#define HWBCM_ACL_COLOR_YELLOW 1
#define HWBCM_ACL_COLOR_RED    2

#define HWBCM_ACL_PBMP_WORDS   4    /* HWBCM_MAX_PORTS / 32 */
#define HWBCM_ACL_MAX_RANGES   4
#define HWBCM_ACL_MAX_MIRRORS  4

/* One hardware entry. Unused fields are ignored unless their bit is set in
 * quals / acts. IPv4 addresses are in host byte order; IPv6 and MAC addresses
 * in network order (as in SAI). */
typedef struct {
    uint32_t quals;                              /* HWBCM_ACL_Q_* */

    uint32_t in_pbmp[HWBCM_ACL_PBMP_WORDS];      /* bit n = bcm logical port n */
    int in_port;
    int out_port;
    uint16_t vlan, vlan_mask;
    uint16_t ether_type, ether_type_mask;
    uint8_t ip_proto, ip_proto_mask;
    uint32_t src_ip, src_ip_mask;
    uint32_t dst_ip, dst_ip_mask;
    uint8_t src_ip6[16], src_ip6_mask[16];
    uint8_t dst_ip6[16], dst_ip6_mask[16];
    uint16_t l4_src, l4_src_mask;
    uint16_t l4_dst, l4_dst_mask;
    uint8_t tcp_flags, tcp_flags_mask;
    uint8_t tos, tos_mask;
    uint16_t icmp, icmp_mask;
    int ip_type;                                 /* HWBCM_ACL_IPT_* */
    uint8_t tc, tc_mask;
    uint8_t ttl, ttl_mask;
    int ip_frag;                                 /* HWBCM_ACL_FRAG_* */
    uint8_t src_mac[6], src_mac_mask[6];
    uint8_t dst_mac[6], dst_mac_mask[6];
    int nranges;
    int ranges[HWBCM_ACL_MAX_RANGES];            /* from hwbcm_acl_range_create */

    uint32_t acts;                               /* HWBCM_ACL_A_* */
    int redirect;                                /* port / trunk / egress id */
    int n_mirror_ing;
    int mirror_ing[HWBCM_ACL_MAX_MIRRORS];
    int n_mirror_egr;
    int mirror_egr[HWBCM_ACL_MAX_MIRRORS];
    uint8_t set_tc, set_dscp, set_ecn, set_outer_pri;
    int set_color;                               /* HWBCM_ACL_COLOR_* */
    uint16_t set_outer_vlan;
} hwbcm_acl_rule_t;

/* Zero a rule (in_port/out_port = -1). */
void hwbcm_acl_rule_init(hwbcm_acl_rule_t *r);

/* Add a bcm logical port to rule->in_pbmp (and set HWBCM_ACL_Q_IN_PORTS). */
void hwbcm_acl_rule_inport_add(hwbcm_acl_rule_t *r, int port);

/* Qualifier bits the stage can carry at all (static, from the TD2+ qualifier
 * tables in src/bcm/esw/trident2plus/field.c). */
uint32_t hwbcm_acl_stage_quals(int stage);

/* Action bits the stage can carry (src/bcm/esw/trx/field.c:10446 for EFP). */
uint32_t hwbcm_acl_stage_acts(int stage);

/* Field group for one SAI ACL table. priority must be unique per stage
 * (include/bcm/field.h:6711 "Each priority value may be used only once").
 * Returns BCM_E_UNAVAIL if quals has a bit the stage does not support,
 * BCM_E_RESOURCE if the TCAM has no room for that key width. */
int hwbcm_acl_group_create(int stage, uint32_t quals, int priority, int *gid);
int hwbcm_acl_group_destroy(int gid);

/* Entries used / free in the group's slices (CRM). */
int hwbcm_acl_group_status(int gid, int *used, int *free_cnt);

/* Create, qualify, add actions, optionally create+attach a stat, install.
 * prio: higher value wins (same sense as SAI_ACL_ENTRY_ATTR_PRIORITY).
 * *stat_id = -1 when with_stat is 0. Nothing is left behind on failure. */
int hwbcm_acl_entry_add(int gid, int prio, const hwbcm_acl_rule_t *r, int with_stat,
                        int *eid, int *stat_id);

/* Re-qualify an installed entry in place (qualify_clear + action_remove_all +
 * reinstall): no traffic gap, stat stays attached. The rule is validated
 * against the group's qset first; on a late SDK failure the entry is removed
 * from hardware (so no half-programmed entry stays installed) and the error
 * is returned. */
int hwbcm_acl_entry_update(int gid, int eid, int prio, const hwbcm_acl_rule_t *r);

/* Remove from hardware, detach and destroy the stat (stat_id may be -1),
 * destroy the entry. */
int hwbcm_acl_entry_delete(int eid, int stat_id);

/* Counter access. A counter type that was not created reads as 0. */
int hwbcm_acl_stat_get(int stat_id, uint64_t *packets, uint64_t *bytes);
int hwbcm_acl_stat_set(int stat_id, uint64_t packets, uint64_t bytes);

/* L4 port range checker (FP_RANGE_CHECK, 32 entries on BCM56860:
 * src/soc/mcm/allmems_f.i:499428). dst: 0 = source port, 1 = destination. */
int hwbcm_acl_range_create(int dst, uint16_t min, uint16_t max, int *range_id);
int hwbcm_acl_range_destroy(int range_id);

/* Debug: dump a group / entry to the SDK console ("fp show group", "fp show entry"). */
int hwbcm_acl_dump_group(int gid);
int hwbcm_acl_dump_entry(int eid);

/* Warm boot helpers (Phase 3): restore group metadata with zero HW writes,
 * reset SW caches on deinit. */
int  hwbcm_acl_group_restore(int gid, int stage, uint32_t quals);
void hwbcm_acl_warm_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* HWBCM_ACL_H */
