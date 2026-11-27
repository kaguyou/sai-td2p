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

/* OpenBCM _shr_error_t (include/shared/error.h) codes exposed to C++ without
 * including SDK headers. */
#define HWBCM_E_NONE       0
#define HWBCM_E_INTERNAL  (-1)
#define HWBCM_E_MEMORY    (-2)
#define HWBCM_E_UNIT      (-3)
#define HWBCM_E_PARAM     (-4)
#define HWBCM_E_EMPTY     (-5)
#define HWBCM_E_FULL      (-6)
#define HWBCM_E_NOT_FOUND (-7)
#define HWBCM_E_EXISTS    (-8)
#define HWBCM_E_TIMEOUT   (-9)
#define HWBCM_E_BUSY      (-10)
#define HWBCM_E_FAIL      (-11)
#define HWBCM_E_RESOURCE  (-14)
#define HWBCM_E_UNAVAIL   (-16)
#define HWBCM_E_PORT      (-18)

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

/*
 * Extended initialization with Level 2 Warm Boot support.
 * warm_boot: 0 = cold boot (identical to hwbcm_init, with an in-memory L2
 *            scache buffer armed so a subsequent warm shutdown can sync);
 *            1 = warm boot (SOC_WARM_BOOT_START, load scache_file, soc_init /
 *            soc_misc_init / soc_mmu_init / bcm_init without resetting ASIC,
 *            ports, VLANs, KNET, or RTAG7 hash, plus SGMII state pre-population).
 * scache_file: path to L2 scache file (may be NULL on cold boot; required on
 *              warm boot).
 */
int hwbcm_init_ex(const char *config_bcm, int warm_boot, const char *scache_file);

/* Sync SDK Level 2 scache (bcmSwitchControlSync=1) and atomically flush to
 * scache_file (via .tmp + fsync + rename). If scache_file is NULL, uses the
 * path supplied to hwbcm_init_ex(). Optional *bytes_written receives the
 * flushed file size. */
int hwbcm_warm_sync(const char *scache_file, size_t *bytes_written);

/* Non-destructive warm shutdown: stops shell socket and linkscan callback
 * WITHOUT destroying KNET netifs/filters or resetting the ASIC, then runs
 * bcm_tx_cleanup(0) -> _bcm_shutdown(0) -> soc_shutdown(0) with
 * warmboot_knet_shutdown_mode=1. */
int hwbcm_warm_deinit(void);

/* Phase 4. for_kexec=0: same as hwbcm_warm_deinit(). for_kexec=1 (system
 * warm-reboot, selected by PRE_SHUTDOWN): netifs/filters are still kept, but
 * every KNET RX/TX DMA channel is aborted (KCOM_M_HW_RESET) before
 * _bcm_shutdown/soc_shutdown, the CMIC DMA engines are read back afterwards
 * and the verdict is written to <base>.dma_idle ("ok ..."/"busy ..."). Returns
 * BCM_E_BUSY if anything is still active or a step failed. */
int hwbcm_warm_deinit_ex(int for_kexec);

/* Kernel KNET state verdict of a warm init (computed before soc_init from
 * /proc/bcm/knet/link, <base>.bootid and <base>.dma_idle). */
#define HWBCM_KNET_LIVE      0   /* syncd restart: rings + netifs alive (Case A) */
#define HWBCM_KNET_DMA_RESET 1   /* netifs alive, rings aborted: HW_INIT, Case A */
#define HWBCM_KNET_FRESH     2   /* 0 netifs (kexec / module reload): HW_INIT, Case B */
#define HWBCM_KNET_UNKNOWN   3   /* /proc unreadable: HW_INIT, case by netif table */
int hwbcm_warm_knet_probe(const char *scache_file);   /* recompute + log, returns state */
int hwbcm_warm_knet_state(void);  /* last verdict, -1 if none (cold build / cold boot) */
int hwbcm_warm_knet_fresh(void);  /* last verdict == HWBCM_KNET_FRESH */

/* Returns 1 if the active SDK session was initialized via warm boot, else 0. */
int hwbcm_is_warm_boot(void);

/* Detach/cleanup (best effort, used on cold switch remove). */
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

/* Port counters by id, for SAI get_port_stats. The C++ side maps
 * SAI_PORT_STAT_* to these ids; hwbcm_core.c maps them to bcm_stat_val_t.
 * HWBCM_PSTAT_NONE (no hardware counter) reads as 0. The two NON_UCAST ids
 * are the sum of the multicast and broadcast counters. */
typedef enum {
    HWBCM_PSTAT_NONE = 0,
    HWBCM_PSTAT_IF_IN_OCTETS,
    HWBCM_PSTAT_IF_IN_UCAST_PKTS,
    HWBCM_PSTAT_IF_IN_NON_UCAST_PKTS,
    HWBCM_PSTAT_IF_IN_DISCARDS,
    HWBCM_PSTAT_IF_IN_ERRORS,
    HWBCM_PSTAT_IF_IN_UNKNOWN_PROTOS,
    HWBCM_PSTAT_IF_IN_BROADCAST_PKTS,
    HWBCM_PSTAT_IF_IN_MULTICAST_PKTS,
    HWBCM_PSTAT_IF_OUT_OCTETS,
    HWBCM_PSTAT_IF_OUT_UCAST_PKTS,
    HWBCM_PSTAT_IF_OUT_NON_UCAST_PKTS,
    HWBCM_PSTAT_IF_OUT_DISCARDS,
    HWBCM_PSTAT_IF_OUT_ERRORS,
    HWBCM_PSTAT_IF_OUT_QLEN,
    HWBCM_PSTAT_IF_OUT_BROADCAST_PKTS,
    HWBCM_PSTAT_IF_OUT_MULTICAST_PKTS,
    HWBCM_PSTAT_ETHER_STATS_DROP_EVENTS,
    HWBCM_PSTAT_ETHER_STATS_MULTICAST_PKTS,
    HWBCM_PSTAT_ETHER_STATS_BROADCAST_PKTS,
    HWBCM_PSTAT_ETHER_STATS_UNDERSIZE_PKTS,
    HWBCM_PSTAT_ETHER_STATS_FRAGMENTS,
    HWBCM_PSTAT_ETHER_STATS_PKTS_64_OCTETS,
    HWBCM_PSTAT_ETHER_STATS_PKTS_65_TO_127_OCTETS,
    HWBCM_PSTAT_ETHER_STATS_PKTS_128_TO_255_OCTETS,
    HWBCM_PSTAT_ETHER_STATS_PKTS_256_TO_511_OCTETS,
    HWBCM_PSTAT_ETHER_STATS_PKTS_512_TO_1023_OCTETS,
    HWBCM_PSTAT_ETHER_STATS_PKTS_1024_TO_1518_OCTETS,
    HWBCM_PSTAT_ETHER_STATS_OVERSIZE_PKTS,
    HWBCM_PSTAT_ETHER_RX_OVERSIZE_PKTS,
    HWBCM_PSTAT_ETHER_TX_OVERSIZE_PKTS,
    HWBCM_PSTAT_ETHER_STATS_JABBERS,
    HWBCM_PSTAT_ETHER_STATS_OCTETS,
    HWBCM_PSTAT_ETHER_STATS_PKTS,
    HWBCM_PSTAT_ETHER_STATS_COLLISIONS,
    HWBCM_PSTAT_ETHER_STATS_CRC_ALIGN_ERRORS,
    HWBCM_PSTAT_ETHER_STATS_TX_NO_ERRORS,
    HWBCM_PSTAT_ETHER_STATS_RX_NO_ERRORS,
    HWBCM_PSTAT_DOT3_STATS_FCS_ERRORS,
    HWBCM_PSTAT_PAUSE_RX_PKTS,
    HWBCM_PSTAT_PAUSE_TX_PKTS,
    HWBCM_PSTAT_ETHER_IN_PKTS_64_OCTETS,
    HWBCM_PSTAT_ETHER_IN_PKTS_65_TO_127_OCTETS,
    HWBCM_PSTAT_ETHER_IN_PKTS_128_TO_255_OCTETS,
    HWBCM_PSTAT_ETHER_IN_PKTS_256_TO_511_OCTETS,
    HWBCM_PSTAT_ETHER_IN_PKTS_512_TO_1023_OCTETS,
    HWBCM_PSTAT_ETHER_IN_PKTS_1024_TO_1518_OCTETS,
    HWBCM_PSTAT_ETHER_IN_PKTS_1519_TO_2047_OCTETS,
    HWBCM_PSTAT_ETHER_IN_PKTS_2048_TO_4095_OCTETS,
    HWBCM_PSTAT_ETHER_IN_PKTS_4096_TO_9216_OCTETS,
    HWBCM_PSTAT_ETHER_IN_PKTS_9217_TO_16383_OCTETS,
    HWBCM_PSTAT_ETHER_OUT_PKTS_64_OCTETS,
    HWBCM_PSTAT_ETHER_OUT_PKTS_65_TO_127_OCTETS,
    HWBCM_PSTAT_ETHER_OUT_PKTS_128_TO_255_OCTETS,
    HWBCM_PSTAT_ETHER_OUT_PKTS_256_TO_511_OCTETS,
    HWBCM_PSTAT_ETHER_OUT_PKTS_512_TO_1023_OCTETS,
    HWBCM_PSTAT_ETHER_OUT_PKTS_1024_TO_1518_OCTETS,
    HWBCM_PSTAT_ETHER_OUT_PKTS_1519_TO_2047_OCTETS,
    HWBCM_PSTAT_ETHER_OUT_PKTS_2048_TO_4095_OCTETS,
    HWBCM_PSTAT_ETHER_OUT_PKTS_4096_TO_9216_OCTETS,
    HWBCM_PSTAT_ETHER_OUT_PKTS_9217_TO_16383_OCTETS,
    HWBCM_PSTAT_PFC_0_RX_PKTS, HWBCM_PSTAT_PFC_1_RX_PKTS,
    HWBCM_PSTAT_PFC_2_RX_PKTS, HWBCM_PSTAT_PFC_3_RX_PKTS,
    HWBCM_PSTAT_PFC_4_RX_PKTS, HWBCM_PSTAT_PFC_5_RX_PKTS,
    HWBCM_PSTAT_PFC_6_RX_PKTS, HWBCM_PSTAT_PFC_7_RX_PKTS,
    HWBCM_PSTAT_PFC_0_TX_PKTS, HWBCM_PSTAT_PFC_1_TX_PKTS,
    HWBCM_PSTAT_PFC_2_TX_PKTS, HWBCM_PSTAT_PFC_3_TX_PKTS,
    HWBCM_PSTAT_PFC_4_TX_PKTS, HWBCM_PSTAT_PFC_5_TX_PKTS,
    HWBCM_PSTAT_PFC_6_TX_PKTS, HWBCM_PSTAT_PFC_7_TX_PKTS,
    HWBCM_PSTAT_COUNT
} hwbcm_pstat_t;

/* Load-balancing hash (RTAG7). Hash set HWBCM_HASH_ECMP uses RTAG7 hash A
 * (field set 0, seed 0, ECMP offset 0 = A0); HWBCM_HASH_LAG uses hash B
 * (field set 1, seed 1, trunk offset 16 = B0), so ECMP and LAG do not
 * polarise. The VXLAN outer UDP source port (entropy) comes from hash A.
 * hwbcm_init programs HWBCM_HASH_F_DEFAULT on both. */
#define HWBCM_HASH_ECMP 0
#define HWBCM_HASH_LAG  1
#define HWBCM_HASH_F_SRC_IP    0x0001
#define HWBCM_HASH_F_DST_IP    0x0002
#define HWBCM_HASH_F_L4_SRC    0x0004
#define HWBCM_HASH_F_L4_DST    0x0008
#define HWBCM_HASH_F_PROTOCOL  0x0010
#define HWBCM_HASH_F_IN_PORT   0x0020
#define HWBCM_HASH_F_VLAN      0x0040
#define HWBCM_HASH_F_ETHERTYPE 0x0080
#define HWBCM_HASH_F_SRC_MAC   0x0100
#define HWBCM_HASH_F_DST_MAC   0x0200
#define HWBCM_HASH_F_DEFAULT   (HWBCM_HASH_F_SRC_IP | HWBCM_HASH_F_DST_IP | \
                                HWBCM_HASH_F_L4_SRC | HWBCM_HASH_F_L4_DST | \
                                HWBCM_HASH_F_PROTOCOL | HWBCM_HASH_F_IN_PORT)
int hwbcm_hash_fields_set(int set, uint32_t fields);
int hwbcm_hash_seed_set(int set, uint32_t seed);

/* Read n counters (ids[i] is a hwbcm_pstat_t) of one port into out[i].
 * A counter the chip does not have (BCM_E_UNAVAIL) reads as 0. */
int hwbcm_port_stat_multi_get(int port, int n, const int *ids, uint64_t *out);
/* Zero n counters of one port (SAI READ_AND_CLEAR / clear_port_stats). */
int hwbcm_port_stat_multi_clear(int port, int n, const int *ids);

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
int hwbcm_l2_flush_vlan_port(int vid, int port);   /* dynamic entries of one (vlan, port) */
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
/* Multicast (BUM) network VP toward the same VTEP: takes an egress made by
 * hwbcm_vxlan_egress_from_l3_mc() and is the VP that joins the flood groups. */
int hwbcm_vxlan_network_port_add_mc(int vpn_id, int tunnel_init_id, int tunnel_term_id,
                                    int ipmc_egr_id, int *vp_id);
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

/* Warm boot helpers (Phase 1 & Phase 2). */
typedef struct {
    int netif_id;
    int port;            /* front-panel logical bcm port, -1 for CPU netif */
    int is_cpu;
    int pvid;            /* PVID from prio-49 strip filter, 0 if none */
    char name[32];
} hwbcm_knet_info_t;

/* Traverse live kernel KNET netifs & filters, repopulate g_knet[] with zero
 * KNET mutations, and fill out[0..max-1]. Returns total recovered netifs (>=0)
 * or <0 on error. */
int hwbcm_knet_warm_recover(hwbcm_knet_info_t *out, int max);

/* Core SW state persisted in .hwstate (IP2ME stations, L2 cache trap indices,
 * switch MAC). */
typedef struct {
    uint8_t switch_mac[6];
    int switch_mac_valid;
    int ip2me_station_id;
    int ip2me_vfi_station_id;
    int ip2me_wanted;
    int trap_l2cache_idx[HWBCM_TRAP_STP + 1];
} hwbcm_core_wb_state_t;

void hwbcm_core_wb_get(hwbcm_core_wb_state_t *out);
void hwbcm_core_wb_restore(const hwbcm_core_wb_state_t *in);
/* Restore g_desired_speed[port] in SW only (no HW write). */
void hwbcm_port_desired_speed_restore(int port, int speed_mbps);

/* Hash SW state persisted in .hwstate (RTAG7 fields & seeds for ECMP/LAG). */
typedef struct {
    uint32_t fields[2];  /* [HWBCM_HASH_ECMP], [HWBCM_HASH_LAG] */
    uint32_t seed[2];    /* [HWBCM_HASH_ECMP], [HWBCM_HASH_LAG] */
} hwbcm_hash_wb_state_t;

void hwbcm_hash_wb_get(hwbcm_hash_wb_state_t *out);
void hwbcm_hash_wb_restore(const hwbcm_hash_wb_state_t *in);

/* VXLAN / EVPN / RIOT SW state persisted in .hwstate (Phase 3). */
typedef struct {
    int vp_idx;
    int is_network;
    int port_gport;
    int egr_id;
    int intf_vid;
    int vpn_id;
    int uvid;
} hwbcm_vxlan_vp_wb_t;

typedef struct {
    int vid;
    int intf_id;
    int refcnt;
} hwbcm_vxlan_acc_intf_wb_t;

typedef struct {
    int vfi;
    int ovl_intf;
    uint8_t mac[6];
} hwbcm_vxlan_vpn_l3_wb_t;

void hwbcm_vxlan_wb_get_globals(int *inited, int *udp_port);
/* Pass out == NULL or max == 0 to query total active count. */
int  hwbcm_vxlan_wb_get_vps(hwbcm_vxlan_vp_wb_t *out, int max);
int  hwbcm_vxlan_wb_get_acc_intfs(hwbcm_vxlan_acc_intf_wb_t *out, int max);
int  hwbcm_vxlan_wb_get_vpn_l3(hwbcm_vxlan_vpn_l3_wb_t *out, int max);
/* Restore VXLAN SW tables with ZERO HW writes (never calls bcm_vxlan_init). */
int  hwbcm_vxlan_warm_restore(int inited, int udp_port,
                              const hwbcm_vxlan_vp_wb_t *vps, int nvps,
                              const hwbcm_vxlan_acc_intf_wb_t *acc_intfs, int nacc,
                              const hwbcm_vxlan_vpn_l3_wb_t *vpn_l3, int nvpn_l3);
void hwbcm_vxlan_warm_reset(void);

/* Reset L2 callback registration state (called during warm deinit). */
void hwbcm_l2_warm_reset(void);

/* Cross-check a trunk id during warm boot; returns member count (>=0) or <0. */
int hwbcm_trunk_member_count_get(int trunk_id);

/* Probe L3 egress/ingress mode from SDK without writing HW, and restore the
 * software VRF bitmap and ECMP id list. */
int hwbcm_l3_warm_restore(const int *vrfs, int nvrfs, const int *ecmp_ids, int necmp);
/* Reset L3 SW state on deinit. */
void hwbcm_l3_warm_reset(void);

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
/* Same, with BCM_L3_IPMC: the egress for the BUM (flood) copy. */
int hwbcm_vxlan_egress_from_l3_mc(int l3_egr_or_ecmp_id, int *vxlan_egr_id, int *port);
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

/* RIOT (config.bcm riot_enable=1): routing in and out of VXLAN VFIs. */
int hwbcm_riot_enabled(void);
/* L3 ingress mode (on with RIOT): every L3 intf has an ingress object of the
 * same id. hwbcm_l3_iif_create makes/replaces one with the given VRF. */
int hwbcm_l3_ingress_mode(void);
int hwbcm_l3_iif_create(int intf_id, int vrf);

/* L3 gateway (SVI) on a VXLAN VPN: overlay L3 intf on the VFI with the router
 * MAC, its ingress object (VRF), VFI ingress_if, non-unicast copy to CPU,
 * router MAC -> CPU in the VFI, IPv4/IPv6 on all VPs of the VPN (and on VPs
 * created later). Idempotent; returns the overlay intf (egress for routing
 * into the VPN). BCM_E_UNAVAIL without RIOT. */
int hwbcm_vxlan_vpn_l3_bind(int vpn_id, const uint8_t mac[6], int vrf, int mtu, int *ovl_intf_id);
int hwbcm_vxlan_vpn_l3_unbind(int vpn_id);
/* Overlay intf of a bound VPN, -1 if not bound. */
int hwbcm_vxlan_vpn_l3_intf(int vpn_id);
/* Routed egress into a VPN toward a host behind a VXLAN VP (network VP for a
 * remote host, access VP for a local one). */
int hwbcm_vxlan_overlay_egress_create(int ovl_intf_id, const uint8_t mac[6], int vp_gport, int *egr_id);
int hwbcm_vxlan_overlay_egress_replace(int egr_id, int ovl_intf_id, const uint8_t mac[6], int vp_gport);

#ifdef __cplusplus
}
#endif
#endif /* HWBCM_H */
