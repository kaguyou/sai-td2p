/*
 * hwbcm_vxlan.c — VXLAN / EVPN section of hwbcm.h for Trident2+ (BCM56864)
 * on OpenBCM sdk-6.5.27, using the legacy bcm_vxlan_* API (include/bcm/vxlan.h).
 *
 * Model (mirrors src/examples/xgs/firebolt6/Vxlan/LegacyAPI/acc_to_net_uc.c and
 * the checks in src/bcm/esw/trident2/vxlan.c):
 *
 *   VPN  (one per VNI)   bcm_vxlan_vpn_create: ELAN | WITH_VPNID |
 *                        SERVICE_TAGGED | SERVICE_VLAN_DELETE (the access-side
 *                        802.1Q tag is stripped before encapsulation). One
 *                        BCM_MULTICAST_TYPE_VXLAN group is used for bcast /
 *                        unknown-ucast / unknown-mcast flooding.
 *   Tunnel (per remote)  bcm_vxlan_tunnel_initiator_create (sip=local VTEP,
 *                        dip=remote VTEP) + bcm_vxlan_tunnel_terminator_create
 *                        WITH_ID on the initiator id (sip=remote, dip=local).
 *   Network VP (per      bcm_vxlan_port_add: NETWORK | EGRESS_TUNNEL |
 *   remote VTEP)         SERVICE_TAGGED, criteria MATCH_VN_ID, egress_if = an
 *                        L3 egress created with BCM_L3_VXLAN_ONLY (mandatory,
 *                        see _bcm_td2_vxlan_port_nh_add -> BCM_E_CONFIG).
 *   Access VP (per       bcm_vxlan_port_add: SERVICE_TAGGED [| SERVICE_VLAN_ADD],
 *   local port+VLAN)     criteria MATCH_PORT_VLAN, egress_if = a VXLAN-only
 *                        egress on the access port (dummy MAC, dedicated
 *                        L3 intf per VLAN), exactly like the SDK example.
 *   Flooding             bcm_multicast_vxlan_encap_get + bcm_multicast_egress_add
 *                        of every VP (access and network) into the VPN group:
 *                        head-end replication, no underlay multicast.
 *   Remote MAC           bcm_l2_addr_add with vid = VPN id, port = VP gport.
 *
 * TD2+ specifics found in the SDK:
 *   - soc_feature_vrf_aware_vxlan_termination is on for BCM56864: the DIP/SIP/
 *     VN_ID hash keys carry an OVID field. We leave it 0 everywhere (the SDK
 *     default); per-VRF termination must stay disabled in hardware. Verify on
 *     the box after the first tunnel comes up ("vxlan" / "dump chg VLAN_XLATE").
 *   - bcm_vxlan_vpn_destroy() runs bcm_vxlan_port_delete_all() on the VPN, so a
 *     network VP that was created inside that VPN disappears with it. The C++
 *     side tears the network VP down and recreates it in another VPN first.
 *   - bcm_vxlan_init() is already run by "init bcm" (soc_feature_vxlan); calling
 *     it again wipes all VXLAN state, so hwbcm_vxlan_init() calls it exactly
 *     once per process.
 *
 * config.bcm properties (not settable at runtime, documented for the deployer):
 *   use_all_splithorizon_groups=1    (optional) all 8 split-horizon groups
 *   riot_enable=1                    only for L3 VNI (RIOT); not needed for L2 VNI
 *   riot_overlay_l3_intf_mem_size=4096, riot_overlay_l3_egress_mem_size=8192,
 *   riot_overlay_ecmp_resilient_hash_size=16384   (with riot_enable=1 only)
 *   l2_mem_entries / l3_mem_entries  as already present in td2-as5812t config
 *   vxlan_udp_dest_port (spn_VXLAN_UDP_DEST_PORT) is NOT used by the TD2 code
 *   path; the UDP port is set at runtime with bcmSwitchVxlanUdpDestPortSet.
 *
 * All functions return 0 or a negative BCM_E_* code; every bcm call is checked
 * and logged with hwbcm_log().
 */
#include "hwbcm.h"

#include <string.h>

#include <bcm/types.h>
#include <bcm/error.h>
#include <bcm/switch.h>
#include <bcm/port.h>
#include <bcm/vlan.h>
#include <bcm/l2.h>
#include <bcm/l3.h>
#include <bcm/tunnel.h>
#include <bcm/multicast.h>
#include <bcm/vxlan.h>

#define U HWBCM_UNIT

/* Default VXLAN UDP destination port (IANA). */
#define HWBCM_VXLAN_UDP_PORT_DEFAULT 4789
/* Entropy: hash the inner flow into the outer UDP source port (ECMP friendly). */
#define HWBCM_VXLAN_ENTROPY 1
/* Copy unknown-tunnel / unknown-VNI packets to the CPU: off in production. */
#define HWBCM_VXLAN_MISS_TO_CPU 0
/* Service TPID used for SD-tag mode on access/network VPs. */
#define HWBCM_VXLAN_TPID 0x8100

/* Per-VP bookkeeping (indexed by VP number; TD2+ has 8K VPs). */
#define HWBCM_VXLAN_MAX_VP 16384

typedef struct {
    int in_use;
    int is_network;
    int port_gport;     /* physical gport (or trunk gport) the VP sits on */
    int egr_id;         /* VXLAN-only egress owned by this VP (access only) */
    int intf_vid;       /* access: VLAN whose shared L3 intf we reference, else -1 */
    int vpn_id;         /* VPN the VP was created in */
} hwbcm_vxlan_vp_t;

static hwbcm_vxlan_vp_t s_vp[HWBCM_VXLAN_MAX_VP];

/* Shared dummy L3 interface per access VLAN (for access-VP egress objects). */
typedef struct {
    int intf_id;
    int refcnt;
} hwbcm_vxlan_acc_intf_t;

static hwbcm_vxlan_acc_intf_t s_acc_intf[BCM_VLAN_MAX + 1];

static int s_vxlan_inited = 0;
static int s_udp_port = HWBCM_VXLAN_UDP_PORT_DEFAULT;

/* Dummy MAC for access-side egress objects (never appears on the wire). */
static const bcm_mac_t s_acc_dummy_mac = { 0x00, 0x00, 0x01, 0x00, 0x00, 0x01 };

/* ------------------------------------------------------------------------ */
/* helpers                                                                  */
/* ------------------------------------------------------------------------ */

#define HWBCM_CHECK(rv, what) \
    do { \
        if (BCM_FAILURE(rv)) { \
            hwbcm_log(HWBCM_LOG_ERR, "hwbcm_vxlan: %s failed: %s (%d)", (what), bcm_errmsg(rv), (rv)); \
            return (rv); \
        } \
    } while (0)

static bcm_ip_t hwbcm_ip4_of(const uint8_t ip[4])
{
    return ((bcm_ip_t)ip[0] << 24) | ((bcm_ip_t)ip[1] << 16) | ((bcm_ip_t)ip[2] << 8) | (bcm_ip_t)ip[3];
}

static int hwbcm_vxlan_vp_index(int vp_gport)
{
    int vp;

    if (!BCM_GPORT_IS_VXLAN_PORT(vp_gport)) {
        return -1;
    }
    vp = BCM_GPORT_VXLAN_PORT_ID_GET(vp_gport);
    if (vp < 0 || vp >= HWBCM_VXLAN_MAX_VP) {
        return -1;
    }
    return vp;
}

static bcm_vpn_t hwbcm_vxlan_vpn_arg(int vpn_id)
{
    return (vpn_id < 0) ? (bcm_vpn_t)BCM_VXLAN_VPN_INVALID : (bcm_vpn_t)vpn_id;
}

/* Build the gport for a port or trunk. */
static int hwbcm_vxlan_gport_of(int port, int trunk_id, bcm_gport_t *gport)
{
    int rv;

    if (trunk_id >= 0) {
        BCM_GPORT_TRUNK_SET(*gport, trunk_id);
        return BCM_E_NONE;
    }
    if (port < 0) {
        return BCM_E_PARAM;
    }
    rv = bcm_port_gport_get(U, port, gport);
    HWBCM_CHECK(rv, "bcm_port_gport_get");
    return BCM_E_NONE;
}

/* Local port number of a gport, -1 if not a local/modport. */
static int hwbcm_vxlan_port_of_gport(bcm_gport_t gport)
{
    if (BCM_GPORT_IS_MODPORT(gport)) {
        return BCM_GPORT_MODPORT_PORT_GET(gport);
    }
    if (BCM_GPORT_IS_LOCAL(gport)) {
        return BCM_GPORT_LOCAL_GET(gport);
    }
    if (BCM_GPORT_IS_TRUNK(gport)) {
        return -1;
    }
    /* Plain port number. */
    if (gport >= 0 && gport < HWBCM_MAX_PORTS) {
        return gport;
    }
    return -1;
}

/* ------------------------------------------------------------------------ */
/* init                                                                     */
/* ------------------------------------------------------------------------ */

int hwbcm_vxlan_init(int udp_dst_port)
{
    int rv;

    if (udp_dst_port <= 0 || udp_dst_port > 65535) {
        udp_dst_port = HWBCM_VXLAN_UDP_PORT_DEFAULT;
    }

    if (s_vxlan_inited) {
        if (udp_dst_port != s_udp_port) {
            hwbcm_log(HWBCM_LOG_WARN, "hwbcm_vxlan: already initialized with UDP port %d, ignoring %d",
                      s_udp_port, udp_dst_port);
        }
        return BCM_E_NONE;
    }

    memset(s_vp, 0, sizeof(s_vp));
    memset(s_acc_intf, 0, sizeof(s_acc_intf));

    /* bcm_vxlan_port_add() refuses to work without L3 egress mode. */
    rv = bcm_switch_control_set(U, bcmSwitchL3EgressMode, 1);
    HWBCM_CHECK(rv, "bcm_switch_control_set(bcmSwitchL3EgressMode)");

    /* "init bcm" already ran bcm_vxlan_init(); running it again resets the
     * module. We do it once here so a stale state from a previous SAI
     * instance is cleaned, and never again (guarded by s_vxlan_inited). */
    rv = bcm_vxlan_init(U);
    HWBCM_CHECK(rv, "bcm_vxlan_init");

    rv = bcm_switch_control_set(U, bcmSwitchVxlanUdpDestPortSet, udp_dst_port);
    HWBCM_CHECK(rv, "bcm_switch_control_set(bcmSwitchVxlanUdpDestPortSet)");
    s_udp_port = udp_dst_port;

    rv = bcm_switch_control_set(U, bcmSwitchVxlanEntropyEnable, HWBCM_VXLAN_ENTROPY);
    if (BCM_FAILURE(rv)) {
        hwbcm_log(HWBCM_LOG_WARN, "hwbcm_vxlan: bcmSwitchVxlanEntropyEnable: %s (continuing)", bcm_errmsg(rv));
    }

    rv = bcm_switch_control_set(U, bcmSwitchVxlanTunnelMissToCpu, HWBCM_VXLAN_MISS_TO_CPU);
    if (BCM_FAILURE(rv)) {
        hwbcm_log(HWBCM_LOG_WARN, "hwbcm_vxlan: bcmSwitchVxlanTunnelMissToCpu: %s (continuing)", bcm_errmsg(rv));
    }

    rv = bcm_switch_control_set(U, bcmSwitchVxlanVnIdMissToCpu, HWBCM_VXLAN_MISS_TO_CPU);
    if (BCM_FAILURE(rv)) {
        hwbcm_log(HWBCM_LOG_WARN, "hwbcm_vxlan: bcmSwitchVxlanVnIdMissToCpu: %s (continuing)", bcm_errmsg(rv));
    }

    s_vxlan_inited = 1;
    hwbcm_log(HWBCM_LOG_INFO, "hwbcm_vxlan: initialized, UDP dst port %d, entropy %d, miss-to-cpu %d",
              s_udp_port, HWBCM_VXLAN_ENTROPY, HWBCM_VXLAN_MISS_TO_CPU);
    return BCM_E_NONE;
}

/* ------------------------------------------------------------------------ */
/* VPN                                                                      */
/* ------------------------------------------------------------------------ */

int hwbcm_vxlan_vpn_create(uint32_t vni, int vid, int *vpn_id)
{
    bcm_vxlan_vpn_config_t cfg;
    bcm_multicast_t group = 0;
    int rv;

    if (vpn_id == NULL || vni > 0xFFFFFF) {
        return BCM_E_PARAM;
    }
    if (!s_vxlan_inited) {
        rv = hwbcm_vxlan_init(s_udp_port);
        if (BCM_FAILURE(rv)) {
            return rv;
        }
    }

    rv = bcm_multicast_create(U, BCM_MULTICAST_TYPE_VXLAN, &group);
    HWBCM_CHECK(rv, "bcm_multicast_create(BCM_MULTICAST_TYPE_VXLAN)");

    bcm_vxlan_vpn_config_t_init(&cfg);
    cfg.flags = BCM_VXLAN_VPN_ELAN | BCM_VXLAN_VPN_WITH_VPNID |
                BCM_VXLAN_VPN_SERVICE_TAGGED | BCM_VXLAN_VPN_SERVICE_VLAN_DELETE;
    cfg.vnid = vni;
    cfg.broadcast_group = group;
    cfg.unknown_unicast_group = group;
    cfg.unknown_multicast_group = group;
    cfg.egress_service_tpid = HWBCM_VXLAN_TPID;
    /* cfg.vlan stays 0: OVID 0 in the VN_ID->VFI key (TD2+ vrf-aware mode). */

    rv = bcm_vxlan_vpn_create(U, &cfg);
    if (BCM_FAILURE(rv)) {
        hwbcm_log(HWBCM_LOG_ERR, "hwbcm_vxlan: bcm_vxlan_vpn_create(vni %u) failed: %s", vni, bcm_errmsg(rv));
        (void)bcm_multicast_destroy(U, group);
        return rv;
    }

    *vpn_id = (int)cfg.vpn;
    hwbcm_log(HWBCM_LOG_INFO, "hwbcm_vxlan: vpn 0x%x created for vni %u (vlan %d), flood group 0x%x",
              cfg.vpn, vni, vid, group);
    return BCM_E_NONE;
}

int hwbcm_vxlan_vpn_destroy(int vpn_id)
{
    bcm_vxlan_vpn_config_t cfg;
    bcm_multicast_t groups[3];
    int ngroups = 0, i, j, rv, have_cfg;

    if (vpn_id < 0) {
        return BCM_E_PARAM;
    }

    bcm_vxlan_vpn_config_t_init(&cfg);
    rv = bcm_vxlan_vpn_get(U, (bcm_vpn_t)vpn_id, &cfg);
    have_cfg = BCM_SUCCESS(rv);
    if (!have_cfg) {
        hwbcm_log(HWBCM_LOG_WARN, "hwbcm_vxlan: bcm_vxlan_vpn_get(0x%x): %s", vpn_id, bcm_errmsg(rv));
    }

    /* Remote MACs point at VPs of this VPN; flush them first. */
    (void)hwbcm_vxlan_l2_flush_vpn(vpn_id);

    /* The SDK deletes all VPs of the VPN here (bcm_vxlan_port_delete_all). */
    rv = bcm_vxlan_vpn_destroy(U, (bcm_vpn_t)vpn_id);
    HWBCM_CHECK(rv, "bcm_vxlan_vpn_destroy");

    /* Forget VPs that belonged to this VPN. */
    for (i = 0; i < HWBCM_VXLAN_MAX_VP; i++) {
        if (s_vp[i].in_use && s_vp[i].vpn_id == vpn_id) {
            if (s_vp[i].egr_id > 0) {
                (void)bcm_l3_egress_destroy(U, s_vp[i].egr_id);
            }
            if (s_vp[i].intf_vid > 0 && s_vp[i].intf_vid <= BCM_VLAN_MAX) {
                hwbcm_vxlan_acc_intf_t *ai = &s_acc_intf[s_vp[i].intf_vid];
                if (ai->refcnt > 0 && --ai->refcnt == 0) {
                    bcm_l3_intf_t intf;
                    bcm_l3_intf_t_init(&intf);
                    intf.l3a_intf_id = ai->intf_id;
                    (void)bcm_l3_intf_delete(U, &intf);
                    ai->intf_id = 0;
                }
            }
            memset(&s_vp[i], 0, sizeof(s_vp[i]));
        }
    }

    if (have_cfg) {
        bcm_multicast_t cand[3];
        cand[0] = cfg.broadcast_group;
        cand[1] = cfg.unknown_unicast_group;
        cand[2] = cfg.unknown_multicast_group;
        for (i = 0; i < 3; i++) {
            int dup = 0;
            for (j = 0; j < ngroups; j++) {
                if (groups[j] == cand[i]) {
                    dup = 1;
                }
            }
            if (!dup) {
                groups[ngroups++] = cand[i];
            }
        }
        for (i = 0; i < ngroups; i++) {
            rv = bcm_multicast_destroy(U, groups[i]);
            if (BCM_FAILURE(rv)) {
                hwbcm_log(HWBCM_LOG_WARN, "hwbcm_vxlan: bcm_multicast_destroy(0x%x): %s", groups[i], bcm_errmsg(rv));
            }
        }
    }

    hwbcm_log(HWBCM_LOG_INFO, "hwbcm_vxlan: vpn 0x%x destroyed", vpn_id);
    return BCM_E_NONE;
}

int hwbcm_vxlan_l2_flush_vpn(int vpn_id)
{
    int rv;

    if (vpn_id < 0) {
        return BCM_E_PARAM;
    }
    /* L2 entries of a VFI are keyed with vid = VPN id. */
    rv = bcm_l2_addr_delete_by_vlan(U, (bcm_vlan_t)vpn_id, BCM_L2_DELETE_STATIC);
    if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
        hwbcm_log(HWBCM_LOG_WARN, "hwbcm_vxlan: bcm_l2_addr_delete_by_vlan(vpn 0x%x): %s", vpn_id, bcm_errmsg(rv));
        return rv;
    }
    return BCM_E_NONE;
}

/* ------------------------------------------------------------------------ */
/* tunnels                                                                  */
/* ------------------------------------------------------------------------ */

int hwbcm_vxlan_tunnel_create2(const uint8_t src_ip[4], const uint8_t dst_ip[4], int ttl, int dscp,
                               int *tunnel_init_id, int *tunnel_term_id)
{
    bcm_tunnel_initiator_t init;
    bcm_tunnel_terminator_t term;
    bcm_ip_t sip, dip;
    int rv;

    if (src_ip == NULL || dst_ip == NULL || tunnel_init_id == NULL || tunnel_term_id == NULL) {
        return BCM_E_PARAM;
    }
    if (!s_vxlan_inited) {
        rv = hwbcm_vxlan_init(s_udp_port);
        if (BCM_FAILURE(rv)) {
            return rv;
        }
    }

    sip = hwbcm_ip4_of(src_ip);
    dip = hwbcm_ip4_of(dst_ip);
    if (sip == 0 || dip == 0) {
        hwbcm_log(HWBCM_LOG_ERR, "hwbcm_vxlan: tunnel needs non-zero sip/dip");
        return BCM_E_PARAM;
    }
    if (ttl <= 0 || ttl > 255) {
        ttl = 64;
    }

    bcm_tunnel_initiator_t_init(&init);
    init.type = bcmTunnelTypeVxlan;
    init.ttl = ttl;
    init.sip = sip;
    init.dip = dip;
    init.udp_dst_port = (uint16)s_udp_port;
    init.udp_src_port = 0;              /* entropy hash fills it when enabled */
    if (dscp >= 0 && dscp <= 63) {
        init.dscp_sel = bcmTunnelDscpAssign;
        init.dscp = dscp;
    } else {
        init.dscp_sel = bcmTunnelDscpPacket; /* copy inner DSCP (uniform) */
        init.dscp = 0;
    }

    rv = bcm_vxlan_tunnel_initiator_create(U, &init);
    HWBCM_CHECK(rv, "bcm_vxlan_tunnel_initiator_create");

    /* TD2 only accepts a terminator WITH_ID on the initiator's tunnel id. The
     * terminator key is the local DIP (OVID 0); sip is kept by the SDK for the
     * network VP's SIP -> SVP match. */
    bcm_tunnel_terminator_t_init(&term);
    term.type = bcmTunnelTypeVxlan;
    term.sip = dip;         /* remote VTEP */
    term.dip = sip;         /* local VTEP */
    term.sip_mask = 0xFFFFFFFFu;
    term.dip_mask = 0xFFFFFFFFu;
    term.flags = BCM_TUNNEL_TERM_TUNNEL_WITH_ID;
    term.tunnel_id = init.tunnel_id;
    term.vlan = 0;

    rv = bcm_vxlan_tunnel_terminator_create(U, &term);
    if (BCM_FAILURE(rv)) {
        hwbcm_log(HWBCM_LOG_ERR, "hwbcm_vxlan: bcm_vxlan_tunnel_terminator_create failed: %s", bcm_errmsg(rv));
        (void)bcm_vxlan_tunnel_initiator_destroy(U, init.tunnel_id);
        return rv;
    }

    *tunnel_init_id = (int)init.tunnel_id;
    *tunnel_term_id = (int)term.tunnel_id;
    hwbcm_log(HWBCM_LOG_INFO, "hwbcm_vxlan: tunnel %u.%u.%u.%u -> %u.%u.%u.%u init 0x%x term 0x%x ttl %d dscp %d",
              src_ip[0], src_ip[1], src_ip[2], src_ip[3], dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3],
              init.tunnel_id, term.tunnel_id, ttl, dscp);
    return BCM_E_NONE;
}

int hwbcm_vxlan_tunnel_create(const uint8_t src_ip[4], const uint8_t dst_ip[4], int ttl,
                              int *tunnel_init_id, int *tunnel_term_id)
{
    return hwbcm_vxlan_tunnel_create2(src_ip, dst_ip, ttl, -1, tunnel_init_id, tunnel_term_id);
}

int hwbcm_vxlan_tunnel_destroy(int tunnel_init_id, int tunnel_term_id)
{
    int rv, rv2 = BCM_E_NONE;

    if (tunnel_term_id > 0) {
        rv = bcm_vxlan_tunnel_terminator_destroy(U, (bcm_gport_t)tunnel_term_id);
        if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
            hwbcm_log(HWBCM_LOG_ERR, "hwbcm_vxlan: bcm_vxlan_tunnel_terminator_destroy(0x%x): %s",
                      tunnel_term_id, bcm_errmsg(rv));
            rv2 = rv;
        }
    }
    if (tunnel_init_id > 0) {
        rv = bcm_vxlan_tunnel_initiator_destroy(U, (bcm_gport_t)tunnel_init_id);
        if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
            hwbcm_log(HWBCM_LOG_ERR, "hwbcm_vxlan: bcm_vxlan_tunnel_initiator_destroy(0x%x): %s",
                      tunnel_init_id, bcm_errmsg(rv));
            rv2 = rv;
        }
    }
    return rv2;
}

/* ------------------------------------------------------------------------ */
/* egress objects                                                           */
/* ------------------------------------------------------------------------ */

int hwbcm_vxlan_egress_create(int intf_id, const uint8_t dst_mac[6], int port, int trunk_id, int vid,
                              int *egr_id)
{
    bcm_l3_egress_t egr;
    bcm_gport_t gport = 0;
    bcm_if_t id = 0;
    int rv;

    if (dst_mac == NULL || egr_id == NULL || intf_id <= 0) {
        return BCM_E_PARAM;
    }
    rv = hwbcm_vxlan_gport_of(port, trunk_id, &gport);
    if (BCM_FAILURE(rv)) {
        return rv;
    }

    bcm_l3_egress_t_init(&egr);
    egr.flags = BCM_L3_VXLAN_ONLY;
    egr.intf = intf_id;
    memcpy(egr.mac_addr, dst_mac, 6);
    egr.vlan = (bcm_vlan_t)((vid > 0 && vid <= BCM_VLAN_MAX) ? vid : 0);
    egr.port = gport;

    rv = bcm_l3_egress_create(U, 0, &egr, &id);
    HWBCM_CHECK(rv, "bcm_l3_egress_create(BCM_L3_VXLAN_ONLY)");

    *egr_id = (int)id;
    hwbcm_log(HWBCM_LOG_DEBUG, "hwbcm_vxlan: vxlan egress %d (intf %d, port %d, trunk %d, vlan %d)",
              id, intf_id, port, trunk_id, vid);
    return BCM_E_NONE;
}

/* Read the unicast egress behind an egress id or the first member of an ECMP
 * group. */
static int hwbcm_vxlan_egress_source_get(int l3_egr_or_ecmp_id, bcm_l3_egress_t *src)
{
    int rv;

    bcm_l3_egress_t_init(src);
    rv = bcm_l3_egress_get(U, (bcm_if_t)l3_egr_or_ecmp_id, src);
    if (BCM_SUCCESS(rv)) {
        return BCM_E_NONE;
    }

    /* Maybe an ECMP group: use its first member. */
    {
        bcm_l3_egress_ecmp_t ecmp;
        bcm_if_t members[1];
        int count = 0;

        bcm_l3_egress_ecmp_t_init(&ecmp);
        ecmp.ecmp_intf = (bcm_if_t)l3_egr_or_ecmp_id;
        rv = bcm_l3_egress_ecmp_get(U, &ecmp, 1, members, &count);
        if (BCM_FAILURE(rv) || count < 1) {
            hwbcm_log(HWBCM_LOG_ERR, "hwbcm_vxlan: egress %d is neither a unicast egress nor an ECMP group (%s)",
                      l3_egr_or_ecmp_id, bcm_errmsg(rv));
            return BCM_FAILURE(rv) ? rv : BCM_E_NOT_FOUND;
        }
        bcm_l3_egress_t_init(src);
        rv = bcm_l3_egress_get(U, members[0], src);
        HWBCM_CHECK(rv, "bcm_l3_egress_get(ecmp member)");
        hwbcm_log(HWBCM_LOG_WARN, "hwbcm_vxlan: ECMP %d toward remote VTEP, using first member %d only",
                  l3_egr_or_ecmp_id, members[0]);
    }
    return BCM_E_NONE;
}

int hwbcm_vxlan_egress_is_stale(int l3_egr_or_ecmp_id, int vxlan_egr_id, int *stale)
{
    bcm_l3_egress_t src, cur;
    int rv;

    if (stale == NULL) {
        return BCM_E_PARAM;
    }
    *stale = 0;

    rv = hwbcm_vxlan_egress_source_get(l3_egr_or_ecmp_id, &src);
    if (BCM_FAILURE(rv)) {
        *stale = 1;     /* source vanished */
        return BCM_E_NONE;
    }
    if (src.flags & (BCM_L3_COPY_TO_CPU | BCM_L3_DST_DISCARD)) {
        *stale = 1;     /* neighbor gone: A3 rewrote it to L2TOCPU */
        return BCM_E_NONE;
    }

    bcm_l3_egress_t_init(&cur);
    rv = bcm_l3_egress_get(U, (bcm_if_t)vxlan_egr_id, &cur);
    if (BCM_FAILURE(rv)) {
        *stale = 1;
        return BCM_E_NONE;
    }

    if (cur.intf != src.intf || memcmp(cur.mac_addr, src.mac_addr, 6) != 0 || cur.vlan != src.vlan) {
        *stale = 1;
    } else if (src.flags & BCM_L3_TGID) {
        if (!BCM_GPORT_IS_TRUNK(cur.port) || BCM_GPORT_TRUNK_GET(cur.port) != src.trunk) {
            *stale = 1;
        }
    } else if (hwbcm_vxlan_port_of_gport(cur.port) != hwbcm_vxlan_port_of_gport(src.port)) {
        *stale = 1;
    }

    if (*stale) {
        hwbcm_log(HWBCM_LOG_INFO, "hwbcm_vxlan: vxlan egress %d is stale w.r.t. source %d", vxlan_egr_id, l3_egr_or_ecmp_id);
    }
    return BCM_E_NONE;
}

int hwbcm_vxlan_egress_from_l3(int l3_egr_or_ecmp_id, int *vxlan_egr_id, int *port)
{
    bcm_l3_egress_t src;
    bcm_l3_egress_t egr;
    bcm_if_t id = 0;
    int rv;

    if (vxlan_egr_id == NULL) {
        return BCM_E_PARAM;
    }

    rv = hwbcm_vxlan_egress_source_get(l3_egr_or_ecmp_id, &src);
    if (BCM_FAILURE(rv)) {
        return rv;
    }

    if (src.flags & (BCM_L3_COPY_TO_CPU | BCM_L3_DST_DISCARD)) {
        hwbcm_log(HWBCM_LOG_WARN, "hwbcm_vxlan: egress %d is a CPU/drop egress, not usable for a tunnel",
                  l3_egr_or_ecmp_id);
        return BCM_E_CONFIG;
    }
    if (src.intf <= 0) {
        hwbcm_log(HWBCM_LOG_WARN, "hwbcm_vxlan: egress %d has no L3 interface", l3_egr_or_ecmp_id);
        return BCM_E_CONFIG;
    }

    bcm_l3_egress_t_init(&egr);
    egr.flags = BCM_L3_VXLAN_ONLY;
    egr.intf = src.intf;
    memcpy(egr.mac_addr, src.mac_addr, 6);
    egr.vlan = src.vlan;
    if (src.flags & BCM_L3_TGID) {
        BCM_GPORT_TRUNK_SET(egr.port, src.trunk);
    } else {
        egr.port = src.port;
        egr.module = src.module;
    }

    rv = bcm_l3_egress_create(U, 0, &egr, &id);
    HWBCM_CHECK(rv, "bcm_l3_egress_create(BCM_L3_VXLAN_ONLY clone)");

    *vxlan_egr_id = (int)id;
    if (port != NULL) {
        *port = (src.flags & BCM_L3_TGID) ? -1 : hwbcm_vxlan_port_of_gport(src.port);
    }
    hwbcm_log(HWBCM_LOG_INFO,
              "hwbcm_vxlan: vxlan egress %d cloned from %d (intf %d, mac %02x:%02x:%02x:%02x:%02x:%02x, port 0x%x)",
              id, l3_egr_or_ecmp_id, src.intf, src.mac_addr[0], src.mac_addr[1], src.mac_addr[2],
              src.mac_addr[3], src.mac_addr[4], src.mac_addr[5], src.port);
    return BCM_E_NONE;
}

int hwbcm_vxlan_egress_destroy(int egr_id)
{
    int rv;

    if (egr_id <= 0) {
        return BCM_E_PARAM;
    }
    rv = bcm_l3_egress_destroy(U, (bcm_if_t)egr_id);
    if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
        hwbcm_log(HWBCM_LOG_ERR, "hwbcm_vxlan: bcm_l3_egress_destroy(%d): %s", egr_id, bcm_errmsg(rv));
        return rv;
    }
    return BCM_E_NONE;
}

/* ------------------------------------------------------------------------ */
/* port controls                                                            */
/* ------------------------------------------------------------------------ */

int hwbcm_vxlan_port_network_enable(int port, int enable)
{
    int rv;

    if (port < 0) {
        return BCM_E_PARAM;
    }
    rv = bcm_port_control_set(U, port, bcmPortControlVxlanEnable, enable ? 1 : 0);
    HWBCM_CHECK(rv, "bcm_port_control_set(bcmPortControlVxlanEnable)");
    /* VN_ID lookup key = VN_ID only (not tunnel based). */
    rv = bcm_port_control_set(U, port, bcmPortControlVxlanTunnelbasedVnId, 0);
    if (BCM_FAILURE(rv)) {
        hwbcm_log(HWBCM_LOG_WARN, "hwbcm_vxlan: bcmPortControlVxlanTunnelbasedVnId(port %d): %s", port, bcm_errmsg(rv));
    }
    hwbcm_log(HWBCM_LOG_INFO, "hwbcm_vxlan: port %d VXLAN termination %s", port, enable ? "enabled" : "disabled");
    return BCM_E_NONE;
}

static int hwbcm_vxlan_access_port_prepare(int port)
{
    int rv;

    /* VLAN_XLATE (port, vid) -> SVP needs ingress VLAN translation on. */
    rv = bcm_vlan_control_port_set(U, port, bcmVlanTranslateIngressEnable, 1);
    HWBCM_CHECK(rv, "bcm_vlan_control_port_set(bcmVlanTranslateIngressEnable)");
    return BCM_E_NONE;
}

/* ------------------------------------------------------------------------ */
/* flood groups                                                             */
/* ------------------------------------------------------------------------ */

int hwbcm_vxlan_vpn_flood_set(int vpn_id, int vp_id, int add)
{
    bcm_vxlan_vpn_config_t cfg;
    bcm_multicast_t groups[3];
    bcm_multicast_t cand[3];
    int ngroups = 0, i, j, rv, vp;
    bcm_if_t encap = 0;

    vp = hwbcm_vxlan_vp_index(vp_id);
    if (vpn_id < 0 || vp < 0) {
        return BCM_E_PARAM;
    }
    if (!s_vp[vp].in_use) {
        hwbcm_log(HWBCM_LOG_WARN, "hwbcm_vxlan: flood_set: vp 0x%x unknown", vp_id);
        return BCM_E_NOT_FOUND;
    }

    bcm_vxlan_vpn_config_t_init(&cfg);
    rv = bcm_vxlan_vpn_get(U, (bcm_vpn_t)vpn_id, &cfg);
    HWBCM_CHECK(rv, "bcm_vxlan_vpn_get");

    cand[0] = cfg.broadcast_group;
    cand[1] = cfg.unknown_unicast_group;
    cand[2] = cfg.unknown_multicast_group;
    for (i = 0; i < 3; i++) {
        int dup = 0;
        for (j = 0; j < ngroups; j++) {
            if (groups[j] == cand[i]) {
                dup = 1;
            }
        }
        if (!dup) {
            groups[ngroups++] = cand[i];
        }
    }

    for (i = 0; i < ngroups; i++) {
        rv = bcm_multicast_vxlan_encap_get(U, groups[i], (bcm_gport_t)s_vp[vp].port_gport, (bcm_gport_t)vp_id, &encap);
        HWBCM_CHECK(rv, "bcm_multicast_vxlan_encap_get");
        if (add) {
            rv = bcm_multicast_egress_add(U, groups[i], (bcm_gport_t)vp_id, encap);
            if (rv == BCM_E_EXISTS) {
                rv = BCM_E_NONE;
            }
            HWBCM_CHECK(rv, "bcm_multicast_egress_add");
        } else {
            rv = bcm_multicast_egress_delete(U, groups[i], (bcm_gport_t)vp_id, encap);
            if (rv == BCM_E_NOT_FOUND) {
                rv = BCM_E_NONE;
            }
            HWBCM_CHECK(rv, "bcm_multicast_egress_delete");
        }
    }

    hwbcm_log(HWBCM_LOG_DEBUG, "hwbcm_vxlan: vp 0x%x %s flood groups of vpn 0x%x", vp_id,
              add ? "added to" : "removed from", vpn_id);
    return BCM_E_NONE;
}

/* ------------------------------------------------------------------------ */
/* virtual ports                                                            */
/* ------------------------------------------------------------------------ */

int hwbcm_vxlan_network_port_add(int vpn_id, int tunnel_init_id, int tunnel_term_id,
                                 int l3_egr_id, int *vp_id)
{
    bcm_vxlan_port_t vxp;
    bcm_l3_egress_t egr;
    bcm_gport_t gport = 0;
    int rv, vp, port = -1;

    if (vp_id == NULL || vpn_id < 0 || tunnel_init_id <= 0 || tunnel_term_id <= 0 || l3_egr_id <= 0) {
        return BCM_E_PARAM;
    }

    /* Resolve the underlay port from the (VXLAN-only) egress. */
    bcm_l3_egress_t_init(&egr);
    rv = bcm_l3_egress_get(U, (bcm_if_t)l3_egr_id, &egr);
    HWBCM_CHECK(rv, "bcm_l3_egress_get(network egress)");
    if (!(egr.flags & BCM_L3_VXLAN_ONLY)) {
        hwbcm_log(HWBCM_LOG_ERR, "hwbcm_vxlan: egress %d lacks BCM_L3_VXLAN_ONLY; use hwbcm_vxlan_egress_from_l3()",
                  l3_egr_id);
        return BCM_E_CONFIG;
    }
    if (egr.flags & BCM_L3_TGID) {
        BCM_GPORT_TRUNK_SET(gport, egr.trunk);
    } else {
        port = hwbcm_vxlan_port_of_gport(egr.port);
        if (port < 0) {
            hwbcm_log(HWBCM_LOG_ERR, "hwbcm_vxlan: cannot resolve local port of egress %d (port 0x%x)",
                      l3_egr_id, egr.port);
            return BCM_E_CONFIG;
        }
        rv = bcm_port_gport_get(U, port, &gport);
        HWBCM_CHECK(rv, "bcm_port_gport_get");
        rv = hwbcm_vxlan_port_network_enable(port, 1);
        if (BCM_FAILURE(rv)) {
            return rv;
        }
    }

    bcm_vxlan_port_t_init(&vxp);
    vxp.flags = BCM_VXLAN_PORT_NETWORK | BCM_VXLAN_PORT_EGRESS_TUNNEL | BCM_VXLAN_PORT_SERVICE_TAGGED;
    vxp.match_port = gport;
    vxp.criteria = BCM_VXLAN_PORT_MATCH_VN_ID;
    vxp.egress_if = (bcm_if_t)l3_egr_id;
    vxp.egress_tunnel_id = (bcm_gport_t)tunnel_init_id;
    vxp.match_tunnel_id = (bcm_gport_t)tunnel_term_id;
    vxp.egress_service_tpid = HWBCM_VXLAN_TPID;
    /* vnid stays 0xffffffff (init value): only tunnel-based VPNs take a vnid. */

    rv = bcm_vxlan_port_add(U, (bcm_vpn_t)vpn_id, &vxp);
    HWBCM_CHECK(rv, "bcm_vxlan_port_add(network)");

    vp = hwbcm_vxlan_vp_index(vxp.vxlan_port_id);
    if (vp < 0) {
        hwbcm_log(HWBCM_LOG_ERR, "hwbcm_vxlan: unexpected network vp gport 0x%x", vxp.vxlan_port_id);
        (void)bcm_vxlan_port_delete(U, (bcm_vpn_t)vpn_id, vxp.vxlan_port_id);
        return BCM_E_INTERNAL;
    }
    memset(&s_vp[vp], 0, sizeof(s_vp[vp]));
    s_vp[vp].in_use = 1;
    s_vp[vp].is_network = 1;
    s_vp[vp].port_gport = (int)gport;
    s_vp[vp].egr_id = 0;        /* owned by the caller */
    s_vp[vp].intf_vid = -1;
    s_vp[vp].vpn_id = vpn_id;

    rv = hwbcm_vxlan_vpn_flood_set(vpn_id, (int)vxp.vxlan_port_id, 1);
    if (BCM_FAILURE(rv)) {
        (void)bcm_vxlan_port_delete(U, (bcm_vpn_t)vpn_id, vxp.vxlan_port_id);
        memset(&s_vp[vp], 0, sizeof(s_vp[vp]));
        return rv;
    }

    *vp_id = (int)vxp.vxlan_port_id;
    hwbcm_log(HWBCM_LOG_INFO, "hwbcm_vxlan: network vp 0x%x (vp %d) in vpn 0x%x, port %d, egress %d, init 0x%x term 0x%x",
              vxp.vxlan_port_id, vp, vpn_id, port, l3_egr_id, tunnel_init_id, tunnel_term_id);
    return BCM_E_NONE;
}

static int hwbcm_vxlan_acc_intf_get(int vid, int *intf_id)
{
    bcm_l3_intf_t intf;
    int rv;

    if (vid <= 0 || vid > BCM_VLAN_MAX) {
        return BCM_E_PARAM;
    }
    if (s_acc_intf[vid].refcnt > 0) {
        s_acc_intf[vid].refcnt++;
        *intf_id = s_acc_intf[vid].intf_id;
        return BCM_E_NONE;
    }

    /* Dummy L3 interface on the access VLAN; only its VLAN is used by the
     * access egress object (SD-tag handling), the MAC never hits the wire. */
    bcm_l3_intf_t_init(&intf);
    memcpy(intf.l3a_mac_addr, s_acc_dummy_mac, 6);
    intf.l3a_vid = (bcm_vlan_t)vid;
    intf.l3a_vrf = 0;
    rv = bcm_l3_intf_create(U, &intf);
    HWBCM_CHECK(rv, "bcm_l3_intf_create(access dummy)");

    s_acc_intf[vid].intf_id = intf.l3a_intf_id;
    s_acc_intf[vid].refcnt = 1;
    *intf_id = intf.l3a_intf_id;
    return BCM_E_NONE;
}

static void hwbcm_vxlan_acc_intf_put(int vid)
{
    if (vid <= 0 || vid > BCM_VLAN_MAX || s_acc_intf[vid].refcnt <= 0) {
        return;
    }
    if (--s_acc_intf[vid].refcnt == 0) {
        bcm_l3_intf_t intf;
        int rv;
        bcm_l3_intf_t_init(&intf);
        intf.l3a_intf_id = s_acc_intf[vid].intf_id;
        rv = bcm_l3_intf_delete(U, &intf);
        if (BCM_FAILURE(rv)) {
            hwbcm_log(HWBCM_LOG_WARN, "hwbcm_vxlan: bcm_l3_intf_delete(%d): %s", intf.l3a_intf_id, bcm_errmsg(rv));
        }
        s_acc_intf[vid].intf_id = 0;
    }
}

int hwbcm_vxlan_access_port_add2(int vpn_id, int port, int trunk_id, int vid, int tagged, int *vp_id)
{
    bcm_vxlan_port_t vxp;
    bcm_gport_t gport = 0;
    int rv, vp, intf_id = 0, egr_id = 0;

    if (vp_id == NULL || vpn_id < 0 || vid <= 0 || vid > BCM_VLAN_MAX) {
        return BCM_E_PARAM;
    }
    rv = hwbcm_vxlan_gport_of(port, trunk_id, &gport);
    if (BCM_FAILURE(rv)) {
        return rv;
    }

    if (trunk_id < 0) {
        rv = hwbcm_vxlan_access_port_prepare(port);
        if (BCM_FAILURE(rv)) {
            return rv;
        }
    }

    rv = hwbcm_vxlan_acc_intf_get(vid, &intf_id);
    if (BCM_FAILURE(rv)) {
        return rv;
    }

    rv = hwbcm_vxlan_egress_create(intf_id, s_acc_dummy_mac, port, trunk_id, vid, &egr_id);
    if (BCM_FAILURE(rv)) {
        hwbcm_vxlan_acc_intf_put(vid);
        return rv;
    }

    bcm_vxlan_port_t_init(&vxp);
    vxp.flags = BCM_VXLAN_PORT_SERVICE_TAGGED;
    if (tagged) {
        vxp.flags |= BCM_VXLAN_PORT_SERVICE_VLAN_ADD;
        vxp.egress_service_vlan = (bcm_vlan_t)vid;
    }
    vxp.egress_service_tpid = HWBCM_VXLAN_TPID;
    vxp.criteria = BCM_VXLAN_PORT_MATCH_PORT_VLAN;
    vxp.match_port = gport;
    vxp.match_vlan = (bcm_vlan_t)vid;
    vxp.egress_if = (bcm_if_t)egr_id;

    rv = bcm_vxlan_port_add(U, (bcm_vpn_t)vpn_id, &vxp);
    if (BCM_FAILURE(rv)) {
        hwbcm_log(HWBCM_LOG_ERR, "hwbcm_vxlan: bcm_vxlan_port_add(access port %d trunk %d vlan %d vpn 0x%x): %s",
                  port, trunk_id, vid, vpn_id, bcm_errmsg(rv));
        (void)bcm_l3_egress_destroy(U, (bcm_if_t)egr_id);
        hwbcm_vxlan_acc_intf_put(vid);
        return rv;
    }

    vp = hwbcm_vxlan_vp_index(vxp.vxlan_port_id);
    if (vp < 0) {
        hwbcm_log(HWBCM_LOG_ERR, "hwbcm_vxlan: unexpected access vp gport 0x%x", vxp.vxlan_port_id);
        (void)bcm_vxlan_port_delete(U, (bcm_vpn_t)vpn_id, vxp.vxlan_port_id);
        (void)bcm_l3_egress_destroy(U, (bcm_if_t)egr_id);
        hwbcm_vxlan_acc_intf_put(vid);
        return BCM_E_INTERNAL;
    }
    memset(&s_vp[vp], 0, sizeof(s_vp[vp]));
    s_vp[vp].in_use = 1;
    s_vp[vp].is_network = 0;
    s_vp[vp].port_gport = (int)gport;
    s_vp[vp].egr_id = egr_id;
    s_vp[vp].intf_vid = vid;
    s_vp[vp].vpn_id = vpn_id;

    rv = hwbcm_vxlan_vpn_flood_set(vpn_id, (int)vxp.vxlan_port_id, 1);
    if (BCM_FAILURE(rv)) {
        (void)hwbcm_vxlan_port_delete(vpn_id, (int)vxp.vxlan_port_id);
        return rv;
    }

    *vp_id = (int)vxp.vxlan_port_id;
    hwbcm_log(HWBCM_LOG_INFO, "hwbcm_vxlan: access vp 0x%x (vp %d) vpn 0x%x port %d trunk %d vlan %d %s",
              vxp.vxlan_port_id, vp, vpn_id, port, trunk_id, vid, tagged ? "tagged" : "untagged");
    return BCM_E_NONE;
}

int hwbcm_vxlan_access_port_add(int vpn_id, int port, int vid, int *vp_id)
{
    return hwbcm_vxlan_access_port_add2(vpn_id, port, -1, vid, 1, vp_id);
}

int hwbcm_vxlan_port_delete(int vpn_id, int vp_id)
{
    int rv, vp, rv2 = BCM_E_NONE;
    bcm_vpn_t vpn;

    vp = hwbcm_vxlan_vp_index(vp_id);
    if (vp < 0) {
        return BCM_E_PARAM;
    }
    vpn = hwbcm_vxlan_vpn_arg(vpn_id);
    if (vpn == (bcm_vpn_t)BCM_VXLAN_VPN_INVALID && s_vp[vp].in_use) {
        vpn = (bcm_vpn_t)s_vp[vp].vpn_id;
    }

    if (s_vp[vp].in_use && s_vp[vp].vpn_id >= 0) {
        rv = hwbcm_vxlan_vpn_flood_set(s_vp[vp].vpn_id, vp_id, 0);
        if (BCM_FAILURE(rv)) {
            hwbcm_log(HWBCM_LOG_WARN, "hwbcm_vxlan: flood removal of vp 0x%x from vpn 0x%x: %s",
                      vp_id, s_vp[vp].vpn_id, bcm_errmsg(rv));
        }
    }

    rv = bcm_vxlan_port_delete(U, vpn, (bcm_gport_t)vp_id);
    if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
        hwbcm_log(HWBCM_LOG_ERR, "hwbcm_vxlan: bcm_vxlan_port_delete(vpn 0x%x, vp 0x%x): %s", vpn, vp_id, bcm_errmsg(rv));
        rv2 = rv;
    }

    if (s_vp[vp].in_use) {
        if (s_vp[vp].egr_id > 0) {
            rv = bcm_l3_egress_destroy(U, (bcm_if_t)s_vp[vp].egr_id);
            if (BCM_FAILURE(rv) && rv != BCM_E_NOT_FOUND) {
                hwbcm_log(HWBCM_LOG_WARN, "hwbcm_vxlan: bcm_l3_egress_destroy(%d): %s", s_vp[vp].egr_id, bcm_errmsg(rv));
            }
        }
        if (s_vp[vp].intf_vid > 0) {
            hwbcm_vxlan_acc_intf_put(s_vp[vp].intf_vid);
        }
        memset(&s_vp[vp], 0, sizeof(s_vp[vp]));
    }

    hwbcm_log(HWBCM_LOG_INFO, "hwbcm_vxlan: vp 0x%x deleted (vpn 0x%x)", vp_id, vpn);
    return rv2;
}

/* ------------------------------------------------------------------------ */
/* L2                                                                       */
/* ------------------------------------------------------------------------ */

int hwbcm_vxlan_l2_addr_add(const uint8_t mac[6], int vpn_id, int vp_id, int is_static)
{
    bcm_l2_addr_t l2;
    bcm_mac_t m;
    int rv;

    if (mac == NULL || vpn_id < 0 || hwbcm_vxlan_vp_index(vp_id) < 0) {
        return BCM_E_PARAM;
    }
    memcpy(m, mac, 6);
    bcm_l2_addr_t_init(&l2, m, (bcm_vlan_t)vpn_id);
    l2.flags = is_static ? BCM_L2_STATIC : 0;
    l2.port = (bcm_port_t)vp_id;    /* VXLAN VP gport */

    rv = bcm_l2_addr_add(U, &l2);
    HWBCM_CHECK(rv, "bcm_l2_addr_add(vxlan vp)");

    hwbcm_log(HWBCM_LOG_DEBUG, "hwbcm_vxlan: l2 %02x:%02x:%02x:%02x:%02x:%02x vpn 0x%x -> vp 0x%x%s",
              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], vpn_id, vp_id, is_static ? " static" : "");
    return BCM_E_NONE;
}

int hwbcm_vxlan_l2_addr_delete(const uint8_t mac[6], int vpn_id)
{
    bcm_mac_t m;
    int rv;

    if (mac == NULL || vpn_id < 0) {
        return BCM_E_PARAM;
    }
    memcpy(m, mac, 6);
    rv = bcm_l2_addr_delete(U, m, (bcm_vlan_t)vpn_id);
    if (rv == BCM_E_NOT_FOUND) {
        return BCM_E_NONE;
    }
    HWBCM_CHECK(rv, "bcm_l2_addr_delete(vxlan)");
    return BCM_E_NONE;
}

int hwbcm_vxlan_vp_info(int vp_gport, int *is_network, int *port, int *trunk_id, int *vpn_id)
{
    int vp = hwbcm_vxlan_vp_index(vp_gport);

    if (vp < 0 || !s_vp[vp].in_use) {
        return BCM_E_NOT_FOUND;
    }
    if (is_network != NULL) {
        *is_network = s_vp[vp].is_network;
    }
    if (port != NULL) {
        *port = hwbcm_vxlan_port_of_gport((bcm_gport_t)s_vp[vp].port_gport);
    }
    if (trunk_id != NULL) {
        *trunk_id = BCM_GPORT_IS_TRUNK((bcm_gport_t)s_vp[vp].port_gport)
                    ? BCM_GPORT_TRUNK_GET((bcm_gport_t)s_vp[vp].port_gport) : -1;
    }
    if (vpn_id != NULL) {
        *vpn_id = s_vp[vp].vpn_id;
    }
    return BCM_E_NONE;
}

/* ------------------------------------------------------------------------ */
/* MY_STATION                                                               */
/* ------------------------------------------------------------------------ */

int hwbcm_vxlan_station_add(const uint8_t mac[6], int vid, int *station_id)
{
    bcm_l2_station_t st;
    int id = 0, rv;

    if (mac == NULL || station_id == NULL) {
        return BCM_E_PARAM;
    }
    bcm_l2_station_t_init(&st);
    memcpy(st.dst_mac, mac, 6);
    memset(st.dst_mac_mask, 0xff, 6);
    if (vid > 0 && vid <= BCM_VLAN_MAX) {
        st.vlan = (bcm_vlan_t)vid;
        st.vlan_mask = 0xfff;
    } else {
        st.vlan = 0;
        st.vlan_mask = 0;       /* any VLAN */
    }
    st.flags = BCM_L2_STATION_IPV4 | BCM_L2_STATION_IPV6 | BCM_L2_STATION_ARP_RARP;

    rv = bcm_l2_station_add(U, &id, &st);
    if (rv == BCM_E_EXISTS) {
        hwbcm_log(HWBCM_LOG_INFO, "hwbcm_vxlan: my-station entry already present (vlan %d)", vid);
        *station_id = -1;
        return BCM_E_NONE;
    }
    HWBCM_CHECK(rv, "bcm_l2_station_add");

    *station_id = id;
    hwbcm_log(HWBCM_LOG_INFO, "hwbcm_vxlan: my-station %d: %02x:%02x:%02x:%02x:%02x:%02x vlan %d (0=any)",
              id, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], vid);
    return BCM_E_NONE;
}

int hwbcm_vxlan_station_delete(int station_id)
{
    int rv;

    if (station_id < 0) {
        return BCM_E_NONE;
    }
    rv = bcm_l2_station_delete(U, station_id);
    if (rv == BCM_E_NOT_FOUND) {
        return BCM_E_NONE;
    }
    HWBCM_CHECK(rv, "bcm_l2_station_delete");
    return BCM_E_NONE;
}
