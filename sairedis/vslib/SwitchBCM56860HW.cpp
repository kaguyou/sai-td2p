/*
 * SwitchBCM56860HW.cpp — core of the Trident2+ hardware switch (A2).
 *
 * Construction, SDK bring-up, port map, link notifications, dispatch of
 * create/set/remove to the per-area handlers, hostif (KNET), VLAN,
 * VLAN_MEMBER, FDB_ENTRY, LAG, LAG_MEMBER and the shared helpers.
 *
 * L3 handlers live in SwitchBCM56860HW_l3.cpp (A3), VXLAN/bridge handlers in
 * SwitchBCM56860HW_vxlan.cpp (A4).
 */

#include "SwitchBCM56860HW.h"
#include "EventPayloadNetLinkMsg.h"
#include "EventPayloadPacket.h"

#include "swss/logger.h"
#include "meta/sai_serialize.h"

#include "hwbcm/hwbcm.h"
#include "hwbcm/hwbcm_stats.h"
#include "hwbcm/hwbcm_acl.h"
#include "hwbcm/hwbcm_copp.h"
#include "hwbcm/hwbcm_qos.h"
#include "hwbcm/hwbcm_mirror.h"

#include <sys/eventfd.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>
#include <net/if.h>
#include <linux/if.h>
#include <linux/rtnetlink.h>

#include <cstring>
#include <cstdlib>
#include <fstream>
#include <algorithm>
#include <chrono>

using namespace saivs;

sai_status_t SwitchBCM56860HW::bcmRvToSai(_In_ int rv)
{
    if (rv >= 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    switch (rv)
    {
        case HWBCM_E_FULL:      return SAI_STATUS_TABLE_FULL;
        case HWBCM_E_RESOURCE:
        case HWBCM_E_MEMORY:    return SAI_STATUS_INSUFFICIENT_RESOURCES;
        case HWBCM_E_EXISTS:    return SAI_STATUS_ITEM_ALREADY_EXISTS;
        case HWBCM_E_NOT_FOUND: return SAI_STATUS_ITEM_NOT_FOUND;
        case HWBCM_E_PARAM:
        case HWBCM_E_PORT:      return SAI_STATUS_INVALID_PARAMETER;
        case HWBCM_E_BUSY:      return SAI_STATUS_OBJECT_IN_USE;
        case HWBCM_E_UNAVAIL:   return SAI_STATUS_NOT_SUPPORTED;
        default:                return SAI_STATUS_FAILURE;
    }
}

#define SAI_VS_BCM_FALLBACK_PROFILE "/usr/share/sonic/hwsku/sai.profile"

/* Same value as the private define in vslib SwitchState.cpp: unit tests set
 * this bit in the counter count to write counters instead of reading them. */
#ifndef VS_COUNTERS_COUNT_MSB
#define VS_COUNTERS_COUNT_MSB (0x80000000)
#endif

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

SwitchBCM56860HW::SwitchBCM56860HW(
        _In_ sai_object_id_t switch_id,
        _In_ std::shared_ptr<RealObjectIdManager> manager,
        _In_ std::shared_ptr<SwitchConfig> config):
    SwitchBCM56850(switch_id, manager, config)
{
    SWSS_LOG_ENTER();

    if (m_switchConfig->m_useTapDevice)
    {
        SWSS_LOG_ERROR("SAI_VS_HOSTIF_USE_TAP_DEVICE is true on hardware switch, "
                "tap devices will conflict with KNET netdevs (should be forced off in Sai.cpp)");
    }
}

SwitchBCM56860HW::SwitchBCM56860HW(
        _In_ sai_object_id_t switch_id,
        _In_ std::shared_ptr<RealObjectIdManager> manager,
        _In_ std::shared_ptr<SwitchConfig> config,
        _In_ std::shared_ptr<WarmBootState> warmBootState):
    SwitchBCM56850(switch_id, manager, config, warmBootState)
{
    SWSS_LOG_ENTER();

    if (m_switchConfig->m_useTapDevice)
    {
        SWSS_LOG_ERROR("SAI_VS_HOSTIF_USE_TAP_DEVICE is true on hardware switch, "
                "tap devices will conflict with KNET netdevs (should be forced off in Sai.cpp)");
    }
}

SwitchBCM56860HW::~SwitchBCM56860HW()
{
    SWSS_LOG_ENTER();

    /* Stop accepting new SDK link/L2 callbacks into our queue and stop the
     * worker thread first, but keep m_linkEventFd open until after SDK
     * deinit has unregistered linkCallbackC/l2CallbackC (so an in-flight
     * callback during hwWarmSync or shutdown cannot write to a closed/reused
     * fd). */
    m_linkRun = false;

    if (m_linkEventFd >= 0)
    {
        uint64_t one = 1;

        ssize_t w = write(m_linkEventFd, &one, sizeof(one));

        (void)w;
    }

    if (m_linkThread.joinable())
    {
        m_linkThread.join();
    }

    if (m_hwReady)
    {
        bool doWarm = m_restartWarm && !m_uninitDataPlaneOnRemoval;

        if (doWarm)
        {
            auto t0 = std::chrono::steady_clock::now();

            // Always sync here: after PRE_SHUTDOWN linkscan and L2 learning
            // kept running (teamd stops in between), so the final scache /
            // .hwstate must reflect the state at the final shutdown.
            SWSS_LOG_NOTICE("wb: destructor warm sync (%s)",
                    m_warmSynced ? "final, after PRE_SHUTDOWN sync" : "PRE_SHUTDOWN not invoked");
            sai_status_t st = hwWarmSync();
            if (st != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("wb: destructor warm sync failed: %s", sai_serialize_status(st).c_str());
            }

            auto t1 = std::chrono::steady_clock::now();

            bool forKexec = warmDeinitForKexec();

            int rv = hwbcm_warm_deinit_ex(forKexec ? 1 : 0);

            auto t2 = std::chrono::steady_clock::now();

            if (rv < 0)
            {
                if (forKexec)
                {
                    SWSS_LOG_ERROR("wb: hwbcm_warm_deinit_ex(kexec) failed: %d: DMA not proven idle, "
                            "the warm-reboot_plugin will check the registers", rv);
                }
                else
                {
                    SWSS_LOG_WARN("hwbcm_warm_deinit failed: %d", rv);
                }
            }

            SWSS_LOG_NOTICE("wb: shutdown total %ld ms (sync %ld ms, deinit %ld ms, mode %s)",
                    (long)std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t0).count(),
                    (long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
                    (long)std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count(),
                    forKexec ? "kexec/dma-reset" : "live");
        }
        else
        {
            int rv = hwbcm_deinit();

            if (rv < 0)
            {
                SWSS_LOG_WARN("hwbcm_deinit failed: %d", rv);
            }
        }

        hwL3RestoreSharedEgress(-1, -1);
        hwbcm_acl_warm_reset();
        hwbcm_copp_warm_reset();
        hwbcm_qos_warm_reset();
        hwbcm_stats_warm_reset();
        m_hwReady = false;
    }

    if (m_linkEventFd >= 0)
    {
        close(m_linkEventFd);

        m_linkEventFd = -1;
    }
}

// ---------------------------------------------------------------------------
// initialization
// ---------------------------------------------------------------------------

std::string SwitchBCM56860HW::resolveConfigBcm() const
{
    SWSS_LOG_ENTER();

    if (!m_switchConfig->m_initConfigFile.empty())
    {
        return m_switchConfig->m_initConfigFile;
    }

    const char *env = getenv(SAI_KEY_INIT_CONFIG_FILE);

    if (env != NULL && env[0] != '\0')
    {
        SWSS_LOG_NOTICE("using %s from environment: %s", SAI_KEY_INIT_CONFIG_FILE, env);

        return env;
    }

    std::ifstream ifs(SAI_VS_BCM_FALLBACK_PROFILE);

    std::string line;

    const std::string key = std::string(SAI_KEY_INIT_CONFIG_FILE) + "=";

    while (std::getline(ifs, line))
    {
        if (line.compare(0, key.size(), key) == 0)
        {
            std::string value = line.substr(key.size());

            while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '\t'))
            {
                value.pop_back();
            }

            SWSS_LOG_NOTICE("using %s from %s: %s", SAI_KEY_INIT_CONFIG_FILE, SAI_VS_BCM_FALLBACK_PROFILE, value.c_str());

            return value;
        }
    }

    return "";
}

sai_status_t SwitchBCM56860HW::hwInitialize(
        _In_ bool warmBoot)
{
    SWSS_LOG_ENTER();

    if (m_hwReady)
    {
        return SAI_STATUS_SUCCESS;
    }

    m_configBcm = resolveConfigBcm();

    if (m_configBcm.empty())
    {
        SWSS_LOG_ERROR("%s not found in profile, environment or %s",
                SAI_KEY_INIT_CONFIG_FILE,
                SAI_VS_BCM_FALLBACK_PROFILE);

        return SAI_STATUS_FAILURE;
    }

    std::string scachePath = warmBootScachePath(!warmBoot);

    SWSS_LOG_NOTICE("initializing Broadcom SDK (%s) with %s (scache=%s)",
            warmBoot ? "WARM" : "COLD",
            m_configBcm.c_str(),
            scachePath.c_str());

    hwbcm_log_level_set(HWBCM_LOG_INFO);

    int rv = hwbcm_init_ex(m_configBcm.c_str(), warmBoot ? 1 : 0, scachePath.c_str());

    if (rv < 0)
    {
        SWSS_LOG_ERROR("hwbcm_init_ex(%s, warm=%d, %s) failed: %d",
                m_configBcm.c_str(), warmBoot ? 1 : 0, scachePath.c_str(), rv);

        return SAI_STATUS_FAILURE;
    }

    uint16_t devId = 0;
    uint8_t revId = 0;
    char name[64] = { 0 };

    if (hwbcm_chip_info(&devId, &revId, name, sizeof(name)) == 0)
    {
        SWSS_LOG_NOTICE("attached to %s (device 0x%04x rev 0x%02x) mode=%s",
                name, devId, revId, warmBoot ? "WARM" : "COLD");
    }

    // BST (buffer watermarks) + counter baselines for queue/PG/pool stats
    (void)hwbcm_stats_init();

    // link notification thread

    m_linkEventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

    if (m_linkEventFd < 0)
    {
        SWSS_LOG_ERROR("eventfd failed: %s", strerror(errno));

        (void)hwbcm_deinit();
        return SAI_STATUS_FAILURE;
    }

    m_hwReady = true;
    m_isWarmBoot = warmBoot;

    m_linkRun = true;

    m_linkThread = std::thread(&SwitchBCM56860HW::linkThreadMain, this);

    rv = hwbcm_link_callback_register(&SwitchBCM56860HW::linkCallbackC, this);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("hwbcm_link_callback_register failed: %d (no link notifications)", rv);
    }

    rv = hwbcm_l2_callback_register(&SwitchBCM56860HW::l2CallbackC, this);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("hwbcm_l2_callback_register failed: %d (no FDB notifications)", rv);
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::initialize_default_objects(
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    CHECK_STATUS(hwInitialize());

    CHECK_STATUS(SwitchBCM56850::initialize_default_objects(attr_count, attr_list));

    (void)qosBuildHwMap();

    // switch create attributes with a HW effect (e.g. SRC_MAC_ADDRESS)

    if (attr_count > 0 && attr_list == nullptr)
    {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    for (uint32_t i = 0; i < attr_count; i++)
    {
        sai_status_t status = hwSetSwitch(&attr_list[i]);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_WARN("failed to apply switch create attribute %d to HW", attr_list[i].id);
        }
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::warm_boot_initialize_objects()
{
    SWSS_LOG_ENTER();

    struct WarmInitGuard
    {
        bool &flag;
        WarmInitGuard(bool &f) : flag(f) { flag = true; }
        ~WarmInitGuard() { flag = false; }
    } warmInitGuard(m_inWarmBootInit);

    m_pendingVxlanOperNotify = false;

    CHECK_STATUS(SwitchBCM56850::warm_boot_initialize_objects());
    hwWarmScrubStaleCallbacks();
    hwWarmRestoreSwitchOids();

    std::string scachePath = warmBootScachePath(false);
    std::string hwStatePath = warmBootHwStatePath(false);

    struct stat scSt;
    bool haveScache = (stat(scachePath.c_str(), &scSt) == 0 && scSt.st_size > 0);
    bool haveHwState = hwStateValidateFile(hwStatePath);

    if (haveScache && haveHwState)
    {
        SWSS_LOG_NOTICE("wb: attempting true warm boot (scache=%s, hwstate=%s)",
                scachePath.c_str(), hwStatePath.c_str());

        sai_status_t wst = hwInitialize(/*warmBoot=*/true);
        if (wst == SAI_STATUS_SUCCESS)
        {
            wst = buildPortMaps(/*applyDefaults=*/false);
            if (wst == SAI_STATUS_SUCCESS)
            {
                wst = hwWarmRecoverKnet();
            }
            if (wst == SAI_STATUS_SUCCESS)
            {
                wst = hwStateLoad(hwStatePath);
            }
            if (wst == SAI_STATUS_SUCCESS)
            {
                wst = qosBuildHwMap(/*warmBoot=*/true);
            }
            if (wst == SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_NOTICE("wb: true warm boot completed (zero ASIC resets, zero port flaps)");
                return SAI_STATUS_SUCCESS;
            }

            SWSS_LOG_ERROR("wb: warm state recovery failed after SDK attach (%s); "
                    "tearing down warm SDK and falling back to cold SDK init + full hardware replay",
                    sai_serialize_status(wst).c_str());
            hwTeardownForColdFallback();
        }
        else
        {
            SWSS_LOG_ERROR("wb: warm SDK attach failed (%s); falling back to cold SDK init + full hardware replay "
                    "(full ASIC reset, data-plane loss; knet verdict %d%s)",
                    sai_serialize_status(wst).c_str(), hwbcm_warm_knet_state(),
                    hwbcm_warm_knet_fresh() ? ", host kexec'd or KNET reloaded" : "");
        }
    }
    else
    {
        int ks = hwbcm_warm_knet_probe(scachePath.c_str());
        SWSS_LOG_ERROR("wb: warm boot sidecar files missing or invalid (scache_ok=%d hwstate_ok=%d); "
                "falling back to cold SDK init + full hardware replay "
                "(full ASIC reset, data-plane loss; knet verdict %d%s)",
                haveScache ? 1 : 0, haveHwState ? 1 : 0, ks,
                ks == HWBCM_KNET_FRESH ? ", host kexec'd or KNET reloaded" : "");
    }

    // Fallback: cold SDK init AND full replay of every restored SAI object into
    // hardware so syncd's WARM path finds a programmed ASIC instead of an empty one.
    CHECK_STATUS(hwInitialize(/*warmBoot=*/false));
    return hwColdFallbackReplay();
}

sai_status_t SwitchBCM56860HW::set_switch_default_attributes()
{
    SWSS_LOG_ENTER();

    return SwitchBCM56850::set_switch_default_attributes();
}

sai_status_t SwitchBCM56860HW::refresh_port_list(
        _In_ const sai_attr_metadata_t *meta)
{
    SWSS_LOG_ENTER();

    return SwitchBCM56850::refresh_port_list(meta);
}

sai_status_t SwitchBCM56860HW::refresh_bridge_port_list(
        _In_ const sai_attr_metadata_t *meta,
        _In_ sai_object_id_t bridge_id)
{
    SWSS_LOG_ENTER();

    (void)meta;

    auto me_port_list = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_BRIDGE, SAI_BRIDGE_ATTR_PORT_LIST);
    auto m_port_id    = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_BRIDGE_PORT, SAI_BRIDGE_PORT_ATTR_PORT_ID);
    auto m_bridge_id  = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_BRIDGE_PORT, SAI_BRIDGE_PORT_ATTR_BRIDGE_ID);
    auto m_type       = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_BRIDGE_PORT, SAI_BRIDGE_PORT_ATTR_TYPE);

    sai_attribute_t attr;

    attr.id = SAI_SWITCH_ATTR_DEFAULT_1Q_BRIDGE_ID;
    CHECK_STATUS(get(SAI_OBJECT_TYPE_SWITCH, m_switch_id, 1, &attr));
    sai_object_id_t default_1q_bridge_id = attr.value.oid;

    auto bpHashIt = m_objectHash.find(SAI_OBJECT_TYPE_BRIDGE_PORT);

    // Populate default 1Q BRIDGE_ID on TYPE_PORT bridge ports created without one
    // (SAI makes BRIDGE_ID optional for TYPE_PORT, defaulting to the 1Q bridge).
    if (bpHashIt != m_objectHash.end())
    {
        for (const auto &bp : bpHashIt->second)
        {
            auto it = bp.second.find(m_type->attridname);
            if (it == bp.second.end() || it->second == nullptr || it->second->getAttr() == nullptr)
            {
                continue;
            }
            if (it->second->getAttr()->value.s32 != SAI_BRIDGE_PORT_TYPE_PORT)
            {
                continue;
            }
            if (bp.second.find(m_bridge_id->attridname) != bp.second.end())
            {
                continue;
            }

            SWSS_LOG_NOTICE("setting default bridge id (%s) on bridge port %s",
                    sai_serialize_object_id(default_1q_bridge_id).c_str(),
                    bp.first.c_str());

            attr.id = SAI_BRIDGE_PORT_ATTR_BRIDGE_ID;
            attr.value.oid = default_1q_bridge_id;

            CHECK_STATUS(set_internal(SAI_OBJECT_TYPE_BRIDGE_PORT, bp.first, &attr));
        }
    }

    // Collect every bridge port whose BRIDGE_ID equals bridge_id.
    // std::map keeps them in stable ascending sai_object_id_t order.
    std::map<sai_object_id_t, SwitchState::AttrHash> bridge_port_list_on_bridge_id;

    if (bpHashIt != m_objectHash.end())
    {
        for (const auto &bp : bpHashIt->second)
        {
            auto it = bp.second.find(m_bridge_id->attridname);
            if (it == bp.second.end() || it->second == nullptr || it->second->getAttr() == nullptr)
            {
                // Expected on the default 1Q_ROUTER bridge port (no BRIDGE_ID).
                SWSS_LOG_NOTICE("not found %s on bridge port: %s", m_bridge_id->attridname, bp.first.c_str());
                continue;
            }

            if (bridge_id == it->second->getAttr()->value.oid)
            {
                sai_object_id_t bridge_port = SAI_NULL_OBJECT_ID;
                sai_deserialize_object_id(bp.first, bridge_port);
                bridge_port_list_on_bridge_id[bridge_port] = bp.second;
            }
        }
    }

    // Only list physical-port TYPE_PORT bridge ports (in m_port_list order).
    // Non-physical / user-created bridge ports (TUNNEL, SUB_PORT, 1D_ROUTER,
    // 1Q_ROUTER, NEXT_HOP_GROUP, and LAG-backed TYPE_PORT whose PORT_ID is a
    // LAG OID) MUST NOT be listed here:
    //   1. PortsOrch::removeDefaultBridgePorts() runs in INIT_VIEW with a
    //      buffer sized to (m_portCount + m_systemPortCount + 1), GETs
    //      SAI_BRIDGE_PORT_ATTR_TYPE on every returned OID, and only removes
    //      TYPE_PORT ones. Any non-PORT OID left in TEMP_ASIC_STATE by
    //      Syncd::snoopGetResponse() stays with only TYPE set, gets pre-matched
    //      by VID in ComparisonLogic::matchOids(), and crashes APPLY_VIEW on
    //      mandatory CREATE_AND_SET attributes like BRIDGE_ID.
    //   2. LAG-backed TYPE_PORT bridge ports are user-created (never default
    //      switch ports); listing them can overflow PortsOrch's buffer,
    //      pollute SaiDiscovery with user LAGs, or be removed outside INIT_VIEW.
    // Leaving all user-created non-physical bridge ports out lets syncd match
    // them normally against temporaryView during APPLY_VIEW without throwing.
    std::map<sai_object_id_t, std::vector<sai_object_id_t>> portToBps;
    for (const auto &bp : bridge_port_list_on_bridge_id)
    {
        auto tit = bp.second.find(m_type->attridname);
        if (tit == bp.second.end() || tit->second == nullptr || tit->second->getAttr() == nullptr ||
            tit->second->getAttr()->value.s32 != SAI_BRIDGE_PORT_TYPE_PORT)
        {
            continue;
        }
        auto pit = bp.second.find(m_port_id->attridname);
        if (pit == bp.second.end() || pit->second == nullptr || pit->second->getAttr() == nullptr)
        {
            continue;
        }
        portToBps[pit->second->getAttr()->value.oid].push_back(bp.first);
    }

    std::vector<sai_object_id_t> bridge_port_list;
    bridge_port_list.reserve(m_port_list.size());
    std::set<sai_object_id_t> placed;

    for (const auto &p : m_port_list)
    {
        auto pit = portToBps.find(p);
        if (pit == portToBps.end())
        {
            continue;
        }
        for (sai_object_id_t bp_oid : pit->second)
        {
            if (placed.insert(bp_oid).second)
            {
                bridge_port_list.push_back(bp_oid);
            }
        }
    }

    uint32_t bridge_port_list_count = (uint32_t)bridge_port_list.size();
    size_t omitted_count = bridge_port_list_on_bridge_id.size() - bridge_port_list.size();

    SWSS_LOG_NOTICE("recalculated %s: %u (port=%zu, omitted=%zu)",
            me_port_list->attridname,
            bridge_port_list_count,
            bridge_port_list.size(),
            omitted_count);

    attr.id = SAI_BRIDGE_ATTR_PORT_LIST;
    attr.value.objlist.count = bridge_port_list_count;
    attr.value.objlist.list = bridge_port_list.data();

    return set_internal(SAI_OBJECT_TYPE_BRIDGE, sai_serialize_object_id(bridge_id), &attr);
}

// ---------------------------------------------------------------------------
// ports
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::create_ports()
{
    SWSS_LOG_ENTER();

    CHECK_STATUS(SwitchBCM56850::create_ports());

    return buildPortMaps(true);
}

void SwitchBCM56860HW::mapPort(
        _In_ sai_object_id_t port_oid,
        _In_ int bcm_port,
        _In_ uint32_t first_lane)
{
    SWSS_LOG_ENTER();

    std::string ifname;

    if (m_switchConfig->m_laneMap)
    {
        ifname = m_switchConfig->m_laneMap->getInterfaceFromLaneNumber(first_lane);
    }

    if (ifname.empty())
    {
        // syncOnLinkMsg skips the lane map name check for names starting with
        // the veth prefix "v", so use a synthetic one
        ifname = "vbcm" + std::to_string(bcm_port);
    }

    {
        std::lock_guard<std::mutex> lock(m_mapMutex);

        m_portToBcm[port_oid] = bcm_port;
        m_bcmToPort[bcm_port] = port_oid;
        m_bcmIfName[bcm_port] = ifname;
    }

    setIfNameToPortId(ifname, port_oid);

    char pname[32] = { 0 };

    hwbcm_port_name(bcm_port, pname, sizeof(pname));

    SWSS_LOG_NOTICE("port %s <-> bcm port %d (%s) lane %u ifname %s",
            sai_serialize_object_id(port_oid).c_str(),
            bcm_port,
            pname,
            first_lane,
            ifname.c_str());
}

void SwitchBCM56860HW::unmapPort(
        _In_ sai_object_id_t port_oid)
{
    SWSS_LOG_ENTER();

    std::string ifname;

    {
        std::lock_guard<std::mutex> lock(m_mapMutex);

        auto it = m_portToBcm.find(port_oid);

        if (it == m_portToBcm.end())
        {
            return;
        }

        int bcm = it->second;

        auto nit = m_bcmIfName.find(bcm);

        if (nit != m_bcmIfName.end())
        {
            ifname = nit->second;

            m_bcmIfName.erase(nit);
        }

        m_bcmToPort.erase(bcm);
        m_portToBcm.erase(it);
    }

    if (!ifname.empty())
    {
        removeIfNameToPortId(ifname);
    }
}

sai_status_t SwitchBCM56860HW::buildPortMaps(
        _In_ bool applyDefaults)
{
    SWSS_LOG_ENTER();

    if (!m_hwReady)
    {
        SWSS_LOG_ERROR("hardware not initialized");

        return SAI_STATUS_FAILURE;
    }

    {
        std::lock_guard<std::mutex> lock(m_mapMutex);

        m_portToBcm.clear();
        m_bcmToPort.clear();
        m_bcmIfName.clear();
    }

    size_t mapped = 0;

    for (auto port_oid: m_port_list)
    {
        sai_attribute_t attr;

        if (!getStoredAttr(SAI_OBJECT_TYPE_PORT, port_oid, SAI_PORT_ATTR_HW_LANE_LIST, attr) ||
                attr.value.u32list.count == 0 || attr.value.u32list.list == NULL)
        {
            SWSS_LOG_WARN("port %s has no HW_LANE_LIST, not mapped to hardware",
                    sai_serialize_object_id(port_oid).c_str());
            continue;
        }

        uint32_t lane = attr.value.u32list.list[0];

        int bcm = hwbcm_port_from_phys((int)lane);

        if (bcm < 0)
        {
            SWSS_LOG_WARN("port %s lane %u has no bcm logical port (check portmap in config.bcm)",
                    sai_serialize_object_id(port_oid).c_str(), lane);
            continue;
        }

        mapPort(port_oid, bcm, lane);

        mapped++;

        if (!applyDefaults)
        {
            // Warm boot: zero HW writes! Restore SW desired speed for SGMII
            // diagnostics and refresh live OPER_STATUS / OPER_SPEED from HW.
            if (getStoredAttr(SAI_OBJECT_TYPE_PORT, port_oid, SAI_PORT_ATTR_SPEED, attr) &&
                    attr.value.u32 > 0)
            {
                hwbcm_port_desired_speed_restore(bcm, static_cast<int>(attr.value.u32));
            }

            int up = 0;

            if (hwbcm_port_link_get(bcm, &up) == 0)
            {
                attr.id = SAI_PORT_ATTR_OPER_STATUS;
                attr.value.s32 = up ? SAI_PORT_OPER_STATUS_UP : SAI_PORT_OPER_STATUS_DOWN;

                set_internal(SAI_OBJECT_TYPE_PORT, sai_serialize_object_id(port_oid), &attr);

                uint32_t operSpeed = 0;

                if (up)
                {
                    int sp = 0;

                    if (hwbcm_port_speed_get(bcm, &sp) == 0 && sp > 0)
                    {
                        operSpeed = static_cast<uint32_t>(sp);
                    }
                }

                attr.id = SAI_PORT_ATTR_OPER_SPEED;
                attr.value.u32 = operSpeed;

                set_internal(SAI_OBJECT_TYPE_PORT, sai_serialize_object_id(port_oid), &attr);
            }

            continue;
        }

        // admin state: push the stored value (default down)

        int enable = 0;

        if (getStoredAttr(SAI_OBJECT_TYPE_PORT, port_oid, SAI_PORT_ATTR_ADMIN_STATE, attr))
        {
            enable = attr.value.booldata ? 1 : 0;
        }

        int rv = hwbcm_port_enable_set(bcm, enable);

        if (rv < 0)
        {
            SWSS_LOG_WARN("hwbcm_port_enable_set(%d, %d) failed: %d", bcm, enable, rv);
        }

        // flow control: SAI default is SAI_PORT_FLOW_CONTROL_MODE_DISABLE, the
        // SDK leaves 802.3x pause on, so a congested egress paused the sender
        // instead of dropping (2026-11-24, shaper test). Apply the stored mode.
        {
            int tx = 0, rx = 0;

            if (getStoredAttr(SAI_OBJECT_TYPE_PORT, port_oid, SAI_PORT_ATTR_GLOBAL_FLOW_CONTROL_MODE, attr))
            {
                tx = (attr.value.s32 == SAI_PORT_FLOW_CONTROL_MODE_TX_ONLY || attr.value.s32 == SAI_PORT_FLOW_CONTROL_MODE_BOTH_ENABLE) ? 1 : 0;
                rx = (attr.value.s32 == SAI_PORT_FLOW_CONTROL_MODE_RX_ONLY || attr.value.s32 == SAI_PORT_FLOW_CONTROL_MODE_BOTH_ENABLE) ? 1 : 0;
            }

            int prv = hwbcm_port_pause_set(bcm, tx, rx);

            if (prv < 0)
            {
                SWSS_LOG_WARN("hwbcm_port_pause_set(%d, %d, %d) failed: %d", bcm, tx, rx, prv);
            }
        }

        // speed: the base class default (40G) is a placeholder, store what the
        // hardware really has so GET returns the truth until orchagent sets it

        int speed = 0;

        if (hwbcm_port_speed_get(bcm, &speed) == 0 && speed > 0)
        {
            attr.id = SAI_PORT_ATTR_SPEED;
            attr.value.u32 = (uint32_t)speed;

            set_internal(SAI_OBJECT_TYPE_PORT, sai_serialize_object_id(port_oid), &attr);
        }

        int mtu = 0;

        if (hwbcm_port_mtu_get(bcm, &mtu) == 0 && mtu > 0)
        {
            attr.id = SAI_PORT_ATTR_MTU;
            attr.value.u32 = (uint32_t)mtu;

            set_internal(SAI_OBJECT_TYPE_PORT, sai_serialize_object_id(port_oid), &attr);
        }

        int up = 0;

        if (hwbcm_port_link_get(bcm, &up) == 0)
        {
            attr.id = SAI_PORT_ATTR_OPER_STATUS;
            attr.value.s32 = up ? SAI_PORT_OPER_STATUS_UP : SAI_PORT_OPER_STATUS_DOWN;

            set_internal(SAI_OBJECT_TYPE_PORT, sai_serialize_object_id(port_oid), &attr);
        }
    }

    SWSS_LOG_NOTICE("mapped %zu of %zu ports to hardware", mapped, m_port_list.size());

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwCreatePort(
        _In_ sai_object_id_t port_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto *lanes = findAttr(attr_count, attr_list, SAI_PORT_ATTR_HW_LANE_LIST);

    if (lanes == NULL || lanes->value.u32list.count == 0 || lanes->value.u32list.list == NULL)
    {
        SWSS_LOG_WARN("port %s created without HW_LANE_LIST, not mapped to hardware",
                sai_serialize_object_id(port_id).c_str());

        return SAI_STATUS_SUCCESS;
    }

    uint32_t lane = lanes->value.u32list.list[0];

    int bcm = hwbcm_port_from_phys((int)lane);

    if (bcm < 0)
    {
        SWSS_LOG_ERROR("port %s lane %u has no bcm logical port",
                sai_serialize_object_id(port_id).c_str(), lane);

        return SAI_STATUS_INVALID_PARAMETER;
    }

    mapPort(port_id, bcm, lane);

    qosOnPortMapped(port_id);

    if (attr_count > 0 && attr_list == nullptr)
    {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    for (uint32_t i = 0; i < attr_count; i++)
    {
        sai_status_t status = hwSetPort(port_id, &attr_list[i]);

        if (status != SAI_STATUS_SUCCESS)
        {
            unmapPort(port_id);

            return status;
        }
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwSetPort(
        _In_ sai_object_id_t port_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    if (attr == NULL)
    {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    if (qosIsPortAttr(attr->id))
    {
        return qosHwSetPort(port_id, attr);
    }

    if (mirrorIsPortAttr(attr->id))
    {
        return mirrorHwSetPort(port_id, attr);
    }

    int bcm = bcmPortOf(port_id);

    if (bcm < 0)
    {
        // CPU port, or a port not mapped to hardware: bookkeeping only
        return SAI_STATUS_SUCCESS;
    }

    int rv = 0;

    sai_attribute_t other;

    switch (attr->id)
    {
        case SAI_PORT_ATTR_ADMIN_STATE:

            rv = hwbcm_port_enable_set(bcm, attr->value.booldata ? 1 : 0);
            break;

        case SAI_PORT_ATTR_SPEED:
            {
                int autoneg = 0;

                if (getStoredAttr(SAI_OBJECT_TYPE_PORT, port_id, SAI_PORT_ATTR_AUTO_NEG_MODE, other))
                {
                    autoneg = other.value.booldata ? 1 : 0;
                }

                rv = hwbcm_port_speed_set(bcm, (int)attr->value.u32, autoneg);
            }
            break;

        case SAI_PORT_ATTR_AUTO_NEG_MODE:
            {
                int speed = 0;

                if (getStoredAttr(SAI_OBJECT_TYPE_PORT, port_id, SAI_PORT_ATTR_SPEED, other))
                {
                    speed = (int)other.value.u32;
                }

                if (speed > 0)
                {
                    rv = hwbcm_port_speed_set(bcm, speed, attr->value.booldata ? 1 : 0);
                }
            }
            break;

        case SAI_PORT_ATTR_MTU:

            rv = hwbcm_port_mtu_set(bcm, (int)attr->value.u32);
            break;

        case SAI_PORT_ATTR_PORT_VLAN_ID:

            rv = hwbcm_port_pvid_set(bcm, (int)attr->value.u16);
            break;

        case SAI_PORT_ATTR_GLOBAL_FLOW_CONTROL_MODE:
            {
                int tx = 0, rx = 0;

                switch (attr->value.s32)
                {
                    case SAI_PORT_FLOW_CONTROL_MODE_TX_ONLY: tx = 1; break;
                    case SAI_PORT_FLOW_CONTROL_MODE_RX_ONLY: rx = 1; break;
                    case SAI_PORT_FLOW_CONTROL_MODE_BOTH_ENABLE: tx = 1; rx = 1; break;
                    default: break;
                }

                rv = hwbcm_port_pause_set(bcm, tx, rx);
            }
            break;

        case SAI_PORT_ATTR_FEC_MODE:
        case SAI_PORT_ATTR_INTERFACE_TYPE:
        case SAI_PORT_ATTR_OPER_STATUS:
        case SAI_PORT_ATTR_HOST_TX_READY_STATUS:
        case SAI_PORT_ATTR_ADVERTISED_SPEED:
        case SAI_PORT_ATTR_PKT_TX_ENABLE:

            // silently ignored (bookkeeping only)
            break;

        default:

            SWSS_LOG_INFO("port attribute %d not programmed to hardware (bookkeeping only)", attr->id);
            break;
    }

    if (rv < 0)
    {
        SWSS_LOG_ERROR("hardware set of port attribute %d on bcm port %d failed: %d", attr->id, bcm, rv);

        return bcmRvToSai(rv);
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemovePort(
        _In_ sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    int bcm = bcmPortOf(port_id);

    if (bcm >= 0)
    {
        int rv = hwbcm_port_enable_set(bcm, 0);

        if (rv < 0)
        {
            SWSS_LOG_WARN("hwbcm_port_enable_set(%d, 0) failed: %d", bcm, rv);
        }
    }

    qosOnPortRemoved(port_id);

    mirrorOnPortRemoved(port_id);

    unmapPort(port_id);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::refresh_port_oper_speed(
        _In_ sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    int bcm = bcmPortOf(port_id);

    if (bcm < 0)
    {
        return SwitchBCM56850::refresh_port_oper_speed(port_id);
    }

    sai_attribute_t attr;

    attr.id = SAI_PORT_ATTR_OPER_STATUS;

    CHECK_STATUS(get(SAI_OBJECT_TYPE_PORT, port_id, 1, &attr));

    uint32_t operSpeed = 0;

    if (attr.value.s32 != SAI_PORT_OPER_STATUS_DOWN)
    {
        int speed = 0;

        if (hwbcm_port_speed_get(bcm, &speed) == 0 && speed > 0)
        {
            operSpeed = (uint32_t)speed;
        }
    }

    attr.id = SAI_PORT_ATTR_OPER_SPEED;
    attr.value.u32 = operSpeed;

    return set_internal(SAI_OBJECT_TYPE_PORT, sai_serialize_object_id(port_id), &attr);
}

// ---------------------------------------------------------------------------
// load-balancing hash
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwSetHash(
        _In_ sai_object_id_t hash_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    if (attr->id != SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST)
    {
        return SAI_STATUS_SUCCESS;
    }

    int set;

    if (hash_id == m_ecmp_hash_id)
    {
        set = HWBCM_HASH_ECMP;
    }
    else if (hash_id == m_lag_hash_id)
    {
        set = HWBCM_HASH_LAG;
    }
    else
    {
        /* Not bound to the switch: nothing in hardware uses it. */
        return SAI_STATUS_SUCCESS;
    }

    if (attr->value.s32list.count > 0 && attr->value.s32list.list == nullptr)
    {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    uint32_t fields = 0;

    for (uint32_t i = 0; i < attr->value.s32list.count; i++)
    {
        switch (attr->value.s32list.list[i])
        {
            case SAI_NATIVE_HASH_FIELD_SRC_IP:
            case SAI_NATIVE_HASH_FIELD_SRC_IPV4:
            case SAI_NATIVE_HASH_FIELD_SRC_IPV6:
            case SAI_NATIVE_HASH_FIELD_INNER_SRC_IP:
            case SAI_NATIVE_HASH_FIELD_INNER_SRC_IPV4:
            case SAI_NATIVE_HASH_FIELD_INNER_SRC_IPV6:   fields |= HWBCM_HASH_F_SRC_IP; break;
            case SAI_NATIVE_HASH_FIELD_DST_IP:
            case SAI_NATIVE_HASH_FIELD_DST_IPV4:
            case SAI_NATIVE_HASH_FIELD_DST_IPV6:
            case SAI_NATIVE_HASH_FIELD_INNER_DST_IP:
            case SAI_NATIVE_HASH_FIELD_INNER_DST_IPV4:
            case SAI_NATIVE_HASH_FIELD_INNER_DST_IPV6:   fields |= HWBCM_HASH_F_DST_IP; break;
            case SAI_NATIVE_HASH_FIELD_L4_SRC_PORT:
            case SAI_NATIVE_HASH_FIELD_INNER_L4_SRC_PORT: fields |= HWBCM_HASH_F_L4_SRC; break;
            case SAI_NATIVE_HASH_FIELD_L4_DST_PORT:
            case SAI_NATIVE_HASH_FIELD_INNER_L4_DST_PORT: fields |= HWBCM_HASH_F_L4_DST; break;
            case SAI_NATIVE_HASH_FIELD_IP_PROTOCOL:
            case SAI_NATIVE_HASH_FIELD_INNER_IP_PROTOCOL: fields |= HWBCM_HASH_F_PROTOCOL; break;
            case SAI_NATIVE_HASH_FIELD_IN_PORT:          fields |= HWBCM_HASH_F_IN_PORT; break;
            case SAI_NATIVE_HASH_FIELD_VLAN_ID:          fields |= HWBCM_HASH_F_VLAN; break;
            case SAI_NATIVE_HASH_FIELD_ETHERTYPE:
            case SAI_NATIVE_HASH_FIELD_INNER_ETHERTYPE:  fields |= HWBCM_HASH_F_ETHERTYPE; break;
            case SAI_NATIVE_HASH_FIELD_SRC_MAC:
            case SAI_NATIVE_HASH_FIELD_INNER_SRC_MAC:    fields |= HWBCM_HASH_F_SRC_MAC; break;
            case SAI_NATIVE_HASH_FIELD_DST_MAC:
            case SAI_NATIVE_HASH_FIELD_INNER_DST_MAC:    fields |= HWBCM_HASH_F_DST_MAC; break;
            default:
                SWSS_LOG_WARN("hash field %d has no TD2+ equivalent, ignored", attr->value.s32list.list[i]);
                break;
        }
    }

    if (fields == 0)
    {
        fields = HWBCM_HASH_F_DEFAULT;
    }

    int rv = hwbcm_hash_fields_set(set, fields);

    SWSS_LOG_NOTICE("%s hash fields 0x%x -> hardware: %d", set == HWBCM_HASH_ECMP ? "ECMP" : "LAG", fields, rv);

    return rv == 0 ? SAI_STATUS_SUCCESS : SAI_STATUS_FAILURE;
}

// ---------------------------------------------------------------------------
// port counters
// ---------------------------------------------------------------------------

static int hwPstatOf(
        _In_ sai_stat_id_t id)
{
    switch (id)
    {
#define M(sai, hw) case SAI_PORT_STAT_##sai: return HWBCM_PSTAT_##hw;
#define M1(name) M(name, name)
        M1(IF_IN_OCTETS) M1(IF_IN_UCAST_PKTS) M1(IF_IN_NON_UCAST_PKTS)
        M1(IF_IN_DISCARDS) M1(IF_IN_ERRORS) M1(IF_IN_UNKNOWN_PROTOS)
        M1(IF_IN_BROADCAST_PKTS) M1(IF_IN_MULTICAST_PKTS)
        M1(IF_OUT_OCTETS) M1(IF_OUT_UCAST_PKTS) M1(IF_OUT_NON_UCAST_PKTS)
        M1(IF_OUT_DISCARDS) M1(IF_OUT_ERRORS) M1(IF_OUT_QLEN)
        M1(IF_OUT_BROADCAST_PKTS) M1(IF_OUT_MULTICAST_PKTS)
        M1(ETHER_STATS_DROP_EVENTS) M1(ETHER_STATS_MULTICAST_PKTS)
        M1(ETHER_STATS_BROADCAST_PKTS) M1(ETHER_STATS_UNDERSIZE_PKTS)
        M1(ETHER_STATS_FRAGMENTS)
        M1(ETHER_STATS_PKTS_64_OCTETS) M1(ETHER_STATS_PKTS_65_TO_127_OCTETS)
        M1(ETHER_STATS_PKTS_128_TO_255_OCTETS) M1(ETHER_STATS_PKTS_256_TO_511_OCTETS)
        M1(ETHER_STATS_PKTS_512_TO_1023_OCTETS) M1(ETHER_STATS_PKTS_1024_TO_1518_OCTETS)
        M1(ETHER_STATS_OVERSIZE_PKTS) M1(ETHER_RX_OVERSIZE_PKTS) M1(ETHER_TX_OVERSIZE_PKTS)
        M1(ETHER_STATS_JABBERS) M1(ETHER_STATS_OCTETS) M1(ETHER_STATS_PKTS)
        M1(ETHER_STATS_COLLISIONS) M1(ETHER_STATS_CRC_ALIGN_ERRORS)
        M1(ETHER_STATS_TX_NO_ERRORS) M1(ETHER_STATS_RX_NO_ERRORS)
        M1(DOT3_STATS_FCS_ERRORS) M1(PAUSE_RX_PKTS) M1(PAUSE_TX_PKTS)
        M1(ETHER_IN_PKTS_64_OCTETS) M1(ETHER_IN_PKTS_65_TO_127_OCTETS)
        M1(ETHER_IN_PKTS_128_TO_255_OCTETS) M1(ETHER_IN_PKTS_256_TO_511_OCTETS)
        M1(ETHER_IN_PKTS_512_TO_1023_OCTETS) M1(ETHER_IN_PKTS_1024_TO_1518_OCTETS)
        M1(ETHER_IN_PKTS_1519_TO_2047_OCTETS) M1(ETHER_IN_PKTS_2048_TO_4095_OCTETS)
        M1(ETHER_IN_PKTS_4096_TO_9216_OCTETS) M1(ETHER_IN_PKTS_9217_TO_16383_OCTETS)
        M1(ETHER_OUT_PKTS_64_OCTETS) M1(ETHER_OUT_PKTS_65_TO_127_OCTETS)
        M1(ETHER_OUT_PKTS_128_TO_255_OCTETS) M1(ETHER_OUT_PKTS_256_TO_511_OCTETS)
        M1(ETHER_OUT_PKTS_512_TO_1023_OCTETS) M1(ETHER_OUT_PKTS_1024_TO_1518_OCTETS)
        M1(ETHER_OUT_PKTS_1519_TO_2047_OCTETS) M1(ETHER_OUT_PKTS_2048_TO_4095_OCTETS)
        M1(ETHER_OUT_PKTS_4096_TO_9216_OCTETS) M1(ETHER_OUT_PKTS_9217_TO_16383_OCTETS)
        M1(PFC_0_RX_PKTS) M1(PFC_1_RX_PKTS) M1(PFC_2_RX_PKTS) M1(PFC_3_RX_PKTS)
        M1(PFC_4_RX_PKTS) M1(PFC_5_RX_PKTS) M1(PFC_6_RX_PKTS) M1(PFC_7_RX_PKTS)
        M1(PFC_0_TX_PKTS) M1(PFC_1_TX_PKTS) M1(PFC_2_TX_PKTS) M1(PFC_3_TX_PKTS)
        M1(PFC_4_TX_PKTS) M1(PFC_5_TX_PKTS) M1(PFC_6_TX_PKTS) M1(PFC_7_TX_PKTS)
#undef M1
#undef M
        default:
            return HWBCM_PSTAT_NONE;
    }
}

int SwitchBCM56860HW::hwPortPstatOf(
        _In_ sai_stat_id_t id)
{
    return hwPstatOf(id);
}

sai_status_t SwitchBCM56860HW::getStatsExt(
        _In_ sai_object_type_t object_type,
        _In_ sai_object_id_t object_id,
        _In_ uint32_t number_of_counters,
        _In_ const sai_stat_id_t* counter_ids,
        _In_ sai_stats_mode_t mode,
        _Out_ uint64_t *counters)
{
    SWSS_LOG_ENTER();

    // queues / priority groups / buffer pools (SwitchBCM56860HW_stats.cpp)
    sai_status_t hwStatus;

    if (hwGetObjectStats(object_type, object_id, number_of_counters, counter_ids, mode, counters, hwStatus))
    {
        return hwStatus;
    }

    // hostif trap flow counters (SwitchBCM56860HW_copp.cpp)
    if (object_type == SAI_OBJECT_TYPE_COUNTER && m_hwReady && !(number_of_counters & VS_COUNTERS_COUNT_MSB) &&
            hwCoppCounterStats(object_id, number_of_counters, counter_ids, mode, counters))
    {
        return SAI_STATUS_SUCCESS;
    }

    int bcm = (object_type == SAI_OBJECT_TYPE_PORT && m_hwReady) ? bcmPortOf(object_id) : -1;

    /* Non-port objects, the CPU port, and the unit-test "set counters" call
     * (count with the MSB set) keep the vslib behaviour. */
    if (bcm < 0 || (number_of_counters & VS_COUNTERS_COUNT_MSB))
    {
        return SwitchBCM56850::getStatsExt(object_type, object_id, number_of_counters,
                counter_ids, mode, counters);
    }

    std::vector<int> ids(number_of_counters);

    for (uint32_t i = 0; i < number_of_counters; i++)
    {
        ids[i] = hwPstatOf(counter_ids[i]);
    }

    int n = (int)number_of_counters;

    int rv = hwbcm_port_stat_multi_get(bcm, n, ids.data(), counters);

    if (rv != 0)
    {
        SWSS_LOG_ERROR("hwbcm_port_stat_multi_get(port %d, %d counters) failed: %d", bcm, n, rv);
        return SAI_STATUS_FAILURE;
    }

    // port counters that are not bcm_stat_val_t (MMU drops, WRED, >1518 buckets, IP)
    sai_status_t xs = hwPortStatsOverlay(bcm, number_of_counters, counter_ids, mode, counters);

    if (xs != SAI_STATUS_SUCCESS)
    {
        return xs;
    }

    if (mode == SAI_STATS_MODE_READ_AND_CLEAR)
    {
        rv = hwbcm_port_stat_multi_clear(bcm, n, ids.data());

        if (rv != 0)
        {
            SWSS_LOG_ERROR("hwbcm_port_stat_multi_clear(port %d) failed: %d", bcm, rv);
            return SAI_STATUS_FAILURE;
        }
    }

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// switch attributes
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwSetSwitch(
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    if (attr == NULL || !m_hwReady)
    {
        return SAI_STATUS_SUCCESS;
    }

    if (qosIsSwitchAttr(attr->id))
    {
        return qosHwSetSwitch(attr);
    }

    int rv = 0;

    switch (attr->id)
    {
        case SAI_SWITCH_ATTR_SRC_MAC_ADDRESS:

            rv = hwbcm_switch_mac_set(attr->value.mac);

            SWSS_LOG_NOTICE("switch mac %s -> hardware: %d", sai_serialize_mac(attr->value.mac).c_str(), rv);
            break;

        case SAI_SWITCH_ATTR_FDB_AGING_TIME:

            rv = hwbcm_l2_age_set((int)attr->value.u32);
            break;

        case SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_SEED:

            rv = hwbcm_hash_seed_set(HWBCM_HASH_ECMP, attr->value.u32);
            break;

        case SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_SEED:

            rv = hwbcm_hash_seed_set(HWBCM_HASH_LAG, attr->value.u32);
            break;

        case SAI_SWITCH_ATTR_RESTART_WARM:

            m_restartWarm = attr->value.booldata;
            if (!m_restartWarm)
            {
                m_warmSynced = false;
                m_preShutdown = false;
            }
            SWSS_LOG_NOTICE("switch RESTART_WARM = %s", m_restartWarm ? "true" : "false");
            break;

        case SAI_SWITCH_ATTR_UNINIT_DATA_PLANE_ON_REMOVAL:

            m_uninitDataPlaneOnRemoval = attr->value.booldata;
            SWSS_LOG_NOTICE("switch UNINIT_DATA_PLANE_ON_REMOVAL = %s",
                    m_uninitDataPlaneOnRemoval ? "true" : "false");
            break;

        case SAI_SWITCH_ATTR_PRE_SHUTDOWN:

            SWSS_LOG_NOTICE("switch PRE_SHUTDOWN = %s (RESTART_WARM=%s)",
                    attr->value.booldata ? "true" : "false",
                    m_restartWarm ? "true" : "false");
            if (attr->value.booldata && m_restartWarm)
            {
                // Only warm-reboot (kexec) sends PRE_SHUTDOWN; remember it so
                // the final deinit aborts the KNET DMA before the kexec.
                m_preShutdown = true;
                SWSS_LOG_NOTICE("wb: PRE_SHUTDOWN: system warm reboot, final deinit will run in kexec mode");
                return hwWarmSync();
            }
            break;

        default:
            break;
    }

    if (rv < 0)
    {
        SWSS_LOG_ERROR("hardware set of switch attribute %d failed: %d", attr->id, rv);

        return bcmRvToSai(rv);
    }

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// link notifications
// ---------------------------------------------------------------------------

void SwitchBCM56860HW::linkCallbackC(
        int port,
        int up,
        void *cookie)
{
    // Called from the SDK linkscan thread: just queue and wake our thread.

    auto *self = static_cast<SwitchBCM56860HW*>(cookie);

    if (self == NULL || !self->m_linkRun.load())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(self->m_linkMutex);

        self->m_linkEvents.emplace_back(port, up);
    }

    if (self->m_linkEventFd >= 0)
    {
        uint64_t one = 1;

        ssize_t w = write(self->m_linkEventFd, &one, sizeof(one));

        (void)w;
    }
}

void SwitchBCM56860HW::linkThreadMain()
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("link thread started");

    while (m_linkRun)
    {
        struct pollfd pfd;

        pfd.fd = m_linkEventFd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, 1000);

        if (pr > 0 && (pfd.revents & POLLIN))
        {
            uint64_t v = 0;

            ssize_t r = read(m_linkEventFd, &v, sizeof(v));

            (void)r;
        }

        if (!m_linkRun)
        {
            break;
        }

        std::vector<std::pair<int,int>> events;
        std::vector<L2Event> l2events;

        {
            std::lock_guard<std::mutex> lock(m_linkMutex);

            events.swap(m_linkEvents);
            l2events.swap(m_l2Events);
        }

        for (auto &ev: events)
        {
            onLinkChange(ev.first, ev.second);
        }

        for (auto &ev: l2events)
        {
            onL2Event(ev);
        }
    }

    SWSS_LOG_NOTICE("link thread ended");
}

// ---------------------------------------------------------------------------
// FDB learn / age notifications
// ---------------------------------------------------------------------------

#define HWBCM_FDB_IFNAME_LEARN "hwbcm-learn:"
#define HWBCM_FDB_IFNAME_AGE   "hwbcm-age:"

void SwitchBCM56860HW::l2CallbackC(
        const uint8_t mac[6],
        int vid,
        int port,
        int add,
        void *cookie)
{
    // SDK thread: queue only

    auto *self = static_cast<SwitchBCM56860HW*>(cookie);

    if (self == NULL || mac == NULL || !self->m_linkRun.load())
    {
        return;
    }

    L2Event ev;

    memcpy(ev.mac, mac, sizeof(ev.mac));
    ev.vid = vid;
    ev.port = port;
    ev.add = add;

    {
        std::lock_guard<std::mutex> lock(self->m_linkMutex);

        self->m_l2Events.push_back(ev);
    }

    if (self->m_linkEventFd >= 0)
    {
        uint64_t one = 1;

        ssize_t w = write(self->m_linkEventFd, &one, sizeof(one));

        (void)w;
    }
}

void SwitchBCM56860HW::onL2Event(
        _In_ const L2Event &ev)
{
    SWSS_LOG_ENTER();

    /*
     * Link thread. Build a minimal tagged ethernet frame (dst broadcast, src =
     * learned mac, 802.1Q tag = vid) and push it as EVENT_TYPE_PACKET. Sai's
     * event thread takes the API mutex and calls our overridden
     * process_packet_for_fdb_event. The bcm port (or trunk encoding) rides in
     * the ifname; the payload port is the switch oid so the dispatcher finds
     * this switch without us touching the port maps here.
     */

    uint8_t frame[18];

    memset(frame, 0xff, 6);
    memcpy(frame + 6, ev.mac, 6);
    frame[12] = 0x81;
    frame[13] = 0x00;
    frame[14] = (uint8_t)((ev.vid >> 8) & 0x0f);
    frame[15] = (uint8_t)(ev.vid & 0xff);
    frame[16] = 0x00;
    frame[17] = 0x00;

    // "<prefix><port>:<vid>" — vid also in the name since a VXLAN VFI id
    // (> 4095) does not fit the 12-bit tag
    std::string name = std::string(ev.add ? HWBCM_FDB_IFNAME_LEARN : HWBCM_FDB_IFNAME_AGE)
        + std::to_string(ev.port) + ":" + std::to_string(ev.vid);

    auto payload = std::make_shared<EventPayloadPacket>(
            m_switch_id,
            ev.port,
            name,
            Buffer(frame, sizeof(frame)));

    if (m_switchConfig->m_eventQueue)
    {
        m_switchConfig->m_eventQueue->enqueue(std::make_shared<Event>(EVENT_TYPE_PACKET, payload));
    }
}

void SwitchBCM56860HW::process_packet_for_fdb_event(
        _In_ sai_object_id_t portId,
        _In_ const std::string& name,
        _In_ const uint8_t *buffer,
        _In_ size_t size)
{
    SWSS_LOG_ENTER();

    // API thread, under the Sai mutex

    bool learn = (name.compare(0, strlen(HWBCM_FDB_IFNAME_LEARN), HWBCM_FDB_IFNAME_LEARN) == 0);
    bool age = (name.compare(0, strlen(HWBCM_FDB_IFNAME_AGE), HWBCM_FDB_IFNAME_AGE) == 0);

    if (!learn && !age)
    {
        // not ours (tap device path, unused on hardware)
        SwitchBCM56850::process_packet_for_fdb_event(portId, name, buffer, size);
        return;
    }

    if (buffer == NULL || size < 18 || buffer[12] != 0x81 || buffer[13] != 0x00)
    {
        SWSS_LOG_ERROR("malformed internal fdb event frame (%zu bytes)", size);
        return;
    }

    int bcm = 0;
    int vid = 0;

    if (sscanf(name.c_str() + (learn ? strlen(HWBCM_FDB_IFNAME_LEARN) : strlen(HWBCM_FDB_IFNAME_AGE)),
                "%d:%d", &bcm, &vid) != 2)
    {
        SWSS_LOG_ERROR("malformed internal fdb event name %s", name.c_str());
        return;
    }

    // VXLAN: events on a VNI-mapped VLAN come as VFI id + VP gport (A4
    // translates them to the real VLAN and bcm port/trunk, or says drop)

    int trunk = -1;

    if (!hwVxlanTranslateL2Event(vid, bcm, trunk))
    {
        SWSS_LOG_DEBUG("fdb %s event dropped by vxlan translation", learn ? "learn" : "age");
        return;
    }

    if (bcm < 0 && trunk >= 0)
    {
        bcm = HWBCM_L2_CB_TRUNK_PORT(trunk);
    }

    // frame with the (possibly translated) vid in the tag, for the stock code

    uint8_t frame[18];

    memcpy(frame, buffer, sizeof(frame));
    frame[14] = (uint8_t)((vid >> 8) & 0x0f);
    frame[15] = (uint8_t)(vid & 0xff);

    const uint8_t *mac = frame + 6;

    // resolve the physical port oid (for a trunk: any current member, the
    // stock code maps it back to the LAG via getLagFromPort)

    sai_object_id_t port_oid = SAI_NULL_OBJECT_ID;

    if (HWBCM_L2_CB_IS_TRUNK(bcm))
    {
        int tid = HWBCM_L2_CB_TRUNK_ID(bcm);

        for (const auto &kvp: m_lagToTrunk)
        {
            if (kvp.second != tid)
            {
                continue;
            }

            auto mit = m_lagMembers.find(kvp.first);

            if (mit != m_lagMembers.end() && !mit->second.empty())
            {
                port_oid = mit->second.front();
            }

            break;
        }
    }
    else
    {
        port_oid = portOidOf(bcm);
    }

    if (port_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_INFO("fdb %s event for %s vid %d on unmapped bcm port %d, ignored",
                learn ? "learn" : "age", sai_serialize_mac(mac).c_str(), vid, bcm);
        return;
    }

    if (age)
    {
        onFdbAged(port_oid, mac, vid);
        return;
    }

    // learn on an admin-down port can only be stale (learned by the ASIC in
    // the init window before the SAI default admin-down was applied): never
    // report it, orchagent would trip over it while removing the default
    // bridge ports (OBJECT_IN_USE -> abort, 2026-11-21). Drop it from the L2
    // table too so it is learned again, with an event, once the port is up.
    {
        sai_attribute_t adm;

        if (getStoredAttr(SAI_OBJECT_TYPE_PORT, port_oid, SAI_PORT_ATTR_ADMIN_STATE, adm) && !adm.value.booldata)
        {
            SWSS_LOG_NOTICE("fdb learn %s vid %d on admin-down bcm port %d ignored and flushed",
                    sai_serialize_mac(mac).c_str(), vid, bcm);

            if (vid > 0 && vid <= 4095)
            {
                hwbcm_l2_addr_delete(mac, vid);
            }

            return;
        }
    }

    // learn: skip entries SONiC already knows (static ones programmed by
    // orchagent come back from the ASIC as ADD events too)

    sai_object_id_t vlan_oid = vlanOidOf(vid);

    if (vlan_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_INFO("fdb learn %s vid %d: no VLAN object for vid (rif vlan?), ignored",
                sai_serialize_mac(mac).c_str(), vid);
        return;
    }

    sai_fdb_entry_t fdb;

    memset(&fdb, 0, sizeof(fdb));
    fdb.switch_id = m_switch_id;
    fdb.bv_id = vlan_oid;
    memcpy(fdb.mac_address, mac, sizeof(fdb.mac_address));

    auto sid = sai_serialize_fdb_entry(fdb);

    auto &fdbHash = m_objectHash.at(SAI_OBJECT_TYPE_FDB_ENTRY);

    if (fdbHash.find(sid) != fdbHash.end())
    {
        SWSS_LOG_DEBUG("fdb %s already known, not re-reported", sid.c_str());
        return;
    }

    SWSS_LOG_NOTICE("fdb learn %s vid %d on bcm port %d (%s)",
            sai_serialize_mac(mac).c_str(), vid, bcm, sai_serialize_object_id(port_oid).c_str());

    // stock vslib learning: bridge port lookup, learning mode check,
    // m_fdb_info_set, local DB create (DYNAMIC, no HW write) and the
    // SAI_FDB_EVENT_LEARNED notification

    SwitchBCM56850::process_packet_for_fdb_event(port_oid, name, frame, sizeof(frame));

    // a neighbor punted to the CPU (A3) may have been waiting for this MAC
    {
        int lp = -1;
        int lt = -1;

        if (HWBCM_L2_CB_IS_TRUNK(bcm))
        {
            lt = HWBCM_L2_CB_TRUNK_ID(bcm);
        }
        else
        {
            lp = bcm;
        }

        hwRefreshNeighborsOnLearn(vlan_oid, mac, lp, lt);
    }
}

void SwitchBCM56860HW::onFdbAged(
        _In_ sai_object_id_t port_oid,
        _In_ const uint8_t mac[6],
        _In_ int vid)
{
    SWSS_LOG_ENTER();

    FdbInfo key;

    key.setVlanId((sai_vlan_id_t)vid);
    memcpy(key.m_fdbEntry.mac_address, mac, sizeof(sai_mac_t));

    auto it = m_fdb_info_set.find(key);

    if (it == m_fdb_info_set.end())
    {
        // static entry removed by orchagent, or never reported: nothing to do
        SWSS_LOG_DEBUG("fdb age %s vid %d not in learned set, ignored",
                sai_serialize_mac(mac).c_str(), vid);
        return;
    }

    FdbInfo fi = *it;

    m_fdb_info_set.erase(it);

    SWSS_LOG_NOTICE("fdb aged %s vid %d (%s)",
            sai_serialize_mac(mac).c_str(), vid, sai_serialize_object_id(port_oid).c_str());

    sai_attribute_t attrs[2];

    attrs[0].id = SAI_FDB_ENTRY_ATTR_TYPE;
    attrs[0].value.s32 = SAI_FDB_ENTRY_TYPE_DYNAMIC;

    attrs[1].id = SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID;
    attrs[1].value.oid = fi.getBridgePortId();

    sai_fdb_event_notification_data_t data;

    data.event_type = SAI_FDB_EVENT_AGED;
    data.fdb_entry = fi.getFdbEntry();
    data.attr_count = 2;
    data.attr = attrs;

    // the ASIC already dropped the entry: bookkeeping only, no HW delete

    // neighbors resolved to this MAC go back to punting until it is re-learned
    hwPuntNeighborsOnAge(data.fdb_entry.bv_id, mac);

    auto sid = sai_serialize_fdb_entry(data.fdb_entry);

    sai_status_t status = SwitchBCM56850::remove(SAI_OBJECT_TYPE_FDB_ENTRY, sid);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("failed to remove aged fdb entry %s: %s", sid.c_str(), sai_serialize_status(status).c_str());
    }

    send_fdb_event_notification(data);
}

void SwitchBCM56860HW::processFdbEntriesForAging()
{
    SWSS_LOG_ENTER();

    // the ASIC ages entries (hwbcm_l2_age_set from SAI_SWITCH_ATTR_FDB_AGING_TIME)
    // and reports them through the l2 callback; the vslib timer must not.
}

sai_object_id_t SwitchBCM56860HW::vlanOidOf(
        _In_ int vid)
{
    SWSS_LOG_ENTER();

    auto oit = m_objectHash.find(SAI_OBJECT_TYPE_VLAN);

    if (oit == m_objectHash.end())
    {
        return SAI_NULL_OBJECT_ID;
    }

    for (const auto &kvp: oit->second)
    {
        sai_attribute_t attr;

        if (getStoredAttr(SAI_OBJECT_TYPE_VLAN, kvp.first, SAI_VLAN_ATTR_VLAN_ID, attr) &&
                (int)attr.value.u16 == vid)
        {
            sai_object_id_t oid;

            sai_deserialize_object_id(kvp.first, oid);

            return oid;
        }
    }

    return SAI_NULL_OBJECT_ID;
}

void SwitchBCM56860HW::onLinkChange(
        int port,
        int up)
{
    SWSS_LOG_ENTER();

    sai_object_id_t port_oid = SAI_NULL_OBJECT_ID;
    std::string ifname;

    {
        std::lock_guard<std::mutex> lock(m_mapMutex);

        auto it = m_bcmToPort.find(port);

        if (it == m_bcmToPort.end())
        {
            SWSS_LOG_INFO("link change on unmapped bcm port %d (%s), ignored", port, up ? "up" : "down");

            return;
        }

        port_oid = it->second;

        auto nit = m_bcmIfName.find(port);

        if (nit != m_bcmIfName.end())
        {
            ifname = nit->second;
        }
    }

    SWSS_LOG_NOTICE("bcm port %d (%s) link %s",
            port,
            sai_serialize_object_id(port_oid).c_str(),
            up ? "UP" : "DOWN");

    /*
     * We are on our own thread; the SAI API is serialized by the mutex in
     * Sai.cpp which we can't take from here. Push a synthetic RTM_NEWLINK into
     * the vslib event queue: Sai's event thread takes the mutex and calls
     * SwitchStateBase::syncOnLinkMsg -> send_port_oper_status_notification,
     * which updates SAI_PORT_ATTR_OPER_STATUS and emits
     * SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY. hasIfIndex() is overridden to
     * accept our synthetic ifindex, ifname was registered in mapPort().
     */

    unsigned int flags = IFF_UP;

    if (up)
    {
        flags |= (unsigned int)(IFF_RUNNING | IFF_LOWER_UP);
    }

    auto payload = std::make_shared<EventPayloadNetLinkMsg>(
            m_switch_id,
            RTM_NEWLINK,
            LINK_IFINDEX_BASE + port,
            flags,
            ifname);

    if (m_switchConfig->m_eventQueue)
    {
        m_switchConfig->m_eventQueue->enqueue(std::make_shared<Event>(EVENT_TYPE_NET_LINK_MSG, payload));
    }
    else
    {
        SWSS_LOG_ERROR("no event queue on switch config, link change dropped");
    }
}

bool SwitchBCM56860HW::hasIfIndex(
        _In_ int ifIndex) const
{
    SWSS_LOG_ENTER();

    if (ifIndex >= LINK_IFINDEX_BASE && ifIndex < LINK_IFINDEX_BASE + HWBCM_MAX_PORTS)
    {
        return true;
    }

    return SwitchBCM56850::hasIfIndex(ifIndex);
}

// ---------------------------------------------------------------------------
// hostif (KNET)
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateHostif(
        _In_ sai_object_id_t hostif_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto *type = findAttr(attr_count, attr_list, SAI_HOSTIF_ATTR_TYPE);

    if (type == NULL || type->value.s32 != SAI_HOSTIF_TYPE_NETDEV)
    {
        SWSS_LOG_INFO("hostif %s is not a netdev, bookkeeping only",
                sai_serialize_object_id(hostif_id).c_str());

        return SAI_STATUS_SUCCESS;
    }

    auto *obj = findAttr(attr_count, attr_list, SAI_HOSTIF_ATTR_OBJ_ID);
    auto *name = findAttr(attr_count, attr_list, SAI_HOSTIF_ATTR_NAME);

    if (obj == NULL || name == NULL)
    {
        SWSS_LOG_ERROR("hostif %s netdev without OBJ_ID or NAME",
                sai_serialize_object_id(hostif_id).c_str());

        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_object_type_t ot = objectTypeQuery(obj->value.oid);

    if (ot != SAI_OBJECT_TYPE_PORT)
    {
        SWSS_LOG_WARN("hostif %s on %s %s: only port netdevs are created on hardware (bookkeeping only)",
                sai_serialize_object_id(hostif_id).c_str(),
                sai_serialize_object_type(ot).c_str(),
                sai_serialize_object_id(obj->value.oid).c_str());

        return SAI_STATUS_SUCCESS;
    }

    int bcm = bcmPortOf(obj->value.oid);

    if (bcm < 0)
    {
        SWSS_LOG_WARN("hostif %s: port %s not mapped to hardware (bookkeeping only)",
                sai_serialize_object_id(hostif_id).c_str(),
                sai_serialize_object_id(obj->value.oid).c_str());

        return SAI_STATUS_SUCCESS;
    }

    char ifname[SAI_HOSTIF_NAME_SIZE + 1];

    memset(ifname, 0, sizeof(ifname));
    memcpy(ifname, name->value.chardata, SAI_HOSTIF_NAME_SIZE);

    uint8_t mac[6] = { 0 };

    sai_attribute_t sattr;

    if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_SRC_MAC_ADDRESS, sattr))
    {
        memcpy(mac, sattr.value.mac, sizeof(mac));
    }
    else
    {
        SWSS_LOG_WARN("switch has no SRC_MAC_ADDRESS yet, netdev %s gets a zero mac", ifname);
    }

    int netif = 0;

    int rv = hwbcm_knet_netif_create(bcm, ifname, mac, &netif);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("hwbcm_knet_netif_create(%d, %s) failed: %d", bcm, ifname, rv);

        return bcmRvToSai(rv);
    }

    m_hostifToNetif[hostif_id] = netif;

    SWSS_LOG_NOTICE("hostif %s: knet netdev %s on bcm port %d (netif %d)",
            sai_serialize_object_id(hostif_id).c_str(), ifname, bcm, netif);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveHostif(
        _In_ sai_object_id_t hostif_id)
{
    SWSS_LOG_ENTER();

    auto it = m_hostifToNetif.find(hostif_id);

    if (it == m_hostifToNetif.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    int rv = hwbcm_knet_netif_destroy(it->second);

    if (rv < 0 && rv != HWBCM_E_NOT_FOUND)
    {
        SWSS_LOG_ERROR("hwbcm_knet_netif_destroy(%d) failed: %d", it->second, rv);

        return bcmRvToSai(rv);
    }

    m_hostifToNetif.erase(it);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwCreateHostifTrap(
        _In_ sai_object_id_t trap_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto *type = findAttr(attr_count, attr_list, SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE);

    if (type == NULL)
    {
        return SAI_STATUS_SUCCESS;
    }

    int reason = -1;

    switch (type->value.s32)
    {
        case SAI_HOSTIF_TRAP_TYPE_ARP_REQUEST:
        case SAI_HOSTIF_TRAP_TYPE_ARP_RESPONSE:
        case SAI_HOSTIF_TRAP_TYPE_IPV6_NEIGHBOR_DISCOVERY:
        case SAI_HOSTIF_TRAP_TYPE_IPV6_NEIGHBOR_SOLICITATION:
        case SAI_HOSTIF_TRAP_TYPE_IPV6_NEIGHBOR_ADVERTISEMENT:
            reason = HWBCM_TRAP_ARP;
            break;

        case SAI_HOSTIF_TRAP_TYPE_LLDP:
            reason = HWBCM_TRAP_LLDP;
            break;

        case SAI_HOSTIF_TRAP_TYPE_LACP:
            reason = HWBCM_TRAP_LACP;
            break;

        case SAI_HOSTIF_TRAP_TYPE_IP2ME:
        case SAI_HOSTIF_TRAP_TYPE_SSH:
        case SAI_HOSTIF_TRAP_TYPE_SNMP:
            reason = HWBCM_TRAP_IP2ME;
            break;

        case SAI_HOSTIF_TRAP_TYPE_DHCP:
        case SAI_HOSTIF_TRAP_TYPE_DHCPV6:
        case SAI_HOSTIF_TRAP_TYPE_DHCP_L2:
        case SAI_HOSTIF_TRAP_TYPE_DHCPV6_L2:
            reason = HWBCM_TRAP_DHCP;
            break;

        case SAI_HOSTIF_TRAP_TYPE_BGP:
        case SAI_HOSTIF_TRAP_TYPE_BGPV6:
            reason = HWBCM_TRAP_BGP;
            break;

        case SAI_HOSTIF_TRAP_TYPE_TTL_ERROR:
            reason = HWBCM_TRAP_TTL_ERR;
            break;

        case SAI_HOSTIF_TRAP_TYPE_L3_MTU_ERROR:
        case SAI_HOSTIF_TRAP_TYPE_NEIGHBOR_MISS:
            reason = HWBCM_TRAP_NEIGH_MISS;
            break;

        case SAI_HOSTIF_TRAP_TYPE_UDLD:
            reason = HWBCM_TRAP_UDLD;
            break;

        case SAI_HOSTIF_TRAP_TYPE_STP:
        case SAI_HOSTIF_TRAP_TYPE_PVRST:
            reason = HWBCM_TRAP_STP;
            break;

        default:
            break;
    }

    if (reason < 0)
    {
        SWSS_LOG_NOTICE("hostif trap %s type %d not supported on hardware, bookkeeping only",
                sai_serialize_object_id(trap_id).c_str(), type->value.s32);

        return SAI_STATUS_SUCCESS;
    }

    int action = 2; // trap

    auto *pa = findAttr(attr_count, attr_list, SAI_HOSTIF_TRAP_ATTR_PACKET_ACTION);

    if (pa != NULL)
    {
        switch (pa->value.s32)
        {
            case SAI_PACKET_ACTION_DROP:
            case SAI_PACKET_ACTION_DENY:
                action = 0;
                break;

            case SAI_PACKET_ACTION_COPY:
            case SAI_PACKET_ACTION_LOG:
                action = 1;
                break;

            case SAI_PACKET_ACTION_TRAP:
                action = 2;
                break;

            case SAI_PACKET_ACTION_FORWARD:
            case SAI_PACKET_ACTION_TRANSIT:
            default:

                SWSS_LOG_NOTICE("hostif trap %s packet action %d: nothing to program",
                        sai_serialize_object_id(trap_id).c_str(), pa->value.s32);

                return SAI_STATUS_SUCCESS;
        }
    }

    int queue = 0;

    auto *tg = findAttr(attr_count, attr_list, SAI_HOSTIF_TRAP_ATTR_TRAP_GROUP);

    if (tg != NULL && tg->value.oid != SAI_NULL_OBJECT_ID)
    {
        sai_attribute_t q;

        if (getStoredAttr(SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP, tg->value.oid, SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE, q))
        {
            queue = (int)q.value.u32;
        }
    }

    int rv = hwbcm_trap_set(reason, action, queue);

    if (rv < 0)
    {
        // trap policy is not critical for the data path: keep orchagent going
        SWSS_LOG_WARN("hwbcm_trap_set(%d, %d, %d) for trap %s failed: %d (ignored)",
                reason, action, queue, sai_serialize_object_id(trap_id).c_str(), rv);
    }

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// VLAN
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateVlan(
        _In_ sai_object_id_t vlan_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto *vid = findAttr(attr_count, attr_list, SAI_VLAN_ATTR_VLAN_ID);

    if (vid == NULL)
    {
        SWSS_LOG_ERROR("vlan %s created without VLAN_ID", sai_serialize_object_id(vlan_id).c_str());

        return SAI_STATUS_INVALID_PARAMETER;
    }

    int rv = hwbcm_vlan_create((int)vid->value.u16);

    if (rv < 0 && rv != HWBCM_E_EXISTS)
    {
        SWSS_LOG_ERROR("hwbcm_vlan_create(%u) failed: %d", vid->value.u16, rv);

        return bcmRvToSai(rv);
    }

    SWSS_LOG_NOTICE("vlan %u created on hardware (%s)", vid->value.u16, sai_serialize_object_id(vlan_id).c_str());

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveVlan(
        _In_ sai_object_id_t vlan_id)
{
    SWSS_LOG_ENTER();

    int vid = vidOf(vlan_id);

    if (vid < 0)
    {
        SWSS_LOG_WARN("vlan %s has no VLAN_ID, nothing to remove on hardware",
                sai_serialize_object_id(vlan_id).c_str());

        return SAI_STATUS_SUCCESS;
    }

    if (vid == DEFAULT_VLAN_NUMBER)
    {
        // default VLAN can't be destroyed on bcm
        return SAI_STATUS_SUCCESS;
    }

    int rv = hwbcm_vlan_destroy(vid);

    if (rv < 0 && rv != HWBCM_E_NOT_FOUND)
    {
        SWSS_LOG_ERROR("hwbcm_vlan_destroy(%d) failed: %d", vid, rv);

        return bcmRvToSai(rv);
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwCreateVlanMember(
        _In_ sai_object_id_t member_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto *vlan = findAttr(attr_count, attr_list, SAI_VLAN_MEMBER_ATTR_VLAN_ID);
    auto *bp = findAttr(attr_count, attr_list, SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID);
    auto *mode = findAttr(attr_count, attr_list, SAI_VLAN_MEMBER_ATTR_VLAN_TAGGING_MODE);

    if (vlan == NULL || bp == NULL)
    {
        SWSS_LOG_ERROR("vlan member %s without VLAN_ID or BRIDGE_PORT_ID",
                sai_serialize_object_id(member_id).c_str());

        return SAI_STATUS_INVALID_PARAMETER;
    }

    int vid = vidOf(vlan->value.oid);

    if (vid < 0)
    {
        SWSS_LOG_ERROR("vlan member %s: vlan %s unknown",
                sai_serialize_object_id(member_id).c_str(),
                sai_serialize_object_id(vlan->value.oid).c_str());

        return SAI_STATUS_INVALID_PARAMETER;
    }

    int tagged = (mode != NULL && mode->value.s32 == SAI_VLAN_TAGGING_MODE_TAGGED) ? 1 : 0;

    sai_object_id_t target = bridgePortTarget(bp->value.oid);

    std::vector<int> ports;

    sai_object_type_t ot = objectTypeQuery(target);

    if (ot == SAI_OBJECT_TYPE_PORT)
    {
        int bcm = bcmPortOf(target);

        if (bcm >= 0)
        {
            ports.push_back(bcm);
        }
    }
    else if (ot == SAI_OBJECT_TYPE_LAG)
    {
        auto it = m_lagMembers.find(target);

        if (it != m_lagMembers.end())
        {
            for (auto p: it->second)
            {
                int bcm = bcmPortOf(p);

                if (bcm >= 0)
                {
                    ports.push_back(bcm);
                }
            }
        }

        SWSS_LOG_NOTICE("vlan member %s on LAG %s: adding current %zu member ports (later members via syncLagMemberPort)",
                sai_serialize_object_id(member_id).c_str(),
                sai_serialize_object_id(target).c_str(),
                ports.size());
    }
    else
    {
        SWSS_LOG_WARN("vlan member %s: bridge port %s target %s is %s, bookkeeping only",
                sai_serialize_object_id(member_id).c_str(),
                sai_serialize_object_id(bp->value.oid).c_str(),
                sai_serialize_object_id(target).c_str(),
                sai_serialize_object_type(ot).c_str());

        return SAI_STATUS_SUCCESS;
    }

    for (int bcm: ports)
    {
        int rv = hwbcm_vlan_port_add(vid, bcm, tagged);

        if (rv < 0 && rv != HWBCM_E_EXISTS)
        {
            SWSS_LOG_ERROR("hwbcm_vlan_port_add(%d, %d, %d) failed: %d", vid, bcm, tagged, rv);

            return bcmRvToSai(rv);
        }

        SWSS_LOG_NOTICE("vlan %d: bcm port %d added %s", vid, bcm, tagged ? "tagged" : "untagged");
    }

    // VXLAN access side (no-op unless the VLAN is mapped to a VNI)

    return hwVxlanVlanMemberChanged(member_id, true);
}

sai_status_t SwitchBCM56860HW::hwRemoveVlanMember(
        _In_ sai_object_id_t member_id)
{
    SWSS_LOG_ENTER();

    // VXLAN access side first (no-op unless the VLAN is mapped to a VNI)

    CHECK_STATUS(hwVxlanVlanMemberChanged(member_id, false));

    sai_attribute_t vlan, bp;

    if (!getStoredAttr(SAI_OBJECT_TYPE_VLAN_MEMBER, member_id, SAI_VLAN_MEMBER_ATTR_VLAN_ID, vlan) ||
            !getStoredAttr(SAI_OBJECT_TYPE_VLAN_MEMBER, member_id, SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID, bp))
    {
        SWSS_LOG_WARN("vlan member %s has no VLAN_ID/BRIDGE_PORT_ID stored",
                sai_serialize_object_id(member_id).c_str());

        return SAI_STATUS_SUCCESS;
    }

    int vid = vidOf(vlan.value.oid);

    if (vid < 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_object_id_t target = bridgePortTarget(bp.value.oid);

    std::vector<int> ports;

    sai_object_type_t ot = objectTypeQuery(target);

    if (ot == SAI_OBJECT_TYPE_PORT)
    {
        int bcm = bcmPortOf(target);

        if (bcm >= 0)
        {
            ports.push_back(bcm);
        }
    }
    else if (ot == SAI_OBJECT_TYPE_LAG)
    {
        auto it = m_lagMembers.find(target);

        if (it != m_lagMembers.end())
        {
            for (auto p: it->second)
            {
                int bcm = bcmPortOf(p);

                if (bcm >= 0)
                {
                    ports.push_back(bcm);
                }
            }
        }
    }

    for (int bcm: ports)
    {
        int rv = hwbcm_vlan_port_remove(vid, bcm);

        if (rv < 0 && rv != HWBCM_E_NOT_FOUND)
        {
            SWSS_LOG_ERROR("hwbcm_vlan_port_remove(%d, %d) failed: %d", vid, bcm, rv);

            return bcmRvToSai(rv);
        }

        SWSS_LOG_NOTICE("vlan %d: bcm port %d removed", vid, bcm);
    }

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// FDB
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateFdbEntry(
        _In_ const std::string &serialized,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    sai_fdb_entry_t fdb;

    sai_deserialize_fdb_entry(serialized, fdb);

    auto *type = findAttr(attr_count, attr_list, SAI_FDB_ENTRY_ATTR_TYPE);

    int isStatic = (type != NULL && type->value.s32 == SAI_FDB_ENTRY_TYPE_STATIC) ? 1 : 0;

    if (!isStatic)
    {
        // dynamic entries are learned by the ASIC itself; this create comes
        // from our own learn notification (updateLocalDB), bookkeeping only
        return SAI_STATUS_SUCCESS;
    }

    auto *pa = findAttr(attr_count, attr_list, SAI_FDB_ENTRY_ATTR_PACKET_ACTION);

    if (pa != NULL && pa->value.s32 != SAI_PACKET_ACTION_FORWARD)
    {
        SWSS_LOG_WARN("fdb %s packet action %d not supported on hardware, bookkeeping only",
                serialized.c_str(), pa->value.s32);

        return SAI_STATUS_SUCCESS;
    }

    auto *bp = findAttr(attr_count, attr_list, SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID);

    if (bp == NULL || bp->value.oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("fdb %s without BRIDGE_PORT_ID, bookkeeping only", serialized.c_str());

        return SAI_STATUS_SUCCESS;
    }

    // remote MAC behind a VXLAN tunnel bridge port: A4 owns that path

    sai_attribute_t bpType;

    if (getStoredAttr(SAI_OBJECT_TYPE_BRIDGE_PORT, bp->value.oid, SAI_BRIDGE_PORT_ATTR_TYPE, bpType) &&
            bpType.value.s32 == SAI_BRIDGE_PORT_TYPE_TUNNEL)
    {
        return hwFdbOnTunnel(fdb, bp->value.oid, true);
    }

    int vid = vidOf(fdb.bv_id);

    if (vid < 0)
    {
        SWSS_LOG_WARN("fdb %s: bv_id %s is not a known VLAN, bookkeeping only",
                serialized.c_str(), sai_serialize_object_id(fdb.bv_id).c_str());

        return SAI_STATUS_SUCCESS;
    }

    if (m_vidToVni.find(vid) != m_vidToVni.end())
    {
        // local MACs of a VNI-mapped VLAN live in the VFI key space, a plain
        // L2 entry keyed by vid would not match (A4 finding)
        SWSS_LOG_NOTICE("fdb %s: vlan %d is mapped to vni %u, static entry not programmed (bookkeeping only)",
                serialized.c_str(), vid, m_vidToVni.at(vid));

        return SAI_STATUS_SUCCESS;
    }

    sai_object_id_t target = bridgePortTarget(bp->value.oid);

    sai_object_type_t ot = objectTypeQuery(target);

    if (ot == SAI_OBJECT_TYPE_LAG)
    {
        int trunk = trunkOf(target);

        if (trunk < 0)
        {
            SWSS_LOG_WARN("fdb %s: lag %s has no trunk, bookkeeping only",
                    serialized.c_str(), sai_serialize_object_id(target).c_str());

            return SAI_STATUS_SUCCESS;
        }

        int rv = hwbcm_l2_addr_add_trunk(fdb.mac_address, vid, trunk, isStatic);

        if (rv < 0 && rv != HWBCM_E_EXISTS)
        {
            SWSS_LOG_ERROR("hwbcm_l2_addr_add_trunk(%s vid %d trunk %d) failed: %d",
                    sai_serialize_mac(fdb.mac_address).c_str(), vid, trunk, rv);

            return bcmRvToSai(rv);
        }

        return SAI_STATUS_SUCCESS;
    }

    if (ot != SAI_OBJECT_TYPE_PORT)
    {
        SWSS_LOG_WARN("fdb %s on %s %s: only ports and lags supported, bookkeeping only",
                serialized.c_str(),
                sai_serialize_object_type(ot).c_str(),
                sai_serialize_object_id(target).c_str());

        return SAI_STATUS_SUCCESS;
    }

    int bcm = bcmPortOf(target);

    if (bcm < 0)
    {
        SWSS_LOG_WARN("fdb %s: port %s not mapped to hardware, bookkeeping only",
                serialized.c_str(), sai_serialize_object_id(target).c_str());

        return SAI_STATUS_SUCCESS;
    }

    int rv = hwbcm_l2_addr_add(fdb.mac_address, vid, bcm, isStatic);

    if (rv < 0 && rv != HWBCM_E_EXISTS)
    {
        SWSS_LOG_ERROR("hwbcm_l2_addr_add(%s vid %d port %d) failed: %d",
                sai_serialize_mac(fdb.mac_address).c_str(), vid, bcm, rv);

        return bcmRvToSai(rv);
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveFdbEntry(
        _In_ const std::string &serialized)
{
    SWSS_LOG_ENTER();

    sai_fdb_entry_t fdb;

    sai_deserialize_fdb_entry(serialized, fdb);

    // the entry still exists in the hash (hw is removed before the base)

    sai_attribute_t bp, bpType;

    if (getStoredAttr(SAI_OBJECT_TYPE_FDB_ENTRY, serialized, SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID, bp) &&
            bp.value.oid != SAI_NULL_OBJECT_ID &&
            getStoredAttr(SAI_OBJECT_TYPE_BRIDGE_PORT, bp.value.oid, SAI_BRIDGE_PORT_ATTR_TYPE, bpType) &&
            bpType.value.s32 == SAI_BRIDGE_PORT_TYPE_TUNNEL)
    {
        return hwFdbOnTunnel(fdb, bp.value.oid, false);
    }

    int vid = vidOf(fdb.bv_id);

    if (vid < 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    if (m_vidToVni.find(vid) != m_vidToVni.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    int rv = hwbcm_l2_addr_delete(fdb.mac_address, vid);

    if (rv < 0 && rv != HWBCM_E_NOT_FOUND)
    {
        SWSS_LOG_ERROR("l2 addr delete for %s failed: %d", serialized.c_str(), rv);

        return bcmRvToSai(rv);
    }

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// LAG
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateLag(
        _In_ sai_object_id_t lag_id)
{
    SWSS_LOG_ENTER();

    int trunk = -1;

    int rv = hwbcm_trunk_create(&trunk);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("hwbcm_trunk_create failed: %d", rv);

        return bcmRvToSai(rv);
    }

    m_lagToTrunk[lag_id] = trunk;
    m_lagMembers[lag_id] = {};

    SWSS_LOG_NOTICE("lag %s -> trunk %d", sai_serialize_object_id(lag_id).c_str(), trunk);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveLag(
        _In_ sai_object_id_t lag_id)
{
    SWSS_LOG_ENTER();

    auto it = m_lagToTrunk.find(lag_id);

    if (it == m_lagToTrunk.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    int rv = hwbcm_trunk_destroy(it->second);

    if (rv < 0 && rv != HWBCM_E_NOT_FOUND)
    {
        SWSS_LOG_ERROR("hwbcm_trunk_destroy(%d) failed: %d", it->second, rv);

        return bcmRvToSai(rv);
    }

    m_lagToTrunk.erase(it);
    m_lagMembers.erase(lag_id);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::applyTrunkMembers(
        _In_ sai_object_id_t lag_id)
{
    SWSS_LOG_ENTER();

    int trunk = trunkOf(lag_id);

    if (trunk < 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    std::vector<int> ports;

    auto it = m_lagMembers.find(lag_id);

    if (it != m_lagMembers.end())
    {
        for (auto p: it->second)
        {
            int bcm = bcmPortOf(p);

            if (bcm >= 0)
            {
                ports.push_back(bcm);
            }
        }
    }

    int rv = hwbcm_trunk_member_set(trunk, ports.data(), (int)ports.size());

    if (rv < 0)
    {
        SWSS_LOG_ERROR("hwbcm_trunk_member_set(%d, %zu ports) failed: %d", trunk, ports.size(), rv);

        return bcmRvToSai(rv);
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwCreateLagMember(
        _In_ sai_object_id_t member_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto *lag = findAttr(attr_count, attr_list, SAI_LAG_MEMBER_ATTR_LAG_ID);
    auto *port = findAttr(attr_count, attr_list, SAI_LAG_MEMBER_ATTR_PORT_ID);

    if (lag == NULL || port == NULL)
    {
        SWSS_LOG_ERROR("lag member %s without LAG_ID or PORT_ID", sai_serialize_object_id(member_id).c_str());

        return SAI_STATUS_INVALID_PARAMETER;
    }

    if (trunkOf(lag->value.oid) < 0)
    {
        SWSS_LOG_WARN("lag member %s: lag %s has no trunk, bookkeeping only",
                sai_serialize_object_id(member_id).c_str(),
                sai_serialize_object_id(lag->value.oid).c_str());

        return SAI_STATUS_SUCCESS;
    }

    m_lagMembers[lag->value.oid].push_back(port->value.oid);
    m_lagMemberInfo[member_id] = std::make_pair(lag->value.oid, port->value.oid);

    sai_status_t status = applyTrunkMembers(lag->value.oid);

    if (status != SAI_STATUS_SUCCESS)
    {
        auto &members = m_lagMembers[lag->value.oid];

        members.erase(std::remove(members.begin(), members.end(), port->value.oid), members.end());

        m_lagMemberInfo.erase(member_id);

        return status;
    }

    syncLagMemberPort(lag->value.oid, port->value.oid, true);

    hwAclLagMembersChanged(lag->value.oid);

    return SAI_STATUS_SUCCESS;
}

void SwitchBCM56860HW::syncLagMemberPort(
        _In_ sai_object_id_t lag_id,
        _In_ sai_object_id_t port_oid,
        _In_ bool add)
{
    SWSS_LOG_ENTER();

    int bcm = bcmPortOf(port_oid);

    if (bcm < 0)
    {
        return;
    }

    // VLAN memberships of the LAG (bridge port whose target is the LAG)

    auto vmit = m_objectHash.find(SAI_OBJECT_TYPE_VLAN_MEMBER);

    if (vmit != m_objectHash.end())
    {
        for (const auto &kvp: vmit->second)
        {
            sai_attribute_t vlan, bp, mode;

            if (!getStoredAttr(SAI_OBJECT_TYPE_VLAN_MEMBER, kvp.first, SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID, bp) ||
                    bridgePortTarget(bp.value.oid) != lag_id ||
                    !getStoredAttr(SAI_OBJECT_TYPE_VLAN_MEMBER, kvp.first, SAI_VLAN_MEMBER_ATTR_VLAN_ID, vlan))
            {
                continue;
            }

            int vid = vidOf(vlan.value.oid);

            if (vid < 0)
            {
                continue;
            }

            int tagged = 0;

            if (getStoredAttr(SAI_OBJECT_TYPE_VLAN_MEMBER, kvp.first, SAI_VLAN_MEMBER_ATTR_VLAN_TAGGING_MODE, mode) &&
                    mode.value.s32 == SAI_VLAN_TAGGING_MODE_TAGGED)
            {
                tagged = 1;
            }

            int rv = add ? hwbcm_vlan_port_add(vid, bcm, tagged) : hwbcm_vlan_port_remove(vid, bcm);

            if (rv < 0 && rv != HWBCM_E_EXISTS && rv != HWBCM_E_NOT_FOUND)
            {
                SWSS_LOG_WARN("lag %s member bcm port %d: vlan %d %s failed: %d",
                        sai_serialize_object_id(lag_id).c_str(), bcm, vid, add ? "add" : "remove", rv);
            }
        }
    }

    // VLAN of a port-type RIF sitting on the LAG (A3 keeps m_rifToVid)

    for (const auto &kvp: m_rifToVid)
    {
        sai_attribute_t rifPort;

        if (!getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, kvp.first, SAI_ROUTER_INTERFACE_ATTR_PORT_ID, rifPort) ||
                rifPort.value.oid != lag_id)
        {
            continue;
        }

        int vid = kvp.second;

        int rv;

        if (add)
        {
            rv = hwbcm_vlan_port_add(vid, bcm, 0);

            if (rv >= 0 || rv == HWBCM_E_EXISTS)
            {
                rv = hwbcm_port_pvid_set(bcm, vid);
            }
        }
        else
        {
            rv = hwbcm_vlan_port_remove(vid, bcm);

            if (rv >= 0 || rv == HWBCM_E_NOT_FOUND)
            {
                rv = hwbcm_port_pvid_set(bcm, DEFAULT_VLAN_NUMBER);
            }
        }

        if (rv < 0 && rv != HWBCM_E_EXISTS && rv != HWBCM_E_NOT_FOUND)
        {
            SWSS_LOG_WARN("lag %s member bcm port %d: rif vlan %d %s failed: %d",
                    sai_serialize_object_id(lag_id).c_str(), bcm, vid, add ? "add" : "remove", rv);
        }
        else
        {
            SWSS_LOG_NOTICE("lag %s member bcm port %d: rif vlan %d %s",
                    sai_serialize_object_id(lag_id).c_str(), bcm, vid, add ? "added" : "removed");
        }
    }
}

sai_status_t SwitchBCM56860HW::hwRemoveLagMember(
        _In_ sai_object_id_t member_id)
{
    SWSS_LOG_ENTER();

    auto it = m_lagMemberInfo.find(member_id);

    if (it == m_lagMemberInfo.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_object_id_t lag = it->second.first;
    sai_object_id_t port = it->second.second;

    m_lagMemberInfo.erase(it);

    auto mit = m_lagMembers.find(lag);

    if (mit != m_lagMembers.end())
    {
        auto &members = mit->second;

        members.erase(std::remove(members.begin(), members.end(), port), members.end());
    }

    CHECK_STATUS(applyTrunkMembers(lag));

    syncLagMemberPort(lag, port, false);

    hwAclLagMembersChanged(lag);

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// dispatch
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::create(
        _In_ sai_object_type_t object_type,
        _In_ const std::string &serializedObjectId,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    CHECK_STATUS(SwitchBCM56850::create(object_type, serializedObjectId, switch_id, attr_count, attr_list));

    if (!m_hwReady)
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_object_id_t oid = SAI_NULL_OBJECT_ID;

    auto info = sai_metadata_get_object_type_info(object_type);

    if (info != NULL && info->isobjectid)
    {
        sai_deserialize_object_id(serializedObjectId, oid);
    }

    sai_status_t hw = SAI_STATUS_SUCCESS;
    bool hwCreated = false;

    switch (object_type)
    {
        case SAI_OBJECT_TYPE_SWITCH:

            for (uint32_t i = 0; i < attr_count && hw == SAI_STATUS_SUCCESS; i++)
            {
                hw = hwSetSwitch(&attr_list[i]);
            }

            if (m_pendingVxlanOperNotify)
            {
                m_pendingVxlanOperNotify = false;
                for (const auto &kv : m_vtepByIp)
                {
                    if (kv.second.vpReady)
                    {
                        vxlanNotifyTunnelOper(kv.second, true);
                    }
                }
            }

            // never roll back the switch
            return SAI_STATUS_SUCCESS;

        // core / L2 (A2)
        case SAI_OBJECT_TYPE_PORT:            hw = hwCreatePort(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_HOSTIF:          hw = hwCreateHostif(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_HOSTIF_TRAP:
            hw = hwCreateHostifTrap(oid, attr_count, attr_list);
            if (hw == SAI_STATUS_SUCCESS)
            {
                hwCreated = true;
                hw = hwCoppOnTrapCreate(oid);
            }
            break;
        case SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP: hw = hwCreateHostifTrapGroup(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_POLICER:         hw = hwCreatePolicer(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_VLAN:            hw = hwCreateVlan(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_VLAN_MEMBER:     hw = hwCreateVlanMember(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_FDB_ENTRY:       hw = hwCreateFdbEntry(serializedObjectId, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_LAG:             hw = hwCreateLag(oid); break;
        case SAI_OBJECT_TYPE_LAG_MEMBER:      hw = hwCreateLagMember(oid, attr_count, attr_list); break;

        // L3 (A3)
        case SAI_OBJECT_TYPE_VIRTUAL_ROUTER:        hw = hwCreateVirtualRouter(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_ROUTER_INTERFACE:      hw = hwCreateRouterInterface(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_NEIGHBOR_ENTRY:        hw = hwCreateNeighbor(serializedObjectId, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_NEXT_HOP:              hw = hwCreateNextHop(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_NEXT_HOP_GROUP:        hw = hwCreateNextHopGroup(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER: hw = hwCreateNextHopGroupMember(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_ROUTE_ENTRY:
            hw = hwCreateRoute(serializedObjectId, attr_count, attr_list);
            if (hw == SAI_STATUS_SUCCESS) hwCoppOnRoute(serializedObjectId);
            break;

        // VXLAN / bridge (A4)
        case SAI_OBJECT_TYPE_TUNNEL:                 hw = hwCreateTunnel(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_TUNNEL_MAP:             hw = hwCreateTunnelMap(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY:       hw = hwCreateTunnelMapEntry(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY: hw = hwCreateTunnelTermEntry(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_BRIDGE:                 hw = hwCreateBridge(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_BRIDGE_PORT:            hw = hwCreateBridgePort(oid, attr_count, attr_list); break;

        // ACL
        case SAI_OBJECT_TYPE_ACL_TABLE:              hw = hwCreateAclTable(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_ACL_ENTRY:              hw = hwCreateAclEntry(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_ACL_COUNTER:            hw = hwCreateAclCounter(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_ACL_RANGE:              hw = hwCreateAclRange(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_ACL_TABLE_GROUP:        hw = hwCreateAclTableGroup(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER: hw = hwCreateAclTableGroupMember(oid, attr_count, attr_list); break;

        // QoS / buffers (A6)
        case SAI_OBJECT_TYPE_QOS_MAP:                hw = hwCreateQosMap(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_SCHEDULER:              hw = hwCreateScheduler(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_WRED:                   hw = hwCreateWred(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_BUFFER_POOL:            hw = hwCreateBufferPool(oid, attr_count, attr_list); break;
        case SAI_OBJECT_TYPE_BUFFER_PROFILE:         hw = hwCreateBufferProfile(oid, attr_count, attr_list); break;

        // Mirror
        case SAI_OBJECT_TYPE_MIRROR_SESSION:         hw = hwCreateMirrorSession(oid, attr_count, attr_list); break;

        default:

            // virtual switch behaviour only
            return SAI_STATUS_SUCCESS;
    }

    if (hw == SAI_STATUS_SUCCESS &&
        (object_type == SAI_OBJECT_TYPE_PORT || object_type == SAI_OBJECT_TYPE_LAG ||
         object_type == SAI_OBJECT_TYPE_VLAN || object_type == SAI_OBJECT_TYPE_ROUTER_INTERFACE))
    {
        hwCreated = true;
        hw = hwAclBindOnCreate(object_type, oid, attr_count, attr_list);
    }

    if (hw != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("hardware create of %s %s failed: %s, rolling back",
                sai_serialize_object_type(object_type).c_str(),
                serializedObjectId.c_str(),
                sai_serialize_status(hw).c_str());

        if (hwCreated)
        {
            sai_status_t hwRb = SAI_STATUS_SUCCESS;

            if (object_type == SAI_OBJECT_TYPE_PORT || object_type == SAI_OBJECT_TYPE_LAG ||
                object_type == SAI_OBJECT_TYPE_VLAN || object_type == SAI_OBJECT_TYPE_ROUTER_INTERFACE)
            {
                hwAclObjectRemoved(oid);
            }

            switch (object_type)
            {
                case SAI_OBJECT_TYPE_PORT:             hwRb = hwRemovePort(oid); break;
                case SAI_OBJECT_TYPE_LAG:              hwRb = hwRemoveLag(oid); break;
                case SAI_OBJECT_TYPE_VLAN:             hwRb = hwRemoveVlan(oid); break;
                case SAI_OBJECT_TYPE_ROUTER_INTERFACE: hwRb = hwRemoveRouterInterface(oid); break;
                case SAI_OBJECT_TYPE_HOSTIF_TRAP:      hwRb = hwCoppOnTrapRemove(oid); break;
                default: break;
            }

            if (hwRb != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("rollback hwRemove of %s %s failed: %s",
                        sai_serialize_object_type(object_type).c_str(),
                        serializedObjectId.c_str(),
                        sai_serialize_status(hwRb).c_str());
            }
        }

        sai_status_t rb = SwitchBCM56850::remove(object_type, serializedObjectId);

        if (rb != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("rollback remove of %s failed: %s",
                    serializedObjectId.c_str(),
                    sai_serialize_status(rb).c_str());
        }

        return hw;
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::remove(
        _In_ sai_object_type_t object_type,
        _In_ const std::string &serializedObjectId)
{
    SWSS_LOG_ENTER();

    if (m_hwReady)
    {
        sai_object_id_t oid = SAI_NULL_OBJECT_ID;

        auto info = sai_metadata_get_object_type_info(object_type);

        if (info != NULL && info->isobjectid)
        {
            sai_deserialize_object_id(serializedObjectId, oid);
        }

        sai_status_t hw = SAI_STATUS_SUCCESS;

        switch (object_type)
        {
            // core / L2 (A2)
            case SAI_OBJECT_TYPE_PORT:            hw = hwRemovePort(oid); break;
            case SAI_OBJECT_TYPE_HOSTIF:          hw = hwRemoveHostif(oid); break;
            case SAI_OBJECT_TYPE_HOSTIF_TRAP:     hw = hwCoppOnTrapRemove(oid); break;
            case SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP: hw = hwRemoveHostifTrapGroup(oid); break;
            case SAI_OBJECT_TYPE_POLICER:         hw = hwRemovePolicer(oid); break;
            case SAI_OBJECT_TYPE_VLAN:            hw = hwRemoveVlan(oid); break;
            case SAI_OBJECT_TYPE_VLAN_MEMBER:     hw = hwRemoveVlanMember(oid); break;
            case SAI_OBJECT_TYPE_FDB_ENTRY:       hw = hwRemoveFdbEntry(serializedObjectId); break;
            case SAI_OBJECT_TYPE_LAG:             hw = hwRemoveLag(oid); break;
            case SAI_OBJECT_TYPE_LAG_MEMBER:      hw = hwRemoveLagMember(oid); break;

            // L3 (A3)
            case SAI_OBJECT_TYPE_VIRTUAL_ROUTER:        hw = hwRemoveVirtualRouter(oid); break;
            case SAI_OBJECT_TYPE_ROUTER_INTERFACE:      hw = hwRemoveRouterInterface(oid); break;
            case SAI_OBJECT_TYPE_NEIGHBOR_ENTRY:        hw = hwRemoveNeighbor(serializedObjectId); break;
            case SAI_OBJECT_TYPE_NEXT_HOP:              hw = hwRemoveNextHop(oid); break;
            case SAI_OBJECT_TYPE_NEXT_HOP_GROUP:        hw = hwRemoveNextHopGroup(oid); break;
            case SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER: hw = hwRemoveNextHopGroupMember(oid); break;
            case SAI_OBJECT_TYPE_ROUTE_ENTRY:           hw = hwRemoveRoute(serializedObjectId); break;

            // VXLAN / bridge (A4)
            case SAI_OBJECT_TYPE_TUNNEL:                 hw = hwRemoveTunnel(oid); break;
            case SAI_OBJECT_TYPE_TUNNEL_MAP:             hw = hwRemoveTunnelMap(oid); break;
            case SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY:       hw = hwRemoveTunnelMapEntry(oid); break;
            case SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY: hw = hwRemoveTunnelTermEntry(oid); break;
            case SAI_OBJECT_TYPE_BRIDGE:                 hw = hwRemoveBridge(oid); break;
            case SAI_OBJECT_TYPE_BRIDGE_PORT:            hw = hwRemoveBridgePort(oid); break;

            // ACL
            case SAI_OBJECT_TYPE_ACL_TABLE:              hw = hwRemoveAclTable(oid); break;
            case SAI_OBJECT_TYPE_ACL_ENTRY:              hw = hwRemoveAclEntry(oid); break;
            case SAI_OBJECT_TYPE_ACL_COUNTER:            hw = hwRemoveAclCounter(oid); break;
            case SAI_OBJECT_TYPE_ACL_RANGE:              hw = hwRemoveAclRange(oid); break;
            case SAI_OBJECT_TYPE_ACL_TABLE_GROUP:        hw = hwRemoveAclTableGroup(oid); break;
            case SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER: hw = hwRemoveAclTableGroupMember(oid); break;

            // QoS / buffers (A6)
            case SAI_OBJECT_TYPE_QOS_MAP:                hw = hwRemoveQosMap(oid); break;
            case SAI_OBJECT_TYPE_SCHEDULER:              hw = hwRemoveScheduler(oid); break;
            case SAI_OBJECT_TYPE_WRED:                   hw = hwRemoveWred(oid); break;
            case SAI_OBJECT_TYPE_BUFFER_POOL:            hw = hwRemoveBufferPool(oid); break;
            case SAI_OBJECT_TYPE_BUFFER_PROFILE:         hw = hwRemoveBufferProfile(oid); break;

            // Mirror
            case SAI_OBJECT_TYPE_MIRROR_SESSION:         hw = hwRemoveMirrorSession(oid); break;

            default:
                break;
        }

        if (hw != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("hardware remove of %s %s failed: %s",
                    sai_serialize_object_type(object_type).c_str(),
                    serializedObjectId.c_str(),
                    sai_serialize_status(hw).c_str());

            return hw;
        }

        if (object_type == SAI_OBJECT_TYPE_PORT || object_type == SAI_OBJECT_TYPE_LAG ||
            object_type == SAI_OBJECT_TYPE_VLAN || object_type == SAI_OBJECT_TYPE_ROUTER_INTERFACE)
        {
            hwAclObjectRemoved(oid);   // drops its bindings, reprograms affected tables
        }
    }

    return SwitchBCM56850::remove(object_type, serializedObjectId);
}

sai_status_t SwitchBCM56860HW::set(
        _In_ sai_object_type_t objectType,
        _In_ const std::string &serializedObjectId,
        _In_ const sai_attribute_t* attr)
{
    SWSS_LOG_ENTER();

    bool snapshotted = false;
    bool hadPrev = false;
    std::string attrKey;
    std::shared_ptr<SaiAttrWrap> prevAttr;

    if (m_hwReady && attr != NULL)
    {
        auto meta = sai_metadata_get_attr_metadata(objectType, attr->id);

        if (meta != NULL)
        {
            attrKey = meta->attridname;
            auto otIt = m_objectHash.find(objectType);

            if (otIt != m_objectHash.end())
            {
                auto objIt = otIt->second.find(serializedObjectId);

                if (objIt != otIt->second.end())
                {
                    snapshotted = true;
                    auto ait = objIt->second.find(attrKey);

                    if (ait != objIt->second.end())
                    {
                        hadPrev = true;
                        prevAttr = ait->second;
                    }
                }
            }
        }
    }

    CHECK_STATUS(SwitchBCM56850::set(objectType, serializedObjectId, attr));

    if (!m_hwReady || attr == NULL)
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_object_id_t oid = SAI_NULL_OBJECT_ID;

    auto info = sai_metadata_get_object_type_info(objectType);

    if (info != NULL && info->isobjectid)
    {
        sai_deserialize_object_id(serializedObjectId, oid);
    }

    sai_status_t hw = SAI_STATUS_SUCCESS;

    if (hwAclIsBindAttr(objectType, attr->id))
    {
        // PORT/LAG/VLAN/RIF *_INGRESS_ACL/_EGRESS_ACL, SWITCH INGRESS/EGRESS/PRE_INGRESS_ACL
        hw = hwAclBind(objectType, oid, attr);
    }
    else switch (objectType)
    {
        case SAI_OBJECT_TYPE_SWITCH:
            hw = hwSetSwitch(attr);
            if (hw == SAI_STATUS_SUCCESS && attr->id == SAI_SWITCH_ATTR_SRC_MAC_ADDRESS) hwCoppRefresh();
            break;
        case SAI_OBJECT_TYPE_PORT:             hw = hwSetPort(oid, attr); break;
        case SAI_OBJECT_TYPE_BRIDGE_PORT:      hw = hwSetBridgePort(oid, attr); break;
        case SAI_OBJECT_TYPE_HOSTIF_TRAP:      hw = hwCoppOnTrapSet(oid, attr); break;
        case SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP: hw = hwSetHostifTrapGroup(oid, attr); break;
        case SAI_OBJECT_TYPE_POLICER:          hw = hwSetPolicer(oid, attr); break;
        case SAI_OBJECT_TYPE_ROUTER_INTERFACE: hw = hwSetRouterInterface(oid, attr); break;
        case SAI_OBJECT_TYPE_ROUTE_ENTRY:
            hw = hwSetRoute(serializedObjectId, attr);
            if (hw == SAI_STATUS_SUCCESS) hwCoppOnRoute(serializedObjectId);
            break;
        case SAI_OBJECT_TYPE_HASH:             hw = hwSetHash(oid, attr); break;
        case SAI_OBJECT_TYPE_ACL_ENTRY:              hw = hwSetAclEntry(oid, attr); break;
        case SAI_OBJECT_TYPE_ACL_COUNTER:            hw = hwSetAclCounter(oid, attr); break;
        case SAI_OBJECT_TYPE_QOS_MAP:                hw = hwSetQosMap(oid, attr); break;
        case SAI_OBJECT_TYPE_SCHEDULER:              hw = hwSetScheduler(oid, attr); break;
        case SAI_OBJECT_TYPE_WRED:                   hw = hwSetWred(oid, attr); break;
        case SAI_OBJECT_TYPE_BUFFER_POOL:            hw = hwSetBufferPool(oid, attr); break;
        case SAI_OBJECT_TYPE_BUFFER_PROFILE:         hw = hwSetBufferProfile(oid, attr); break;
        case SAI_OBJECT_TYPE_QUEUE:                  hw = hwSetQueue(oid, attr); break;
        case SAI_OBJECT_TYPE_SCHEDULER_GROUP:        hw = hwSetSchedulerGroup(oid, attr); break;
        case SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP: hw = hwSetIngressPriorityGroup(oid, attr); break;
        case SAI_OBJECT_TYPE_MIRROR_SESSION:         hw = hwSetMirrorSession(oid, attr); break;

        default:
            break;
    }

    if (hw != SAI_STATUS_SUCCESS)
    {
        if (snapshotted)
        {
            auto otIt = m_objectHash.find(objectType);

            if (otIt != m_objectHash.end())
            {
                auto objIt = otIt->second.find(serializedObjectId);

                if (objIt != otIt->second.end())
                {
                    if (hadPrev)
                    {
                        objIt->second[attrKey] = prevAttr;
                    }
                    else
                    {
                        objIt->second.erase(attrKey);
                    }
                }
            }
        }

        SWSS_LOG_ERROR("hardware set of %s %s attr %d failed: %s (virtual state rolled back)",
                sai_serialize_object_type(objectType).c_str(),
                serializedObjectId.c_str(),
                attr->id,
                sai_serialize_status(hw).c_str());

        return hw;
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::bulkSet(
        _In_ sai_object_type_t object_type,
        _In_ const std::vector<std::string> &serialized_object_ids,
        _In_ const sai_attribute_t *attr_list,
        _In_ sai_bulk_op_error_mode_t mode,
        _Out_ sai_status_t *object_statuses)
{
    SWSS_LOG_ENTER();

    uint32_t object_count = (uint32_t)serialized_object_ids.size();

    if (!object_count || !attr_list || !object_statuses)
    {
        SWSS_LOG_ERROR("Invalid arguments");
        return SAI_STATUS_FAILURE;
    }

    sai_status_t status = SAI_STATUS_SUCCESS;
    uint32_t it;

    // SwitchStateBase::bulkSet calls set_internal() directly, bypassing our
    // set() override (and therefore skipping HW programming for bulk queue/PG
    // buffer profile bindings from SONiC BufferOrch). Route through set().
    for (it = 0; it < object_count; it++)
    {
        object_statuses[it] = set(object_type, serialized_object_ids[it], &attr_list[it]);

        if (object_statuses[it] != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to bulk-set attribute for object with type = %u", object_type);

            status = SAI_STATUS_FAILURE;

            if (mode == SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR)
            {
                break;
            }
        }
    }

    while (++it < object_count)
    {
        object_statuses[it] = SAI_STATUS_NOT_EXECUTED;
    }

    return status;
}

sai_status_t SwitchBCM56860HW::get(
        _In_ sai_object_type_t objectType,
        _In_ const std::string &serializedObjectId,
        _In_ uint32_t attr_count,
        _Out_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    if (objectType == SAI_OBJECT_TYPE_ACL_COUNTER)
    {
        // PACKETS/BYTES live in hardware and are never stored, so the base
        // get fails on them ("failed to find attribute"): ask the base only
        // for the other attributes, then fill the counters from the ASIC.
        std::vector<sai_attribute_t> other;
        std::vector<uint32_t> otherIdx;

        if (attr_count > 0 && attr_list == nullptr)
        {
            return SAI_STATUS_INVALID_PARAMETER;
        }

        for (uint32_t i = 0; i < attr_count; i++)
        {
            if (attr_list[i].id == SAI_ACL_COUNTER_ATTR_PACKETS || attr_list[i].id == SAI_ACL_COUNTER_ATTR_BYTES)
            {
                attr_list[i].value.u64 = 0;
                continue;
            }

            other.push_back(attr_list[i]);
            otherIdx.push_back(i);
        }

        if (!other.empty())
        {
            CHECK_STATUS(SwitchBCM56850::get(objectType, serializedObjectId, (uint32_t)other.size(), other.data()));

            for (size_t k = 0; k < other.size(); k++)
            {
                attr_list[otherIdx[k]] = other[k];
            }
        }

        return hwAclCounterGet(serializedObjectId, attr_count, attr_list);
    }

    if (m_hwReady && (objectType == SAI_OBJECT_TYPE_SWITCH || objectType == SAI_OBJECT_TYPE_BUFFER_POOL))
    {
        sai_object_id_t oid = SAI_NULL_OBJECT_ID;
        if (objectType == SAI_OBJECT_TYPE_BUFFER_POOL)
        {
            sai_deserialize_object_id(serializedObjectId, oid);
        }

        std::vector<sai_attribute_t> other;
        std::vector<uint32_t> otherIdx;
        bool anyHandled = false;

        if (attr_count > 0 && attr_list == nullptr)
        {
            return SAI_STATUS_INVALID_PARAMETER;
        }

        for (uint32_t i = 0; i < attr_count; i++)
        {
            bool handled = (objectType == SAI_OBJECT_TYPE_SWITCH)
                    ? qosGetSwitchAttr(attr_list[i])
                    : qosGetBufferPoolAttr(oid, attr_list[i]);
            if (handled)
            {
                anyHandled = true;
                continue;
            }
            other.push_back(attr_list[i]);
            otherIdx.push_back(i);
        }

        if (anyHandled)
        {
            if (!other.empty())
            {
                CHECK_STATUS(SwitchBCM56850::get(objectType, serializedObjectId, (uint32_t)other.size(), other.data()));
                for (size_t k = 0; k < other.size(); k++)
                {
                    attr_list[otherIdx[k]] = other[k];
                }
            }
            return SAI_STATUS_SUCCESS;
        }
    }

    return SwitchBCM56850::get(objectType, serializedObjectId, attr_count, attr_list);
}

// ---------------------------------------------------------------------------
// shared helpers
// ---------------------------------------------------------------------------

bool SwitchBCM56860HW::getStoredAttr(
        _In_ sai_object_type_t ot,
        _In_ sai_object_id_t oid,
        _In_ sai_attr_id_t attr_id,
        _Out_ sai_attribute_t &attr)
{
    SWSS_LOG_ENTER();

    return getStoredAttr(ot, sai_serialize_object_id(oid), attr_id, attr);
}

bool SwitchBCM56860HW::getStoredAttr(
        _In_ sai_object_type_t ot,
        _In_ const std::string &serialized,
        _In_ sai_attr_id_t attr_id,
        _Out_ sai_attribute_t &attr)
{
    SWSS_LOG_ENTER();

    auto meta = sai_metadata_get_attr_metadata(ot, attr_id);

    if (meta == NULL)
    {
        return false;
    }

    auto oit = m_objectHash.find(ot);

    if (oit == m_objectHash.end())
    {
        return false;
    }

    auto it = oit->second.find(serialized);

    if (it == oit->second.end())
    {
        return false;
    }

    auto ait = it->second.find(meta->attridname);

    if (ait == it->second.end() || ait->second == nullptr || ait->second->getAttr() == nullptr)
    {
        return false;
    }

    // shallow copy: list pointers stay owned by the SaiAttrWrap in the hash
    attr = *ait->second->getAttr();

    return true;
}

int SwitchBCM56860HW::bcmPortOf(
        _In_ sai_object_id_t port_oid) const
{
    std::lock_guard<std::mutex> lock(m_mapMutex);

    auto it = m_portToBcm.find(port_oid);

    return (it == m_portToBcm.end()) ? -1 : it->second;
}

sai_object_id_t SwitchBCM56860HW::portOidOf(
        _In_ int bcm_port) const
{
    std::lock_guard<std::mutex> lock(m_mapMutex);

    auto it = m_bcmToPort.find(bcm_port);

    return (it == m_bcmToPort.end()) ? SAI_NULL_OBJECT_ID : it->second;
}

int SwitchBCM56860HW::vidOf(
        _In_ sai_object_id_t vlan_oid)
{
    SWSS_LOG_ENTER();

    if (vlan_oid == SAI_NULL_OBJECT_ID)
    {
        return -1;
    }

    sai_attribute_t attr;

    if (!getStoredAttr(SAI_OBJECT_TYPE_VLAN, vlan_oid, SAI_VLAN_ATTR_VLAN_ID, attr))
    {
        return -1;
    }

    return (int)attr.value.u16;
}

int SwitchBCM56860HW::trunkOf(
        _In_ sai_object_id_t lag_oid) const
{
    auto it = m_lagToTrunk.find(lag_oid);

    return (it == m_lagToTrunk.end()) ? -1 : it->second;
}

sai_object_id_t SwitchBCM56860HW::bridgePortTarget(
        _In_ sai_object_id_t bridge_port_oid)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    if (!getStoredAttr(SAI_OBJECT_TYPE_BRIDGE_PORT, bridge_port_oid, SAI_BRIDGE_PORT_ATTR_PORT_ID, attr))
    {
        return SAI_NULL_OBJECT_ID;
    }

    return attr.value.oid;
}

const sai_attribute_t *SwitchBCM56860HW::findAttr(
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list,
        _In_ sai_attr_id_t id)
{
    if (attr_list == NULL)
    {
        return NULL;
    }

    for (uint32_t i = 0; i < attr_count; i++)
    {
        if (attr_list[i].id == id)
        {
            return &attr_list[i];
        }
    }

    return NULL;
}

sai_status_t SwitchBCM56860HW::hwFlushFdbEntries(
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    /*
     * orchagent flushes the dynamic MACs of a bridge port when the port goes
     * down (PortsOrch) and of a (port, vlan) when a VLAN member is removed.
     * Stock vslib only forgets them; the ASIC kept them, so they were never
     * re-learned (the SDK reports a learn once), "show mac" stayed empty and a
     * neighbor created afterwards was punted to the CPU for good (seen
     * 2026-11-21 after a 1G link bounce). Mirror the flush into the L2 table;
     * hwbcm's flushes delete dynamic entries only, static ones are removed
     * one by one through the normal SAI remove.
     */

    if (!m_hwReady)
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_object_id_t bp = SAI_NULL_OBJECT_ID;
    sai_object_id_t bv = SAI_NULL_OBJECT_ID;
    sai_fdb_flush_entry_type_t type = SAI_FDB_FLUSH_ENTRY_TYPE_DYNAMIC;

    if (attr_count > 0 && attr_list == nullptr)
    {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    for (uint32_t i = 0; i < attr_count; i++)
    {
        switch (attr_list[i].id)
        {
            case SAI_FDB_FLUSH_ATTR_BRIDGE_PORT_ID:
                bp = attr_list[i].value.oid;
                break;

            case SAI_FDB_FLUSH_ATTR_BV_ID:
                bv = attr_list[i].value.oid;
                break;

            case SAI_FDB_FLUSH_ATTR_ENTRY_TYPE:
                type = (sai_fdb_flush_entry_type_t)attr_list[i].value.s32;
                break;

            default:
                break;
        }
    }

    if (type == SAI_FDB_FLUSH_ENTRY_TYPE_STATIC)
    {
        return SAI_STATUS_SUCCESS;
    }

    int vid = -1;

    if (bv != SAI_NULL_OBJECT_ID && objectTypeQuery(bv) == SAI_OBJECT_TYPE_VLAN)
    {
        sai_attribute_t a;

        if (getStoredAttr(SAI_OBJECT_TYPE_VLAN, bv, SAI_VLAN_ATTR_VLAN_ID, a))
        {
            vid = a.value.u16;
        }
    }

    int port = -1;
    int trunk = -1;
    int rv = 0;

    // Called before the local tables are cleared: walk the dynamic entries in
    // scope and put the neighbors that resolve to them back on the punt path
    // (the ASIC flush below does not raise per-entry age callbacks we could
    // act on, and the local entry is gone by the time one would arrive).
    {
        auto fit = m_objectHash.find(SAI_OBJECT_TYPE_FDB_ENTRY);

        if (fit != m_objectHash.end())
        {
            for (auto &kv: fit->second)
            {
                auto ty = kv.second.find("SAI_FDB_ENTRY_ATTR_TYPE");

                if (ty != kv.second.end() && ty->second != nullptr && ty->second->getAttr() != nullptr &&
                        ty->second->getAttr()->value.s32 != SAI_FDB_ENTRY_TYPE_DYNAMIC)
                {
                    continue;
                }

                sai_fdb_entry_t fe;
                sai_deserialize_fdb_entry(kv.first, fe);

                if (bv != SAI_NULL_OBJECT_ID && fe.bv_id != bv)
                {
                    continue;
                }

                if (bp != SAI_NULL_OBJECT_ID)
                {
                    auto ebp = kv.second.find("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID");

                    if (ebp == kv.second.end() || ebp->second == nullptr || ebp->second->getAttr() == nullptr ||
                            ebp->second->getAttr()->value.oid != bp)
                    {
                        continue;
                    }
                }

                hwPuntNeighborsOnAge(fe.bv_id, fe.mac_address);
            }
        }
    }

    if (bp != SAI_NULL_OBJECT_ID)
    {
        sai_object_id_t target = bridgePortTarget(bp);
        sai_object_type_t ot = objectTypeQuery(target);

        if (ot == SAI_OBJECT_TYPE_LAG)
        {
            trunk = trunkOf(target);
            rv = (trunk >= 0) ? hwbcm_l2_flush_trunk(trunk) : 0; // (vlan filter not available for trunks)
        }
        else if (ot == SAI_OBJECT_TYPE_PORT)
        {
            port = bcmPortOf(target);

            if (port >= 0)
            {
                rv = (vid > 0) ? hwbcm_l2_flush_vlan_port(vid, port) : hwbcm_l2_flush_port(port);
            }
        }
        else
        {
            return SAI_STATUS_SUCCESS; // tunnel bridge port: remote MACs are static, nothing to flush
        }
    }
    else if (vid > 0)
    {
        rv = hwbcm_l2_flush_vlan(vid);
    }
    else
    {
        auto it = m_objectHash.find(SAI_OBJECT_TYPE_VLAN);

        if (it != m_objectHash.end())
        {
            for (auto &kv: it->second)
            {
                sai_object_id_t oid;
                sai_deserialize_object_id(kv.first, oid);

                sai_attribute_t a;

                if (getStoredAttr(SAI_OBJECT_TYPE_VLAN, oid, SAI_VLAN_ATTR_VLAN_ID, a))
                {
                    int r = hwbcm_l2_flush_vlan(a.value.u16);

                    if (r < 0)
                    {
                        rv = r;
                    }
                }
            }
        }
    }

    SWSS_LOG_NOTICE("fdb flush mirrored to ASIC: bridge port %s (bcm port %d trunk %d) vlan %d: %d",
            sai_serialize_object_id(bp).c_str(), port, trunk, vid, rv);

    return SAI_STATUS_SUCCESS; // the local flush must proceed even if the ASIC call failed
}

#include "SwitchBCM56860HW_wb.inc.cpp"
