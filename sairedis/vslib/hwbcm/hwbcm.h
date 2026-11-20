/*
 * hwbcm.h — thin C wrapper around the OpenBCM (sdk-6.5.27) bcm_* API for the
 * Trident2+ (BCM56864) hardware SAI in sonic-sairedis vslib.
 *
 * Rules:
 *  - This header is included from C++ (SwitchBCM56860HW*.cpp). It must NOT
 *    include any SDK header. Only plain C types here.
 *  - All functions return 0 on success and a negative BCM_E_* code on failure.
 *  - unit is always 0 on this box.
 *  - Implementations live in hwbcm_core.c (init/port/link/knet),
 *    hwbcm_l2.c (vlan/l2), hwbcm_l3.c (intf/egress/host/route),
 *    hwbcm_vxlan.c (tunnels). Each owner only edits their own section below.
 */
#ifndef HWBCM_H
#define HWBCM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* core (owner: A1)                                                         */
/* ------------------------------------------------------------------------ */

#define HWBCM_UNIT 0
#define HWBCM_MAX_PORTS 128

typedef enum {
    HWBCM_LOG_ERR = 0,
    HWBCM_LOG_WARN = 1,
    HWBCM_LOG_INFO = 2,
    HWBCM_LOG_DEBUG = 3,
} hwbcm_log_level_t;

void hwbcm_log_level_set(hwbcm_log_level_t level);
void hwbcm_log(hwbcm_log_level_t level, const char *fmt, ...);

/*
 * Initialize SAL, BDE, SOC and BCM layers exactly like bcm.user does:
 *   sal_config_file_set(config_bcm) -> sal_core_init -> sal_appl_init ->
 *   knet config -> sysconf_init -> sysconf_probe -> sysconf_attach(0) ->
 *   "init soc" / "init misc" / "init mmu" / "init bcm" via sh_process_command ->
 *   linkscan enable.
 * config_bcm: absolute path to the Broadcom config (SAI_INIT_CONFIG_FILE).
 * Idempotent: second call returns 0 without re-initializing.
 */
int hwbcm_init(const char *config_bcm);

/* Detach/cleanup (best effort, used on switch remove). */
int hwbcm_deinit(void);

/* Chip identification after init. */
int hwbcm_chip_info(uint16_t *dev_id, uint8_t *rev_id, char *name, size_t name_len);

/*
 * Port map: logical bcm port <-> first physical lane (portmap_<logical>=<phys>:..).
 * Returns number of front-panel ports found (excluding CPU/loopback).
 * logical[i], phys[i] filled for i < max.
 */
int hwbcm_port_map_get(int *logical, int *phys, int max);

/* Logical bcm port for a given first physical lane number; -1 if none. */
int hwbcm_port_from_phys(int phys);

/* Port name (xe0, ge1, ...) for diagnostics. */
int hwbcm_port_name(int port, char *buf, size_t len);

/* Admin state. */
int hwbcm_port_enable_set(int port, int enable);
int hwbcm_port_enable_get(int port, int *enable);

/* Speed in Mbps (1000, 10000, 40000). autoneg: 0/1. Sets interface
 * appropriately (SGMII for <=1G on copper PHYs, XFI for 10G). Also applies
 * the known SerDes work-around after link at 1G (see DESIGN.md). */
int hwbcm_port_speed_set(int port, int speed_mbps, int autoneg);
int hwbcm_port_speed_get(int port, int *speed_mbps);
int hwbcm_port_autoneg_get(int port, int *autoneg);

/* Operational link state: 1 up, 0 down. */
int hwbcm_port_link_get(int port, int *up);

/* MTU / max frame size in bytes. */
int hwbcm_port_mtu_set(int port, int mtu);
int hwbcm_port_mtu_get(int port, int *mtu);

/* Flow control (pause) and learning. */
int hwbcm_port_learn_set(int port, int learn_enable);
int hwbcm_port_pause_set(int port, int pause_tx, int pause_rx);

/* Port default VLAN (PVID) and untagged handling. */
int hwbcm_port_pvid_set(int port, int vid);

/* Port counters, a small fixed set so the C++ side needs no SDK enums. */
typedef struct {
    uint64_t rx_packets, rx_bytes, rx_errors, rx_discards;
    uint64_t tx_packets, tx_bytes, tx_errors, tx_discards;
    uint64_t rx_ucast, rx_mcast, rx_bcast, tx_ucast, tx_mcast, tx_bcast;
} hwbcm_port_stats_t;
int hwbcm_port_stats_get(int port, hwbcm_port_stats_t *stats);
int hwbcm_port_stats_clear(int port);

/*
 * Link state change callback. Registered once; invoked from the SDK linkscan
 * thread. The C++ side must marshal to its own thread (it is NOT safe to call
 * back into SAI synchronously from here).
 */
typedef void (*hwbcm_link_cb_t)(int port, int up, void *cookie);
int hwbcm_link_callback_register(hwbcm_link_cb_t cb, void *cookie);

/* Switch MAC (used for L3 interfaces and as source MAC). */
int hwbcm_switch_mac_set(const uint8_t mac[6]);

/*
 * KNET host interfaces (netdevs). Creates a netdev named `name` bound to
 * `port` (packets to/from that front-panel port appear on the netdev), and
 * the rx filter so that packets punted to CPU from that port reach it.
 * Returns the knet netif id (>0) via *netif_id.
 */
int hwbcm_knet_netif_create(int port, const char *name, const uint8_t mac[6], int *netif_id);
int hwbcm_knet_netif_destroy(int netif_id);
/* Netdev for the CPU/switch itself (e.g. for L3 packets to the router MAC). */
int hwbcm_knet_cpu_netif_create(const char *name, const uint8_t mac[6], int *netif_id);

/* Trap-to-CPU policy for well-known protocols (ARP, LLDP, LACP, BGP/IP2ME ...).
 * reason: one of HWBCM_TRAP_*; action: 0 drop, 1 copy to cpu, 2 trap (cpu only). */
#define HWBCM_TRAP_ARP      1
#define HWBCM_TRAP_LLDP     2
#define HWBCM_TRAP_LACP     3
#define HWBCM_TRAP_IP2ME    4
#define HWBCM_TRAP_DHCP     5
#define HWBCM_TRAP_BGP      6
#define HWBCM_TRAP_TTL_ERR  7
#define HWBCM_TRAP_NEIGH_MISS 8
#define HWBCM_TRAP_UDLD     9
#define HWBCM_TRAP_STP      10
int hwbcm_trap_set(int reason, int action, int priority_queue);

/* Run a diag-shell command (debug aid; output goes to the SDK console/log). */
int hwbcm_shell(const char *cmd);

/* ------------------------------------------------------------------------ */
/* L2 (owner: A1)                                                           */
/* ------------------------------------------------------------------------ */

int hwbcm_vlan_create(int vid);
int hwbcm_vlan_destroy(int vid);
/* tagged: 1 = egress tagged, 0 = untagged on this port */
int hwbcm_vlan_port_add(int vid, int port, int tagged);
int hwbcm_vlan_port_remove(int vid, int port);
/* Flooding / learning controls per VLAN. */
int hwbcm_vlan_learn_set(int vid, int enable);
int hwbcm_vlan_flood_set(int vid, int unknown_ucast, int unknown_mcast, int bcast);

/* Static MAC entry. port < 0 with trunk_id >= 0 for LAGs (optional). */
int hwbcm_l2_addr_add(const uint8_t mac[6], int vid, int port, int is_static);
int hwbcm_l2_addr_delete(const uint8_t mac[6], int vid);
int hwbcm_l2_flush_port(int port);
int hwbcm_l2_flush_vlan(int vid);
int hwbcm_l2_age_set(int seconds);

/* Learned-MAC event callback (for SAI FDB notifications). add: 1 learn, 0 age/delete. */
typedef void (*hwbcm_l2_cb_t)(const uint8_t mac[6], int vid, int port, int add, void *cookie);
int hwbcm_l2_callback_register(hwbcm_l2_cb_t cb, void *cookie);

/* LAG / trunk. */
int hwbcm_trunk_create(int *trunk_id);
int hwbcm_trunk_destroy(int trunk_id);
int hwbcm_trunk_member_set(int trunk_id, const int *ports, int nports);

/* ------------------------------------------------------------------------ */
/* L3 (owner: A3)                                                           */
/* ------------------------------------------------------------------------ */

/* L3 interface (router interface). vid: VLAN the RIF sits on (for a port RIF
 * the caller creates a dedicated VLAN and adds the port untagged). Returns intf_id. */
int hwbcm_l3_intf_create(const uint8_t mac[6], int vid, int vrf, int mtu, int *intf_id);
int hwbcm_l3_intf_destroy(int intf_id);
int hwbcm_l3_intf_mtu_set(int intf_id, int mtu);

/* Egress object (next hop): dst mac + port/trunk + intf. Returns egr_id. */
int hwbcm_l3_egress_create(int intf_id, const uint8_t dst_mac[6], int port, int trunk_id, int *egr_id);
int hwbcm_l3_egress_destroy(int egr_id);
/* Egress that traps to CPU (for neighbor-miss / glean). */
int hwbcm_l3_egress_cpu_create(int intf_id, int *egr_id);
/* Egress that drops (blackhole). */
int hwbcm_l3_egress_drop_create(int *egr_id);

/* ECMP group of egress objects. Returns ecmp_id (usable where egr_id is). */
int hwbcm_l3_ecmp_create(const int *egr_ids, int n, int *ecmp_id);
int hwbcm_l3_ecmp_set(int ecmp_id, const int *egr_ids, int n);
int hwbcm_l3_ecmp_destroy(int ecmp_id);

/* Host (neighbor, /32 or /128) entries. af: 4 or 6. addr: network order. */
int hwbcm_l3_host_add(int vrf, int af, const uint8_t *addr, int egr_id);
int hwbcm_l3_host_delete(int vrf, int af, const uint8_t *addr);

/* Route (prefix). egr_id may be an ECMP id. */
int hwbcm_l3_route_add(int vrf, int af, const uint8_t *prefix, int prefix_len, int egr_id);
int hwbcm_l3_route_delete(int vrf, int af, const uint8_t *prefix, int prefix_len);

/* VRF create/destroy (bcm_vrf). vrf 0 = default. */
int hwbcm_l3_vrf_create(int *vrf);
int hwbcm_l3_vrf_destroy(int vrf);

/* Enable IPv4/IPv6 routing on a VLAN interface / globally. */
int hwbcm_l3_enable_set(int enable);

/* ------------------------------------------------------------------------ */
/* VXLAN / EVPN (owner: A4)                                                 */
/* ------------------------------------------------------------------------ */

/* Global VXLAN init (bcm_vxlan_init, UDP dest port, etc.). */
int hwbcm_vxlan_init(int udp_dst_port);

/* VPN (= one VNI / one L2 segment). Returns vpn_id. bcast_group: multicast
 * group used for flooding (created internally if <0). */
int hwbcm_vxlan_vpn_create(uint32_t vni, int vid, int *vpn_id);
int hwbcm_vxlan_vpn_destroy(int vpn_id);

/* Tunnel initiator/terminator pair for a remote VTEP. Returns tunnel ids. */
int hwbcm_vxlan_tunnel_create(const uint8_t src_ip[4], const uint8_t dst_ip[4], int ttl,
                              int *tunnel_init_id, int *tunnel_term_id);
int hwbcm_vxlan_tunnel_destroy(int tunnel_init_id, int tunnel_term_id);

/* Network-side virtual port (remote VTEP in a VPN). Returns vp_id (gport). */
int hwbcm_vxlan_network_port_add(int vpn_id, int tunnel_init_id, int tunnel_term_id,
                                 int l3_egr_id, int *vp_id);
/* Access-side virtual port (local VLAN/port into the VPN). */
int hwbcm_vxlan_access_port_add(int vpn_id, int port, int vid, int *vp_id);
int hwbcm_vxlan_port_delete(int vpn_id, int vp_id);

/* MAC on a VXLAN VP (remote EVPN type-2 MAC). */
int hwbcm_vxlan_l2_addr_add(const uint8_t mac[6], int vpn_id, int vp_id, int is_static);
int hwbcm_vxlan_l2_addr_delete(const uint8_t mac[6], int vpn_id);

/* ------------------------------------------------------------------------ */
/* owner-specific additions go below this line, in your own section only.   */
/* ------------------------------------------------------------------------ */

/* A1 additions: */

/* Current switch MAC as given to hwbcm_switch_mac_set; BCM_E_NOT_FOUND if unset. */
int hwbcm_switch_mac_get(uint8_t mac[6]);

/* Static/dynamic MAC entry pointing at a trunk (LAG) instead of a port. */
int hwbcm_l2_addr_add_trunk(const uint8_t mac[6], int vid, int trunk_id, int is_static);

/* Flush dynamic entries learned on a trunk. */
int hwbcm_l2_flush_trunk(int trunk_id);

/* Make the CPU port a tagged member of a VLAN (done automatically by
 * hwbcm_vlan_create and for VLAN 1 at init; exposed for VLANs created
 * elsewhere, e.g. by the L3/VXLAN code). */
int hwbcm_vlan_cpu_add(int vid);

/* In hwbcm_l2_cb_t, an entry that sits on a trunk is reported with a
 * negative port: port = -(1 + trunk_id). */
#define HWBCM_L2_CB_TRUNK_PORT(tid)   (-(1 + (tid)))
#define HWBCM_L2_CB_IS_TRUNK(port)    ((port) < 0)
#define HWBCM_L2_CB_TRUNK_ID(port)    (-(port) - 1)

/* A3 additions: */

/* Rewrite an existing egress object in place (BCM_L3_WITH_ID | BCM_L3_REPLACE),
 * keeping its id so routes / ECMP groups that reference it stay valid.
 * Used when a neighbor resolves (CPU egress -> real mac/port) or goes away
 * (real -> CPU). trunk_id >= 0 selects the trunk, otherwise port is used. */
int hwbcm_l3_egress_replace(int egr_id, int intf_id, const uint8_t dst_mac[6], int port, int trunk_id);
int hwbcm_l3_egress_cpu_replace(int egr_id, int intf_id);

/* 1 if id was created by hwbcm_l3_ecmp_create (route/host add sets
 * BCM_L3_MULTIPATH automatically), 0 otherwise. */
int hwbcm_l3_is_ecmp(int id);

/* A4 additions: */

/*
 * TD2+ notes (see hwbcm_vxlan.c header comment for the full list):
 *  - A network VP needs an L3 egress created with BCM_L3_VXLAN_ONLY; a plain
 *    egress from hwbcm_l3_egress_create() is rejected (BCM_E_CONFIG). Use
 *    hwbcm_vxlan_egress_from_l3() to clone an existing unicast egress / first
 *    ECMP member into a VXLAN-only egress.
 *  - One network VP per remote VTEP (the VN_ID match is tunnel-based, shared by
 *    all VPNs). The VP is created inside one VPN (the SDK requires a valid VPN)
 *    and added to the flood groups of every other VPN with
 *    hwbcm_vxlan_vpn_flood_set(). bcm_vxlan_vpn_destroy() deletes every VP in
 *    that VPN, so the C++ side re-homes network VPs before destroying a VPN.
 *  - Tunnel ids: hwbcm_vxlan_tunnel_create() returns tunnel_term_id ==
 *    tunnel_init_id (the terminator is created WITH_ID on the initiator id).
 */

/* Tunnel pair with DSCP control: dscp < 0 copies the inner DSCP (uniform),
 * 0..63 sets a fixed outer DSCP (pipe). ttl 1..255. */
int hwbcm_vxlan_tunnel_create2(const uint8_t src_ip[4], const uint8_t dst_ip[4], int ttl, int dscp,
                               int *tunnel_init_id, int *tunnel_term_id);

/* VXLAN-only L3 egress (BCM_L3_VXLAN_ONLY). trunk_id >= 0 wins over port. */
int hwbcm_vxlan_egress_create(int intf_id, const uint8_t dst_mac[6], int port, int trunk_id, int vid,
                              int *egr_id);
/* Clone an existing unicast egress (or the first member of an ECMP group) into
 * a VXLAN-only egress. *port receives the local bcm port (-1 for a trunk/CPU). */
int hwbcm_vxlan_egress_from_l3(int l3_egr_or_ecmp_id, int *vxlan_egr_id, int *port);
int hwbcm_vxlan_egress_destroy(int egr_id);
/* Compare a VXLAN-only clone with its source egress (A3 rewrites egresses in
 * place when a neighbor goes away / comes back). *stale = 1 when the source
 * now has a different intf/mac/port/trunk or became a CPU/drop egress. */
int hwbcm_vxlan_egress_is_stale(int l3_egr_or_ecmp_id, int vxlan_egr_id, int *stale);

/* Access VP with tagging and LAG support. tagged: 1 = egress frames toward
 * this port carry the VLAN tag (SD-tag add), 0 = untagged. trunk_id >= 0 for a
 * LAG (port ignored). hwbcm_vxlan_access_port_add() == (trunk -1, tagged 1). */
int hwbcm_vxlan_access_port_add2(int vpn_id, int port, int trunk_id, int vid, int tagged, int *vp_id);

/* Add (add=1) or remove (add=0) a VP in the flood groups (bcast / unknown
 * ucast / unknown mcast) of a VPN. Used for head-end replication: every remote
 * VTEP's network VP goes into every VPN's flood group. Idempotent. */
int hwbcm_vxlan_vpn_flood_set(int vpn_id, int vp_id, int add);

/* MY_STATION entry for the router MAC so IPv4/IPv6/ARP packets to it are
 * terminated (needed for the underlay to accept VXLAN packets). vid 0 = any
 * VLAN. BCM_E_EXISTS is treated as success (*station_id = -1). */
int hwbcm_vxlan_station_add(const uint8_t mac[6], int vid, int *station_id);
int hwbcm_vxlan_station_delete(int station_id);

/* Enable/disable VXLAN termination on an underlay (network) port
 * (bcmPortControlVxlanEnable). Called automatically by network_port_add. */
int hwbcm_vxlan_port_network_enable(int port, int enable);

/* Flush all L2 entries of a VPN (remote MACs) — used before VPN destroy. */
int hwbcm_vxlan_l2_flush_vpn(int vpn_id);

/* L2 learn/age events for MACs in a VPN carry vid = VPN id and port = VXLAN VP
 * gport. Describe such a VP: is_network (1 = remote VTEP, 0 = access), the
 * physical port (-1 if trunk) / trunk id (-1 if port), and the VPN it belongs
 * to. Returns BCM_E_NOT_FOUND when gport is not one of our VPs. */
int hwbcm_vxlan_vp_info(int vp_gport, int *is_network, int *port, int *trunk_id, int *vpn_id);

#ifdef __cplusplus
}
#endif
#endif /* HWBCM_H */
