/*
 * SwitchBCM56860HW_l3.cpp — L3 handlers of the Trident2+ hardware switch:
 * VIRTUAL_ROUTER, ROUTER_INTERFACE, NEIGHBOR_ENTRY, NEXT_HOP,
 * NEXT_HOP_GROUP (+MEMBER), ROUTE_ENTRY.
 *
 * Called by the dispatcher in SwitchBCM56860HW.cpp after the base class has
 * stored the object (create) or before it removes it (remove), so stored
 * attributes can be read with getStoredAttr().
 *
 * Egress object ownership
 * -----------------------
 * A NEXT_HOP (rif, ip) and a NEIGHBOR_ENTRY (rif, ip) share one bcm egress
 * object. Whoever comes first creates it: a neighbor creates it with the real
 * mac/port, a next hop without a neighbor creates a CPU (L2TOCPU) egress so
 * traffic is punted until the neighbor resolves. When the neighbor shows up it
 * rewrites the shared egress in place (BCM_L3_REPLACE), so ECMP groups and
 * routes referencing it stay valid; when it goes away the egress is rewritten
 * back to CPU. The egress is destroyed when neither side references it.
 */
#include "SwitchBCM56860HW.h"
#include "hwbcm/hwbcm.h"

#include "swss/logger.h"
#include "meta/sai_serialize.h"

#include <cstring>
#include <set>
#include <algorithm>

using namespace saivs;

namespace
{
    /* Dedicated VLANs for port RIFs are taken from this pool. */
    const int RIF_VID_MIN = 3000;
    const int RIF_VID_MAX = 3999;

    /* One switch instance per process: shared trap/drop egress objects.
     * (L2TOCPU egress ignores intf, so one CPU egress serves every RIF.) */
    int g_cpuEgr = -1;
    int g_dropEgr = -1;

    int sharedCpuEgress()
    {
        SWSS_LOG_ENTER();

        if (g_cpuEgr < 0)
        {
            int rv = hwbcm_l3_egress_cpu_create(-1, &g_cpuEgr);

            if (rv < 0)
            {
                SWSS_LOG_ERROR("hwbcm_l3_egress_cpu_create failed: %d", rv);
                g_cpuEgr = -1;
            }
        }

        return g_cpuEgr;
    }

    int sharedDropEgress()
    {
        SWSS_LOG_ENTER();

        if (g_dropEgr < 0)
        {
            int rv = hwbcm_l3_egress_drop_create(&g_dropEgr);

            if (rv < 0)
            {
                SWSS_LOG_ERROR("hwbcm_l3_egress_drop_create failed: %d", rv);
                g_dropEgr = -1;
            }
        }

        return g_dropEgr;
    }

    /* sai_ip_address_t -> (af, 4/16 network-order bytes) */
    int ipBytes(
            _In_ const sai_ip_address_t &ip,
            _Out_ uint8_t buf[16])
    {
        SWSS_LOG_ENTER();

        if (ip.addr_family == SAI_IP_ADDR_FAMILY_IPV6)
        {
            memcpy(buf, ip.addr.ip6, 16);
            return 6;
        }

        memcpy(buf, &ip.addr.ip4, 4);
        return 4;
    }

    /* sai_ip_prefix_t -> (af, prefix bytes, prefix length) */
    int prefixBytes(
            _In_ const sai_ip_prefix_t &pfx,
            _Out_ uint8_t buf[16],
            _Out_ int &len)
    {
        SWSS_LOG_ENTER();

        uint8_t mask[16];
        int af;
        size_t n;

        if (pfx.addr_family == SAI_IP_ADDR_FAMILY_IPV6)
        {
            memcpy(buf, pfx.addr.ip6, 16);
            memcpy(mask, pfx.mask.ip6, 16);
            af = 6;
            n = 16;
        }
        else
        {
            memcpy(buf, &pfx.addr.ip4, 4);
            memcpy(mask, &pfx.mask.ip4, 4);
            af = 4;
            n = 4;
        }

        len = 0;

        for (size_t i = 0; i < n; i++)
        {
            uint8_t b = mask[i];

            while (b)
            {
                len += (b & 1);
                b = (uint8_t)(b >> 1);
            }
        }

        return af;
    }

    bool macEqual(
            _In_ const sai_mac_t a,
            _In_ const sai_mac_t b)
    {
        SWSS_LOG_ENTER();

        return memcmp(a, b, sizeof(sai_mac_t)) == 0;
    }

    bool valueInMap(
            _In_ const std::map<sai_object_id_t, int> &m,
            _In_ int value,
            _In_ sai_object_id_t except = SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ENTER();

        for (auto &kv: m)
        {
            if (kv.second == value && kv.first != except)
            {
                return true;
            }
        }

        return false;
    }

    bool valueInMap(
            _In_ const std::map<std::string, int> &m,
            _In_ int value)
    {
        SWSS_LOG_ENTER();

        for (auto &kv: m)
        {
            if (kv.second == value)
            {
                return true;
            }
        }

        return false;
    }

    /* Resolve a virtual router oid to a bcm vrf. The default VR is created by
     * the base class in initialize_default_objects before any hw dispatch, so
     * it is mapped here on first use (-> vrf 0). Any other VR seen for the
     * first time gets a fresh vrf id (allocate) or falls back to 0 with a WARN. */
    int resolveVrf(
            _In_ std::map<sai_object_id_t, int> &vrToVrf,
            _In_ sai_object_id_t defaultVr,
            _In_ sai_object_id_t vr,
            _In_ bool allocate,
            _In_ const char *ctx)
    {
        SWSS_LOG_ENTER();

        auto it = vrToVrf.find(vr);

        if (it != vrToVrf.end())
        {
            return it->second;
        }

        if (vr == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_WARN("%s: null virtual router, using vrf 0", ctx);
            return 0;
        }

        if (vr == defaultVr || defaultVr == SAI_NULL_OBJECT_ID)
        {
            // default VR (or default not yet published by the base: treat the
            // first VR we meet as the default one)
            vrToVrf[vr] = 0;
            SWSS_LOG_NOTICE("%s: virtual router %s -> vrf 0 (default)", ctx, sai_serialize_object_id(vr).c_str());
            return 0;
        }

        if (!allocate)
        {
            SWSS_LOG_WARN("%s: virtual router %s unknown, using vrf 0", ctx, sai_serialize_object_id(vr).c_str());
            return 0;
        }

        int vrf = 0;
        int rv = hwbcm_l3_vrf_create(&vrf);

        if (rv < 0)
        {
            SWSS_LOG_ERROR("%s: hwbcm_l3_vrf_create failed: %d, using vrf 0", ctx, rv);
            return 0;
        }

        vrToVrf[vr] = vrf;
        SWSS_LOG_NOTICE("%s: virtual router %s -> vrf %d (late)", ctx, sai_serialize_object_id(vr).c_str(), vrf);
        return vrf;
    }

    /* Egress ids of all members of a next hop group, in member creation
     * order is not preserved (std::map order), duplicates kept out. */
    std::vector<int> groupEgressList(
            _In_ sai_object_id_t nhg,
            _In_ const std::map<sai_object_id_t, sai_object_id_t> &memberToNhg,
            _In_ const std::map<sai_object_id_t, sai_object_id_t> &memberToNh,
            _In_ const std::map<sai_object_id_t, int> &nhToEgr)
    {
        SWSS_LOG_ENTER();

        std::vector<int> egrs;

        for (auto &kv: memberToNhg)
        {
            if (kv.second != nhg)
            {
                continue;
            }

            auto nhIt = memberToNh.find(kv.first);

            if (nhIt == memberToNh.end())
            {
                continue;
            }

            auto egrIt = nhToEgr.find(nhIt->second);

            if (egrIt == nhToEgr.end())
            {
                SWSS_LOG_WARN("nhg member %s: next hop %s has no egress, skipped",
                        sai_serialize_object_id(kv.first).c_str(),
                        sai_serialize_object_id(nhIt->second).c_str());
                continue;
            }

            if (std::find(egrs.begin(), egrs.end(), egrIt->second) == egrs.end())
            {
                egrs.push_back(egrIt->second);
            }
        }

        return egrs;
    }
}

// --- VIRTUAL_ROUTER -------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateVirtualRouter(
        _In_ sai_object_id_t vr_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    if (m_vrToVrf.find(vr_id) != m_vrToVrf.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    // The default VR is created by the base before hw dispatch exists, so a
    // VR reaching this handler is normally a user VRF. Still honour the
    // stored default oid if it matches, and treat the very first VR as the
    // default when the base has not published it yet.

    sai_object_id_t defaultVr = SAI_NULL_OBJECT_ID;

    sai_attribute_t sw;

    if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID, sw))
    {
        defaultVr = sw.value.oid;
    }

    bool isDefault = (vr_id == defaultVr) || (defaultVr == SAI_NULL_OBJECT_ID && m_vrToVrf.empty());

    int vrf = 0;

    if (!isDefault)
    {
        int rv = hwbcm_l3_vrf_create(&vrf);

        if (rv < 0)
        {
            SWSS_LOG_ERROR("hwbcm_l3_vrf_create failed: %d", rv);
            return SAI_STATUS_FAILURE;
        }
    }

    m_vrToVrf[vr_id] = vrf;

    SWSS_LOG_NOTICE("virtual router %s -> vrf %d%s", sai_serialize_object_id(vr_id).c_str(), vrf, isDefault ? " (default)" : "");

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveVirtualRouter(
        _In_ sai_object_id_t vr_id)
{
    SWSS_LOG_ENTER();

    auto it = m_vrToVrf.find(vr_id);

    if (it == m_vrToVrf.end())
    {
        SWSS_LOG_WARN("virtual router %s unknown to hw", sai_serialize_object_id(vr_id).c_str());
        return SAI_STATUS_SUCCESS;
    }

    if (it->second != 0)
    {
        int rv = hwbcm_l3_vrf_destroy(it->second);

        if (rv < 0)
        {
            SWSS_LOG_ERROR("hwbcm_l3_vrf_destroy(%d) failed: %d", it->second, rv);
            return SAI_STATUS_FAILURE;
        }
    }

    m_vrToVrf.erase(it);

    return SAI_STATUS_SUCCESS;
}

// --- ROUTER_INTERFACE -----------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateRouterInterface(
        _In_ sai_object_id_t rif_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    std::string srif = sai_serialize_object_id(rif_id);

    auto *typeAttr = findAttr(attr_count, attr_list, SAI_ROUTER_INTERFACE_ATTR_TYPE);

    if (typeAttr == nullptr)
    {
        SWSS_LOG_ERROR("rif %s: missing SAI_ROUTER_INTERFACE_ATTR_TYPE", srif.c_str());
        return SAI_STATUS_FAILURE;
    }

    sai_router_interface_type_t type = (sai_router_interface_type_t)typeAttr->value.s32;

    switch (type)
    {
        case SAI_ROUTER_INTERFACE_TYPE_PORT:
        case SAI_ROUTER_INTERFACE_TYPE_VLAN:
            break;

        case SAI_ROUTER_INTERFACE_TYPE_LOOPBACK:
            SWSS_LOG_NOTICE("rif %s: loopback, no hw object", srif.c_str());
            return SAI_STATUS_SUCCESS;

        case SAI_ROUTER_INTERFACE_TYPE_SUB_PORT:
        default:
            SWSS_LOG_WARN("rif %s: type %d not supported in hw, ignored", srif.c_str(), (int)type);
            return SAI_STATUS_SUCCESS;
    }

    // vrf

    int vrf = 0;

    auto *vrAttr = findAttr(attr_count, attr_list, SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID);

    if (vrAttr != nullptr)
    {
        sai_object_id_t defaultVr = SAI_NULL_OBJECT_ID;

        {
            sai_attribute_t sw;

            if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID, sw))
            {
                defaultVr = sw.value.oid;
            }
        }

        vrf = resolveVrf(m_vrToVrf, defaultVr, vrAttr->value.oid, true, srif.c_str());
    }

    // mac: RIF attribute, else switch mac

    sai_mac_t mac;
    memset(mac, 0, sizeof(mac));

    auto *macAttr = findAttr(attr_count, attr_list, SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS);

    if (macAttr != nullptr)
    {
        memcpy(mac, macAttr->value.mac, sizeof(mac));
    }
    else
    {
        sai_attribute_t sw;

        if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_SRC_MAC_ADDRESS, sw))
        {
            memcpy(mac, sw.value.mac, sizeof(mac));
        }
        else
        {
            SWSS_LOG_ERROR("rif %s: no SRC_MAC on rif and no switch SRC_MAC_ADDRESS", srif.c_str());
            return SAI_STATUS_FAILURE;
        }
    }

    // mtu

    int mtu = 1514;

    auto *mtuAttr = findAttr(attr_count, attr_list, SAI_ROUTER_INTERFACE_ATTR_MTU);

    if (mtuAttr != nullptr)
    {
        mtu = (int)mtuAttr->value.u32;
    }

    // vlan

    int vid = -1;
    int port = -1;
    int trunk = -1;
    std::vector<int> memberPorts;   // ports added to the dedicated vlan (port RIF)
    bool dedicatedVlan = false;

    if (type == SAI_ROUTER_INTERFACE_TYPE_VLAN)
    {
        auto *vlanAttr = findAttr(attr_count, attr_list, SAI_ROUTER_INTERFACE_ATTR_VLAN_ID);

        if (vlanAttr == nullptr)
        {
            SWSS_LOG_ERROR("rif %s: missing SAI_ROUTER_INTERFACE_ATTR_VLAN_ID", srif.c_str());
            return SAI_STATUS_FAILURE;
        }

        vid = vidOf(vlanAttr->value.oid);

        if (vid < 0)
        {
            SWSS_LOG_ERROR("rif %s: vlan %s unknown", srif.c_str(), sai_serialize_object_id(vlanAttr->value.oid).c_str());
            return SAI_STATUS_FAILURE;
        }
    }
    else // PORT
    {
        auto *portAttr = findAttr(attr_count, attr_list, SAI_ROUTER_INTERFACE_ATTR_PORT_ID);

        if (portAttr == nullptr)
        {
            SWSS_LOG_ERROR("rif %s: missing SAI_ROUTER_INTERFACE_ATTR_PORT_ID", srif.c_str());
            return SAI_STATUS_FAILURE;
        }

        sai_object_id_t target = portAttr->value.oid;
        sai_object_type_t tot = objectTypeQuery(target);

        if (tot == SAI_OBJECT_TYPE_PORT)
        {
            port = bcmPortOf(target);

            if (port < 0)
            {
                SWSS_LOG_ERROR("rif %s: port %s has no bcm port", srif.c_str(), sai_serialize_object_id(target).c_str());
                return SAI_STATUS_FAILURE;
            }

            memberPorts.push_back(port);
        }
        else if (tot == SAI_OBJECT_TYPE_LAG)
        {
            trunk = trunkOf(target);

            if (trunk < 0)
            {
                SWSS_LOG_ERROR("rif %s: lag %s has no bcm trunk", srif.c_str(), sai_serialize_object_id(target).c_str());
                return SAI_STATUS_FAILURE;
            }

            std::lock_guard<std::mutex> lock(m_mapMutex);

            auto lm = m_lagMembers.find(target);

            if (lm != m_lagMembers.end())
            {
                for (auto poid: lm->second)
                {
                    int p = bcmPortOf(poid);

                    if (p >= 0)
                    {
                        memberPorts.push_back(p);
                    }
                }
            }

            if (memberPorts.empty())
            {
                SWSS_LOG_WARN("rif %s: lag %s has no members yet, vlan membership deferred to lag member add",
                        srif.c_str(), sai_serialize_object_id(target).c_str());
            }
        }
        else
        {
            SWSS_LOG_ERROR("rif %s: PORT_ID %s is neither port nor lag", srif.c_str(), sai_serialize_object_id(target).c_str());
            return SAI_STATUS_FAILURE;
        }

        // pick a free vid from the pool

        std::set<int> used;

        for (auto &kv: m_rifToVid)
        {
            used.insert(kv.second);
        }

        for (int v = RIF_VID_MIN; v <= RIF_VID_MAX; v++)
        {
            if (used.find(v) == used.end())
            {
                vid = v;
                break;
            }
        }

        if (vid < 0)
        {
            SWSS_LOG_ERROR("rif %s: no free vlan in pool %d-%d", srif.c_str(), RIF_VID_MIN, RIF_VID_MAX);
            return SAI_STATUS_FAILURE;
        }

        int rv = hwbcm_vlan_create(vid);

        if (rv < 0)
        {
            SWSS_LOG_ERROR("rif %s: hwbcm_vlan_create(%d) failed: %d", srif.c_str(), vid, rv);
            return SAI_STATUS_FAILURE;
        }

        dedicatedVlan = true;

        for (int p: memberPorts)
        {
            rv = hwbcm_vlan_port_add(vid, p, 0);

            if (rv < 0)
            {
                SWSS_LOG_ERROR("rif %s: hwbcm_vlan_port_add(%d, %d) failed: %d", srif.c_str(), vid, p, rv);
                hwbcm_vlan_destroy(vid);
                return SAI_STATUS_FAILURE;
            }

            rv = hwbcm_port_pvid_set(p, vid);

            if (rv < 0)
            {
                SWSS_LOG_ERROR("rif %s: hwbcm_port_pvid_set(%d, %d) failed: %d", srif.c_str(), p, vid, rv);
                hwbcm_vlan_destroy(vid);
                return SAI_STATUS_FAILURE;
            }
        }
    }

    int intf = -1;

    int rv = hwbcm_l3_intf_create(mac, vid, vrf, mtu, &intf);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("rif %s: hwbcm_l3_intf_create(vid=%d vrf=%d) failed: %d", srif.c_str(), vid, vrf, rv);

        if (dedicatedVlan)
        {
            hwbcm_vlan_destroy(vid);
        }

        return SAI_STATUS_FAILURE;
    }

    m_rifToIntf[rif_id] = intf;
    m_rifToVid[rif_id] = vid;

    SWSS_LOG_NOTICE("rif %s: intf %d vid %d vrf %d port %d trunk %d mtu %d",
            srif.c_str(), intf, vid, vrf, port, trunk, mtu);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwSetRouterInterface(
        _In_ sai_object_id_t rif_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    auto it = m_rifToIntf.find(rif_id);

    if (it == m_rifToIntf.end())
    {
        // loopback / unsupported type: nothing in hw
        return SAI_STATUS_SUCCESS;
    }

    switch (attr->id)
    {
        case SAI_ROUTER_INTERFACE_ATTR_MTU:
            {
                int rv = hwbcm_l3_intf_mtu_set(it->second, (int)attr->value.u32);

                if (rv < 0)
                {
                    SWSS_LOG_ERROR("rif %s: hwbcm_l3_intf_mtu_set(%d, %u) failed: %d",
                            sai_serialize_object_id(rif_id).c_str(), it->second, attr->value.u32, rv);
                    return SAI_STATUS_FAILURE;
                }

                return SAI_STATUS_SUCCESS;
            }

        case SAI_ROUTER_INTERFACE_ATTR_ADMIN_V4_STATE:
        case SAI_ROUTER_INTERFACE_ATTR_ADMIN_V6_STATE:
            SWSS_LOG_NOTICE("rif %s: admin %s state %d (routing stays enabled on the vlan)",
                    sai_serialize_object_id(rif_id).c_str(),
                    attr->id == SAI_ROUTER_INTERFACE_ATTR_ADMIN_V4_STATE ? "v4" : "v6",
                    attr->value.booldata ? 1 : 0);
            return SAI_STATUS_SUCCESS;

        case SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS:
            SWSS_LOG_WARN("rif %s: changing SRC_MAC after create not supported in hw",
                    sai_serialize_object_id(rif_id).c_str());
            return SAI_STATUS_SUCCESS;

        default:
            SWSS_LOG_INFO("rif %s: attr %d ignored by hw", sai_serialize_object_id(rif_id).c_str(), attr->id);
            return SAI_STATUS_SUCCESS;
    }
}

sai_status_t SwitchBCM56860HW::hwRemoveRouterInterface(
        _In_ sai_object_id_t rif_id)
{
    SWSS_LOG_ENTER();

    std::string srif = sai_serialize_object_id(rif_id);

    auto it = m_rifToIntf.find(rif_id);

    if (it == m_rifToIntf.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    int rv = hwbcm_l3_intf_destroy(it->second);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("rif %s: hwbcm_l3_intf_destroy(%d) failed: %d", srif.c_str(), it->second, rv);
        return SAI_STATUS_FAILURE;
    }

    m_rifToIntf.erase(it);

    auto vit = m_rifToVid.find(rif_id);

    if (vit != m_rifToVid.end())
    {
        int vid = vit->second;

        if (vid >= RIF_VID_MIN && vid <= RIF_VID_MAX)
        {
            // port RIF: move the port(s) back to the default vlan, drop the vlan

            int defVid = vidOf(m_default_vlan_id);

            if (defVid < 0)
            {
                defVid = 1;
            }

            sai_attribute_t portAttr;

            if (getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, SAI_ROUTER_INTERFACE_ATTR_PORT_ID, portAttr))
            {
                std::vector<int> ports;
                sai_object_id_t target = portAttr.value.oid;

                if (objectTypeQuery(target) == SAI_OBJECT_TYPE_LAG)
                {
                    std::lock_guard<std::mutex> lock(m_mapMutex);

                    auto lm = m_lagMembers.find(target);

                    if (lm != m_lagMembers.end())
                    {
                        for (auto poid: lm->second)
                        {
                            int p = bcmPortOf(poid);

                            if (p >= 0)
                            {
                                ports.push_back(p);
                            }
                        }
                    }
                }
                else
                {
                    int p = bcmPortOf(target);

                    if (p >= 0)
                    {
                        ports.push_back(p);
                    }
                }

                for (int p: ports)
                {
                    hwbcm_port_pvid_set(p, defVid);
                    hwbcm_vlan_port_remove(vid, p);
                }
            }

            rv = hwbcm_vlan_destroy(vid);

            if (rv < 0)
            {
                SWSS_LOG_WARN("rif %s: hwbcm_vlan_destroy(%d) failed: %d", srif.c_str(), vid, rv);
            }
        }

        m_rifToVid.erase(vit);
    }

    SWSS_LOG_NOTICE("rif %s removed", srif.c_str());

    return SAI_STATUS_SUCCESS;
}

// --- NEIGHBOR_ENTRY -------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateNeighbor(
        _In_ const std::string &serialized,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    sai_neighbor_entry_t ne;
    sai_deserialize_neighbor_entry(serialized, ne);

    auto *macAttr = findAttr(attr_count, attr_list, SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS);

    if (macAttr == nullptr)
    {
        SWSS_LOG_ERROR("neighbor %s: missing DST_MAC_ADDRESS", serialized.c_str());
        return SAI_STATUS_FAILURE;
    }

    bool noHostRoute = false;

    auto *nhrAttr = findAttr(attr_count, attr_list, SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE);

    if (nhrAttr != nullptr)
    {
        noHostRoute = nhrAttr->value.booldata;
    }

    sai_packet_action_t action = SAI_PACKET_ACTION_FORWARD;

    auto *paAttr = findAttr(attr_count, attr_list, SAI_NEIGHBOR_ENTRY_ATTR_PACKET_ACTION);

    if (paAttr != nullptr)
    {
        action = (sai_packet_action_t)paAttr->value.s32;
    }

    auto rit = m_rifToIntf.find(ne.rif_id);

    if (rit == m_rifToIntf.end())
    {
        SWSS_LOG_WARN("neighbor %s: rif %s has no hw intf (loopback?), ignored",
                serialized.c_str(), sai_serialize_object_id(ne.rif_id).c_str());
        return SAI_STATUS_SUCCESS;
    }

    int intf = rit->second;

    // vrf of the RIF

    int vrf = 0;

    sai_attribute_t vrAttr;

    if (getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, ne.rif_id, SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID, vrAttr))
    {
        sai_object_id_t defaultVr = SAI_NULL_OBJECT_ID;

        {
            sai_attribute_t sw;

            if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID, sw))
            {
                defaultVr = sw.value.oid;
            }
        }

        vrf = resolveVrf(m_vrToVrf, defaultVr, vrAttr.value.oid, true, serialized.c_str());
    }

    // where does the neighbor live: port / trunk, or unknown (-> CPU)

    int port = -1;
    int trunk = -1;
    bool toCpu = false;

    sai_attribute_t rifType;

    if (!getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, ne.rif_id, SAI_ROUTER_INTERFACE_ATTR_TYPE, rifType))
    {
        SWSS_LOG_ERROR("neighbor %s: rif %s has no TYPE attribute", serialized.c_str(), sai_serialize_object_id(ne.rif_id).c_str());
        return SAI_STATUS_FAILURE;
    }

    if (rifType.value.s32 == SAI_ROUTER_INTERFACE_TYPE_PORT)
    {
        sai_attribute_t portAttr;

        if (getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, ne.rif_id, SAI_ROUTER_INTERFACE_ATTR_PORT_ID, portAttr))
        {
            sai_object_id_t target = portAttr.value.oid;

            if (objectTypeQuery(target) == SAI_OBJECT_TYPE_LAG)
            {
                trunk = trunkOf(target);
            }
            else
            {
                port = bcmPortOf(target);
            }
        }
    }
    else if (rifType.value.s32 == SAI_ROUTER_INTERFACE_TYPE_VLAN)
    {
        sai_attribute_t vlanAttr;

        if (getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, ne.rif_id, SAI_ROUTER_INTERFACE_ATTR_VLAN_ID, vlanAttr))
        {
            sai_object_id_t vlanOid = vlanAttr.value.oid;

            auto fdbIt = m_objectHash.find(SAI_OBJECT_TYPE_FDB_ENTRY);

            if (fdbIt != m_objectHash.end())
            {
                for (auto &kv: fdbIt->second)
                {
                    sai_fdb_entry_t fe;
                    sai_deserialize_fdb_entry(kv.first, fe);

                    if (fe.bv_id != vlanOid || !macEqual(fe.mac_address, macAttr->value.mac))
                    {
                        continue;
                    }

                    auto bp = kv.second.find("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID");

                    if (bp == kv.second.end())
                    {
                        continue;
                    }

                    sai_object_id_t target = bridgePortTarget(bp->second->getAttr()->value.oid);

                    if (objectTypeQuery(target) == SAI_OBJECT_TYPE_LAG)
                    {
                        trunk = trunkOf(target);
                    }
                    else
                    {
                        port = bcmPortOf(target);
                    }

                    break;
                }
            }
        }
    }

    if (port < 0 && trunk < 0)
    {
        SWSS_LOG_NOTICE("neighbor %s: mac not in fdb yet, punting to cpu", serialized.c_str());
        toCpu = true;
    }

    // egress object: reuse the one a NEXT_HOP (same rif+ip) already created,
    // rewriting it in place; otherwise create a new one.

    int egr = -1;
    std::vector<sai_object_id_t> matchingNh;

    for (auto &kv: m_nhToEgr)
    {
        sai_attribute_t nhRif, nhIp;

        if (!getStoredAttr(SAI_OBJECT_TYPE_NEXT_HOP, kv.first, SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID, nhRif) ||
            !getStoredAttr(SAI_OBJECT_TYPE_NEXT_HOP, kv.first, SAI_NEXT_HOP_ATTR_IP, nhIp))
        {
            continue;
        }

        if (nhRif.value.oid != ne.rif_id)
        {
            continue;
        }

        if (sai_serialize_ip_address(nhIp.value.ipaddr) != sai_serialize_ip_address(ne.ip_address))
        {
            continue;
        }

        matchingNh.push_back(kv.first);
    }

    if (action == SAI_PACKET_ACTION_DROP)
    {
        int rv = hwbcm_l3_egress_drop_create(&egr);

        if (rv < 0)
        {
            SWSS_LOG_ERROR("neighbor %s: hwbcm_l3_egress_drop_create failed: %d", serialized.c_str(), rv);
            return SAI_STATUS_FAILURE;
        }
    }
    else if (!matchingNh.empty())
    {
        for (auto nh: matchingNh)
        {
            int e = m_nhToEgr[nh];
            int rv;

            if (toCpu)
            {
                rv = hwbcm_l3_egress_cpu_replace(e, intf);
            }
            else
            {
                rv = hwbcm_l3_egress_replace(e, intf, macAttr->value.mac, port, trunk);
            }

            if (rv < 0)
            {
                SWSS_LOG_ERROR("neighbor %s: egress %d replace failed: %d", serialized.c_str(), e, rv);
                return SAI_STATUS_FAILURE;
            }

            SWSS_LOG_NOTICE("neighbor %s: next hop %s egress %d resolved (port %d trunk %d cpu %d)",
                    serialized.c_str(), sai_serialize_object_id(nh).c_str(), e, port, trunk, toCpu ? 1 : 0);

            if (egr < 0)
            {
                egr = e;
            }
        }
    }
    else
    {
        int rv;

        if (toCpu)
        {
            rv = hwbcm_l3_egress_cpu_create(intf, &egr);
        }
        else
        {
            rv = hwbcm_l3_egress_create(intf, macAttr->value.mac, port, trunk, &egr);
        }

        if (rv < 0)
        {
            SWSS_LOG_ERROR("neighbor %s: egress create failed: %d", serialized.c_str(), rv);
            return SAI_STATUS_FAILURE;
        }
    }

    // host entry

    if (!noHostRoute)
    {
        uint8_t addr[16];
        int af = ipBytes(ne.ip_address, addr);

        int rv = hwbcm_l3_host_add(vrf, af, addr, egr);

        if (rv < 0)
        {
            SWSS_LOG_ERROR("neighbor %s: hwbcm_l3_host_add failed: %d", serialized.c_str(), rv);

            if (matchingNh.empty() && !valueInMap(m_nhToEgr, egr))
            {
                hwbcm_l3_egress_destroy(egr);
            }

            return SAI_STATUS_FAILURE;
        }
    }

    m_neighborEgr[serialized] = egr;

    SWSS_LOG_NOTICE("neighbor %s -> egress %d (intf %d vrf %d port %d trunk %d cpu %d host %d)",
            serialized.c_str(), egr, intf, vrf, port, trunk, toCpu ? 1 : 0, noHostRoute ? 0 : 1);

    if (m_hwReady)
    {
        hwVxlanRetryPending(); // A4: a remote VTEP may have been waiting for this underlay
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveNeighbor(
        _In_ const std::string &serialized)
{
    SWSS_LOG_ENTER();

    auto it = m_neighborEgr.find(serialized);

    if (it == m_neighborEgr.end())
    {
        SWSS_LOG_WARN("neighbor %s unknown to hw", serialized.c_str());
        return SAI_STATUS_SUCCESS;
    }

    int egr = it->second;

    sai_neighbor_entry_t ne;
    sai_deserialize_neighbor_entry(serialized, ne);

    bool noHostRoute = false;

    sai_attribute_t nhrAttr;

    if (getStoredAttr(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, serialized, SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE, nhrAttr))
    {
        noHostRoute = nhrAttr.value.booldata;
    }

    int vrf = 0;

    sai_attribute_t vrAttr;

    if (getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, ne.rif_id, SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID, vrAttr))
    {
        sai_object_id_t defaultVr = SAI_NULL_OBJECT_ID;

        {
            sai_attribute_t sw;

            if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID, sw))
            {
                defaultVr = sw.value.oid;
            }
        }

        vrf = resolveVrf(m_vrToVrf, defaultVr, vrAttr.value.oid, true, serialized.c_str());
    }

    if (!noHostRoute)
    {
        uint8_t addr[16];
        int af = ipBytes(ne.ip_address, addr);

        int rv = hwbcm_l3_host_delete(vrf, af, addr);

        if (rv < 0)
        {
            SWSS_LOG_WARN("neighbor %s: hwbcm_l3_host_delete failed: %d (continuing)", serialized.c_str(), rv);
        }
    }

    m_neighborEgr.erase(it);

    if (valueInMap(m_nhToEgr, egr))
    {
        // next hop(s) still reference it: back to punt-to-cpu

        int intf = -1;

        auto rit = m_rifToIntf.find(ne.rif_id);

        if (rit != m_rifToIntf.end())
        {
            intf = rit->second;
        }

        int rv = hwbcm_l3_egress_cpu_replace(egr, intf);

        if (rv < 0)
        {
            SWSS_LOG_ERROR("neighbor %s: egress %d cpu replace failed: %d", serialized.c_str(), egr, rv);
            return SAI_STATUS_FAILURE;
        }

        SWSS_LOG_NOTICE("neighbor %s removed, egress %d now punts to cpu", serialized.c_str(), egr);
    }
    else if (!valueInMap(m_neighborEgr, egr))
    {
        int rv = hwbcm_l3_egress_destroy(egr);

        if (rv < 0)
        {
            SWSS_LOG_ERROR("neighbor %s: hwbcm_l3_egress_destroy(%d) failed: %d", serialized.c_str(), egr, rv);
            return SAI_STATUS_FAILURE;
        }

        SWSS_LOG_NOTICE("neighbor %s removed, egress %d destroyed", serialized.c_str(), egr);
    }

    return SAI_STATUS_SUCCESS;
}

// --- NEXT_HOP -------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateNextHop(
        _In_ sai_object_id_t nh_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    std::string snh = sai_serialize_object_id(nh_id);

    sai_next_hop_type_t type = SAI_NEXT_HOP_TYPE_IP;

    auto *typeAttr = findAttr(attr_count, attr_list, SAI_NEXT_HOP_ATTR_TYPE);

    if (typeAttr != nullptr)
    {
        type = (sai_next_hop_type_t)typeAttr->value.s32;
    }

    switch (type)
    {
        case SAI_NEXT_HOP_TYPE_IP:
            break;

        case SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP:
            SWSS_LOG_NOTICE("next hop %s: tunnel encap, handled by vxlan side", snh.c_str());
            return SAI_STATUS_SUCCESS;

        default:
            SWSS_LOG_WARN("next hop %s: type %d not supported in hw, ignored", snh.c_str(), (int)type);
            return SAI_STATUS_SUCCESS;
    }

    auto *ipAttr = findAttr(attr_count, attr_list, SAI_NEXT_HOP_ATTR_IP);
    auto *rifAttr = findAttr(attr_count, attr_list, SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID);

    if (ipAttr == nullptr || rifAttr == nullptr)
    {
        SWSS_LOG_ERROR("next hop %s: missing IP or ROUTER_INTERFACE_ID", snh.c_str());
        return SAI_STATUS_FAILURE;
    }

    // existing neighbor on the same rif+ip ?

    sai_neighbor_entry_t ne;
    memset(&ne, 0, sizeof(ne));
    ne.switch_id = m_switch_id;
    ne.rif_id = rifAttr->value.oid;
    ne.ip_address = ipAttr->value.ipaddr;

    std::string key = sai_serialize_neighbor_entry(ne);

    auto nit = m_neighborEgr.find(key);

    if (nit != m_neighborEgr.end())
    {
        m_nhToEgr[nh_id] = nit->second;

        SWSS_LOG_NOTICE("next hop %s -> egress %d (shared with neighbor %s)", snh.c_str(), nit->second, key.c_str());

        if (m_hwReady)
        {
            hwVxlanRetryPending();
        }

        return SAI_STATUS_SUCCESS;
    }

    // no neighbor yet: punt to cpu until it resolves

    int intf = -1;

    auto rit = m_rifToIntf.find(rifAttr->value.oid);

    if (rit != m_rifToIntf.end())
    {
        intf = rit->second;
    }
    else
    {
        SWSS_LOG_WARN("next hop %s: rif %s has no hw intf", snh.c_str(), sai_serialize_object_id(rifAttr->value.oid).c_str());
    }

    int egr = -1;

    int rv = hwbcm_l3_egress_cpu_create(intf, &egr);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("next hop %s: hwbcm_l3_egress_cpu_create failed: %d", snh.c_str(), rv);
        return SAI_STATUS_FAILURE;
    }

    m_nhToEgr[nh_id] = egr;

    SWSS_LOG_NOTICE("next hop %s -> egress %d (cpu, neighbor %s unresolved)", snh.c_str(), egr, key.c_str());

    if (m_hwReady)
    {
        hwVxlanRetryPending();
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveNextHop(
        _In_ sai_object_id_t nh_id)
{
    SWSS_LOG_ENTER();

    auto it = m_nhToEgr.find(nh_id);

    if (it == m_nhToEgr.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    int egr = it->second;

    m_nhToEgr.erase(it);

    if (valueInMap(m_nhToEgr, egr) || valueInMap(m_neighborEgr, egr))
    {
        SWSS_LOG_NOTICE("next hop %s removed, egress %d still in use", sai_serialize_object_id(nh_id).c_str(), egr);
        return SAI_STATUS_SUCCESS;
    }

    int rv = hwbcm_l3_egress_destroy(egr);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("next hop %s: hwbcm_l3_egress_destroy(%d) failed: %d", sai_serialize_object_id(nh_id).c_str(), egr, rv);
        return SAI_STATUS_FAILURE;
    }

    return SAI_STATUS_SUCCESS;
}

// --- NEXT_HOP_GROUP -------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateNextHopGroup(
        _In_ sai_object_id_t nhg_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto *typeAttr = findAttr(attr_count, attr_list, SAI_NEXT_HOP_GROUP_ATTR_TYPE);

    if (typeAttr != nullptr)
    {
        switch ((sai_next_hop_group_type_t)typeAttr->value.s32)
        {
            case SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP:
            case SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_ORDERED_ECMP:
                break;

            default:
                SWSS_LOG_WARN("next hop group %s: type %d not supported in hw, ignored",
                        sai_serialize_object_id(nhg_id).c_str(), typeAttr->value.s32);
                return SAI_STATUS_SUCCESS;
        }
    }

    // the ecmp object is created when the first member arrives

    SWSS_LOG_NOTICE("next hop group %s created (ecmp deferred to first member)", sai_serialize_object_id(nhg_id).c_str());

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveNextHopGroup(
        _In_ sai_object_id_t nhg_id)
{
    SWSS_LOG_ENTER();

    // drop stale members (should be gone already)

    for (auto it = m_nhgMemberToNhg.begin(); it != m_nhgMemberToNhg.end();)
    {
        if (it->second == nhg_id)
        {
            m_nhgMemberToNh.erase(it->first);
            it = m_nhgMemberToNhg.erase(it);
        }
        else
        {
            ++it;
        }
    }

    auto it = m_nhgToEcmp.find(nhg_id);

    if (it == m_nhgToEcmp.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    int rv = hwbcm_l3_ecmp_destroy(it->second);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("next hop group %s: hwbcm_l3_ecmp_destroy(%d) failed: %d",
                sai_serialize_object_id(nhg_id).c_str(), it->second, rv);
        return SAI_STATUS_FAILURE;
    }

    m_nhgToEcmp.erase(it);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwCreateNextHopGroupMember(
        _In_ sai_object_id_t member_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    std::string sm = sai_serialize_object_id(member_id);

    auto *nhgAttr = findAttr(attr_count, attr_list, SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID);
    auto *nhAttr = findAttr(attr_count, attr_list, SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID);

    if (nhgAttr == nullptr || nhAttr == nullptr)
    {
        SWSS_LOG_ERROR("nhg member %s: missing NEXT_HOP_GROUP_ID or NEXT_HOP_ID", sm.c_str());
        return SAI_STATUS_FAILURE;
    }

    sai_object_id_t nhg = nhgAttr->value.oid;
    sai_object_id_t nh = nhAttr->value.oid;

    if (m_nhToEgr.find(nh) == m_nhToEgr.end())
    {
        SWSS_LOG_ERROR("nhg member %s: next hop %s has no egress", sm.c_str(), sai_serialize_object_id(nh).c_str());
        return SAI_STATUS_FAILURE;
    }

    m_nhgMemberToNh[member_id] = nh;
    m_nhgMemberToNhg[member_id] = nhg;

    std::vector<int> egrs = groupEgressList(nhg, m_nhgMemberToNhg, m_nhgMemberToNh, m_nhToEgr);

    auto eit = m_nhgToEcmp.find(nhg);

    int rv;

    if (eit == m_nhgToEcmp.end())
    {
        int ecmp = -1;

        rv = hwbcm_l3_ecmp_create(egrs.data(), (int)egrs.size(), &ecmp);

        if (rv < 0)
        {
            SWSS_LOG_ERROR("nhg member %s: hwbcm_l3_ecmp_create failed: %d", sm.c_str(), rv);
            m_nhgMemberToNh.erase(member_id);
            m_nhgMemberToNhg.erase(member_id);
            return SAI_STATUS_FAILURE;
        }

        m_nhgToEcmp[nhg] = ecmp;

        SWSS_LOG_NOTICE("next hop group %s -> ecmp %d (%zu members)", sai_serialize_object_id(nhg).c_str(), ecmp, egrs.size());
    }
    else
    {
        rv = hwbcm_l3_ecmp_set(eit->second, egrs.data(), (int)egrs.size());

        if (rv < 0)
        {
            SWSS_LOG_ERROR("nhg member %s: hwbcm_l3_ecmp_set(%d) failed: %d", sm.c_str(), eit->second, rv);
            m_nhgMemberToNh.erase(member_id);
            m_nhgMemberToNhg.erase(member_id);
            return SAI_STATUS_FAILURE;
        }

        SWSS_LOG_NOTICE("next hop group %s ecmp %d now %zu members", sai_serialize_object_id(nhg).c_str(), eit->second, egrs.size());
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveNextHopGroupMember(
        _In_ sai_object_id_t member_id)
{
    SWSS_LOG_ENTER();

    auto git = m_nhgMemberToNhg.find(member_id);

    if (git == m_nhgMemberToNhg.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_object_id_t nhg = git->second;

    m_nhgMemberToNhg.erase(git);
    m_nhgMemberToNh.erase(member_id);

    auto eit = m_nhgToEcmp.find(nhg);

    if (eit == m_nhgToEcmp.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    std::vector<int> egrs = groupEgressList(nhg, m_nhgMemberToNhg, m_nhgMemberToNh, m_nhToEgr);

    int rv = hwbcm_l3_ecmp_set(eit->second, egrs.data(), (int)egrs.size());

    if (rv < 0)
    {
        if (egrs.empty())
        {
            // chip may refuse an empty group; the group is about to be removed anyway
            SWSS_LOG_WARN("next hop group %s ecmp %d: empty set failed: %d (ignored)",
                    sai_serialize_object_id(nhg).c_str(), eit->second, rv);
            return SAI_STATUS_SUCCESS;
        }

        SWSS_LOG_ERROR("nhg member %s: hwbcm_l3_ecmp_set(%d) failed: %d",
                sai_serialize_object_id(member_id).c_str(), eit->second, rv);
        return SAI_STATUS_FAILURE;
    }

    SWSS_LOG_NOTICE("next hop group %s ecmp %d now %zu members", sai_serialize_object_id(nhg).c_str(), eit->second, egrs.size());

    return SAI_STATUS_SUCCESS;
}

// --- ROUTE_ENTRY ----------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateRoute(
        _In_ const std::string &serialized,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    sai_route_entry_t re;
    sai_deserialize_route_entry(serialized, re);

    // vrf

    sai_object_id_t defaultVr = SAI_NULL_OBJECT_ID;

    {
        sai_attribute_t sw;

        if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID, sw))
        {
            defaultVr = sw.value.oid;
        }
    }

    int vrf = resolveVrf(m_vrToVrf, defaultVr, re.vr_id, true, serialized.c_str());

    // action + next hop

    sai_packet_action_t action = SAI_PACKET_ACTION_FORWARD;

    auto *paAttr = findAttr(attr_count, attr_list, SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION);

    if (paAttr != nullptr)
    {
        action = (sai_packet_action_t)paAttr->value.s32;
    }

    sai_object_id_t nhOid = SAI_NULL_OBJECT_ID;

    auto *nhAttr = findAttr(attr_count, attr_list, SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID);

    if (nhAttr != nullptr)
    {
        nhOid = nhAttr->value.oid;
    }

    int egr = -1;

    switch (action)
    {
        case SAI_PACKET_ACTION_DROP:
        case SAI_PACKET_ACTION_DENY:
            egr = sharedDropEgress();
            break;

        case SAI_PACKET_ACTION_TRAP:
        case SAI_PACKET_ACTION_LOG:
        case SAI_PACKET_ACTION_COPY:
            egr = sharedCpuEgress();
            break;

        case SAI_PACKET_ACTION_FORWARD:
        case SAI_PACKET_ACTION_TRANSIT:
        case SAI_PACKET_ACTION_COPY_CANCEL:
        default:
            if (nhOid == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_NOTICE("route %s: forward with null next hop, dropping", serialized.c_str());
                egr = sharedDropEgress();
                break;
            }

            if (nhOid == m_cpu_port_id)
            {
                egr = sharedCpuEgress();
                break;
            }

            switch (objectTypeQuery(nhOid))
            {
                case SAI_OBJECT_TYPE_NEXT_HOP:
                    {
                        auto it = m_nhToEgr.find(nhOid);

                        if (it == m_nhToEgr.end())
                        {
                            SWSS_LOG_ERROR("route %s: next hop %s has no egress", serialized.c_str(), sai_serialize_object_id(nhOid).c_str());
                            return SAI_STATUS_FAILURE;
                        }

                        egr = it->second;
                        break;
                    }

                case SAI_OBJECT_TYPE_NEXT_HOP_GROUP:
                    {
                        auto it = m_nhgToEcmp.find(nhOid);

                        if (it == m_nhgToEcmp.end())
                        {
                            // group without members yet: create the ecmp now
                            std::vector<int> egrs = groupEgressList(nhOid, m_nhgMemberToNhg, m_nhgMemberToNh, m_nhToEgr);
                            int ecmp = -1;

                            int rv = hwbcm_l3_ecmp_create(egrs.data(), (int)egrs.size(), &ecmp);

                            if (rv < 0)
                            {
                                SWSS_LOG_ERROR("route %s: next hop group %s has no ecmp and create failed: %d",
                                        serialized.c_str(), sai_serialize_object_id(nhOid).c_str(), rv);
                                return SAI_STATUS_FAILURE;
                            }

                            m_nhgToEcmp[nhOid] = ecmp;
                            egr = ecmp;
                        }
                        else
                        {
                            egr = it->second;
                        }

                        break;
                    }

                case SAI_OBJECT_TYPE_ROUTER_INTERFACE:
                    // directly connected subnet: punt so the kernel resolves neighbors
                    egr = sharedCpuEgress();
                    break;

                case SAI_OBJECT_TYPE_PORT:
                    SWSS_LOG_WARN("route %s: next hop is port %s (not cpu), punting", serialized.c_str(), sai_serialize_object_id(nhOid).c_str());
                    egr = sharedCpuEgress();
                    break;

                default:
                    SWSS_LOG_ERROR("route %s: unsupported next hop object %s", serialized.c_str(), sai_serialize_object_id(nhOid).c_str());
                    return SAI_STATUS_FAILURE;
            }

            break;
    }

    if (egr < 0)
    {
        SWSS_LOG_ERROR("route %s: could not resolve an egress object", serialized.c_str());
        return SAI_STATUS_FAILURE;
    }

    uint8_t prefix[16];
    int len = 0;
    int af = prefixBytes(re.destination, prefix, len);

    int rv = hwbcm_l3_route_add(vrf, af, prefix, len, egr);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("route %s: hwbcm_l3_route_add(vrf=%d) failed: %d", serialized.c_str(), vrf, rv);
        return SAI_STATUS_FAILURE;
    }

    m_routeEgr[serialized] = egr;

    SWSS_LOG_INFO("route %s -> %d (vrf %d action %d)", serialized.c_str(), egr, vrf, (int)action);

    if (m_hwReady)
    {
        hwVxlanRetryPending();
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwSetRoute(
        _In_ const std::string &serialized,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    if (attr->id != SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID && attr->id != SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION)
    {
        SWSS_LOG_INFO("route %s: attr %d ignored by hw", serialized.c_str(), attr->id);
        return SAI_STATUS_SUCCESS;
    }

    // rebuild the (action, next hop) pair from the new attr + stored other one,
    // then re-add: hwbcm_l3_route_add replaces an existing prefix in place.

    sai_attribute_t attrs[2];
    uint32_t n = 0;

    attrs[n++] = *attr;

    sai_attr_id_t other = (attr->id == SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID)
        ? SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION
        : SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;

    if (getStoredAttr(SAI_OBJECT_TYPE_ROUTE_ENTRY, serialized, other, attrs[n]))
    {
        n++;
    }

    return hwCreateRoute(serialized, n, attrs);
}

sai_status_t SwitchBCM56860HW::hwRemoveRoute(
        _In_ const std::string &serialized)
{
    SWSS_LOG_ENTER();

    auto it = m_routeEgr.find(serialized);

    if (it == m_routeEgr.end())
    {
        SWSS_LOG_WARN("route %s unknown to hw", serialized.c_str());
        return SAI_STATUS_SUCCESS;
    }

    sai_route_entry_t re;
    sai_deserialize_route_entry(serialized, re);

    sai_object_id_t defaultVr = SAI_NULL_OBJECT_ID;

    {
        sai_attribute_t sw;

        if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID, sw))
        {
            defaultVr = sw.value.oid;
        }
    }

    int vrf = resolveVrf(m_vrToVrf, defaultVr, re.vr_id, false, serialized.c_str());

    uint8_t prefix[16];
    int len = 0;
    int af = prefixBytes(re.destination, prefix, len);

    int rv = hwbcm_l3_route_delete(vrf, af, prefix, len);

    m_routeEgr.erase(it);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("route %s: hwbcm_l3_route_delete(vrf=%d) failed: %d", serialized.c_str(), vrf, rv);
        return SAI_STATUS_FAILURE;
    }

    return SAI_STATUS_SUCCESS;
}
