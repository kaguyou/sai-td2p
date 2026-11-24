/*
 * hwbcm_acl.c — data-plane ACL (field processor) for Trident2+ (BCM56864),
 * OpenBCM sdk-6.5.27. API: hwbcm_acl.h.
 *
 * TD2+ field processor facts this file relies on (sdk-6.5.27):
 *  - IFP: 12 slices (src/bcm/esw/trident2/field.c:11237-11242), FP_TCAM has
 *    16384 rows on BCM56860_A0 (src/soc/mcm/allmems_f.i:504621-504631); group
 *    priorities are evaluated in parallel, a numerically higher group priority
 *    wins conflicting actions, and each value may be used once per stage
 *    (include/bcm/field.h:6711-6717).
 *  - Group mode Auto tries single, intra-slice double, double (paired slices)
 *    and quad (paired + intra) on TD2-class chips; triple is skipped
 *    (src/bcm/esw/field_common.c:25649-25720). The combined v4/v6 MIRROR table
 *    SONiC uses on Broadcom therefore lands in quad mode (two slices).
 *  - InPorts lives in the IPBM part of FP_GLOBAL_MASK_TCAM and is available in
 *    every IFP key (trident2plus/field.c:172 "handled outside normal qualifier
 *    scheme"), so binding a table to ports costs no key width.
 *  - IFP has no DSCP qualifier, only Tos (trident2plus/field.c ingress section);
 *    ICMP type/code is IcmpTypeCode, overlaid on L4SrcPort
 *    (trident2plus/field.c:1942-1950); TcpControl is 6 bits (:1952).
 *  - EFP: 4 slices (trident2/field.c:11257), EFP_TCAM 1024 rows (allmems_e.i:27837); no InPorts and no
 *    RangeCheck (trident2plus/field.c:5650-6550); actions limited to drop,
 *    DSCP/ECN/VLAN rewrite and egress mirror (src/bcm/esw/trx/field.c:10446).
 *  - VFP: 4 slices (trident2/field.c:11219), no IntPriority/OutPort/ranges.
 *  - IFP counters are per slice (no _FP_STAGE_GLOBAL_COUNTERS on IFP,
 *    trident2/field.c:11229-11236), so stats are never shared between entries.
 *  - L3Switch takes an egress object or an ECMP id (trx/field.c:5570-5600).
 *  - Mirror actions take (0, mirror destination gport) (include/bcm/field.h:7029).
 */
#include "hwbcm.h"
#include "hwbcm_acl.h"

#include <string.h>

#include <bcm/types.h>
#include <bcm/error.h>
#include <bcm/field.h>
#include <bcm/port.h>
#include <bcm/stack.h>

#define U HWBCM_UNIT

/* ------------------------------------------------------------------------ */
/* group bookkeeping (gid -> stage, qset bits) for rule validation           */
/* ------------------------------------------------------------------------ */

#define HWBCM_ACL_MAX_GROUPS 64

typedef struct {
    int used;
    int gid;
    int stage;
    uint32_t quals;
} acl_group_info_t;

static acl_group_info_t s_groups[HWBCM_ACL_MAX_GROUPS];

static acl_group_info_t *group_info(int gid)
{
    int i;

    for (i = 0; i < HWBCM_ACL_MAX_GROUPS; i++) {
        if (s_groups[i].used && s_groups[i].gid == gid) {
            return &s_groups[i];
        }
    }
    return NULL;
}

static acl_group_info_t *group_info_alloc(void)
{
    int i;

    for (i = 0; i < HWBCM_ACL_MAX_GROUPS; i++) {
        if (!s_groups[i].used) {
            memset(&s_groups[i], 0, sizeof(s_groups[i]));
            s_groups[i].used = 1;
            return &s_groups[i];
        }
    }
    return NULL;
}

static int s_modid = -1;

static int my_modid(int *modid)
{
    int rv;

    if (s_modid < 0) {
        int m = 0;

        rv = bcm_stk_my_modid_get(U, &m);
        if (rv < 0) {
            hwbcm_log(HWBCM_LOG_ERR, "acl: bcm_stk_my_modid_get: %s", bcm_errmsg(rv));
            return rv;
        }
        s_modid = m;
    }
    *modid = s_modid;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* rule helpers                                                             */
/* ------------------------------------------------------------------------ */

void hwbcm_acl_rule_init(hwbcm_acl_rule_t *r)
{
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof(*r));
    r->in_port = -1;
    r->out_port = -1;
    r->redirect = -1;
}

void hwbcm_acl_rule_inport_add(hwbcm_acl_rule_t *r, int port)
{
    if (r == NULL || port < 0 || port >= HWBCM_ACL_PBMP_WORDS * 32) {
        return;
    }
    r->in_pbmp[port / 32] |= (1u << (port % 32));
    r->quals |= HWBCM_ACL_Q_IN_PORTS;
}

#define Q_COMMON (HWBCM_ACL_Q_VLAN | HWBCM_ACL_Q_ETHER_TYPE | HWBCM_ACL_Q_IP_PROTO | \
                  HWBCM_ACL_Q_SRC_IP | HWBCM_ACL_Q_DST_IP | HWBCM_ACL_Q_SRC_IP6 | \
                  HWBCM_ACL_Q_DST_IP6 | HWBCM_ACL_Q_L4_SRC | HWBCM_ACL_Q_L4_DST | \
                  HWBCM_ACL_Q_TCP_FLAGS | HWBCM_ACL_Q_TOS | HWBCM_ACL_Q_ICMP | \
                  HWBCM_ACL_Q_IP_TYPE | HWBCM_ACL_Q_SRC_MAC | HWBCM_ACL_Q_DST_MAC | \
                  HWBCM_ACL_Q_TTL | HWBCM_ACL_Q_IP_FRAG)

uint32_t hwbcm_acl_stage_quals(int stage)
{
    switch (stage) {
    case HWBCM_ACL_STAGE_INGRESS:
        return Q_COMMON | HWBCM_ACL_Q_IN_PORTS | HWBCM_ACL_Q_OUT_PORT |
               HWBCM_ACL_Q_TC | HWBCM_ACL_Q_RANGE;
    case HWBCM_ACL_STAGE_EGRESS:
        return Q_COMMON | HWBCM_ACL_Q_IN_PORT | HWBCM_ACL_Q_OUT_PORT | HWBCM_ACL_Q_TC;
    case HWBCM_ACL_STAGE_LOOKUP:
        return Q_COMMON | HWBCM_ACL_Q_IN_PORT;
    default:
        return 0;
    }
}

uint32_t hwbcm_acl_stage_acts(int stage)
{
    switch (stage) {
    case HWBCM_ACL_STAGE_INGRESS:
        return 0xffffffffu;
    case HWBCM_ACL_STAGE_EGRESS:
        /* trx/field.c:10446-10500; CopyToCpu needs an internal loopback port
         * at EFP, not advertised. */
        return HWBCM_ACL_A_DROP | HWBCM_ACL_A_DROP_CANCEL | HWBCM_ACL_A_MIRROR_EGR |
               HWBCM_ACL_A_SET_DSCP | HWBCM_ACL_A_SET_ECN | HWBCM_ACL_A_SET_OUTER_VLAN |
               HWBCM_ACL_A_SET_OUTER_PRI;
    case HWBCM_ACL_STAGE_LOOKUP:
        /* trx/field.c:10677-10700 */
        return HWBCM_ACL_A_DROP | HWBCM_ACL_A_DROP_CANCEL | HWBCM_ACL_A_COPY_CPU |
               HWBCM_ACL_A_COPY_CPU_CANCEL | HWBCM_ACL_A_SET_TC | HWBCM_ACL_A_SET_COLOR |
               HWBCM_ACL_A_SET_OUTER_VLAN;
    default:
        return 0;
    }
}

static const char *stage_name(int stage)
{
    switch (stage) {
    case HWBCM_ACL_STAGE_INGRESS: return "IFP";
    case HWBCM_ACL_STAGE_EGRESS:  return "EFP";
    case HWBCM_ACL_STAGE_LOOKUP:  return "VFP";
    default:                      return "?";
    }
}

/* ------------------------------------------------------------------------ */
/* groups                                                                   */
/* ------------------------------------------------------------------------ */

static void qset_build(int stage, uint32_t q, bcm_field_qset_t *qset)
{
    BCM_FIELD_QSET_INIT(*qset);

    switch (stage) {
    case HWBCM_ACL_STAGE_EGRESS:
        BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyStageEgress);
        break;
    case HWBCM_ACL_STAGE_LOOKUP:
        BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyStageLookup);
        break;
    default:
        BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyStageIngress);
        break;
    }

    if (q & HWBCM_ACL_Q_IN_PORTS)   BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyInPorts);
    if (q & HWBCM_ACL_Q_IN_PORT)    BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyInPort);
    if (q & HWBCM_ACL_Q_OUT_PORT) {
        if (stage == HWBCM_ACL_STAGE_INGRESS) {
            BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyDstPort);
        } else {
            BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyOutPort);
        }
    }
    if (q & HWBCM_ACL_Q_VLAN)       BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyOuterVlanId);
    if (q & HWBCM_ACL_Q_ETHER_TYPE) BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyEtherType);
    if (q & HWBCM_ACL_Q_IP_PROTO)   BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyIpProtocol);
    if (q & HWBCM_ACL_Q_SRC_IP)     BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifySrcIp);
    if (q & HWBCM_ACL_Q_DST_IP)     BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyDstIp);
    if (q & HWBCM_ACL_Q_SRC_IP6)    BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifySrcIp6);
    if (q & HWBCM_ACL_Q_DST_IP6)    BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyDstIp6);
    if (q & HWBCM_ACL_Q_L4_SRC)     BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyL4SrcPort);
    if (q & HWBCM_ACL_Q_L4_DST)     BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyL4DstPort);
    if (q & HWBCM_ACL_Q_TCP_FLAGS)  BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyTcpControl);
    if (q & HWBCM_ACL_Q_TOS)        BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyTos);
    if (q & HWBCM_ACL_Q_ICMP)       BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyIcmpTypeCode);
    if (q & HWBCM_ACL_Q_IP_TYPE)    BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyIpType);
    if (q & HWBCM_ACL_Q_TC)         BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyIntPriority);
    if (q & HWBCM_ACL_Q_RANGE)      BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyRangeCheck);
    if (q & HWBCM_ACL_Q_SRC_MAC)    BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifySrcMac);
    if (q & HWBCM_ACL_Q_DST_MAC)    BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyDstMac);
    if (q & HWBCM_ACL_Q_TTL)        BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyTtl);
    if (q & HWBCM_ACL_Q_IP_FRAG)    BCM_FIELD_QSET_ADD(*qset, bcmFieldQualifyIpFrag);
}

int hwbcm_acl_group_create(int stage, uint32_t quals, int priority, int *gid)
{
    bcm_field_group_config_t cfg;
    acl_group_info_t *gi;
    uint32_t bad;
    int rv;

    if (gid == NULL) {
        return BCM_E_PARAM;
    }

    bad = quals & ~hwbcm_acl_stage_quals(stage);
    if (bad != 0) {
        hwbcm_log(HWBCM_LOG_ERR, "acl: %s cannot match qualifier bits 0x%x", stage_name(stage), bad);
        return BCM_E_UNAVAIL;
    }

    gi = group_info_alloc();
    if (gi == NULL) {
        hwbcm_log(HWBCM_LOG_ERR, "acl: more than %d ACL groups", HWBCM_ACL_MAX_GROUPS);
        return BCM_E_RESOURCE;
    }

    bcm_field_group_config_t_init(&cfg);
    qset_build(stage, quals, &cfg.qset);
    cfg.priority = priority;
    cfg.mode = bcmFieldGroupModeAuto;
    cfg.flags = BCM_FIELD_GROUP_CREATE_WITH_MODE;

    rv = bcm_field_group_config_create(U, &cfg);
    if (rv < 0) {
        hwbcm_log(HWBCM_LOG_ERR, "acl: %s group create (quals 0x%x, prio %d): %s",
                  stage_name(stage), quals, priority, bcm_errmsg(rv));
        gi->used = 0;
        return rv;
    }

    gi->gid = (int)cfg.group;
    gi->stage = stage;
    gi->quals = quals;
    *gid = (int)cfg.group;

    hwbcm_log(HWBCM_LOG_INFO, "acl: %s group %d created (quals 0x%x, prio %d)",
              stage_name(stage), *gid, quals, priority);
    return 0;
}

int hwbcm_acl_group_destroy(int gid)
{
    acl_group_info_t *gi = group_info(gid);
    int rv;

    rv = bcm_field_group_destroy(U, (bcm_field_group_t)gid);
    if (rv < 0 && rv != BCM_E_NOT_FOUND) {
        hwbcm_log(HWBCM_LOG_ERR, "acl: group %d destroy: %s", gid, bcm_errmsg(rv));
        return rv;
    }
    if (gi != NULL) {
        gi->used = 0;
    }
    return 0;
}

int hwbcm_acl_group_status(int gid, int *used, int *free_cnt)
{
    bcm_field_group_status_t st;
    int rv;

    memset(&st, 0, sizeof(st));
    rv = bcm_field_group_status_get(U, (bcm_field_group_t)gid, &st);
    if (rv < 0) {
        return rv;
    }
    if (used != NULL) {
        *used = st.entry_count;
    }
    if (free_cnt != NULL) {
        *free_cnt = st.entries_free;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* qualify / actions                                                        */
/* ------------------------------------------------------------------------ */

#define TRY(what, call)                                                       \
    do {                                                                      \
        int _rv = (call);                                                     \
        if (_rv < 0) {                                                        \
            hwbcm_log(HWBCM_LOG_ERR, "acl: entry %d: %s: %s", eid, what,      \
                      bcm_errmsg(_rv));                                       \
            return _rv;                                                       \
        }                                                                     \
    } while (0)

static int ip_type_of(int t, bcm_field_IpType_t *out)
{
    switch (t) {
    case HWBCM_ACL_IPT_IP:        *out = bcmFieldIpTypeIp; return 0;
    case HWBCM_ACL_IPT_NON_IP:    *out = bcmFieldIpTypeNonIp; return 0;
    case HWBCM_ACL_IPT_IPV4:      *out = bcmFieldIpTypeIpv4Any; return 0;
    case HWBCM_ACL_IPT_NON_IPV4:  *out = bcmFieldIpTypeIpv4Not; return 0;
    case HWBCM_ACL_IPT_IPV6:      *out = bcmFieldIpTypeIpv6; return 0;
    case HWBCM_ACL_IPT_NON_IPV6:  *out = bcmFieldIpTypeIpv6Not; return 0;
    case HWBCM_ACL_IPT_ARP:       *out = bcmFieldIpTypeArp; return 0;
    case HWBCM_ACL_IPT_ARP_REQ:   *out = bcmFieldIpTypeArpRequest; return 0;
    case HWBCM_ACL_IPT_ARP_REPLY: *out = bcmFieldIpTypeArpReply; return 0;
    default:                      return BCM_E_PARAM;
    }
}

static int ip_frag_of(int f, bcm_field_IpFrag_t *out)
{
    switch (f) {
    case HWBCM_ACL_FRAG_ANY:         *out = bcmFieldIpFragAny; return 0;
    case HWBCM_ACL_FRAG_NON:         *out = bcmFieldIpFragNon; return 0;
    case HWBCM_ACL_FRAG_NON_OR_HEAD: *out = bcmFieldIpFragNonOrFirst; return 0;
    case HWBCM_ACL_FRAG_HEAD:        *out = bcmFieldIpFragFirst; return 0;
    case HWBCM_ACL_FRAG_NON_HEAD:    *out = bcmFieldIpFragNotFirst; return 0;
    default:                         return BCM_E_PARAM;
    }
}

static int apply_quals(int eid, int stage, const hwbcm_acl_rule_t *r)
{
    bcm_field_entry_t e = (bcm_field_entry_t)eid;
    uint32_t q = r->quals;
    int modid = 0;
    int i;

    if (q & HWBCM_ACL_Q_IN_PORTS) {
        bcm_pbmp_t data, mask;
        bcm_port_config_t pcfg;
        int p;

        BCM_PBMP_CLEAR(data);
        for (p = 0; p < HWBCM_ACL_PBMP_WORDS * 32; p++) {
            if (r->in_pbmp[p / 32] & (1u << (p % 32))) {
                BCM_PBMP_PORT_ADD(data, p);
            }
        }
        /* mask = every port of the unit (CPU included), so a packet from a
         * port outside data never matches */
        bcm_port_config_t_init(&pcfg);
        TRY("port_config_get", bcm_port_config_get(U, &pcfg));
        BCM_PBMP_ASSIGN(mask, pcfg.all);
        TRY("InPorts", bcm_field_qualify_InPorts(U, e, data, mask));
    }
    if (q & HWBCM_ACL_Q_IN_PORT) {
        TRY("InPort", bcm_field_qualify_InPort(U, e, r->in_port, BCM_FIELD_EXACT_MATCH_MASK));
    }
    if (q & HWBCM_ACL_Q_OUT_PORT) {
        if (stage == HWBCM_ACL_STAGE_INGRESS) {
            TRY("modid", my_modid(&modid));
            TRY("DstPort", bcm_field_qualify_DstPort(U, e, modid, BCM_FIELD_EXACT_MATCH_MASK,
                                                     r->out_port, BCM_FIELD_EXACT_MATCH_MASK));
        } else {
            TRY("OutPort", bcm_field_qualify_OutPort(U, e, r->out_port, BCM_FIELD_EXACT_MATCH_MASK));
        }
    }
    if (q & HWBCM_ACL_Q_VLAN) {
        TRY("OuterVlanId", bcm_field_qualify_OuterVlanId(U, e, r->vlan, r->vlan_mask));
    }
    if (q & HWBCM_ACL_Q_ETHER_TYPE) {
        TRY("EtherType", bcm_field_qualify_EtherType(U, e, r->ether_type, r->ether_type_mask));
    }
    if (q & HWBCM_ACL_Q_IP_PROTO) {
        TRY("IpProtocol", bcm_field_qualify_IpProtocol(U, e, r->ip_proto, r->ip_proto_mask));
    }
    if (q & HWBCM_ACL_Q_SRC_IP) {
        TRY("SrcIp", bcm_field_qualify_SrcIp(U, e, (bcm_ip_t)r->src_ip, (bcm_ip_t)r->src_ip_mask));
    }
    if (q & HWBCM_ACL_Q_DST_IP) {
        TRY("DstIp", bcm_field_qualify_DstIp(U, e, (bcm_ip_t)r->dst_ip, (bcm_ip_t)r->dst_ip_mask));
    }
    if (q & HWBCM_ACL_Q_SRC_IP6) {
        bcm_ip6_t d, m;
        memcpy(d, r->src_ip6, 16);
        memcpy(m, r->src_ip6_mask, 16);
        TRY("SrcIp6", bcm_field_qualify_SrcIp6(U, e, d, m));
    }
    if (q & HWBCM_ACL_Q_DST_IP6) {
        bcm_ip6_t d, m;
        memcpy(d, r->dst_ip6, 16);
        memcpy(m, r->dst_ip6_mask, 16);
        TRY("DstIp6", bcm_field_qualify_DstIp6(U, e, d, m));
    }
    if (q & HWBCM_ACL_Q_L4_SRC) {
        TRY("L4SrcPort", bcm_field_qualify_L4SrcPort(U, e, r->l4_src, r->l4_src_mask));
    }
    if (q & HWBCM_ACL_Q_L4_DST) {
        TRY("L4DstPort", bcm_field_qualify_L4DstPort(U, e, r->l4_dst, r->l4_dst_mask));
    }
    if (q & HWBCM_ACL_Q_TCP_FLAGS) {
        TRY("TcpControl", bcm_field_qualify_TcpControl(U, e, (uint8)(r->tcp_flags & 0x3f),
                                                       (uint8)(r->tcp_flags_mask & 0x3f)));
    }
    if (q & HWBCM_ACL_Q_TOS) {
        TRY("Tos", bcm_field_qualify_Tos(U, e, r->tos, r->tos_mask));
    }
    if (q & HWBCM_ACL_Q_ICMP) {
        TRY("IcmpTypeCode", bcm_field_qualify_IcmpTypeCode(U, e, r->icmp, r->icmp_mask));
    }
    if (q & HWBCM_ACL_Q_IP_TYPE) {
        bcm_field_IpType_t t;
        TRY("IpType value", ip_type_of(r->ip_type, &t));
        TRY("IpType", bcm_field_qualify_IpType(U, e, t));
    }
    if (q & HWBCM_ACL_Q_TC) {
        TRY("IntPriority", bcm_field_qualify_IntPriority(U, e, r->tc, r->tc_mask));
    }
    if (q & HWBCM_ACL_Q_RANGE) {
        for (i = 0; i < r->nranges && i < HWBCM_ACL_MAX_RANGES; i++) {
            TRY("RangeCheck", bcm_field_qualify_RangeCheck(U, e, (bcm_field_range_t)r->ranges[i], 0));
        }
    }
    if (q & HWBCM_ACL_Q_SRC_MAC) {
        bcm_mac_t d, m;
        memcpy(d, r->src_mac, 6);
        memcpy(m, r->src_mac_mask, 6);
        TRY("SrcMac", bcm_field_qualify_SrcMac(U, e, d, m));
    }
    if (q & HWBCM_ACL_Q_DST_MAC) {
        bcm_mac_t d, m;
        memcpy(d, r->dst_mac, 6);
        memcpy(m, r->dst_mac_mask, 6);
        TRY("DstMac", bcm_field_qualify_DstMac(U, e, d, m));
    }
    if (q & HWBCM_ACL_Q_TTL) {
        TRY("Ttl", bcm_field_qualify_Ttl(U, e, r->ttl, r->ttl_mask));
    }
    if (q & HWBCM_ACL_Q_IP_FRAG) {
        bcm_field_IpFrag_t f;
        TRY("IpFrag value", ip_frag_of(r->ip_frag, &f));
        TRY("IpFrag", bcm_field_qualify_IpFrag(U, e, f));
    }
    return 0;
}

static int apply_actions(int eid, const hwbcm_acl_rule_t *r)
{
    bcm_field_entry_t e = (bcm_field_entry_t)eid;
    uint32_t a = r->acts;
    int modid = 0;
    int i;

    if (a & HWBCM_ACL_A_DROP) {
        TRY("Drop", bcm_field_action_add(U, e, bcmFieldActionDrop, 0, 0));
    }
    if (a & HWBCM_ACL_A_DROP_CANCEL) {
        TRY("DropCancel", bcm_field_action_add(U, e, bcmFieldActionDropCancel, 0, 0));
    }
    if (a & HWBCM_ACL_A_COPY_CPU) {
        TRY("CopyToCpu", bcm_field_action_add(U, e, bcmFieldActionCopyToCpu, 0, 0));
    }
    if (a & HWBCM_ACL_A_COPY_CPU_CANCEL) {
        TRY("CopyToCpuCancel", bcm_field_action_add(U, e, bcmFieldActionCopyToCpuCancel, 0, 0));
    }
    if (a & HWBCM_ACL_A_REDIRECT_PORT) {
        TRY("modid", my_modid(&modid));
        TRY("RedirectPort", bcm_field_action_add(U, e, bcmFieldActionRedirectPort,
                                                 (uint32)modid, (uint32)r->redirect));
    }
    if (a & HWBCM_ACL_A_REDIRECT_TRUNK) {
        TRY("RedirectTrunk", bcm_field_action_add(U, e, bcmFieldActionRedirectTrunk,
                                                  (uint32)r->redirect, 0));
    }
    if (a & HWBCM_ACL_A_REDIRECT_L3) {
        TRY("L3Switch", bcm_field_action_add(U, e, bcmFieldActionL3Switch, (uint32)r->redirect, 0));
    }
    if (a & HWBCM_ACL_A_MIRROR_ING) {
        for (i = 0; i < r->n_mirror_ing && i < HWBCM_ACL_MAX_MIRRORS; i++) {
            TRY("MirrorIngress", bcm_field_action_add(U, e, bcmFieldActionMirrorIngress,
                                                      0, (uint32)r->mirror_ing[i]));
        }
    }
    if (a & HWBCM_ACL_A_MIRROR_EGR) {
        for (i = 0; i < r->n_mirror_egr && i < HWBCM_ACL_MAX_MIRRORS; i++) {
            TRY("MirrorEgress", bcm_field_action_add(U, e, bcmFieldActionMirrorEgress,
                                                     0, (uint32)r->mirror_egr[i]));
        }
    }
    if (a & HWBCM_ACL_A_SET_TC) {
        TRY("PrioIntNew", bcm_field_action_add(U, e, bcmFieldActionPrioIntNew, r->set_tc, 0));
    }
    if (a & HWBCM_ACL_A_SET_DSCP) {
        TRY("DscpNew", bcm_field_action_add(U, e, bcmFieldActionDscpNew, r->set_dscp, 0));
    }
    if (a & HWBCM_ACL_A_SET_ECN) {
        TRY("EcnNew", bcm_field_action_add(U, e, bcmFieldActionEcnNew, r->set_ecn, 0));
    }
    if (a & HWBCM_ACL_A_SET_COLOR) {
        uint32 c = BCM_FIELD_COLOR_GREEN;
        if (r->set_color == HWBCM_ACL_COLOR_YELLOW) {
            c = BCM_FIELD_COLOR_YELLOW;
        } else if (r->set_color == HWBCM_ACL_COLOR_RED) {
            c = BCM_FIELD_COLOR_RED;
        }
        TRY("DropPrecedence", bcm_field_action_add(U, e, bcmFieldActionDropPrecedence, c, 0));
    }
    if (a & HWBCM_ACL_A_SET_OUTER_VLAN) {
        TRY("OuterVlanNew", bcm_field_action_add(U, e, bcmFieldActionOuterVlanNew, r->set_outer_vlan, 0));
    }
    if (a & HWBCM_ACL_A_SET_OUTER_PRI) {
        TRY("OuterVlanPrioNew", bcm_field_action_add(U, e, bcmFieldActionOuterVlanPrioNew, r->set_outer_pri, 0));
    }
    return 0;
}

static int rule_check(const acl_group_info_t *gi, const hwbcm_acl_rule_t *r)
{
    uint32_t badq = r->quals & ~gi->quals;
    uint32_t bada = r->acts & ~hwbcm_acl_stage_acts(gi->stage);

    if (badq != 0) {
        hwbcm_log(HWBCM_LOG_ERR, "acl: group %d (%s): rule uses qualifiers 0x%x outside the table qset 0x%x",
                  gi->gid, stage_name(gi->stage), badq, gi->quals);
        return BCM_E_PARAM;
    }
    if (bada != 0) {
        hwbcm_log(HWBCM_LOG_ERR, "acl: group %d (%s): actions 0x%x not available in this stage",
                  gi->gid, stage_name(gi->stage), bada);
        return BCM_E_UNAVAIL;
    }
    if ((r->quals & HWBCM_ACL_Q_RANGE) && r->nranges > HWBCM_ACL_MAX_RANGES) {
        return BCM_E_PARAM;
    }
    return 0;
}

static int program(int eid, const acl_group_info_t *gi, const hwbcm_acl_rule_t *r)
{
    int rv = apply_quals(eid, gi->stage, r);

    if (rv < 0) {
        return rv;
    }
    return apply_actions(eid, r);
}

/* ------------------------------------------------------------------------ */
/* entries                                                                  */
/* ------------------------------------------------------------------------ */

int hwbcm_acl_entry_add(int gid, int prio, const hwbcm_acl_rule_t *r, int with_stat,
                        int *eid_out, int *stat_out)
{
    acl_group_info_t *gi = group_info(gid);
    bcm_field_entry_t e = 0;
    int stat_id = -1;
    int eid;
    int rv;

    if (r == NULL || eid_out == NULL || stat_out == NULL) {
        return BCM_E_PARAM;
    }
    if (gi == NULL) {
        hwbcm_log(HWBCM_LOG_ERR, "acl: entry add: unknown group %d", gid);
        return BCM_E_NOT_FOUND;
    }
    rv = rule_check(gi, r);
    if (rv < 0) {
        return rv;
    }

    rv = bcm_field_entry_create(U, (bcm_field_group_t)gid, &e);
    if (rv < 0) {
        hwbcm_log(HWBCM_LOG_ERR, "acl: entry create in group %d: %s", gid, bcm_errmsg(rv));
        return rv;
    }
    eid = (int)e;

    rv = bcm_field_entry_prio_set(U, e, prio);
    if (rv < 0) {
        hwbcm_log(HWBCM_LOG_ERR, "acl: entry %d prio %d: %s", eid, prio, bcm_errmsg(rv));
        goto fail;
    }

    rv = program(eid, gi, r);
    if (rv < 0) {
        goto fail;
    }

    if (with_stat) {
        bcm_field_stat_t types[2] = { bcmFieldStatPackets, bcmFieldStatBytes };

        rv = bcm_field_stat_create(U, (bcm_field_group_t)gid, 2, types, &stat_id);
        if (rv < 0) {
            hwbcm_log(HWBCM_LOG_ERR, "acl: entry %d stat create: %s", eid, bcm_errmsg(rv));
            stat_id = -1;
            goto fail;
        }
        rv = bcm_field_entry_stat_attach(U, e, stat_id);
        if (rv < 0) {
            hwbcm_log(HWBCM_LOG_ERR, "acl: entry %d stat %d attach: %s", eid, stat_id, bcm_errmsg(rv));
            goto fail;
        }
    }

    rv = bcm_field_entry_install(U, e);
    if (rv < 0) {
        hwbcm_log(HWBCM_LOG_ERR, "acl: entry %d install (group %d): %s", eid, gid, bcm_errmsg(rv));
        goto fail;
    }

    *eid_out = eid;
    *stat_out = stat_id;

    hwbcm_log(HWBCM_LOG_DEBUG, "acl: entry %d in group %d prio %d quals 0x%x acts 0x%x stat %d",
              eid, gid, prio, r->quals, r->acts, stat_id);
    return 0;

fail:
    if (stat_id >= 0) {
        (void)bcm_field_entry_stat_detach(U, e, stat_id);
        (void)bcm_field_stat_destroy(U, stat_id);
    }
    (void)bcm_field_entry_destroy(U, e);
    return rv;
}

int hwbcm_acl_entry_update(int gid, int eid, int prio, const hwbcm_acl_rule_t *r)
{
    bcm_field_entry_t e = (bcm_field_entry_t)eid;
    acl_group_info_t *gi;
    int rv;

    if (r == NULL) {
        return BCM_E_PARAM;
    }
    gi = group_info(gid);
    if (gi == NULL) {
        hwbcm_log(HWBCM_LOG_ERR, "acl: entry %d update: unknown group %d", eid, gid);
        return BCM_E_NOT_FOUND;
    }
    rv = rule_check(gi, r);
    if (rv < 0) {
        return rv;
    }

    /* From here the software copy is rewritten; the installed TCAM row keeps
     * the old rule until bcm_field_entry_reinstall. */
    rv = bcm_field_qualify_clear(U, e);
    if (rv < 0) {
        hwbcm_log(HWBCM_LOG_ERR, "acl: entry %d qualify_clear: %s", eid, bcm_errmsg(rv));
        goto broken;
    }
    rv = bcm_field_action_remove_all(U, e);
    if (rv < 0 && rv != BCM_E_NOT_FOUND) {
        hwbcm_log(HWBCM_LOG_ERR, "acl: entry %d action_remove_all: %s", eid, bcm_errmsg(rv));
        goto broken;
    }
    rv = program(eid, gi, r);
    if (rv < 0) {
        goto broken;
    }
    {
        int cur = prio;

        /* prio_set moves an installed entry in the TCAM; skip it when unchanged */
        if (bcm_field_entry_prio_get(U, e, &cur) < 0 || cur != prio) {
            rv = bcm_field_entry_prio_set(U, e, prio);
            if (rv < 0) {
                hwbcm_log(HWBCM_LOG_ERR, "acl: entry %d prio %d: %s", eid, prio, bcm_errmsg(rv));
                goto broken;
            }
        }
    }
    rv = bcm_field_entry_reinstall(U, e);
    if (rv < 0) {
        hwbcm_log(HWBCM_LOG_ERR, "acl: entry %d reinstall: %s", eid, bcm_errmsg(rv));
        goto broken;
    }
    return 0;

broken:
    /* never leave a half-programmed entry installed (a cleared qualifier set
     * matches everything) */
    (void)bcm_field_entry_remove(U, e);
    return rv;
}

int hwbcm_acl_entry_delete(int eid, int stat_id)
{
    bcm_field_entry_t e = (bcm_field_entry_t)eid;
    int rv;

    rv = bcm_field_entry_remove(U, e);
    if (rv < 0 && rv != BCM_E_NOT_FOUND) {
        hwbcm_log(HWBCM_LOG_WARN, "acl: entry %d remove: %s", eid, bcm_errmsg(rv));
    }
    if (stat_id >= 0) {
        rv = bcm_field_entry_stat_detach(U, e, stat_id);
        if (rv < 0 && rv != BCM_E_NOT_FOUND) {
            hwbcm_log(HWBCM_LOG_WARN, "acl: entry %d stat %d detach: %s", eid, stat_id, bcm_errmsg(rv));
        }
        rv = bcm_field_stat_destroy(U, stat_id);
        if (rv < 0 && rv != BCM_E_NOT_FOUND) {
            hwbcm_log(HWBCM_LOG_WARN, "acl: stat %d destroy: %s", stat_id, bcm_errmsg(rv));
        }
    }
    rv = bcm_field_entry_destroy(U, e);
    if (rv < 0 && rv != BCM_E_NOT_FOUND) {
        hwbcm_log(HWBCM_LOG_ERR, "acl: entry %d destroy: %s", eid, bcm_errmsg(rv));
        return rv;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* counters                                                                 */
/* ------------------------------------------------------------------------ */

int hwbcm_acl_stat_get(int stat_id, uint64_t *packets, uint64_t *bytes)
{
    uint64 v;
    int rv;

    if (packets != NULL) {
        *packets = 0;
        COMPILER_64_ZERO(v);
        rv = bcm_field_stat_get(U, stat_id, bcmFieldStatPackets, &v);
        if (rv < 0 && rv != BCM_E_NOT_FOUND && rv != BCM_E_UNAVAIL) {
            return rv;
        }
        if (rv >= 0) {
            *packets = (uint64_t)v;
        }
    }
    if (bytes != NULL) {
        *bytes = 0;
        COMPILER_64_ZERO(v);
        rv = bcm_field_stat_get(U, stat_id, bcmFieldStatBytes, &v);
        if (rv < 0 && rv != BCM_E_NOT_FOUND && rv != BCM_E_UNAVAIL) {
            return rv;
        }
        if (rv >= 0) {
            *bytes = (uint64_t)v;
        }
    }
    return 0;
}

int hwbcm_acl_stat_set(int stat_id, uint64_t packets, uint64_t bytes)
{
    uint64 v;
    int rv;

    v = (uint64)packets;
    rv = bcm_field_stat_set(U, stat_id, bcmFieldStatPackets, v);
    if (rv < 0 && rv != BCM_E_UNAVAIL) {
        hwbcm_log(HWBCM_LOG_ERR, "acl: stat %d set packets: %s", stat_id, bcm_errmsg(rv));
        return rv;
    }
    v = (uint64)bytes;
    rv = bcm_field_stat_set(U, stat_id, bcmFieldStatBytes, v);
    if (rv < 0 && rv != BCM_E_UNAVAIL) {
        hwbcm_log(HWBCM_LOG_ERR, "acl: stat %d set bytes: %s", stat_id, bcm_errmsg(rv));
        return rv;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* ranges                                                                   */
/* ------------------------------------------------------------------------ */

int hwbcm_acl_range_create(int dst, uint16_t min, uint16_t max, int *range_id)
{
    bcm_field_range_t r = 0;
    int rv;

    if (range_id == NULL || min > max) {
        return BCM_E_PARAM;
    }
    rv = bcm_field_range_create(U, &r, dst ? BCM_FIELD_RANGE_DSTPORT : BCM_FIELD_RANGE_SRCPORT,
                                (bcm_l4_port_t)min, (bcm_l4_port_t)max);
    if (rv < 0) {
        hwbcm_log(HWBCM_LOG_ERR, "acl: range %s %u-%u: %s", dst ? "dst" : "src",
                  (unsigned)min, (unsigned)max, bcm_errmsg(rv));
        return rv;
    }
    *range_id = (int)r;
    return 0;
}

int hwbcm_acl_range_destroy(int range_id)
{
    int rv = bcm_field_range_destroy(U, (bcm_field_range_t)range_id);

    if (rv < 0 && rv != BCM_E_NOT_FOUND) {
        hwbcm_log(HWBCM_LOG_ERR, "acl: range %d destroy: %s", range_id, bcm_errmsg(rv));
        return rv;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* debug                                                                    */
/* ------------------------------------------------------------------------ */

int hwbcm_acl_dump_group(int gid)
{
#if defined(BROADCOM_DEBUG)
    return bcm_field_group_dump(U, (bcm_field_group_t)gid);
#else
    (void)gid;
    return BCM_E_UNAVAIL;
#endif
}

int hwbcm_acl_dump_entry(int eid)
{
#if defined(BROADCOM_DEBUG)
    return bcm_field_entry_dump(U, (bcm_field_entry_t)eid);
#else
    (void)eid;
    return BCM_E_UNAVAIL;
#endif
}

/* ------------------------------------------------------------------------ */
/* warm boot (Phase 3)                                                      */
/* ------------------------------------------------------------------------ */

void hwbcm_acl_warm_reset(void)
{
    memset(s_groups, 0, sizeof(s_groups));
    s_modid = -1;
}

int hwbcm_acl_group_restore(int gid, int stage, uint32_t quals)
{
    acl_group_info_t *gi;
    bcm_field_group_status_t st;
    int rv;

    gi = group_info(gid);
    if (gi == NULL) {
        gi = group_info_alloc();
        if (gi == NULL) {
            hwbcm_log(HWBCM_LOG_ERR, "wb acl: more than %d ACL groups", HWBCM_ACL_MAX_GROUPS);
            return BCM_E_RESOURCE;
        }
    }
    gi->gid   = gid;
    gi->stage = stage;
    gi->quals = quals;

    memset(&st, 0, sizeof(st));
    rv = bcm_field_group_status_get(U, (bcm_field_group_t)gid, &st);
    if (rv < 0) {
        hwbcm_log(HWBCM_LOG_WARN, "wb acl: restored %s group %d not found in SDK (%s)",
                  stage_name(stage), gid, bcm_errmsg(rv));
    } else {
        hwbcm_log(HWBCM_LOG_DEBUG, "wb acl: restored %s group %d (quals=0x%x, entries=%d, free=%d)",
                  stage_name(stage), gid, quals, st.entry_count, st.entries_free);
    }
    return 0;
}
