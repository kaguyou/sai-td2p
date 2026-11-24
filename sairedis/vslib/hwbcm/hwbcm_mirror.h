/*
 * hwbcm_mirror.h — port mirroring (SPAN / RSPAN / ERSPAN) section of the
 * hwbcm C layer for Trident2+ (BCM56864), OpenBCM sdk-6.5.27.
 *
 * Same rules as hwbcm.h: included from C++ (SwitchBCM56860HW_mirror.cpp), no
 * SDK header here, plain C types only, 0 on success / negative BCM_E_* on
 * failure, unit is always HWBCM_UNIT. Implementation: hwbcm_mirror.c.
 *
 * Model (TD2+ / BCM56860_A0, src/bcm/esw/mirror.c):
 *  - Directed mirroring is always enabled on TD2+ (bcmSwitchDirectedMirroring
 *    is unconditionally 1 for SOC_IS_TD_TT).
 *  - One SAI MIRROR_SESSION = one bcm mirror destination descriptor allocated
 *    by bcm_mirror_destination_create(). Its bcm_gport_t (BCM_GPORT_MIRROR_*)
 *    is passed to bcm_mirror_port_dest_add/delete for port mirroring and to
 *    SwitchBCM56860HW::hwAclMirrorSessionUpdated() for ACL mirroring
 *    (bcmFieldActionMirrorIngress / bcmFieldActionMirrorEgress).
 *  - Hardware MTP table has 4 shared slots (BCM_MIRROR_MTP_COUNT = 4). An MTP
 *    slot (and EGR_MIRROR_ENCAP_CONTROL/DATA_1 profile for RSPAN/ERSPAN) is
 *    reserved when the first port or FP entry attaches to the destination, and
 *    freed when the last user detaches.
 *  - ERSPAN on TD2+ is IPv4 L3 GRE (BCM_MIRROR_DEST_TUNNEL_IP_GRE, 4-byte GRE
 *    header with configurable protocol, default 0x88be). Outer VLAN tag is
 *    added when vlan_valid != 0 and 1 <= vlan_id <= 4094.
 */
#ifndef HWBCM_MIRROR_H
#define HWBCM_MIRROR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* BCM_E_* codes tested on the C++ side (include/shared/error.h). */
#define HWBCM_MIRROR_E_PARAM      (-4)
#define HWBCM_MIRROR_E_NOT_FOUND  (-7)
#define HWBCM_MIRROR_E_EXISTS     (-8)
#define HWBCM_MIRROR_E_BUSY       (-10)
#define HWBCM_MIRROR_E_RESOURCE   (-14)
#define HWBCM_MIRROR_E_UNAVAIL    (-16)

/* TD2+ hardware MTP slot count (BCM_MIRROR_MTP_COUNT). */
#define HWBCM_MIRROR_MAX_MTPS     4

/* Session encapsulation type. */
#define HWBCM_MIRROR_TYPE_LOCAL   0   /* SPAN (no tunnel encapsulation) */
#define HWBCM_MIRROR_TYPE_RSPAN   1   /* RSPAN (L2 VLAN tag tunnel) */
#define HWBCM_MIRROR_TYPE_ERSPAN  2   /* ERSPAN (IPv4 L3 GRE tunnel) */

/* Port mirroring direction flags (bitmask). */
#define HWBCM_MIRROR_DIR_INGRESS  (1u << 0)
#define HWBCM_MIRROR_DIR_EGRESS   (1u << 1)

/* Default ERSPAN GRE protocol type (ERSPAN Type I/II ethertype). */
#define HWBCM_MIRROR_GRE_PROTO_DEFAULT 0x88beu

/* Mirror destination parameters. IPv4 addresses are in host byte order;
 * MAC addresses are in network byte order (as in SAI). */
typedef struct {
    int type;                /* HWBCM_MIRROR_TYPE_* */

    /* Monitor destination: trunk_id >= 0 selects a LAG trunk; otherwise port. */
    int port;                /* bcm logical port, -1 if trunk */
    int trunk_id;            /* bcm trunk id, -1 if port */

    /* Internal priority (TC) override (BCM_MIRROR_DEST_INT_PRI_SET):
     * 0..15 to override, -1 to leave packet priority unchanged. */
    int tc;

    /* Outer VLAN tag (always used for RSPAN; used for ERSPAN when vlan_valid != 0). */
    int vlan_valid;          /* 1 = insert outer VLAN tag, 0 = untagged outer */
    uint16_t vlan_tpid;      /* outer TPID (0 -> 0x8100) */
    uint16_t vlan_id;        /* 1..4094 */
    uint8_t vlan_pri;        /* PCP 0..7 */
    uint8_t vlan_cfi;        /* CFI/DEI 0..1 */

    /* ERSPAN (HWBCM_MIRROR_TYPE_ERSPAN, IPv4 L3 GRE). */
    uint32_t src_ip;         /* host byte order */
    uint32_t dst_ip;         /* host byte order */
    uint8_t src_mac[6];
    uint8_t dst_mac[6];
    uint8_t tos;             /* DSCP << 2 | ECN */
    uint8_t ttl;             /* IPv4 TTL */
    uint16_t gre_protocol;   /* 0 -> HWBCM_MIRROR_GRE_PROTO_DEFAULT (0x88be) */
    int payload_untagged;    /* 1 = strip inner VLAN tag (BCM_MIRROR_DEST_PAYLOAD_UNTAGGED) */
} hwbcm_mirror_dest_cfg_t;

/* Zero a config struct and apply defaults (port = -1, trunk_id = -1, tc = -1,
 * ttl = 255, vlan_tpid = 0x8100, gre_protocol = 0x88be). */
void hwbcm_mirror_dest_cfg_init(hwbcm_mirror_dest_cfg_t *cfg);

/* Allocate a bcm mirror destination. *dest_gport receives the bcm_gport_t
 * (BCM_GPORT_MIRROR_*) for port mirroring and ACL mirror actions. */
int hwbcm_mirror_dest_create(const hwbcm_mirror_dest_cfg_t *cfg, int *dest_gport);

/* Update an existing mirror destination in place (BCM_MIRROR_DEST_WITH_ID |
 * BCM_MIRROR_DEST_REPLACE), keeping dest_gport. OpenBCM ESW only rewrites the
 * software descriptor here; callers must detach/re-attach ports and reprogram
 * ACL entries so hardware MTP and EGR_MIRROR_ENCAP entries update. */
int hwbcm_mirror_dest_replace(int dest_gport, const hwbcm_mirror_dest_cfg_t *cfg);

/* Destroy a mirror destination. Returns HWBCM_MIRROR_E_BUSY if any port or
 * FP entry still holds a reference to it. */
int hwbcm_mirror_dest_destroy(int dest_gport);

/* Attach/detach dest_gport on a bcm logical port for direction(s) in dir
 * (HWBCM_MIRROR_DIR_INGRESS | HWBCM_MIRROR_DIR_EGRESS).
 * port_add treats BCM_E_EXISTS as 0; port_delete treats BCM_E_NOT_FOUND as 0. */
int hwbcm_mirror_port_add(int port, uint32_t dir, int dest_gport);
int hwbcm_mirror_port_delete(int port, uint32_t dir, int dest_gport);
int hwbcm_mirror_port_delete_all(int port, uint32_t dir);

/* Log all currently installed mirror destinations at INFO level. */
int hwbcm_mirror_dump(void);

/* Warm boot helper (Phase 3): read-only check whether dest_gport exists in SDK
 * (1 = exists, 0 = not found). */
int hwbcm_mirror_dest_exists(int dest_gport);

#ifdef __cplusplus
}
#endif
#endif /* HWBCM_MIRROR_H */
