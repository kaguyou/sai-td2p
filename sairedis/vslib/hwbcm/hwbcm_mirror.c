/*
 * hwbcm_mirror.c — port mirroring (SPAN / RSPAN / ERSPAN) for Trident2+
 * (BCM56864), OpenBCM sdk-6.5.27. API: hwbcm_mirror.h.
 *
 * TD2+ mirroring facts verified against sdk-6.5.27 (src/bcm/esw/mirror.c):
 *  - Directed mirroring is always on for SOC_IS_TD_TT (switch.c:18858,
 *    soc_feature_directed_mirror_only). No bcm_mirror_mode_set call is needed:
 *    bcm_mirror_port_dest_add enables mirroring automatically (:26996).
 *  - 4 shared MTP slots (BCM_MIRROR_MTP_COUNT = 4). Each active (dest, dir)
 *    pair reserves one MTP slot; RSPAN cannot use slot 0 on TD2+
 *    (soc_feature_mirror_rspan_no_mtp0, :17088).
 *  - ERSPAN uses EGR_MIRROR_ENCAP_CONTROL/DATA_1 (soc_feature_mirror_encap_profile)
 *    via _bcm_trident_mirror_ipv4_gre_tunnel_set (:12745-12887): IPv4 only
 *    (IPv6 returns BCM_E_UNAVAIL at :14876), 4-byte GRE header with protocol
 *    field (default 0x88be). Outer VLAN tag is inserted iff
 *    BCM_VLAN_VALID(BCM_VLAN_CTRL_ID(vlan_id)), and the 16-bit TCI is taken
 *    directly from vlan_id (:12828-12829), so we pack PRI/CFI/VID with
 *    BCM_VLAN_CTRL(pri, cfi, vid).
 *  - BCM_MIRROR_DEST_TUNNEL_WITH_SPAN_ID and BCM_MIRROR_DEST_TUNNEL_RSPAN
 *    return BCM_E_PARAM on ESW (:27696); RSPAN uses BCM_MIRROR_DEST_TUNNEL_L2.
 *  - BCM_MIRROR_DEST_INT_PRI_SET programs EGR_IM/EM_MTP_INDEX.CHANGE_INT_PRI
 *    and NEW_INT_PRI (:15888-15900), supported on SOC_IS_TD_TT (:27770).
 *  - Duplicate-gport quirk (:27924 -> _bcm_mirror_destination_match :5682):
 *    bcm_mirror_destination_create() without BCM_MIRROR_DEST_REPLACE rejects a
 *    new destination (even an ERSPAN tunnel!) with BCM_E_EXISTS if any
 *    existing non-tunnel (SPAN) destination targets the same monitor gport.
 *    If that happens, retrying with BCM_MIRROR_DEST_REPLACE (without WITH_ID)
 *    skips the match check and allocates a fresh mirror_dest_id (:6861).
 */

#ifdef BCM_HIDE_DISPATCHABLE
#undef BCM_HIDE_DISPATCHABLE
#endif

#include "hwbcm.h"
#include "hwbcm_mirror.h"

#include <string.h>

#include <bcm/types.h>
#include <bcm/error.h>
#include <bcm/mirror.h>
#include <bcm/port.h>

#define U HWBCM_UNIT

/* ------------------------------------------------------------------------ */
/* helpers                                                                  */
/* ------------------------------------------------------------------------ */

void hwbcm_mirror_dest_cfg_init(hwbcm_mirror_dest_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->type = HWBCM_MIRROR_TYPE_LOCAL;
    cfg->port = -1;
    cfg->trunk_id = -1;
    cfg->tc = -1;
    cfg->vlan_tpid = 0x8100u;
    cfg->ttl = 255u;
    cfg->gre_protocol = HWBCM_MIRROR_GRE_PROTO_DEFAULT;
}

static uint32 dir_to_bcm_flags(uint32_t dir)
{
    uint32 flags = 0;

    if (dir & HWBCM_MIRROR_DIR_INGRESS) {
        flags |= BCM_MIRROR_PORT_INGRESS;
    }
    if (dir & HWBCM_MIRROR_DIR_EGRESS) {
        flags |= BCM_MIRROR_PORT_EGRESS;
    }
    return flags;
}

static const char *type_name(int type)
{
    switch (type) {
    case HWBCM_MIRROR_TYPE_LOCAL:  return "SPAN";
    case HWBCM_MIRROR_TYPE_RSPAN:  return "RSPAN";
    case HWBCM_MIRROR_TYPE_ERSPAN: return "ERSPAN";
    default:                       return "?";
    }
}

static int fill_mirror_dest(const hwbcm_mirror_dest_cfg_t *cfg,
                            bcm_mirror_destination_t *mdest)
{
    int rv;

    if (cfg == NULL || mdest == NULL) {
        return BCM_E_PARAM;
    }

    bcm_mirror_destination_t_init(mdest);

    /* Destination gport must be MODPORT, TRUNK, or DEVPORT (mirror.c:27818). */
    if (cfg->trunk_id >= 0) {
        BCM_GPORT_TRUNK_SET(mdest->gport, (bcm_trunk_t)cfg->trunk_id);
    } else if (cfg->port >= 0) {
        rv = bcm_port_gport_get(U, (bcm_port_t)cfg->port, &mdest->gport);
        if (BCM_FAILURE(rv)) {
            hwbcm_log(HWBCM_LOG_ERR, "mirror: bcm_port_gport_get(port %d): %s (%d)",
                      cfg->port, bcm_errmsg(rv), rv);
            return rv;
        }
    } else {
        hwbcm_log(HWBCM_LOG_ERR, "mirror: destination requires port or trunk_id");
        return BCM_E_PARAM;
    }

    if (cfg->tc >= 0) {
        mdest->flags |= BCM_MIRROR_DEST_INT_PRI_SET;
        mdest->int_pri = (uint8)(cfg->tc & 0x0f);
    }

    switch (cfg->type) {
    case HWBCM_MIRROR_TYPE_LOCAL:
        break;

    case HWBCM_MIRROR_TYPE_RSPAN:
        if (cfg->vlan_id < 1u || cfg->vlan_id > 4094u) {
            hwbcm_log(HWBCM_LOG_ERR, "mirror: RSPAN requires valid vlan_id (got %u)",
                      (unsigned)cfg->vlan_id);
            return BCM_E_PARAM;
        }
        mdest->flags |= BCM_MIRROR_DEST_TUNNEL_L2;
        mdest->tpid = cfg->vlan_tpid ? cfg->vlan_tpid : 0x8100u;
        mdest->vlan_id = (bcm_vlan_t)BCM_VLAN_CTRL(cfg->vlan_pri & 0x7u,
                                                   cfg->vlan_cfi & 0x1u,
                                                   cfg->vlan_id & 0xfffu);
        mdest->pkt_prio = (uint8)(cfg->vlan_pri & 0x7u);
        mdest->cfi = (uint8)(cfg->vlan_cfi & 0x1u);
        break;

    case HWBCM_MIRROR_TYPE_ERSPAN:
        mdest->flags |= BCM_MIRROR_DEST_TUNNEL_IP_GRE;
        if (cfg->payload_untagged) {
            mdest->flags |= BCM_MIRROR_DEST_PAYLOAD_UNTAGGED;
        }
        mdest->version = 4;
        mdest->src_addr = (bcm_ip_t)cfg->src_ip;
        mdest->dst_addr = (bcm_ip_t)cfg->dst_ip;
        memcpy(mdest->src_mac, cfg->src_mac, sizeof(mdest->src_mac));
        memcpy(mdest->dst_mac, cfg->dst_mac, sizeof(mdest->dst_mac));
        mdest->tos = cfg->tos;
        mdest->ttl = cfg->ttl;
        mdest->gre_protocol = cfg->gre_protocol ? cfg->gre_protocol
                                                : BCM_MIRROR_ERSPAN1_GRE_PROTOCOL_TYPE;
        if (cfg->vlan_valid && cfg->vlan_id >= 1u && cfg->vlan_id <= 4094u) {
            mdest->tpid = cfg->vlan_tpid ? cfg->vlan_tpid : 0x8100u;
            mdest->vlan_id = (bcm_vlan_t)BCM_VLAN_CTRL(cfg->vlan_pri & 0x7u,
                                                       cfg->vlan_cfi & 0x1u,
                                                       cfg->vlan_id & 0xfffu);
            mdest->pkt_prio = (uint8)(cfg->vlan_pri & 0x7u);
            mdest->cfi = (uint8)(cfg->vlan_cfi & 0x1u);
        } else {
            mdest->tpid = 0;
            mdest->vlan_id = 0;
        }
        break;

    default:
        hwbcm_log(HWBCM_LOG_ERR, "mirror: unsupported type %d", cfg->type);
        return BCM_E_PARAM;
    }

    return BCM_E_NONE;
}

/* ------------------------------------------------------------------------ */
/* mirror destinations                                                      */
/* ------------------------------------------------------------------------ */

int hwbcm_mirror_dest_create(const hwbcm_mirror_dest_cfg_t *cfg, int *dest_gport)
{
    bcm_mirror_destination_t mdest;
    int rv;

    if (cfg == NULL || dest_gport == NULL) {
        return BCM_E_PARAM;
    }

    rv = fill_mirror_dest(cfg, &mdest);
    if (BCM_FAILURE(rv)) {
        return rv;
    }

    rv = bcm_mirror_destination_create(U, &mdest);
    if (rv == BCM_E_EXISTS) {
        /* OpenBCM ESW _bcm_mirror_destination_match() matches any existing
         * non-tunnel destination with the same monitor gport, even when the
         * new destination is a tunnel (or a distinct SPAN session). Setting
         * BCM_MIRROR_DEST_REPLACE without WITH_ID skips that match check and
         * allocates a fresh mirror_dest_id. */
        mdest.flags |= BCM_MIRROR_DEST_REPLACE;
        mdest.mirror_dest_id = 0;
        rv = bcm_mirror_destination_create(U, &mdest);
    }

    if (BCM_FAILURE(rv)) {
        hwbcm_log(HWBCM_LOG_ERR,
                  "mirror: bcm_mirror_destination_create(%s, %s %d): %s (%d)",
                  type_name(cfg->type),
                  cfg->trunk_id >= 0 ? "trunk" : "port",
                  cfg->trunk_id >= 0 ? cfg->trunk_id : cfg->port,
                  bcm_errmsg(rv), rv);
        return rv;
    }

    *dest_gport = (int)mdest.mirror_dest_id;

    hwbcm_log(HWBCM_LOG_INFO,
              "mirror: created %s dest 0x%x -> %s %d (vlan=%u tc=%d)",
              type_name(cfg->type), (unsigned)*dest_gport,
              cfg->trunk_id >= 0 ? "trunk" : "port",
              cfg->trunk_id >= 0 ? cfg->trunk_id : cfg->port,
              cfg->vlan_valid ? (unsigned)cfg->vlan_id : 0u,
              cfg->tc);
    return 0;
}

int hwbcm_mirror_dest_replace(int dest_gport, const hwbcm_mirror_dest_cfg_t *cfg)
{
    bcm_mirror_destination_t mdest;
    int rv;

    if (cfg == NULL || dest_gport == -1) {
        return BCM_E_PARAM;
    }

    rv = fill_mirror_dest(cfg, &mdest);
    if (BCM_FAILURE(rv)) {
        return rv;
    }

    mdest.flags |= BCM_MIRROR_DEST_WITH_ID | BCM_MIRROR_DEST_REPLACE;
    mdest.mirror_dest_id = (bcm_gport_t)dest_gport;

    rv = bcm_mirror_destination_create(U, &mdest);
    if (BCM_FAILURE(rv)) {
        hwbcm_log(HWBCM_LOG_ERR,
                  "mirror: bcm_mirror_destination_create(REPLACE 0x%x, %s): %s (%d)",
                  (unsigned)dest_gport, type_name(cfg->type), bcm_errmsg(rv), rv);
        return rv;
    }

    hwbcm_log(HWBCM_LOG_INFO,
              "mirror: replaced %s dest 0x%x -> %s %d (vlan=%u tc=%d)",
              type_name(cfg->type), (unsigned)dest_gport,
              cfg->trunk_id >= 0 ? "trunk" : "port",
              cfg->trunk_id >= 0 ? cfg->trunk_id : cfg->port,
              cfg->vlan_valid ? (unsigned)cfg->vlan_id : 0u,
              cfg->tc);
    return 0;
}

int hwbcm_mirror_dest_destroy(int dest_gport)
{
    int rv;

    if (dest_gport == -1) {
        return BCM_E_PARAM;
    }

    rv = bcm_mirror_destination_destroy(U, (bcm_gport_t)dest_gport);
    if (BCM_FAILURE(rv)) {
        hwbcm_log(rv == BCM_E_BUSY ? HWBCM_LOG_WARN : HWBCM_LOG_ERR,
                  "mirror: bcm_mirror_destination_destroy(0x%x): %s (%d)",
                  (unsigned)dest_gport, bcm_errmsg(rv), rv);
        return rv;
    }

    hwbcm_log(HWBCM_LOG_INFO, "mirror: destroyed dest 0x%x", (unsigned)dest_gport);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* port mirroring                                                           */
/* ------------------------------------------------------------------------ */

int hwbcm_mirror_port_add(int port, uint32_t dir, int dest_gport)
{
    uint32 flags = dir_to_bcm_flags(dir);
    int rv;

    if (port < 0 || flags == 0 || dest_gport == -1) {
        return BCM_E_PARAM;
    }

    rv = bcm_mirror_port_dest_add(U, (bcm_port_t)port, flags, (bcm_gport_t)dest_gport);
    if (rv == BCM_E_EXISTS) {
        hwbcm_log(HWBCM_LOG_INFO,
                  "mirror: port %d dir 0x%x already bound to dest 0x%x",
                  port, (unsigned)dir, (unsigned)dest_gport);
        return 0;
    }
    if (BCM_FAILURE(rv)) {
        hwbcm_log(HWBCM_LOG_ERR,
                  "mirror: bcm_mirror_port_dest_add(port %d, dir 0x%x, dest 0x%x): %s (%d)",
                  port, (unsigned)dir, (unsigned)dest_gport, bcm_errmsg(rv), rv);
        return rv;
    }

    hwbcm_log(HWBCM_LOG_INFO, "mirror: port %d dir 0x%x += dest 0x%x",
              port, (unsigned)dir, (unsigned)dest_gport);
    return 0;
}

int hwbcm_mirror_port_delete(int port, uint32_t dir, int dest_gport)
{
    uint32 flags = dir_to_bcm_flags(dir);
    int rv;

    if (port < 0 || flags == 0 || dest_gport == -1) {
        return BCM_E_PARAM;
    }

    rv = bcm_mirror_port_dest_delete(U, (bcm_port_t)port, flags, (bcm_gport_t)dest_gport);
    if (rv == BCM_E_NOT_FOUND) {
        hwbcm_log(HWBCM_LOG_DEBUG,
                  "mirror: port %d dir 0x%x dest 0x%x not found on delete",
                  port, (unsigned)dir, (unsigned)dest_gport);
        return 0;
    }
    if (BCM_FAILURE(rv)) {
        hwbcm_log(HWBCM_LOG_ERR,
                  "mirror: bcm_mirror_port_dest_delete(port %d, dir 0x%x, dest 0x%x): %s (%d)",
                  port, (unsigned)dir, (unsigned)dest_gport, bcm_errmsg(rv), rv);
        return rv;
    }

    hwbcm_log(HWBCM_LOG_INFO, "mirror: port %d dir 0x%x -= dest 0x%x",
              port, (unsigned)dir, (unsigned)dest_gport);
    return 0;
}

int hwbcm_mirror_port_delete_all(int port, uint32_t dir)
{
    uint32 flags = dir_to_bcm_flags(dir);
    int rv;

    if (port < 0 || flags == 0) {
        return BCM_E_PARAM;
    }

    rv = bcm_mirror_port_dest_delete_all(U, (bcm_port_t)port, flags);
    if (BCM_FAILURE(rv)) {
        hwbcm_log(HWBCM_LOG_ERR,
                  "mirror: bcm_mirror_port_dest_delete_all(port %d, dir 0x%x): %s (%d)",
                  port, (unsigned)dir, bcm_errmsg(rv), rv);
        return rv;
    }

    hwbcm_log(HWBCM_LOG_INFO, "mirror: port %d dir 0x%x cleared all mirror dests",
              port, (unsigned)dir);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* debug dump                                                               */
/* ------------------------------------------------------------------------ */

static int dump_dest_cb(int unit, bcm_mirror_destination_t *mdest, void *user_data)
{
    int *count = (int *)user_data;

    (void)unit;
    if (mdest == NULL) {
        return BCM_E_NONE;
    }
    if (count != NULL) {
        (*count)++;
    }

    if (mdest->flags & BCM_MIRROR_DEST_TUNNEL_IP_GRE) {
        hwbcm_log(HWBCM_LOG_INFO,
                  "mirror dump: dest 0x%x ERSPAN v%u gport 0x%x src=%u.%u.%u.%u dst=%u.%u.%u.%u "
                  "tos=0x%02x ttl=%u gre=0x%04x tpid=0x%04x tci=0x%04x "
                  "smac=%02x:%02x:%02x:%02x:%02x:%02x dmac=%02x:%02x:%02x:%02x:%02x:%02x",
                  (unsigned)mdest->mirror_dest_id, (unsigned)mdest->version,
                  (unsigned)mdest->gport,
                  (unsigned)((mdest->src_addr >> 24) & 0xffu),
                  (unsigned)((mdest->src_addr >> 16) & 0xffu),
                  (unsigned)((mdest->src_addr >> 8) & 0xffu),
                  (unsigned)(mdest->src_addr & 0xffu),
                  (unsigned)((mdest->dst_addr >> 24) & 0xffu),
                  (unsigned)((mdest->dst_addr >> 16) & 0xffu),
                  (unsigned)((mdest->dst_addr >> 8) & 0xffu),
                  (unsigned)(mdest->dst_addr & 0xffu),
                  (unsigned)mdest->tos, (unsigned)mdest->ttl,
                  (unsigned)mdest->gre_protocol, (unsigned)mdest->tpid,
                  (unsigned)mdest->vlan_id,
                  mdest->src_mac[0], mdest->src_mac[1], mdest->src_mac[2],
                  mdest->src_mac[3], mdest->src_mac[4], mdest->src_mac[5],
                  mdest->dst_mac[0], mdest->dst_mac[1], mdest->dst_mac[2],
                  mdest->dst_mac[3], mdest->dst_mac[4], mdest->dst_mac[5]);
    } else if (mdest->flags & BCM_MIRROR_DEST_TUNNEL_L2) {
        hwbcm_log(HWBCM_LOG_INFO,
                  "mirror dump: dest 0x%x RSPAN gport 0x%x tpid=0x%04x tci=0x%04x",
                  (unsigned)mdest->mirror_dest_id, (unsigned)mdest->gport,
                  (unsigned)mdest->tpid, (unsigned)mdest->vlan_id);
    } else {
        hwbcm_log(HWBCM_LOG_INFO,
                  "mirror dump: dest 0x%x SPAN gport 0x%x flags=0x%x int_pri=%u",
                  (unsigned)mdest->mirror_dest_id, (unsigned)mdest->gport,
                  (unsigned)mdest->flags, (unsigned)mdest->int_pri);
    }
    return BCM_E_NONE;
}

int hwbcm_mirror_dump(void)
{
    int count = 0;
    int rv;

    rv = bcm_mirror_destination_traverse(U, dump_dest_cb, &count);
    if (BCM_FAILURE(rv)) {
        hwbcm_log(HWBCM_LOG_ERR, "mirror: bcm_mirror_destination_traverse: %s (%d)",
                  bcm_errmsg(rv), rv);
        return rv;
    }

    hwbcm_log(HWBCM_LOG_INFO, "mirror dump: %d destination(s)", count);
    return 0;
}
