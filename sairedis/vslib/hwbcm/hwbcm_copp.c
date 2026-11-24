/*
 * hwbcm_copp.c — control-plane policing (CoPP) on Trident2+ (BCM56864) with
 * OpenBCM sdk-6.5.27: one IFP group, per-trap classifier entries, per trap
 * group FP policers, CPU CoS queue per trap group.
 *
 * Why IFP + FP policer (and not CPU-queue shaping): SONiC puts several trap
 * groups with different rates on the same CPU queue (copp_cfg.json:
 * queue4_group1 / queue4_group2 / queue4_group3 all use queue 4, only group2
 * has a 600 pps policer), so the rate has to be attached to the classifier,
 * not to the queue. That is also what SAI means by
 * SAI_HOSTIF_TRAP_GROUP_ATTR_POLICER (aggregate of the group's traps).
 *
 * SDK facts this file relies on (paths relative to sdk-6.5.27/):
 *  - TD2+ IFP has global meter pools (src/bcm/esw/trident2/field.c:11231-11234,
 *    used for TD2+ too); a policer shared by several entries must live in the
 *    entries' slice (src/bcm/esw/field.c:17660-17732), so every CoPP entry is
 *    in one group.
 *  - Packet-mode meters (BCM_POLICER_MODE_PACKETS) are available on TD2+:
 *    soc_feature_field_packet_based_metering comes from the 56634 feature list
 *    the 56860 list falls back to (src/soc/common/feature.c:813, chain
 *    6487 -> 2724 -> 2152 -> 1992 -> 769), checked in
 *    src/bcm/esw/field.c:18011-18015. In packet mode ckbits_sec is packets/s
 *    and ckbits_burst is packets (src/bcm/esw/firebolt/meter.c:225-239,
 *    611-619; TD2 granularity tweak field_common.c:30210-30222).
 *  - srTCM programs committed + peak buckets with REFRESH_MODE=1
 *    (src/bcm/esw/trx/field.c:15670-15672, 16166-16245), so pkbits_sec is set
 *    to the committed rate and pkbits_burst carries EBS (SAI PBS).
 *  - On TD2/TT2 bcmFieldActionRpCopyToCpuCancel + RpSwitchToCpuCancel on the
 *    same entry encode R_COPY_TO_CPU = COPY_AND_SWITCH_TO_CPU_CANCEL (6)
 *    (src/bcm/esw/trx/field.c:3240-3252, 8194-8206;
 *    include/bcm_int/esw/field.h:5506-5508) and are explicitly allowed
 *    together (trx/field.c:12180-12230). That cancels both the switch-logic
 *    copy (ArpRequestToCpu, L2 user entry CPU bit, L2TOCPU next hop ...) and
 *    FP copies for red packets. Same for yellow / green.
 *  - Gp/YpCopyToCpu do not conflict with the Rp cancels or with Drop
 *    (trx/field.c:12331-12345, 12417-12436), which is how the self-steering
 *    entries express "copy green/yellow only".
 *  - bcmFieldActionCosQCpuNew on IFP sets CHANGE_CPU_COS=1, CPU_COS=q
 *    (trx/field.c:7806-7818).
 *  - TD2+ IFP qualifiers used here are all in
 *    _field_td2plus_ingress_qualifiers_init (src/bcm/esw/trident2plus/field.c:
 *    DstL3Egress 365, EtherType 891, L4 ports/IcmpTypeCode 1942-1948, Ttl
 *    2010, DstMac 2324, MyStationHit 4776).
 *  - Per-entry statistics {Red, NotRed} x {packets, bytes} is HW counter mode
 *    3 (2 counters) in _bcm_field_trx_stat_hw_mode_to_bmap
 *    (trx/field.c:15409-15420).
 *  - bcm_policer_destroy returns BCM_E_BUSY while attached
 *    (field_common.c:7223-7224).
 */

/* The SDK build flags carry -DBCM_HIDE_DISPATCHABLE; it hides every bcm_*
 * prototype, which we need (same as hwbcm_core.c). */
#ifdef BCM_HIDE_DISPATCHABLE
#undef BCM_HIDE_DISPATCHABLE
#endif

#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include <bcm/types.h>
#include <bcm/error.h>
#include <bcm/field.h>
#include <bcm/policer.h>
#include <bcm/rx.h>

#include "hwbcm.h"
#include "hwbcm_copp.h"

/* hwbcm_copp.h repeats two BCM_E_* values for the C++ side */
typedef char copp_e_busy_matches[(BCM_E_BUSY == HWBCM_COPP_E_BUSY) ? 1 : -1];
typedef char copp_e_not_found_matches[(BCM_E_NOT_FOUND == HWBCM_COPP_E_NOT_FOUND) ? 1 : -1];

#define CLOGE(...) hwbcm_log(HWBCM_LOG_ERR, "copp: " __VA_ARGS__)
#define CLOGW(...) hwbcm_log(HWBCM_LOG_WARN, "copp: " __VA_ARGS__)
#define CLOGI(...) hwbcm_log(HWBCM_LOG_INFO, "copp: " __VA_ARGS__)
#define CLOGD(...) hwbcm_log(HWBCM_LOG_DEBUG, "copp: " __VA_ARGS__)

#define COPP_MAX_TRAPS      64
#define COPP_MAX_RULES      6      /* FP entries per trap */
#define COPP_MAX_POLICERS   32
#define COPP_PRIO_SUBS      16     /* entry prio = sai prio * 16 + sub */
#define COPP_PRIO_MAX       0xfffff

/* ------------------------------------------------------------------------ */
/* rule templates                                                           */
/* ------------------------------------------------------------------------ */

enum {
    DMAC_ANY = 0,
    DMAC_FIXED,         /* rule.dmac */
    DMAC_BCAST,         /* ff:ff:ff:ff:ff:ff */
    DMAC_SWITCH,        /* switch MAC (hwbcm_switch_mac_get), any if unknown */
};

typedef struct {
    uint16_t ethertype;     /* 0 = any */
    int dmac_kind;
    uint8_t dmac[6];
    int ip_proto;           /* -1 = any */
    int l4_dst;             /* -1 = any */
    int l4_src;             /* -1 = any */
    int icmp_type;          /* -1 = any (ICMPv4/v6 type, code wildcarded) */
    int ttl_le1;            /* 1 = TTL / hop limit 0 or 1 */
    int my_station;         /* 1 = MyStationHit (routed / to router MAC) */
    int to_me;              /* 1 = DstL3Egress == local (ip2me) egress */
    int sub;                /* specificity inside one SAI priority, 0..15 */
} copp_rule_t;

#define ETH_IP4   0x0800
#define ETH_IP6   0x86dd
#define ETH_ARP   0x0806
#define ETH_EAPOL 0x888e

#define SUB_SPECIFIC  12
#define SUB_ARP_REPLY 11
#define SUB_IP2ME     4
#define SUB_TTL       2

static void rule_init(copp_rule_t *r)
{
    memset(r, 0, sizeof(*r));
    r->ip_proto = -1;
    r->l4_dst = -1;
    r->l4_src = -1;
    r->icmp_type = -1;
    r->sub = SUB_SPECIFIC;
}

static copp_rule_t *rule_add(copp_rule_t *rules, int *n)
{
    copp_rule_t *r;

    if (*n >= COPP_MAX_RULES) {
        return NULL;
    }
    r = &rules[(*n)++];
    rule_init(r);
    return r;
}

static void rule_dmac(copp_rule_t *r, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e, uint8_t f)
{
    r->dmac_kind = DMAC_FIXED;
    r->dmac[0] = a; r->dmac[1] = b; r->dmac[2] = c;
    r->dmac[3] = d; r->dmac[4] = e; r->dmac[5] = f;
}

static void rule_l4(copp_rule_t *rules, int *n, uint16_t eth, int proto, int dst, int src, int to_me)
{
    copp_rule_t *r = rule_add(rules, n);

    if (r == NULL) {
        return;
    }
    r->ethertype = eth;
    r->ip_proto = proto;
    r->l4_dst = dst;
    r->l4_src = src;
    r->to_me = to_me;
}

static void rule_icmp6(copp_rule_t *rules, int *n, int type)
{
    copp_rule_t *r = rule_add(rules, n);

    if (r == NULL) {
        return;
    }
    r->ethertype = ETH_IP6;
    r->ip_proto = 58;
    r->icmp_type = type;
}

static void rule_proto(copp_rule_t *rules, int *n, uint16_t eth, int proto)
{
    copp_rule_t *r = rule_add(rules, n);

    if (r == NULL) {
        return;
    }
    r->ethertype = eth;
    r->ip_proto = proto;
}

/*
 * Rules of one trap kind. *steer = 1 when no other code steers this traffic
 * to the CPU, so the entries copy it themselves (hwCreateHostifTrap /
 * hwbcm_trap_set have no reason for it).
 */
static int copp_rules_for(int trap, copp_rule_t *rules, int *steer)
{
    int n = 0;
    copp_rule_t *r;
    int t;

    *steer = 0;

    switch (trap) {
    case HWBCM_COPP_TRAP_ARP_REQUEST:
        /* no ARP opcode qualifier on TD2+ IFP: a request is broadcast */
        r = rule_add(rules, &n);
        r->ethertype = ETH_ARP;
        r->dmac_kind = DMAC_BCAST;
        break;
    case HWBCM_COPP_TRAP_ARP_RESPONSE:
        /* replies to us (ArpReplyToCpu); unicast host-to-host replies are
         * not copied and must not use up the ARP budget */
        r = rule_add(rules, &n);
        r->ethertype = ETH_ARP;
        r->dmac_kind = DMAC_SWITCH;
        r->sub = SUB_ARP_REPLY;
        break;
    case HWBCM_COPP_TRAP_IPV6_ND:
        for (t = 133; t <= 137; t++) {
            rule_icmp6(rules, &n, t);
        }
        break;
    case HWBCM_COPP_TRAP_IPV6_NS:
        rule_icmp6(rules, &n, 135);
        break;
    case HWBCM_COPP_TRAP_IPV6_NA:
        rule_icmp6(rules, &n, 136);
        break;
    case HWBCM_COPP_TRAP_LLDP:
        r = rule_add(rules, &n);
        rule_dmac(r, 0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e);
        break;
    case HWBCM_COPP_TRAP_LACP:
        r = rule_add(rules, &n);
        rule_dmac(r, 0x01, 0x80, 0xc2, 0x00, 0x00, 0x02);
        break;
    case HWBCM_COPP_TRAP_STP:
        r = rule_add(rules, &n);
        rule_dmac(r, 0x01, 0x80, 0xc2, 0x00, 0x00, 0x00);
        break;
    case HWBCM_COPP_TRAP_PVRST:
        /* hwbcm_trap_set(HWBCM_TRAP_STP) only covers 01:80:c2:00:00:00 */
        r = rule_add(rules, &n);
        rule_dmac(r, 0x01, 0x00, 0x0c, 0xcc, 0xcc, 0xcd);
        *steer = 1;
        break;
    case HWBCM_COPP_TRAP_UDLD:
        r = rule_add(rules, &n);
        rule_dmac(r, 0x01, 0x00, 0x0c, 0xcc, 0xcc, 0xcc);
        break;
    case HWBCM_COPP_TRAP_EAPOL:
        r = rule_add(rules, &n);
        r->ethertype = ETH_EAPOL;
        *steer = 1;
        break;
    case HWBCM_COPP_TRAP_DHCP:
        rule_l4(rules, &n, ETH_IP4, 17, 67, -1, 0);
        rule_l4(rules, &n, ETH_IP4, 17, 68, -1, 0);
        break;
    case HWBCM_COPP_TRAP_DHCPV6:
        /* DhcpPktToCpu is the IPv4 control; copy v6 ourselves (one CPU copy
         * per packet either way) */
        rule_l4(rules, &n, ETH_IP6, 17, 547, -1, 0);
        rule_l4(rules, &n, ETH_IP6, 17, 546, -1, 0);
        *steer = 1;
        break;
    case HWBCM_COPP_TRAP_BGP:
        rule_l4(rules, &n, ETH_IP4, 6, 179, -1, 1);
        rule_l4(rules, &n, ETH_IP4, 6, -1, 179, 1);
        break;
    case HWBCM_COPP_TRAP_BGPV6:
        rule_l4(rules, &n, ETH_IP6, 6, 179, -1, 1);
        rule_l4(rules, &n, ETH_IP6, 6, -1, 179, 1);
        break;
    case HWBCM_COPP_TRAP_SSH:
        rule_l4(rules, &n, ETH_IP4, 6, 22, -1, 1);
        rule_l4(rules, &n, ETH_IP6, 6, 22, -1, 1);
        break;
    case HWBCM_COPP_TRAP_SNMP:
        rule_l4(rules, &n, ETH_IP4, 17, 161, -1, 1);
        rule_l4(rules, &n, ETH_IP6, 17, 161, -1, 1);
        break;
    case HWBCM_COPP_TRAP_IP2ME:
        r = rule_add(rules, &n);
        r->ethertype = ETH_IP4;
        r->to_me = 1;
        r->sub = SUB_IP2ME;
        r = rule_add(rules, &n);
        r->ethertype = ETH_IP6;
        r->to_me = 1;
        r->sub = SUB_IP2ME;
        break;
    case HWBCM_COPP_TRAP_TTL_ERROR:
        r = rule_add(rules, &n);
        r->ethertype = ETH_IP4;
        r->my_station = 1;
        r->ttl_le1 = 1;
        r->sub = SUB_TTL;
        r = rule_add(rules, &n);
        r->ethertype = ETH_IP6;
        r->my_station = 1;
        r->ttl_le1 = 1;
        r->sub = SUB_TTL;
        break;
    case HWBCM_COPP_TRAP_OSPF:
        rule_proto(rules, &n, ETH_IP4, 89);
        *steer = 1;
        break;
    case HWBCM_COPP_TRAP_OSPFV6:
        rule_proto(rules, &n, ETH_IP6, 89);
        *steer = 1;
        break;
    case HWBCM_COPP_TRAP_VRRP:
        rule_proto(rules, &n, ETH_IP4, 112);
        *steer = 1;
        break;
    case HWBCM_COPP_TRAP_VRRPV6:
        rule_proto(rules, &n, ETH_IP6, 112);
        *steer = 1;
        break;
    case HWBCM_COPP_TRAP_PIM:
        rule_proto(rules, &n, ETH_IP4, 103);
        *steer = 1;
        break;
    case HWBCM_COPP_TRAP_IGMP:
        rule_proto(rules, &n, ETH_IP4, 2);
        *steer = 1;
        break;
    case HWBCM_COPP_TRAP_BFD:
        rule_l4(rules, &n, ETH_IP4, 17, 3784, -1, 1);
        rule_l4(rules, &n, ETH_IP4, 17, 4784, -1, 1);
        break;
    case HWBCM_COPP_TRAP_BFDV6:
        rule_l4(rules, &n, ETH_IP6, 17, 3784, -1, 1);
        rule_l4(rules, &n, ETH_IP6, 17, 4784, -1, 1);
        break;
    case HWBCM_COPP_TRAP_LDP:
        rule_l4(rules, &n, ETH_IP4, 6, 646, -1, 1);
        rule_l4(rules, &n, ETH_IP4, 6, -1, 646, 1);
        rule_l4(rules, &n, ETH_IP4, 17, 646, -1, 1);
        break;
    default:
        return 0;
    }
    return n;
}

static int rules_need_to_me(const copp_rule_t *rules, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (rules[i].to_me) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* state                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct {
    int used;
    hwbcm_copp_trap_cfg_t cfg;
    int neid;
    bcm_field_entry_t eid[COPP_MAX_RULES];
    int stat_id[COPP_MAX_RULES];          /* -1 = no stat */
    uint64_t base_nr_pkts;                /* counts of destroyed entries */
    uint64_t base_nr_bytes;
    uint64_t base_red_pkts;
} copp_slot_t;

typedef struct {
    int used;
    bcm_policer_t pid;
    hwbcm_copp_policer_cfg_t cfg;
} copp_policer_t;

static pthread_mutex_t g_copp_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_group_valid = 0;
static bcm_field_group_t g_group = 0;
static int g_have_to_me = 0;           /* DstL3Egress in the group qset */
static int g_have_icmp = 0;            /* IcmpTypeCode in the group qset */
static int g_have_stats = 1;           /* cleared after the first stat failure */
static int g_local_egr = -1;
static int g_qmax = -1;                /* bcm_rx_queue_max_get */
static copp_slot_t g_slot[COPP_MAX_TRAPS];
static copp_policer_t g_pol[COPP_MAX_POLICERS];

static const char *trap_name(int trap)
{
    static const char *names[HWBCM_COPP_TRAP_COUNT] = {
        "none", "arp_req", "arp_resp", "nd", "nd_ns", "nd_na", "lldp", "lacp",
        "stp", "pvrst", "udld", "eapol", "dhcp", "dhcpv6", "bgp", "bgpv6",
        "ssh", "snmp", "ip2me", "ttl_error", "ospf", "ospfv6", "vrrp",
        "vrrpv6", "pim", "igmp", "bfd", "bfdv6", "ldp",
    };

    if (trap < 0 || trap >= HWBCM_COPP_TRAP_COUNT) {
        return "?";
    }
    return names[trap];
}

/* ------------------------------------------------------------------------ */
/* FP group                                                                 */
/* ------------------------------------------------------------------------ */

static int group_try(int with_to_me, int with_icmp)
{
    bcm_field_group_config_t gc;
    int rv;

    bcm_field_group_config_t_init(&gc);
    BCM_FIELD_QSET_INIT(gc.qset);
    BCM_FIELD_QSET_ADD(gc.qset, bcmFieldQualifyStageIngress);
    BCM_FIELD_QSET_ADD(gc.qset, bcmFieldQualifyEtherType);
    BCM_FIELD_QSET_ADD(gc.qset, bcmFieldQualifyDstMac);
    BCM_FIELD_QSET_ADD(gc.qset, bcmFieldQualifyIpProtocol);
    BCM_FIELD_QSET_ADD(gc.qset, bcmFieldQualifyL4DstPort);
    BCM_FIELD_QSET_ADD(gc.qset, bcmFieldQualifyL4SrcPort);
    BCM_FIELD_QSET_ADD(gc.qset, bcmFieldQualifyTtl);
    BCM_FIELD_QSET_ADD(gc.qset, bcmFieldQualifyMyStationHit);
    if (with_icmp) {
        BCM_FIELD_QSET_ADD(gc.qset, bcmFieldQualifyIcmpTypeCode);
    }
    if (with_to_me) {
        BCM_FIELD_QSET_ADD(gc.qset, bcmFieldQualifyDstL3Egress);
    }
    gc.priority = HWBCM_COPP_FP_GROUP_PRIO;
    gc.mode = bcmFieldGroupModeAuto;
    gc.flags = BCM_FIELD_GROUP_CREATE_WITH_MODE;

    rv = bcm_field_group_config_create(HWBCM_UNIT, &gc);
    if (BCM_FAILURE(rv)) {
        CLOGW("bcm_field_group_config_create(prio %d, to_me %d, icmp %d): %s (%d)",
              HWBCM_COPP_FP_GROUP_PRIO, with_to_me, with_icmp, bcm_errmsg(rv), rv);
        return rv;
    }
    g_group = gc.group;
    g_group_valid = 1;
    g_have_to_me = with_to_me;
    g_have_icmp = with_icmp;
    CLOGI("IFP group %d created (prio %d, DstL3Egress %s, IcmpTypeCode %s)",
          (int)g_group, HWBCM_COPP_FP_GROUP_PRIO,
          with_to_me ? "yes" : "no", with_icmp ? "yes" : "no");
    return 0;
}

static int group_ensure(void)
{
    int rv;

    if (g_group_valid) {
        return 0;
    }
    /* Full qualifier set first; if the slice key cannot hold it, drop the
     * to-me qualifier (those traps stay pending), then the ICMP type (ND
     * rules become "any ICMPv6"). */
    rv = group_try(1, 1);
    if (BCM_FAILURE(rv)) {
        rv = group_try(0, 1);
    }
    if (BCM_FAILURE(rv)) {
        rv = group_try(0, 0);
    }
    if (BCM_FAILURE(rv)) {
        CLOGE("no IFP group for CoPP: %s", bcm_errmsg(rv));
        return rv;
    }
    if (g_qmax < 0) {
        bcm_cos_queue_t q = 0;

        if (BCM_SUCCESS(bcm_rx_queue_max_get(HWBCM_UNIT, &q))) {
            g_qmax = (int)q;
        } else {
            g_qmax = 7;
        }
        CLOGI("CPU CoS queues 0..%d", g_qmax);
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* policers                                                                 */
/* ------------------------------------------------------------------------ */

static copp_policer_t *pol_find(int pid)
{
    int i;

    for (i = 0; i < COPP_MAX_POLICERS; i++) {
        if (g_pol[i].used && (int)g_pol[i].pid == pid) {
            return &g_pol[i];
        }
    }
    return NULL;
}

static uint32 sat32(uint64_t v)
{
    return (v > 0xffffffffULL) ? 0xffffffffU : (uint32)v;
}

/* SAI bytes(/s) -> SDK kbits(/s), rounded up, at least 1 when non-zero. */
static uint32 bytes_to_kbits(uint64_t bytes)
{
    uint64_t kb;

    if (bytes == 0) {
        return 0;
    }
    if (bytes > (0xffffffffffffffffULL / 8)) {
        return 0xffffffffU;
    }
    kb = (bytes * 8 + 999) / 1000;
    return sat32(kb);
}

static int pol_cfg_fill(const hwbcm_copp_policer_cfg_t *in, bcm_policer_config_t *pc)
{
    uint64_t cbs = in->cbs;
    uint64_t pbs = in->pbs;

    bcm_policer_config_t_init(pc);

    switch (in->mode) {
    case HWBCM_COPP_POLICER_SR_TCM:
        /* TD2+ srTCM refills the committed and the excess bucket independently
         * at CIR (no overflow from C into E), so with yellow forwarded (SONiC
         * CoPP drops red only) the long-term rate is ~2x CIR: measured 1402
         * pps through a 600 pps ARP policer on 2026-11-24. A single committed
         * bucket of CBS+EBS gives the RFC 2697 long-term rate (CIR) and the
         * same maximum burst; excess packets are red. */
        pc->mode = bcmPolicerModeCommitted;
        cbs = (cbs ? cbs : in->cir) + pbs;
        pbs = 0;
        break;
    case HWBCM_COPP_POLICER_TR_TCM:
        pc->mode = bcmPolicerModeTrTcm;
        break;
    case HWBCM_COPP_POLICER_COMMITTED:
        pc->mode = bcmPolicerModeCommitted;
        break;
    default:
        return BCM_E_PARAM;
    }
    if (in->color_blind) {
        pc->flags |= BCM_POLICER_COLOR_BLIND;
    }
    /* SAI default CBS is 0, and a zero bucket would turn every packet red:
     * fall back to one second worth of the committed rate. */
    if (cbs == 0) {
        cbs = in->cir;
    }
    if (in->packets) {
        pc->flags |= BCM_POLICER_MODE_PACKETS;
        pc->ckbits_sec = sat32(in->cir);
        pc->ckbits_burst = sat32(cbs);
        pc->pkbits_sec = sat32(in->pir);
        pc->pkbits_burst = sat32(pbs);
    } else {
        pc->ckbits_sec = bytes_to_kbits(in->cir);
        pc->ckbits_burst = bytes_to_kbits(cbs);
        pc->pkbits_sec = bytes_to_kbits(in->pir);
        pc->pkbits_burst = bytes_to_kbits(pbs);
    }
    if (pc->mode == bcmPolicerModeSrTcm) {
        /* single rate: the excess bucket refills at CIR (REFRESH_MODE=1) */
        pc->pkbits_sec = pc->ckbits_sec;
    } else if (pc->mode == bcmPolicerModeCommitted) {
        /* field.c:17986 rejects peak values in committed mode */
        pc->pkbits_sec = 0;
        pc->pkbits_burst = 0;
    }
    return 0;
}

int hwbcm_copp_policer_create(const hwbcm_copp_policer_cfg_t *cfg, int *policer_id)
{
    bcm_policer_config_t pc;
    bcm_policer_t pid = 0;
    copp_policer_t *slot = NULL;
    int rv;
    int i;

    if (cfg == NULL || policer_id == NULL) {
        return BCM_E_PARAM;
    }
    rv = pol_cfg_fill(cfg, &pc);
    if (BCM_FAILURE(rv)) {
        CLOGE("policer mode %d not supported", cfg->mode);
        return rv;
    }
    pthread_mutex_lock(&g_copp_lock);
    for (i = 0; i < COPP_MAX_POLICERS; i++) {
        if (!g_pol[i].used) {
            slot = &g_pol[i];
            break;
        }
    }
    if (slot == NULL) {
        pthread_mutex_unlock(&g_copp_lock);
        CLOGE("policer table full (%d)", COPP_MAX_POLICERS);
        return BCM_E_RESOURCE;
    }
    rv = bcm_policer_create(HWBCM_UNIT, &pc, &pid);
    if (BCM_FAILURE(rv)) {
        pthread_mutex_unlock(&g_copp_lock);
        CLOGE("bcm_policer_create(mode %d flags 0x%x cir %u cbs %u pir %u pbs %u): %s (%d)",
              (int)pc.mode, (unsigned)pc.flags, (unsigned)pc.ckbits_sec, (unsigned)pc.ckbits_burst,
              (unsigned)pc.pkbits_sec, (unsigned)pc.pkbits_burst, bcm_errmsg(rv), rv);
        return rv;
    }
    slot->used = 1;
    slot->pid = pid;
    slot->cfg = *cfg;
    *policer_id = (int)pid;
    pthread_mutex_unlock(&g_copp_lock);
    CLOGI("policer %d: mode %d %s cir %llu cbs %llu pir %llu pbs %llu%s", (int)pid, cfg->mode,
          cfg->packets ? "pps" : "Bps",
          (unsigned long long)cfg->cir, (unsigned long long)cfg->cbs,
          (unsigned long long)cfg->pir, (unsigned long long)cfg->pbs,
          cfg->color_blind ? " blind" : "");
    return 0;
}

int hwbcm_copp_policer_set(int policer_id, const hwbcm_copp_policer_cfg_t *cfg)
{
    bcm_policer_config_t pc;
    copp_policer_t *p;
    int rv;

    if (cfg == NULL) {
        return BCM_E_PARAM;
    }
    rv = pol_cfg_fill(cfg, &pc);
    if (BCM_FAILURE(rv)) {
        return rv;
    }
    pthread_mutex_lock(&g_copp_lock);
    p = pol_find(policer_id);
    if (p == NULL) {
        pthread_mutex_unlock(&g_copp_lock);
        return BCM_E_NOT_FOUND;
    }
    /* bcm_esw_policer_set rewrites the meters of all attached entries
     * (field.c:17211) */
    rv = bcm_policer_set(HWBCM_UNIT, (bcm_policer_t)policer_id, &pc);
    if (BCM_FAILURE(rv)) {
        pthread_mutex_unlock(&g_copp_lock);
        CLOGE("bcm_policer_set(%d): %s (%d)", policer_id, bcm_errmsg(rv), rv);
        return rv;
    }
    p->cfg = *cfg;
    pthread_mutex_unlock(&g_copp_lock);
    CLOGI("policer %d updated: cir %llu cbs %llu pir %llu pbs %llu", policer_id,
          (unsigned long long)cfg->cir, (unsigned long long)cfg->cbs,
          (unsigned long long)cfg->pir, (unsigned long long)cfg->pbs);
    return 0;
}

int hwbcm_copp_policer_destroy(int policer_id)
{
    copp_policer_t *p;
    int rv;
    int i, j;

    pthread_mutex_lock(&g_copp_lock);
    p = pol_find(policer_id);
    if (p == NULL) {
        pthread_mutex_unlock(&g_copp_lock);
        return BCM_E_NOT_FOUND;
    }
    for (i = 0; i < COPP_MAX_TRAPS; i++) {
        if (g_slot[i].used && g_slot[i].cfg.policer_id == policer_id && g_slot[i].neid > 0) {
            pthread_mutex_unlock(&g_copp_lock);
            CLOGW("policer %d still used by trap handle %d (%s)", policer_id, i,
                  trap_name(g_slot[i].cfg.trap));
            return BCM_E_BUSY;
        }
    }
    rv = bcm_policer_destroy(HWBCM_UNIT, (bcm_policer_t)policer_id);
    if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
        pthread_mutex_unlock(&g_copp_lock);
        CLOGE("bcm_policer_destroy(%d): %s (%d)", policer_id, bcm_errmsg(rv), rv);
        return rv;
    }
    /* pending slots that still name it fall back to "no policer" */
    for (j = 0; j < COPP_MAX_TRAPS; j++) {
        if (g_slot[j].used && g_slot[j].cfg.policer_id == policer_id) {
            g_slot[j].cfg.policer_id = -1;
        }
    }
    memset(p, 0, sizeof(*p));
    pthread_mutex_unlock(&g_copp_lock);
    CLOGI("policer %d destroyed", policer_id);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* entries                                                                  */
/* ------------------------------------------------------------------------ */

#define ECHK(what, call)                                                     \
    do {                                                                     \
        rv = (call);                                                         \
        if (BCM_FAILURE(rv)) {                                               \
            CLOGE("%s %s rule %d: %s (%d)", trap_name(cfg->trap), (what),   \
                  ridx, bcm_errmsg(rv), rv);                                 \
            goto fail;                                                       \
        }                                                                    \
    } while (0)

static int entry_prio(const hwbcm_copp_trap_cfg_t *cfg, const copp_rule_t *r)
{
    uint64_t p = (uint64_t)cfg->priority * COPP_PRIO_SUBS + (uint64_t)r->sub;

    if (p > COPP_PRIO_MAX) {
        p = COPP_PRIO_MAX;
    }
    return (int)p;
}

/* Colour actions. steer: the entry itself copies to the CPU. */
static int entry_actions(bcm_field_entry_t eid, const hwbcm_copp_trap_cfg_t *cfg, int steer, int ridx, int to_me)
{
    int rv = 0;
    int policed = (cfg->policer_id >= 0);
    int drop_g = policed && cfg->drop_green;
    int drop_y = policed && cfg->drop_yellow;
    int drop_r = policed && cfg->drop_red;

    if (cfg->queue >= 0) {
        ECHK("CosQCpuNew", bcm_field_action_add(HWBCM_UNIT, eid, bcmFieldActionCosQCpuNew,
                                                (uint32)cfg->queue, 0));
    }
    if (steer) {
        /* Gp/YpCopyToCpu instead of CopyToCpu: CopyToCpu conflicts with the
         * Rp cancels on TD2 (trx/field.c:12180-12230). */
        if (!drop_g) {
            ECHK("GpCopyToCpu", bcm_field_action_add(HWBCM_UNIT, eid, bcmFieldActionGpCopyToCpu, 0, 0));
        }
        if (!drop_y) {
            ECHK("YpCopyToCpu", bcm_field_action_add(HWBCM_UNIT, eid, bcmFieldActionYpCopyToCpu, 0, 0));
        }
        if (!drop_r) {
            ECHK("RpCopyToCpu", bcm_field_action_add(HWBCM_UNIT, eid, bcmFieldActionRpCopyToCpu, 0, 0));
        }
        if (cfg->action == HWBCM_COPP_ACT_TRAP) {
            ECHK("Drop", bcm_field_action_add(HWBCM_UNIT, eid, bcmFieldActionDrop, 0, 0));
        }
    }
    /* Cancel every CPU copy (switch logic + FP) of a dropped colour. With
     * steer the entry has no copy for that colour already, but another
     * mechanism (e.g. a duplicate of the same match) might. */
    /* "To me" packets (DstL3Egress = the ip2me CPU egress) reach the CPU by
     * L3 forwarding to the CPU port, not by a copy, so the copy/switch-to-CPU
     * cancels do not stop them (measured 2026-11-24: ip2me policer red count
     * rising while the CPU still took ~2600 pps). They have no other
     * destination, so drop the over-rate colour outright. */
    if (to_me) {
        if (drop_r) {
            ECHK("RpDrop", bcm_field_action_add(HWBCM_UNIT, eid, bcmFieldActionRpDrop, 0, 0));
        }
        if (drop_y) {
            ECHK("YpDrop", bcm_field_action_add(HWBCM_UNIT, eid, bcmFieldActionYpDrop, 0, 0));
        }
        if (drop_g) {
            ECHK("GpDrop", bcm_field_action_add(HWBCM_UNIT, eid, bcmFieldActionGpDrop, 0, 0));
        }
    }
    if (drop_r) {
        ECHK("RpCopyToCpuCancel", bcm_field_action_add(HWBCM_UNIT, eid, bcmFieldActionRpCopyToCpuCancel, 0, 0));
        ECHK("RpSwitchToCpuCancel", bcm_field_action_add(HWBCM_UNIT, eid, bcmFieldActionRpSwitchToCpuCancel, 0, 0));
    }
    if (drop_y) {
        ECHK("YpCopyToCpuCancel", bcm_field_action_add(HWBCM_UNIT, eid, bcmFieldActionYpCopyToCpuCancel, 0, 0));
        ECHK("YpSwitchToCpuCancel", bcm_field_action_add(HWBCM_UNIT, eid, bcmFieldActionYpSwitchToCpuCancel, 0, 0));
    }
    if (drop_g) {
        ECHK("GpCopyToCpuCancel", bcm_field_action_add(HWBCM_UNIT, eid, bcmFieldActionGpCopyToCpuCancel, 0, 0));
        ECHK("GpSwitchToCpuCancel", bcm_field_action_add(HWBCM_UNIT, eid, bcmFieldActionGpSwitchToCpuCancel, 0, 0));
    }
    return 0;
fail:
    return rv;
}

static int entry_qualify(bcm_field_entry_t eid, const hwbcm_copp_trap_cfg_t *cfg, const copp_rule_t *r, int ridx)
{
    int rv = 0;
    bcm_mac_t mac;
    bcm_mac_t mask;

    if (r->ethertype != 0) {
        ECHK("EtherType", bcm_field_qualify_EtherType(HWBCM_UNIT, eid, r->ethertype, 0xffff));
    }
    if (r->dmac_kind != DMAC_ANY) {
        int have = 1;

        memset(mask, 0xff, sizeof(mask));
        if (r->dmac_kind == DMAC_FIXED) {
            memcpy(mac, r->dmac, sizeof(mac));
        } else if (r->dmac_kind == DMAC_BCAST) {
            memset(mac, 0xff, sizeof(mac));
        } else {
            uint8_t sw[6];

            if (hwbcm_switch_mac_get(sw) == 0) {
                memcpy(mac, sw, sizeof(mac));
            } else {
                CLOGW("%s: switch MAC unknown, matching any destination MAC", trap_name(cfg->trap));
                have = 0;
            }
        }
        if (have) {
            ECHK("DstMac", bcm_field_qualify_DstMac(HWBCM_UNIT, eid, mac, mask));
        }
    }
    if (r->ip_proto >= 0) {
        ECHK("IpProtocol", bcm_field_qualify_IpProtocol(HWBCM_UNIT, eid, (uint8)r->ip_proto, 0xff));
    }
    if (r->l4_dst >= 0) {
        ECHK("L4DstPort", bcm_field_qualify_L4DstPort(HWBCM_UNIT, eid, (bcm_l4_port_t)r->l4_dst, 0xffff));
    }
    if (r->l4_src >= 0) {
        ECHK("L4SrcPort", bcm_field_qualify_L4SrcPort(HWBCM_UNIT, eid, (bcm_l4_port_t)r->l4_src, 0xffff));
    }
    if (r->icmp_type >= 0 && g_have_icmp) {
        /* type in the high byte, code wildcarded */
        ECHK("IcmpTypeCode", bcm_field_qualify_IcmpTypeCode(HWBCM_UNIT, eid,
                                                            (uint16)(r->icmp_type << 8), 0xff00));
    }
    if (r->ttl_le1) {
        ECHK("Ttl", bcm_field_qualify_Ttl(HWBCM_UNIT, eid, 0, 0xfe));
    }
    if (r->my_station) {
        ECHK("MyStationHit", bcm_field_qualify_MyStationHit(HWBCM_UNIT, eid, 1, 1));
    }
    if (r->to_me) {
        ECHK("DstL3Egress", bcm_field_qualify_DstL3Egress(HWBCM_UNIT, eid, (bcm_if_t)g_local_egr));
    }
    return 0;
fail:
    return rv;
}

static void entry_stats_read(bcm_field_entry_t eid, int sid, uint64_t *nr_p, uint64_t *nr_b, uint64_t *red_p)
{
    uint64 v;

    (void)eid;
    if (sid < 0) {
        return;
    }
    v = 0;
    if (BCM_SUCCESS(bcm_field_stat_get(HWBCM_UNIT, sid, bcmFieldStatNotRedPackets, &v))) {
        *nr_p += (uint64_t)v;
    }
    v = 0;
    if (BCM_SUCCESS(bcm_field_stat_get(HWBCM_UNIT, sid, bcmFieldStatNotRedBytes, &v))) {
        *nr_b += (uint64_t)v;
    }
    v = 0;
    if (BCM_SUCCESS(bcm_field_stat_get(HWBCM_UNIT, sid, bcmFieldStatRedPackets, &v))) {
        *red_p += (uint64_t)v;
    }
}

/* Tear one entry down completely. Errors are logged, not returned: the entry
 * is gone from our books either way. */
static void entry_destroy(bcm_field_entry_t eid, int sid, int policed)
{
    int rv;

    rv = bcm_field_entry_remove(HWBCM_UNIT, eid);
    if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
        CLOGW("bcm_field_entry_remove(%d): %s", (int)eid, bcm_errmsg(rv));
    }
    if (policed) {
        rv = bcm_field_entry_policer_detach(HWBCM_UNIT, eid, 0);
        if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND && rv != BCM_E_EMPTY) {
            CLOGW("bcm_field_entry_policer_detach(%d): %s", (int)eid, bcm_errmsg(rv));
        }
    }
    if (sid >= 0) {
        rv = bcm_field_entry_stat_detach(HWBCM_UNIT, eid, sid);
        if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
            CLOGW("bcm_field_entry_stat_detach(%d, %d): %s", (int)eid, sid, bcm_errmsg(rv));
        }
        rv = bcm_field_stat_destroy(HWBCM_UNIT, sid);
        if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
            CLOGW("bcm_field_stat_destroy(%d): %s", sid, bcm_errmsg(rv));
        }
    }
    rv = bcm_field_entry_destroy(HWBCM_UNIT, eid);
    if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
        CLOGW("bcm_field_entry_destroy(%d): %s", (int)eid, bcm_errmsg(rv));
    }
}

/* Create + install one rule. */
static int entry_build(const hwbcm_copp_trap_cfg_t *cfg, const copp_rule_t *r, int steer, int ridx,
                       bcm_field_entry_t *eid_out, int *sid_out)
{
    bcm_field_entry_t eid = 0;
    int sid = -1;
    int created = 0;
    int policed = 0;
    int rv = 0;

    *sid_out = -1;

    ECHK("entry_create", bcm_field_entry_create(HWBCM_UNIT, g_group, &eid));
    created = 1;
    rv = entry_qualify(eid, cfg, r, ridx);
    if (BCM_FAILURE(rv)) {
        goto fail;
    }
    rv = entry_actions(eid, cfg, steer, ridx, r->to_me);
    if (BCM_FAILURE(rv)) {
        goto fail;
    }
    ECHK("entry_prio_set", bcm_field_entry_prio_set(HWBCM_UNIT, eid, entry_prio(cfg, r)));
    if (cfg->policer_id >= 0) {
        ECHK("policer_attach", bcm_field_entry_policer_attach(HWBCM_UNIT, eid, 0,
                                                              (bcm_policer_t)cfg->policer_id));
        policed = 1;
    }
    if (g_have_stats) {
        bcm_field_stat_t st[4] = {
            bcmFieldStatRedPackets, bcmFieldStatNotRedPackets,
            bcmFieldStatRedBytes, bcmFieldStatNotRedBytes,
        };

        rv = bcm_field_stat_create(HWBCM_UNIT, g_group, 4, st, &sid);
        if (BCM_SUCCESS(rv)) {
            rv = bcm_field_entry_stat_attach(HWBCM_UNIT, eid, sid);
            if (BCM_FAILURE(rv)) {
                (void)bcm_field_stat_destroy(HWBCM_UNIT, sid);
                sid = -1;
            }
        } else {
            sid = -1;
        }
        if (sid < 0) {
            CLOGW("per-entry stats unavailable (%s), CoPP counters disabled", bcm_errmsg(rv));
            g_have_stats = 0;
        }
    }
    rv = bcm_field_entry_install(HWBCM_UNIT, eid);
    if (BCM_FAILURE(rv) && sid >= 0) {
        /* counters are a nice-to-have; the meter is not */
        CLOGW("%s rule %d: install with counters failed (%s), retrying without",
              trap_name(cfg->trap), ridx, bcm_errmsg(rv));
        (void)bcm_field_entry_stat_detach(HWBCM_UNIT, eid, sid);
        (void)bcm_field_stat_destroy(HWBCM_UNIT, sid);
        sid = -1;
        g_have_stats = 0;
        rv = bcm_field_entry_install(HWBCM_UNIT, eid);
    }
    ECHK("entry_install", rv);
    *eid_out = eid;
    *sid_out = sid;
    return 0;
fail:
    if (created) {
        entry_destroy(eid, sid, policed);
    }
    return rv;
}

/* Build every entry of cfg into slot (which holds no entries). */
static int slot_build(copp_slot_t *s, const hwbcm_copp_trap_cfg_t *cfg)
{
    copp_rule_t rules[COPP_MAX_RULES];
    int steer = 0;
    int n, i, rv;

    s->neid = 0;
    n = copp_rules_for(cfg->trap, rules, &steer);
    if (n <= 0) {
        CLOGW("trap kind %d has no classifier", cfg->trap);
        return BCM_E_UNAVAIL;
    }
    if (rules_need_to_me(rules, n) && (!g_have_to_me || g_local_egr < 0)) {
        CLOGI("%s: waiting for the ip2me egress (%s), pending", trap_name(cfg->trap),
              g_have_to_me ? "no ip2me route yet" : "DstL3Egress not in qset");
        return 0;
    }
    for (i = 0; i < n; i++) {
        bcm_field_entry_t eid = 0;
        int sid = -1;

        rv = entry_build(cfg, &rules[i], steer, i, &eid, &sid);
        if (BCM_FAILURE(rv)) {
            /* roll back what this build installed */
            while (s->neid > 0) {
                s->neid--;
                entry_destroy(s->eid[s->neid], s->stat_id[s->neid], cfg->policer_id >= 0);
            }
            return rv;
        }
        s->eid[s->neid] = eid;
        s->stat_id[s->neid] = sid;
        s->neid++;
    }
    CLOGI("%s: %d entr%s, prio %u, queue %d, policer %d (drop g%d y%d r%d), %s%s",
          trap_name(cfg->trap), s->neid, s->neid == 1 ? "y" : "ies", (unsigned)cfg->priority,
          cfg->queue, cfg->policer_id, cfg->drop_green, cfg->drop_yellow, cfg->drop_red,
          cfg->action == HWBCM_COPP_ACT_TRAP ? "trap" : "copy",
          steer ? " (FP copies to CPU)" : "");
    return 0;
}

static void slot_clear_entries(copp_slot_t *s, int policed)
{
    int i;

    for (i = 0; i < s->neid; i++) {
        entry_stats_read(s->eid[i], s->stat_id[i], &s->base_nr_pkts, &s->base_nr_bytes, &s->base_red_pkts);
        entry_destroy(s->eid[i], s->stat_id[i], policed);
    }
    s->neid = 0;
}

static int cfg_check(const hwbcm_copp_trap_cfg_t *cfg)
{
    if (cfg->trap <= HWBCM_COPP_TRAP_NONE || cfg->trap >= HWBCM_COPP_TRAP_COUNT) {
        return BCM_E_PARAM;
    }
    if (cfg->action != HWBCM_COPP_ACT_TRAP && cfg->action != HWBCM_COPP_ACT_COPY) {
        return BCM_E_PARAM;
    }
    if (cfg->queue > g_qmax) {
        CLOGE("%s: CPU queue %d out of range 0..%d", trap_name(cfg->trap), cfg->queue, g_qmax);
        return BCM_E_PARAM;
    }
    if (cfg->policer_id >= 0 && pol_find(cfg->policer_id) == NULL) {
        CLOGE("%s: unknown policer %d", trap_name(cfg->trap), cfg->policer_id);
        return BCM_E_NOT_FOUND;
    }
    return 0;
}

int hwbcm_copp_trap_apply(const hwbcm_copp_trap_cfg_t *cfg, int *handle)
{
    copp_slot_t fresh;
    copp_slot_t *s = NULL;
    int idx = -1;
    int rv;
    int i;

    if (cfg == NULL || handle == NULL) {
        return BCM_E_PARAM;
    }
    pthread_mutex_lock(&g_copp_lock);
    rv = group_ensure();
    if (BCM_FAILURE(rv)) {
        pthread_mutex_unlock(&g_copp_lock);
        return rv;
    }
    rv = cfg_check(cfg);
    if (BCM_FAILURE(rv)) {
        pthread_mutex_unlock(&g_copp_lock);
        return rv;
    }
    if (*handle >= 0 && *handle < COPP_MAX_TRAPS && g_slot[*handle].used) {
        idx = *handle;
    } else {
        for (i = 0; i < COPP_MAX_TRAPS; i++) {
            if (!g_slot[i].used) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            pthread_mutex_unlock(&g_copp_lock);
            CLOGE("trap table full (%d)", COPP_MAX_TRAPS);
            return BCM_E_RESOURCE;
        }
        memset(&g_slot[idx], 0, sizeof(g_slot[idx]));
    }
    s = &g_slot[idx];

    /* make-before-break: new entries first, then the old ones go */
    memset(&fresh, 0, sizeof(fresh));
    rv = slot_build(&fresh, cfg);
    if (BCM_FAILURE(rv)) {
        if (!s->used) {
            /* brand new and nothing installed: keep it as pending so a later
             * refresh can retry, and report the error */
            s->used = 1;
            s->cfg = *cfg;
            s->neid = 0;
            *handle = idx;
        }
        pthread_mutex_unlock(&g_copp_lock);
        return rv;
    }
    if (s->used) {
        slot_clear_entries(s, s->cfg.policer_id >= 0);
    }
    s->used = 1;
    s->cfg = *cfg;
    s->neid = fresh.neid;
    memcpy(s->eid, fresh.eid, sizeof(s->eid));
    memcpy(s->stat_id, fresh.stat_id, sizeof(s->stat_id));
    *handle = idx;
    pthread_mutex_unlock(&g_copp_lock);
    return 0;
}

int hwbcm_copp_trap_remove(int handle)
{
    copp_slot_t *s;

    if (handle < 0 || handle >= COPP_MAX_TRAPS) {
        return BCM_E_PARAM;
    }
    pthread_mutex_lock(&g_copp_lock);
    s = &g_slot[handle];
    if (!s->used) {
        pthread_mutex_unlock(&g_copp_lock);
        return BCM_E_NOT_FOUND;
    }
    slot_clear_entries(s, s->cfg.policer_id >= 0);
    CLOGI("%s: removed (handle %d)", trap_name(s->cfg.trap), handle);
    memset(s, 0, sizeof(*s));
    pthread_mutex_unlock(&g_copp_lock);
    return 0;
}

int hwbcm_copp_trap_entries(int handle)
{
    int n;

    if (handle < 0 || handle >= COPP_MAX_TRAPS) {
        return BCM_E_PARAM;
    }
    pthread_mutex_lock(&g_copp_lock);
    n = g_slot[handle].used ? g_slot[handle].neid : BCM_E_NOT_FOUND;
    pthread_mutex_unlock(&g_copp_lock);
    return n;
}

int hwbcm_copp_trap_stats_get(int handle, uint64_t *not_red_pkts, uint64_t *not_red_bytes,
                              uint64_t *red_pkts)
{
    copp_slot_t *s;
    uint64_t nr_p, nr_b, red_p;
    int i;

    if (handle < 0 || handle >= COPP_MAX_TRAPS) {
        return BCM_E_PARAM;
    }
    pthread_mutex_lock(&g_copp_lock);
    s = &g_slot[handle];
    if (!s->used) {
        pthread_mutex_unlock(&g_copp_lock);
        return BCM_E_NOT_FOUND;
    }
    nr_p = s->base_nr_pkts;
    nr_b = s->base_nr_bytes;
    red_p = s->base_red_pkts;
    for (i = 0; i < s->neid; i++) {
        entry_stats_read(s->eid[i], s->stat_id[i], &nr_p, &nr_b, &red_p);
    }
    pthread_mutex_unlock(&g_copp_lock);
    if (not_red_pkts) {
        *not_red_pkts = nr_p;
    }
    if (not_red_bytes) {
        *not_red_bytes = nr_b;
    }
    if (red_pkts) {
        *red_pkts = red_p;
    }
    return 0;
}

int hwbcm_copp_trap_stats_clear(int handle)
{
    copp_slot_t *s;
    int i;

    if (handle < 0 || handle >= COPP_MAX_TRAPS) {
        return BCM_E_PARAM;
    }
    pthread_mutex_lock(&g_copp_lock);
    s = &g_slot[handle];
    if (!s->used) {
        pthread_mutex_unlock(&g_copp_lock);
        return BCM_E_NOT_FOUND;
    }
    s->base_nr_pkts = 0;
    s->base_nr_bytes = 0;
    s->base_red_pkts = 0;
    for (i = 0; i < s->neid; i++) {
        if (s->stat_id[i] >= 0) {
            (void)bcm_field_stat_set(HWBCM_UNIT, s->stat_id[i], bcmFieldStatRedPackets, 0);
            (void)bcm_field_stat_set(HWBCM_UNIT, s->stat_id[i], bcmFieldStatNotRedPackets, 0);
            (void)bcm_field_stat_set(HWBCM_UNIT, s->stat_id[i], bcmFieldStatRedBytes, 0);
            (void)bcm_field_stat_set(HWBCM_UNIT, s->stat_id[i], bcmFieldStatNotRedBytes, 0);
        }
    }
    pthread_mutex_unlock(&g_copp_lock);
    return 0;
}

/* Re-install every used slot, optionally only those with to-me rules. */
static int reapply_all(int only_to_me)
{
    int i;
    int bad = 0;

    for (i = 0; i < COPP_MAX_TRAPS; i++) {
        copp_slot_t *s = &g_slot[i];
        copp_slot_t fresh;
        copp_rule_t rules[COPP_MAX_RULES];
        int steer = 0;
        int n;
        int rv;

        if (!s->used) {
            continue;
        }
        n = copp_rules_for(s->cfg.trap, rules, &steer);
        if (only_to_me && !rules_need_to_me(rules, n)) {
            continue;
        }
        memset(&fresh, 0, sizeof(fresh));
        rv = slot_build(&fresh, &s->cfg);
        if (BCM_FAILURE(rv)) {
            bad++;
            continue;
        }
        slot_clear_entries(s, s->cfg.policer_id >= 0);
        s->neid = fresh.neid;
        memcpy(s->eid, fresh.eid, sizeof(s->eid));
        memcpy(s->stat_id, fresh.stat_id, sizeof(s->stat_id));
    }
    return bad ? BCM_E_FAIL : 0;
}

int hwbcm_copp_local_egress_set(int egr_id)
{
    int rv;

    pthread_mutex_lock(&g_copp_lock);
    if (egr_id == g_local_egr) {
        pthread_mutex_unlock(&g_copp_lock);
        return 0;
    }
    CLOGI("ip2me egress %d -> %d", g_local_egr, egr_id);
    g_local_egr = egr_id;
    if (!g_group_valid) {
        pthread_mutex_unlock(&g_copp_lock);
        return 0;
    }
    if (egr_id < 0) {
        int i;

        /* forget the to-me entries, keep the slots pending */
        for (i = 0; i < COPP_MAX_TRAPS; i++) {
            copp_rule_t rules[COPP_MAX_RULES];
            int steer = 0;
            int n;

            if (!g_slot[i].used) {
                continue;
            }
            n = copp_rules_for(g_slot[i].cfg.trap, rules, &steer);
            if (rules_need_to_me(rules, n)) {
                slot_clear_entries(&g_slot[i], g_slot[i].cfg.policer_id >= 0);
            }
        }
        pthread_mutex_unlock(&g_copp_lock);
        return 0;
    }
    rv = reapply_all(1);
    pthread_mutex_unlock(&g_copp_lock);
    return rv;
}

int hwbcm_copp_refresh(void)
{
    int rv;

    pthread_mutex_lock(&g_copp_lock);
    if (!g_group_valid) {
        pthread_mutex_unlock(&g_copp_lock);
        return 0;
    }
    rv = reapply_all(0);
    pthread_mutex_unlock(&g_copp_lock);
    return rv;
}

int hwbcm_copp_dump(void)
{
    int i, j;

    pthread_mutex_lock(&g_copp_lock);
    CLOGI("group %d valid %d (to_me %d icmp %d stats %d) ip2me egress %d cpu queues 0..%d",
          (int)g_group, g_group_valid, g_have_to_me, g_have_icmp, g_have_stats, g_local_egr, g_qmax);
    for (i = 0; i < COPP_MAX_POLICERS; i++) {
        if (g_pol[i].used) {
            CLOGI("  policer %d mode %d %s cir %llu cbs %llu pir %llu pbs %llu", (int)g_pol[i].pid,
                  g_pol[i].cfg.mode, g_pol[i].cfg.packets ? "pps" : "Bps",
                  (unsigned long long)g_pol[i].cfg.cir, (unsigned long long)g_pol[i].cfg.cbs,
                  (unsigned long long)g_pol[i].cfg.pir, (unsigned long long)g_pol[i].cfg.pbs);
        }
    }
    for (i = 0; i < COPP_MAX_TRAPS; i++) {
        const copp_slot_t *s = &g_slot[i];
        char eids[COPP_MAX_RULES * 12 + 1];
        size_t off = 0;

        if (!s->used) {
            continue;
        }
        eids[0] = '\0';
        for (j = 0; j < s->neid && off < sizeof(eids); j++) {
            int w = snprintf(eids + off, sizeof(eids) - off, " %d", (int)s->eid[j]);

            if (w < 0) {
                break;
            }
            off += (size_t)w;
        }
        CLOGI("  handle %d %s %s prio %u queue %d policer %d drop g%d y%d r%d entries%s%s", i,
              trap_name(s->cfg.trap), s->cfg.action == HWBCM_COPP_ACT_TRAP ? "trap" : "copy",
              (unsigned)s->cfg.priority, s->cfg.queue, s->cfg.policer_id,
              s->cfg.drop_green, s->cfg.drop_yellow, s->cfg.drop_red,
              eids, s->neid ? "" : " (pending)");
    }
    pthread_mutex_unlock(&g_copp_lock);
    return 0;
}

int hwbcm_copp_deinit(void)
{
    int i;
    int rv;

    pthread_mutex_lock(&g_copp_lock);
    for (i = 0; i < COPP_MAX_TRAPS; i++) {
        if (g_slot[i].used) {
            slot_clear_entries(&g_slot[i], g_slot[i].cfg.policer_id >= 0);
            memset(&g_slot[i], 0, sizeof(g_slot[i]));
        }
    }
    for (i = 0; i < COPP_MAX_POLICERS; i++) {
        if (g_pol[i].used) {
            rv = bcm_policer_destroy(HWBCM_UNIT, g_pol[i].pid);
            if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
                CLOGW("bcm_policer_destroy(%d): %s", (int)g_pol[i].pid, bcm_errmsg(rv));
            }
            memset(&g_pol[i], 0, sizeof(g_pol[i]));
        }
    }
    if (g_group_valid) {
        rv = bcm_field_group_destroy(HWBCM_UNIT, g_group);
        if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
            CLOGW("bcm_field_group_destroy(%d): %s", (int)g_group, bcm_errmsg(rv));
        }
        g_group_valid = 0;
    }
    g_local_egr = -1;
    g_have_stats = 1;
    pthread_mutex_unlock(&g_copp_lock);
    return 0;
}
