#pragma once

/*
 * SwitchBCM56860HW — Trident2+ (BCM56864, Edgecore AS5812-54T) hardware switch
 * for the sonic-sairedis virtual-switch SAI.
 *
 * Inherits all bookkeeping from SwitchBCM56850 (queues, scheduler groups,
 * default objects ...). Overrides create/set/remove to program the ASIC via
 * the C wrapper in hwbcm/hwbcm.h, then calls the base class so the virtual
 * state stays consistent. Unsupported object types fall through to the base
 * class unchanged.
 *
 * File split (one owner per file, same class):
 *   SwitchBCM56860HW.cpp        core: construction, initialize, ports, link
 *                               notifications, hostif (KNET), VLAN, VLAN_MEMBER,
 *                               FDB_ENTRY, BRIDGE_PORT, LAG   (A2)
 *   SwitchBCM56860HW_l3.cpp     ROUTER_INTERFACE, NEIGHBOR_ENTRY, NEXT_HOP,
 *                               NEXT_HOP_GROUP(+MEMBER), ROUTE_ENTRY, VIRTUAL_ROUTER (A3)
 *   SwitchBCM56860HW_vxlan.cpp  TUNNEL, TUNNEL_MAP(+ENTRY), TUNNEL_TERM_TABLE_ENTRY,
 *                               1D BRIDGE / tunnel BRIDGE_PORT, FDB on tunnels   (A4)
 *   SwitchBCM56860HW_copp.cpp   POLICER, HOSTIF_TRAP_GROUP, hostif trap policing (CoPP)
 *   SwitchBCM56860HW_qos.cpp    QOS_MAP, SCHEDULER, WRED, BUFFER_POOL/PROFILE, QUEUE/SG/PG (A6)
 *   SwitchBCM56860HW_mirror.cpp MIRROR_SESSION (SPAN/RSPAN/ERSPAN), port mirror
 */

#include "SwitchBCM56850.h"

#include <map>
#include <unordered_map>
#include <cstring>
#include <mutex>
#include <thread>
#include <atomic>
#include <string>
#include <vector>

namespace saivs
{
    class SwitchBCM56860HW:
        public SwitchBCM56850
    {
        public:

            SwitchBCM56860HW(
                    _In_ sai_object_id_t switch_id,
                    _In_ std::shared_ptr<RealObjectIdManager> manager,
                    _In_ std::shared_ptr<SwitchConfig> config);

            SwitchBCM56860HW(
                    _In_ sai_object_id_t switch_id,
                    _In_ std::shared_ptr<RealObjectIdManager> manager,
                    _In_ std::shared_ptr<SwitchConfig> config,
                    _In_ std::shared_ptr<WarmBootState> warmBootState);

            virtual ~SwitchBCM56860HW();

        public:

            // --- dispatch entry points (A2 owns these; they route by object
            //     type to the per-area handlers below and then to the base).

            virtual sai_status_t create(
                    _In_ sai_object_type_t object_type,
                    _In_ const std::string &serializedObjectId,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list) override;

            virtual sai_status_t remove(
                    _In_ sai_object_type_t object_type,
                    _In_ const std::string &serializedObjectId) override;

            virtual sai_status_t set(
                    _In_ sai_object_type_t objectType,
                    _In_ const std::string &serializedObjectId,
                    _In_ const sai_attribute_t* attr) override;

            virtual sai_status_t bulkSet(
                    _In_ sai_object_type_t object_type,
                    _In_ const std::vector<std::string> &serialized_object_ids,
                    _In_ const sai_attribute_t *attr_list,
                    _In_ sai_bulk_op_error_mode_t mode,
                    _Out_ sai_status_t *object_statuses) override;

            virtual sai_status_t get(
                    _In_ sai_object_type_t objectType,
                    _In_ const std::string &serializedObjectId,
                    _In_ uint32_t attr_count,
                    _Out_ sai_attribute_t *attr_list) override;

            virtual sai_status_t initialize_default_objects(
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list) override;

            virtual sai_status_t queryAttributeCapability(
                    _In_ sai_object_id_t switch_id,
                    _In_ sai_object_type_t object_type,
                    _In_ sai_attr_id_t attr_id,
                    _Out_ sai_attr_capability_t *attr_capability) override;

        protected:

            // --- core / ports (A2) -------------------------------------------

            /* Bring up the SDK (hwbcm_init_ex with SAI_INIT_CONFIG_FILE), build
             * the lane -> bcm port map, start the link notification thread. */
            sai_status_t hwInitialize(_In_ bool warmBoot = false);

            virtual sai_status_t create_ports() override;
            virtual sai_status_t set_switch_default_attributes() override;
            virtual sai_status_t refresh_port_list(
                    _In_ const sai_attr_metadata_t *meta) override;
            virtual sai_status_t refresh_bridge_port_list(
                    _In_ const sai_attr_metadata_t *meta,
                    _In_ sai_object_id_t bridge_id) override;

            sai_status_t hwCreatePort(_In_ sai_object_id_t port_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwSetPort(_In_ sai_object_id_t port_id, _In_ const sai_attribute_t *attr);
            sai_status_t hwRemovePort(_In_ sai_object_id_t port_id);

            sai_status_t hwCreateHostif(_In_ sai_object_id_t hostif_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwRemoveHostif(_In_ sai_object_id_t hostif_id);
            sai_status_t hwCreateHostifTrap(_In_ sai_object_id_t trap_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            static int hwTrapReasonOf(_In_ int32_t saiTrapType);
            sai_status_t hwReconcileTrapReason(_In_ int reason, _In_ sai_object_id_t excludeTrapId);

            sai_status_t hwCreateVlan(_In_ sai_object_id_t vlan_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwRemoveVlan(_In_ sai_object_id_t vlan_id);
            sai_status_t hwCreateVlanMember(_In_ sai_object_id_t member_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwRemoveVlanMember(_In_ sai_object_id_t member_id);
            sai_status_t hwCreateFdbEntry(_In_ const std::string &serialized, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwRemoveFdbEntry(_In_ const std::string &serialized);
            sai_status_t hwCreateLag(_In_ sai_object_id_t lag_id);
            sai_status_t hwRemoveLag(_In_ sai_object_id_t lag_id);
            sai_status_t hwCreateLagMember(_In_ sai_object_id_t member_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwRemoveLagMember(_In_ sai_object_id_t member_id);

            /* Link thread: polls hwbcm link callback queue and emits
             * SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY. */
            void linkThreadMain();
            static void linkCallbackC(int port, int up, void *cookie);
            void onLinkChange(int port, int up);

            // --- A2 additions (core) ----------------------------------------

            /* Unhide the oid-based overloads of the base class (the string
             * based ones are overridden above and would otherwise hide them). */
            using SwitchStateBase::create;
            using SwitchStateBase::remove;
            using SwitchStateBase::set;
            using SwitchStateBase::get;

            virtual sai_status_t warm_boot_initialize_objects() override;

            /* Synthetic ifindex range used for link events injected into the
             * vslib event queue (see onLinkChange / syncOnLinkMsg). */
            virtual bool hasIfIndex(_In_ int ifIndex) const override;

            virtual sai_status_t refresh_port_oper_speed(_In_ sai_object_id_t port_id) override;

            /* SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST of the switch's ECMP/LAG hash -> RTAG7 fields. */
            sai_status_t hwSetHash(_In_ sai_object_id_t hash_id, _In_ const sai_attribute_t *attr);

            /* Port counters come from the ASIC (bcm_stat_get via hwbcm); every
             * other object type keeps the vslib behaviour. */
            virtual sai_status_t getStatsExt(
                    _In_ sai_object_type_t object_type,
                    _In_ sai_object_id_t object_id,
                    _In_ uint32_t number_of_counters,
                    _In_ const sai_stat_id_t* counter_ids,
                    _In_ sai_stats_mode_t mode,
                    _Out_ uint64_t *counters) override;

            /* (Re)build m_portToBcm / m_bcmToPort from the ports in the object
             * hash (SAI_PORT_ATTR_HW_LANE_LIST, first lane = physical port).
             * applyDefaults: push admin state to HW and read back speed/link. */
            sai_status_t buildPortMaps(_In_ bool applyDefaults);

            /* Register one port oid <-> bcm port pair (and its ifname). */
            void mapPort(_In_ sai_object_id_t port_oid, _In_ int bcm_port, _In_ uint32_t first_lane);
            void unmapPort(_In_ sai_object_id_t port_oid);

            /* Switch attributes that have a HW effect (SRC_MAC, FDB aging). */
            sai_status_t hwSetSwitch(_In_ const sai_attribute_t *attr);

            /* Apply the current LAG membership to the trunk. */
            sai_status_t applyTrunkMembers(_In_ sai_object_id_t lag_id);

            /* Late-joining/leaving LAG member: copy the LAG's VLAN memberships
             * and the VLAN of a port-type RIF on the LAG (m_rifToVid) to the
             * physical port (add) or undo it (remove). */
            void syncLagMemberPort(_In_ sai_object_id_t lag_id, _In_ sai_object_id_t port_oid, _In_ bool add);

            /* Resolve SAI_INIT_CONFIG_FILE: switch config -> env -> sai.profile. */
            std::string resolveConfigBcm() const;

            /* MAC learn/age events from the ASIC (hwbcm_l2_cb_t). The SDK
             * thread queues them, the link thread turns each into a synthetic
             * EVENT_TYPE_PACKET on the vslib event queue, and the overridden
             * process_packet_for_fdb_event (API thread, under the mutex)
             * either feeds the stock learning code or runs onFdbAged. */
            struct L2Event
            {
                uint8_t mac[6];
                int vid;
                int port;   /* bcm port, or HWBCM_L2_CB_TRUNK_PORT(tid) */
                int add;
            };

            static void l2CallbackC(const uint8_t mac[6], int vid, int port, int add, void *cookie);
            void onL2Event(_In_ const L2Event &ev);

            virtual void process_packet_for_fdb_event(
                    _In_ sai_object_id_t portId,
                    _In_ const std::string& name,
                    _In_ const uint8_t *buffer,
                    _In_ size_t size) override;


            virtual sai_status_t hwFlushFdbEntries(

                    _In_ uint32_t attr_count,

                    _In_ const sai_attribute_t *attr_list) override;

            /* Hardware ages entries itself; disable the vslib timer ageing. */
            virtual void processFdbEntriesForAging() override;

            void onFdbAged(_In_ sai_object_id_t port_oid, _In_ const uint8_t mac[6], _In_ int vid);

            /* vid -> VLAN oid from the object hash, SAI_NULL_OBJECT_ID if none. */
            sai_object_id_t vlanOidOf(_In_ int vid);

            std::vector<L2Event> m_l2Events;               /* guarded by m_linkMutex */

            static constexpr int LINK_IFINDEX_BASE = 0x40000000;

            // --- L3 (A3) ----------------------------------------------------

            sai_status_t hwCreateVirtualRouter(_In_ sai_object_id_t vr_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwRemoveVirtualRouter(_In_ sai_object_id_t vr_id);
            sai_status_t hwCreateRouterInterface(_In_ sai_object_id_t rif_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwSetRouterInterface(_In_ sai_object_id_t rif_id, _In_ const sai_attribute_t *attr);
            sai_status_t hwRemoveRouterInterface(_In_ sai_object_id_t rif_id);
            sai_status_t hwCreateNeighbor(_In_ const std::string &serialized, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwSetNeighbor(_In_ const std::string &serialized, _In_ const sai_attribute_t *attr);
            sai_status_t hwRemoveNeighbor(_In_ const std::string &serialized);
            void hwRefreshNeighborsOnLearn(_In_ sai_object_id_t vlanOid, _In_ const uint8_t mac[6], _In_ int port, _In_ int trunk);
            void hwPuntNeighborsOnAge(_In_ sai_object_id_t vlanOid, _In_ const uint8_t mac[6]);

            /* Where a neighbor's MAC lives, from the FDB: port / trunk, or (RIOT
             * SVI on a VNI-mapped VLAN, MAC behind a remote VTEP) the network VP
             * plus the VPN's overlay L3 intf. Returns false when unresolved. */
            bool neighborTarget(_In_ sai_object_id_t rif_id, _In_ const sai_mac_t mac,
                    _Out_ int &port, _Out_ int &trunk, _Out_ int &vpGport, _Out_ int &ovlIntf);
            /* Re-point the egress of every neighbor on vid's VLAN RIF at its current target. */
            void hwReresolveNeighborsOnVlan(_In_ int vid, _In_ bool punt = false);
            /* RIOT: bind/unbind the L3 gateway of vid's VPN to match "VLAN RIF exists && VLAN has a VNI". */
            void vxlanSviSync(_In_ int vid, _In_ sai_object_id_t removingRif = SAI_NULL_OBJECT_ID);
            sai_status_t hwCreateNextHop(_In_ sai_object_id_t nh_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwRemoveNextHop(_In_ sai_object_id_t nh_id);
            sai_status_t hwCreateNextHopGroup(_In_ sai_object_id_t nhg_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwRemoveNextHopGroup(_In_ sai_object_id_t nhg_id);
            sai_status_t hwCreateNextHopGroupMember(_In_ sai_object_id_t member_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwRemoveNextHopGroupMember(_In_ sai_object_id_t member_id);
            sai_status_t hwCreateRoute(_In_ const std::string &serialized, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwSetRoute(_In_ const std::string &serialized, _In_ const sai_attribute_t *attr);
            sai_status_t hwRemoveRoute(_In_ const std::string &serialized);

            // --- VXLAN / EVPN (A4) ------------------------------------------

            sai_status_t hwCreateTunnel(_In_ sai_object_id_t tunnel_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwRemoveTunnel(_In_ sai_object_id_t tunnel_id);
            sai_status_t hwCreateTunnelMap(_In_ sai_object_id_t map_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwRemoveTunnelMap(_In_ sai_object_id_t map_id);
            sai_status_t hwCreateTunnelMapEntry(_In_ sai_object_id_t entry_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwRemoveTunnelMapEntry(_In_ sai_object_id_t entry_id);
            sai_status_t hwCreateTunnelTermEntry(_In_ sai_object_id_t term_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwRemoveTunnelTermEntry(_In_ sai_object_id_t term_id);
            sai_status_t hwCreateBridge(_In_ sai_object_id_t bridge_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwRemoveBridge(_In_ sai_object_id_t bridge_id);
            sai_status_t hwCreateBridgePort(_In_ sai_object_id_t bp_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwRemoveBridgePort(_In_ sai_object_id_t bp_id);

            // --- shared helpers (A2 implements in SwitchBCM56860HW.cpp) -----

            /* Read a stored attribute of an existing object from the vslib
             * object hash (so handlers can resolve e.g. a RIF's VLAN). Returns
             * false if missing. */
            bool getStoredAttr(_In_ sai_object_type_t ot, _In_ sai_object_id_t oid, _In_ sai_attr_id_t attr_id, _Out_ sai_attribute_t &attr);
            bool getStoredAttr(_In_ sai_object_type_t ot, _In_ const std::string &serialized, _In_ sai_attr_id_t attr_id, _Out_ sai_attribute_t &attr);

            /* sai port oid -> bcm logical port, -1 if unknown. */
            int bcmPortOf(_In_ sai_object_id_t port_oid) const;
            /* bcm logical port -> sai port oid, SAI_NULL_OBJECT_ID if unknown. */
            sai_object_id_t portOidOf(_In_ int bcm_port) const;
            /* sai vlan oid -> vid, -1 if unknown. */
            int vidOf(_In_ sai_object_id_t vlan_oid);
            /* sai lag oid -> bcm trunk id, -1 if unknown. */
            int trunkOf(_In_ sai_object_id_t lag_oid) const;
            /* A bridge port oid -> underlying port oid or lag oid. */
            sai_object_id_t bridgePortTarget(_In_ sai_object_id_t bridge_port_oid);

            static const sai_attribute_t *findAttr(_In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list, _In_ sai_attr_id_t id);

        public:

            /* Map negative OpenBCM BCM_E_* return codes to SAI status. */
            static sai_status_t bcmRvToSai(_In_ int rv);

        protected:

        protected:

            // --- shared state ---------------------------------------------

            bool m_hwReady = false;
            std::string m_configBcm;                       /* SAI_INIT_CONFIG_FILE */

            std::map<sai_object_id_t, int> m_portToBcm;    /* port oid -> bcm port */
            std::map<int, sai_object_id_t> m_bcmToPort;    /* bcm port -> port oid */
            std::map<sai_object_id_t, int> m_lagToTrunk;   /* lag oid -> trunk id */
            std::map<sai_object_id_t, int> m_hostifToNetif;/* hostif oid -> knet netif id */

            // A2 additions
            mutable std::mutex m_mapMutex;                 /* guards the three maps below (link thread reads them) */
            std::map<int, std::string> m_bcmIfName;        /* bcm port -> ifname used for link events */
            std::map<sai_object_id_t, std::vector<sai_object_id_t>> m_lagMembers; /* lag oid -> member port oids */
            std::map<sai_object_id_t, std::pair<sai_object_id_t, sai_object_id_t>> m_lagMemberInfo; /* member oid -> (lag, port) */

            // L3 state (A3)
            struct RifIpKey
            {
                sai_object_id_t rif = SAI_NULL_OBJECT_ID;
                uint8_t af = 0;         /* 4 or 6 */
                uint8_t addr[16] = {};  /* network byte order, zero-padded for IPv4 */

                bool operator==(const RifIpKey &o) const noexcept
                {
                    return rif == o.rif && af == o.af && memcmp(addr, o.addr, 16) == 0;
                }
            };

            struct RifIpKeyHash
            {
                size_t operator()(const RifIpKey &k) const noexcept
                {
                    uint64_t a = 0, b = 0;
                    memcpy(&a, k.addr, 8);
                    memcpy(&b, k.addr + 8, 8);
                    size_t h = std::hash<uint64_t>{}(static_cast<uint64_t>(k.rif));
                    h ^= std::hash<uint64_t>{}(a) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
                    h ^= std::hash<uint64_t>{}(b) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
                    h ^= static_cast<size_t>(k.af) * 0x9e3779b9ULL;
                    return h;
                }
            };

            /* Derived L3/L2/VXLAN lookup indexes (never saved to .hwstate;
             * rebuilt from primary maps + m_objectHash on warm restore). */
            struct VlanMacKey
            {
                sai_object_id_t vlanOid = SAI_NULL_OBJECT_ID;
                uint8_t mac[6] = {};

                bool operator==(const VlanMacKey &o) const noexcept
                {
                    return vlanOid == o.vlanOid && memcmp(mac, o.mac, 6) == 0;
                }
            };

            struct VlanMacKeyHash
            {
                size_t operator()(const VlanMacKey &k) const noexcept
                {
                    uint64_t m = 0;
                    memcpy(&m, k.mac, 6);
                    size_t h = std::hash<uint64_t>{}(static_cast<uint64_t>(k.vlanOid));
                    h ^= std::hash<uint64_t>{}(m) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
                    return h;
                }
            };

            static RifIpKey makeRifIpKey(_In_ sai_object_id_t rif, _In_ const sai_ip_address_t &ip);
            static VlanMacKey makeVlanMacKey(_In_ sai_object_id_t vlanOid, _In_ const uint8_t mac[6]);
            void nhIndexAdd(_In_ sai_object_id_t nh_id, _In_ sai_object_id_t rif, _In_ const sai_ip_address_t &ip);
            void nhIndexRemove(_In_ sai_object_id_t nh_id);
            void egrNhRefInc(_In_ int egr);
            void egrNhRefDec(_In_ int egr);
            void egrNeighborRefInc(_In_ int egr);
            void egrNeighborRefDec(_In_ int egr);
            void neighborVlanIndexAdd(_In_ const std::string &serialized, _In_ sai_object_id_t rif_id, _In_ const uint8_t mac[6]);
            void neighborVlanIndexUpdateMac(_In_ const std::string &serialized, _In_ const uint8_t newMac[6]);
            void neighborVlanIndexRemove(_In_ const std::string &serialized);
            void v4NeighborIndexAdd(_In_ int vrf, _In_ uint32_t ip4, _In_ const std::string &serialized, _In_ int egr);
            void v4NeighborIndexRemove(_In_ int vrf, _In_ uint32_t ip4, _In_ const std::string &serialized);
            void v4RouteIndexAdd(_In_ int vrf, _In_ uint32_t ip4, _In_ int len, _In_ int egr);
            void v4RouteIndexRemove(_In_ int vrf, _In_ uint32_t ip4, _In_ int len);
            sai_object_id_t defaultVirtualRouterId();
            sai_object_id_t bridgePortOfTarget(_In_ sai_object_id_t targetOid);

            sai_object_id_t m_defaultVrId = SAI_NULL_OBJECT_ID; /* cached SWITCH DEFAULT_VIRTUAL_ROUTER_ID */
            std::map<sai_object_id_t, int> m_vrToVrf;      /* virtual router oid -> bcm vrf */
            std::map<sai_object_id_t, int> m_rifToIntf;    /* rif oid -> bcm l3 intf id */
            std::map<sai_object_id_t, int> m_rifToVid;     /* rif oid -> vlan used for the RIF */
            std::unordered_map<sai_object_id_t, int> m_nhToEgr;      /* next hop oid -> bcm egress id */
            std::unordered_map<RifIpKey, std::vector<sai_object_id_t>, RifIpKeyHash> m_nhByRifIp; /* derived: (rif, ip) -> nh oids */
            std::unordered_map<sai_object_id_t, RifIpKey> m_nhKey; /* derived: nh oid -> (rif, ip) */
            std::unordered_map<int, uint32_t> m_egrNhRefCount;       /* derived: egr -> count in m_nhToEgr */
            std::unordered_map<int, uint32_t> m_egrNeighborRefCount; /* derived: egr -> count in m_neighborEgr */
            std::map<sai_object_id_t, int> m_nhgToEcmp;    /* next hop group oid -> bcm ecmp id */
            std::unordered_map<int, sai_object_id_t> m_ecmpToNhg;    /* derived: bcm ecmp id -> nhg oid */
            std::map<sai_object_id_t, sai_object_id_t> m_nhgMemberToNh; /* nhg member -> nh */
            std::map<sai_object_id_t, sai_object_id_t> m_nhgMemberToNhg;/* nhg member -> nhg */
            std::unordered_map<sai_object_id_t, std::set<sai_object_id_t>> m_nhgToMembers; /* derived: nhg -> member oids */
            std::unordered_map<std::string, int> m_neighborEgr;      /* serialized neighbor -> egress id */
            std::unordered_map<VlanMacKey, std::vector<std::string>, VlanMacKeyHash> m_vlanNeighborsByMac; /* derived: (vlanOid, mac) -> neighbor keys */
            std::unordered_map<int, std::vector<std::string>> m_vlanNeighborsByVid; /* derived: vid -> neighbor keys */
            std::unordered_map<std::string, std::pair<int, VlanMacKey>> m_neighborVlanMac; /* derived: neighbor key -> (vid, (vlanOid, mac)) */
            std::unordered_map<uint64_t, std::vector<std::pair<std::string, int>>> m_v4NeighborsByVrfIp; /* derived: (vrf, ip4) -> [(ser, egr)] */
            std::unordered_map<uint64_t, int> m_v4RoutesByVrfLen[33]; /* derived: [len][(vrf, masked_ip4)] -> egr */
            uint64_t m_v4RouteLenMask = 0;                            /* derived: bit len set if m_v4RoutesByVrfLen[len] non-empty */
            std::unordered_map<std::string, int> m_routeEgr;         /* serialized route -> egress/ecmp id */
            std::unordered_map<int, sai_object_id_t> m_vidToVlanOid;  /* derived: vid -> vlan oid */
            std::unordered_map<sai_object_id_t, int> m_vlanOidToVid;  /* derived: vlan oid -> vid */
            std::unordered_map<sai_object_id_t, sai_object_id_t> m_portToLag; /* derived: port oid -> lag oid */
            std::unordered_map<sai_object_id_t, sai_object_id_t> m_targetToBridgePort; /* derived: port/lag -> 1Q bp */
            std::unordered_map<sai_object_id_t, sai_object_id_t> m_bridgePortToTarget; /* derived: 1Q bp -> port/lag */
            std::unordered_map<int, int> m_vpnToVid;                  /* derived: vxlan vpn -> vid */

            // VXLAN state (A4)
            std::map<sai_object_id_t, int> m_tunnelInit;   /* tunnel oid -> bcm tunnel init id */
            std::map<sai_object_id_t, int> m_tunnelTerm;   /* tunnel oid -> bcm tunnel term id */
            std::map<uint32_t, int> m_vniToVpn;            /* vni -> bcm vpn id */
            std::map<sai_object_id_t, int> m_bridgeToVpn;  /* 1D bridge oid -> vpn id */
            std::map<sai_object_id_t, int> m_bridgePortToVp;/* bridge port oid -> vxlan vp */

            // link thread
            std::thread m_linkThread;
            std::atomic<bool> m_linkRun{false};
            std::mutex m_linkMutex;
            std::vector<std::pair<int,int>> m_linkEvents;  /* (bcm port, up) */
            int m_linkEventFd = -1;                        /* eventfd to wake the thread */

        // --- A4 additions (VXLAN / EVPN) ----------------------------------
        //
        // Model: SONiC programs one P2MP SAI_TUNNEL (local VTEP) with the
        // VLAN<->VNI maps, then per remote VTEP a P2P SAI_TUNNEL (+ a tunnel
        // term entry) and a SAI_BRIDGE_PORT of type TUNNEL; remote MACs are
        // FDB entries on that bridge port. On TD2+ this maps to: one VXLAN VPN
        // per VNI (m_vniToVpn), one tunnel initiator/terminator + one network
        // VP per remote VTEP (VxlanRemoteVtep, shared by all VPNs through the
        // flood groups), one access VP per (local VLAN member, VNI).

        public:

            /* Remote MAC on a tunnel bridge port (EVPN type-2). Called by the
             * FDB handler (A2) when SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID is a
             * tunnel bridge port (present in m_bridgePortToVp or
             * m_bridgePortRemoteIp). bv_id must be a VLAN whose VNI map exists;
             * otherwise the MAC is kept as desired state and installed later. */
            sai_status_t hwFdbOnTunnel(_In_ const sai_fdb_entry_t &fdb, _In_ sai_object_id_t bridge_port, _In_ bool add);

            /* Optional hook for A2: a VLAN_MEMBER was created/removed. Adds or
             * removes the access VP if the member's VLAN has a VNI. Safe to
             * call for every VLAN member (no-op when the VLAN has no VNI). */
            sai_status_t hwVxlanVlanMemberChanged(_In_ sai_object_id_t vlan_member_oid, _In_ bool add);

            /* Optional hook for A3: after a NEIGHBOR / ROUTE / NEXT_HOP was
             * created, retry remote VTEPs whose underlay next hop could not be
             * resolved yet (their network VP is still pending). */
            sai_status_t hwVxlanRetryPending();
            sai_status_t hwVxlanRetryPendingForNeighbor(_In_ const sai_ip_address_t &ip, _In_ int egr);
            sai_status_t hwVxlanRetryPendingForRoute(_In_ const sai_ip_prefix_t &pfx);
            sai_status_t hwVxlanRetryPendingForNextHop(_In_ int egr);
            sai_status_t hwVxlanRetryPendingForEcmp(_In_ int ecmpId);

            /* Hook for A2's L2 learn/age callback. MACs learned on a VNI-mapped
             * VLAN come from the SDK as VFI events: vid = VPN id (> 4095) and
             * port = VXLAN VP gport. Call this first; it rewrites vid/port to
             * the real VLAN and physical port (trunk set, port -1, for a LAG)
             * and returns true. Returns false when the event must be dropped:
             * a remote MAC on a network VP (EVPN controlled, never report it)
             * or an unknown VFI. Plain VLAN events pass through unchanged. */
            bool hwVxlanTranslateL2Event(_Inout_ int &vid, _Inout_ int &port, _Out_ int &trunk);

        protected:

            struct VxlanMac
            {
                sai_mac_t mac = {0, 0, 0, 0, 0, 0};
                int vid = -1;
            };

            struct VxlanRemoteVtep
            {
                std::string ip;                            /* serialized remote VTEP ip (key) */
                uint8_t ip4[4] = {0, 0, 0, 0};
                int tunnelInit = -1;                       /* bcm tunnel initiator gport */
                int tunnelTerm = -1;                       /* bcm tunnel terminator gport */
                int egr = -1;                              /* VXLAN-only L3 egress toward the VTEP */
                int srcEgr = -1;                           /* A3 egress/ecmp id the clone was made from */
                int vp = -1;                               /* network VP gport (bit 31 set: never test its sign) */
                int egrMc = -1;                            /* IPMC VXLAN egress for the BUM copy */
                int vpMc = -1;                             /* multicast network VP (the one in the flood groups) */
                bool vpReady = false;                      /* true once vp holds a live network VP */
                int vpVpn = -1;                            /* VPN the VP was created in */
                int port = -1;                             /* underlay bcm port (-1 = trunk) */
                int underlayVrf = -1;                      /* derived: underlay VRF (-1 = use m_vtepUnderlayVrf) */
                std::set<sai_object_id_t> tunnels;         /* P2P tunnel oids (normally one) */
                std::set<sai_object_id_t> bridgePorts;     /* tunnel bridge port oids */
                std::set<int> floodVpns;                   /* VPNs whose flood group has the VP */
                std::map<std::string, VxlanMac> macs;     /* serialized fdb -> desired remote MACs */
            };

            struct VxlanAccessVp
            {
                sai_object_id_t member = SAI_NULL_OBJECT_ID; /* VLAN member oid */
                int vp = -1;
            };

            /* --- remote VTEP side --- */
            std::map<std::string, VxlanRemoteVtep> m_vtepByIp;          /* remote ip -> state */
            std::map<sai_object_id_t, std::string> m_tunnelRemoteIp;    /* P2P tunnel oid -> remote ip */
            std::map<sai_object_id_t, std::string> m_bridgePortRemoteIp;/* tunnel bridge port oid -> remote ip */
            std::map<sai_object_id_t, sai_object_id_t> m_termEntryTunnel;/* term entry oid -> tunnel oid */

            /* --- local VTEP side --- */
            sai_object_id_t m_vtepTunnel = SAI_NULL_OBJECT_ID;         /* the P2MP tunnel */
            bool m_vtepSrcValid = false;
            uint8_t m_vtepSrcIp4[4] = {0, 0, 0, 0};
            int m_vtepTtl = 64;
            int m_vtepDscp = -1;                                        /* -1 = copy inner */
            int m_vtepUnderlayVrf = 0;                                  /* derived: underlay RIF's bcm VRF */
            bool m_vxlanInited = false;
            int m_vxlanStationId = -1;

            /* --- VNI side --- */
            std::map<sai_object_id_t, int32_t> m_tunnelMapType;         /* tunnel map oid -> sai_tunnel_map_type_t */
            std::map<sai_object_id_t, std::pair<int, uint32_t>> m_mapEntryVidVni; /* map entry oid -> (vid, vni) */
            std::map<uint32_t, int> m_vniRefs;                          /* vni -> number of map entries */
            std::map<int, uint32_t> m_vidToVni;                         /* vid -> vni */
            std::map<int, std::vector<VxlanAccessVp>> m_vpnAccessVps;   /* vpn -> access VPs */

            /* --- internals (SwitchBCM56860HW_vxlan.cpp) --- */
            sai_status_t vxlanEnsureInit();
            bool vxlanIp4Of(_In_ const sai_ip_address_t &ip, _Out_ uint8_t out[4]) const;
            bool vxlanEcmpContainsEgress(_In_ int ecmpId, _In_ int egr) const;
            int vxlanVrfOfUnderlayRif(_In_ sai_object_id_t rifOid);
            void vxlanCheckVtepUnderlay(_Inout_ VxlanRemoteVtep &vtep);
            int vxlanResolveUnderlayEgress(_In_ int vrf, _In_ const uint8_t ip4[4]);
            bool vxlanTryBringUpVtep(_Inout_ VxlanRemoteVtep &vtep);
            void vxlanTearDownVtepVp(_Inout_ VxlanRemoteVtep &vtep);
            void vxlanNotifyTunnelOper(_In_ const VxlanRemoteVtep &vtep, _In_ bool up);
            void vxlanInstallVtepMacs(_Inout_ VxlanRemoteVtep &vtep, _In_ int onlyVid);
            void vxlanAddVtepToVpn(_Inout_ VxlanRemoteVtep &vtep, _In_ int vpn);
            sai_status_t vxlanAddAccessPorts(_In_ int vid, _In_ int vpn);
            sai_status_t vxlanAddAccessPort(_In_ sai_object_id_t member_oid, _In_ int vid, _In_ int vpn);
            void vxlanRemoveAccessPorts(_In_ int vpn);
            int vxlanVpnOfVid(_In_ int vid) const;
#include "SwitchBCM56860HW_stats.inc.h"
#include "SwitchBCM56860HW_acl.inc.h"
#include "SwitchBCM56860HW_copp.inc.h"
#include "SwitchBCM56860HW_qos.inc.h"
#include "SwitchBCM56860HW_mirror.inc.h"
#include "SwitchBCM56860HW_wb.inc.h"
    };
}
