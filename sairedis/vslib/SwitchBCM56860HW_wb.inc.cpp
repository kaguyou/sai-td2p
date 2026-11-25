/*
 * SwitchBCM56860HW_wb.inc.cpp — Warm Boot Phase 1, Phase 2 & Phase 3 implementation
 * (included by SwitchBCM56860HW.cpp).
 *
 * Covers:
 *  - Sibling .scache / .hwstate path derivation from SwitchConfig
 *  - Versioned TLV binary .hwstate persistence (magic, version, payload CRC32,
 *    config.bcm CRC32, atomic .tmp + fsync + rename + dir fsync)
 *  - Phase 2: Core (IP2ME stations, L2 cache traps, switch MAC), LAG, L3, KNET,
 *    bridge port FDB learning mode freeze/unfreeze
 *  - Phase 3: Hash (RTAG7 fields/seeds + base switch OIDs), VXLAN/EVPN/RIOT
 *    (VPNs, access/network VPs, tunnels, VTEPs, overlay intfs), ACL (groups,
 *    entries/variants, counters + offsets, ranges, bindings), CoPP (group,
 *    entries, policers, stat ids, pending to-me traps), QoS (maps, pools,
 *    PG/queue MMU snapshots, scheduler/WRED/buffer bindings), Mirror sessions
 *    & port/ACL bindings, and Stats READ_AND_CLEAR baselines
 */

#include <fcntl.h>
#include <sys/stat.h>
#include <chrono>
#include <cerrno>

namespace
{
    constexpr uint32_t HWSTATE_MAGIC   = 0x48575354u; /* "HWST" */
    constexpr uint32_t HWSTATE_VERSION = 1u;
    constexpr uint32_t HWSTATE_HDR_SZ  = 32u;

    enum HwStateTag : uint32_t
    {
        HWSTATE_TAG_CORE          = 1u,
        HWSTATE_TAG_LAG_TRUNKS    = 2u,
        HWSTATE_TAG_L3_GLOBALS    = 3u,
        HWSTATE_TAG_L3_VR         = 4u,
        HWSTATE_TAG_L3_RIF        = 5u,
        HWSTATE_TAG_L3_NH         = 6u,
        HWSTATE_TAG_L3_NHG        = 7u,
        HWSTATE_TAG_L3_NEIGH      = 8u,
        HWSTATE_TAG_L3_ROUTE      = 9u,
        /* Phase 3 subsystems */
        HWSTATE_TAG_HASH          = 10u,
        HWSTATE_TAG_VXLAN_GLOBALS = 11u,
        HWSTATE_TAG_VXLAN_VPNS    = 12u,
        HWSTATE_TAG_VXLAN_VTEPS   = 13u,
        HWSTATE_TAG_ACL_TABLES    = 14u,
        HWSTATE_TAG_ACL_ENTRIES   = 15u,
        HWSTATE_TAG_ACL_COUNTERS  = 16u,
        HWSTATE_TAG_ACL_RANGES    = 17u,
        HWSTATE_TAG_COPP          = 18u,
        HWSTATE_TAG_QOS_MAPS      = 19u,
        HWSTATE_TAG_QOS_POOLS     = 20u,
        HWSTATE_TAG_QOS_SNAPS     = 21u,
        HWSTATE_TAG_MIRROR        = 22u,
        HWSTATE_TAG_STATS_BASE    = 23u,
        HWSTATE_TAG_QOS_QGRP_MIN  = 24u,
    };

    uint32_t wbCrc32Update(uint32_t crc, const uint8_t *data, size_t len)
    {
        crc = ~crc;
        for (size_t i = 0; i < len; i++)
        {
            crc ^= data[i];
            for (int b = 0; b < 8; b++)
            {
                uint32_t mask = (crc & 1u) ? 0xEDB88320u : 0u;
                crc = (crc >> 1) ^ mask;
            }
        }
        return ~crc;
    }

    uint32_t wbCrc32Buf(const uint8_t *data, size_t len)
    {
        return wbCrc32Update(0u, data, len);
    }

    bool wbCrc32File(const std::string &path, uint32_t &outCrc)
    {
        outCrc = 0u;
        if (path.empty())
        {
            return false;
        }
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0)
        {
            return false;
        }
        uint8_t buf[4096];
        uint32_t crc = 0u;
        for (;;)
        {
            ssize_t r = read(fd, buf, sizeof(buf));
            if (r < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                close(fd);
                return false;
            }
            if (r == 0)
            {
                break;
            }
            crc = wbCrc32Update(crc, buf, static_cast<size_t>(r));
        }
        close(fd);
        outCrc = crc;
        return true;
    }

    class WbWriter
    {
        public:
            std::vector<uint8_t> buf;

            void putU8(uint8_t v)
            {
                buf.push_back(v);
            }

            void putU16(uint16_t v)
            {
                buf.push_back(static_cast<uint8_t>(v & 0xffu));
                buf.push_back(static_cast<uint8_t>((v >> 8) & 0xffu));
            }

            void putU32(uint32_t v)
            {
                uint8_t b[4];
                b[0] = static_cast<uint8_t>(v & 0xffu);
                b[1] = static_cast<uint8_t>((v >> 8) & 0xffu);
                b[2] = static_cast<uint8_t>((v >> 16) & 0xffu);
                b[3] = static_cast<uint8_t>((v >> 24) & 0xffu);
                buf.insert(buf.end(), b, b + 4);
            }

            void putI32(int32_t v)
            {
                putU32(static_cast<uint32_t>(v));
            }

            void putU64(uint64_t v)
            {
                putU32(static_cast<uint32_t>(v & 0xffffffffu));
                putU32(static_cast<uint32_t>((v >> 32) & 0xffffffffu));
            }

            void putBytes(const void *p, size_t n)
            {
                const auto *b = static_cast<const uint8_t *>(p);
                buf.insert(buf.end(), b, b + n);
            }

            void putStr(const std::string &s)
            {
                putU32(static_cast<uint32_t>(s.size()));
                if (!s.empty())
                {
                    putBytes(s.data(), s.size());
                }
            }

            void putTlv(uint32_t tag, const std::vector<uint8_t> &val)
            {
                putU32(tag);
                putU32(static_cast<uint32_t>(val.size()));
                if (!val.empty())
                {
                    putBytes(val.data(), val.size());
                }
            }
    };

    class WbReader
    {
        public:
            const uint8_t *data;
            size_t size;
            size_t pos;

            WbReader(const uint8_t *d, size_t s) : data(d), size(s), pos(0) {}

            size_t remaining() const { return size - pos; }

            bool getU8(uint8_t &v)
            {
                if (remaining() < 1)
                {
                    return false;
                }
                v = data[pos++];
                return true;
            }

            bool getU16(uint16_t &v)
            {
                if (remaining() < 2)
                {
                    return false;
                }
                v = static_cast<uint16_t>(data[pos]) |
                    static_cast<uint16_t>(static_cast<uint16_t>(data[pos + 1]) << 8);
                pos += 2;
                return true;
            }

            bool getU32(uint32_t &v)
            {
                if (remaining() < 4)
                {
                    return false;
                }
                v = static_cast<uint32_t>(data[pos]) |
                    (static_cast<uint32_t>(data[pos + 1]) << 8) |
                    (static_cast<uint32_t>(data[pos + 2]) << 16) |
                    (static_cast<uint32_t>(data[pos + 3]) << 24);
                pos += 4;
                return true;
            }

            bool getI32(int32_t &v)
            {
                uint32_t u = 0;
                if (!getU32(u))
                {
                    return false;
                }
                v = static_cast<int32_t>(u);
                return true;
            }

            bool getU64(uint64_t &v)
            {
                uint32_t lo = 0, hi = 0;
                if (!getU32(lo) || !getU32(hi))
                {
                    return false;
                }
                v = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
                return true;
            }

            bool getBytes(void *out, size_t n)
            {
                if (remaining() < n)
                {
                    return false;
                }
                memcpy(out, data + pos, n);
                pos += n;
                return true;
            }

            bool getStr(std::string &out)
            {
                uint32_t len = 0;
                if (!getU32(len) || remaining() < len)
                {
                    return false;
                }
                out.assign(reinterpret_cast<const char *>(data + pos), len);
                pos += len;
                return true;
            }
    };

    bool wbWriteFileAtomic(const std::string &path, const uint8_t *data, size_t len)
    {
        std::string tmp = path + ".tmp";
        int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0640);
        if (fd < 0)
        {
            SWSS_LOG_ERROR("wb: open(%s) failed: %s", tmp.c_str(), strerror(errno));
            return false;
        }
        size_t done = 0;
        while (done < len)
        {
            ssize_t w = write(fd, data + done, len - done);
            if (w < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                SWSS_LOG_ERROR("wb: write(%s) failed: %s", tmp.c_str(), strerror(errno));
                close(fd);
                unlink(tmp.c_str());
                return false;
            }
            done += static_cast<size_t>(w);
        }
        if (fsync(fd) != 0)
        {
            SWSS_LOG_ERROR("wb: fsync(%s) failed: %s", tmp.c_str(), strerror(errno));
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        close(fd);
        if (rename(tmp.c_str(), path.c_str()) != 0)
        {
            SWSS_LOG_ERROR("wb: rename(%s -> %s) failed: %s", tmp.c_str(), path.c_str(), strerror(errno));
            unlink(tmp.c_str());
            return false;
        }

        std::string dir = ".";
        auto slash = path.find_last_of('/');
        if (slash != std::string::npos)
        {
            dir = (slash == 0) ? "/" : path.substr(0, slash);
        }
        int dfd = open(dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (dfd >= 0)
        {
            (void)fsync(dfd);
            close(dfd);
        }
        return true;
    }

    bool wbReadWholeFile(const std::string &path, std::vector<uint8_t> &out)
    {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0)
        {
            return false;
        }
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(HWSTATE_HDR_SZ) ||
            st.st_size > 64 * 1024 * 1024)
        {
            close(fd);
            return false;
        }
        out.resize(static_cast<size_t>(st.st_size));
        size_t done = 0;
        while (done < out.size())
        {
            ssize_t r = read(fd, out.data() + done, out.size() - done);
            if (r < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                close(fd);
                return false;
            }
            if (r == 0)
            {
                break;
            }
            done += static_cast<size_t>(r);
        }
        close(fd);
        return done == out.size();
    }
}

std::string SwitchBCM56860HW::warmBootBasePath(
        _In_ bool forWrite) const
{
    SWSS_LOG_ENTER();

    const std::string &primary = forWrite ? m_switchConfig->m_warmBootWriteFile
                                          : m_switchConfig->m_warmBootReadFile;
    if (!primary.empty())
    {
        return primary;
    }
    const std::string &alt = forWrite ? m_switchConfig->m_warmBootReadFile
                                      : m_switchConfig->m_warmBootWriteFile;
    if (!alt.empty())
    {
        return alt;
    }
    return "/var/warmboot/sai-warmboot.bin";
}

std::string SwitchBCM56860HW::warmBootScachePath(
        _In_ bool forWrite) const
{
    return warmBootBasePath(forWrite) + ".scache";
}

std::string SwitchBCM56860HW::warmBootHwStatePath(
        _In_ bool forWrite) const
{
    return warmBootBasePath(forWrite) + ".hwstate";
}

bool SwitchBCM56860HW::warmDeinitForKexec() const
{
    SWSS_LOG_ENTER();

    if (m_preShutdown)
    {
        SWSS_LOG_NOTICE("wb: kexec-mode deinit (PRE_SHUTDOWN seen: system warm reboot)");
        return true;
    }

    const char *env = getenv("HWBCM_WB_KEXEC");
    if (env != NULL && env[0] == '1')
    {
        SWSS_LOG_NOTICE("wb: kexec-mode deinit forced by HWBCM_WB_KEXEC=1");
        return true;
    }

    std::string base = warmBootBasePath(true);
    size_t slash = base.rfind('/');
    std::string pending = (slash == std::string::npos ? std::string(".") : base.substr(0, slash)) + "/kexec-pending";

    struct stat st;
    if (stat(pending.c_str(), &st) == 0)
    {
        SWSS_LOG_NOTICE("wb: kexec-mode deinit forced by %s", pending.c_str());
        return true;
    }

    const char *keepKnet = getenv("HWBCM_WB_KEEP_KNET");
    if (keepKnet != NULL && keepKnet[0] == '1')
    {
        SWSS_LOG_NOTICE("wb: live-mode deinit (keep KNET rings) selected by HWBCM_WB_KEEP_KNET=1");
        return false;
    }

    SWSS_LOG_NOTICE("wb: dma-reset deinit (default for syncd warm restart; set HWBCM_WB_KEEP_KNET=1 for live mode)");
    return true;
}

sai_status_t SwitchBCM56860HW::hwWarmSync()
{
    SWSS_LOG_ENTER();

    if (!m_hwReady)
    {
        SWSS_LOG_ERROR("wb: hwWarmSync called before hardware initialized");
        return SAI_STATUS_FAILURE;
    }

    std::string scachePath = warmBootScachePath(true);
    std::string hwStatePath = warmBootHwStatePath(true);
    size_t scacheBytes = 0;

    SWSS_LOG_NOTICE("wb: syncing SDK scache -> %s and hwstate -> %s",
            scachePath.c_str(), hwStatePath.c_str());

    int rv = hwbcm_warm_sync(scachePath.c_str(), &scacheBytes);
    if (rv < 0)
    {
        SWSS_LOG_ERROR("wb: hwbcm_warm_sync(%s) failed: %d", scachePath.c_str(), rv);
        return SAI_STATUS_FAILURE;
    }

    sai_status_t status = hwStateSave(hwStatePath);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("wb: hwStateSave(%s) failed: %s",
                hwStatePath.c_str(), sai_serialize_status(status).c_str());
        return status;
    }

    m_warmSynced = true;
    SWSS_LOG_NOTICE("wb: warm sync succeeded (scache=%zu bytes, hwstate=%s)",
            scacheBytes, hwStatePath.c_str());
    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwStateSave(
        _In_ const std::string &path) const
{
    SWSS_LOG_ENTER();

    WbWriter payload;

    // 1. Core hwbcm state
    {
        hwbcm_core_wb_state_t cs;
        hwbcm_core_wb_get(&cs);
        WbWriter w;
        w.putBytes(cs.switch_mac, 6);
        w.putI32(cs.switch_mac_valid);
        w.putI32(cs.ip2me_station_id);
        w.putI32(cs.ip2me_vfi_station_id);
        w.putI32(cs.ip2me_wanted);
        uint32_t ntraps = static_cast<uint32_t>(HWBCM_TRAP_STP + 1);
        w.putU32(ntraps);
        for (uint32_t i = 0; i < ntraps; i++)
        {
            w.putI32(cs.trap_l2cache_idx[i]);
        }
        payload.putTlv(HWSTATE_TAG_CORE, w.buf);
    }

    // 2. LAG -> trunk map
    {
        WbWriter w;
        w.putU32(static_cast<uint32_t>(m_lagToTrunk.size()));
        for (const auto &kv : m_lagToTrunk)
        {
            w.putU64(static_cast<uint64_t>(kv.first));
            w.putI32(kv.second);
        }
        payload.putTlv(HWSTATE_TAG_LAG_TRUNKS, w.buf);
    }

    // 3. L3 shared egress globals (g_cpuEgr, g_dropEgr)
    {
        int cpuEgr = -1, dropEgr = -1;
        hwL3GetSharedEgress(cpuEgr, dropEgr);
        WbWriter w;
        w.putI32(cpuEgr);
        w.putI32(dropEgr);
        payload.putTlv(HWSTATE_TAG_L3_GLOBALS, w.buf);
    }

    // 4. L3 VR -> VRF
    {
        WbWriter w;
        w.putU32(static_cast<uint32_t>(m_vrToVrf.size()));
        for (const auto &kv : m_vrToVrf)
        {
            w.putU64(static_cast<uint64_t>(kv.first));
            w.putI32(kv.second);
        }
        payload.putTlv(HWSTATE_TAG_L3_VR, w.buf);
    }

    // 5. L3 RIF -> (intf, vid)
    {
        std::set<sai_object_id_t> rifs;
        for (const auto &kv : m_rifToIntf) rifs.insert(kv.first);
        for (const auto &kv : m_rifToVid)  rifs.insert(kv.first);
        WbWriter w;
        w.putU32(static_cast<uint32_t>(rifs.size()));
        for (auto rif : rifs)
        {
            auto iit = m_rifToIntf.find(rif);
            auto vit = m_rifToVid.find(rif);
            w.putU64(static_cast<uint64_t>(rif));
            w.putI32(iit != m_rifToIntf.end() ? iit->second : -1);
            w.putI32(vit != m_rifToVid.end() ? vit->second : -1);
        }
        payload.putTlv(HWSTATE_TAG_L3_RIF, w.buf);
    }

    // 6. L3 NH -> egress
    {
        WbWriter w;
        w.putU32(static_cast<uint32_t>(m_nhToEgr.size()));
        for (const auto &kv : m_nhToEgr)
        {
            w.putU64(static_cast<uint64_t>(kv.first));
            w.putI32(kv.second);
        }
        payload.putTlv(HWSTATE_TAG_L3_NH, w.buf);
    }

    // 7. L3 NHG -> ECMP
    {
        WbWriter w;
        w.putU32(static_cast<uint32_t>(m_nhgToEcmp.size()));
        for (const auto &kv : m_nhgToEcmp)
        {
            w.putU64(static_cast<uint64_t>(kv.first));
            w.putI32(kv.second);
        }
        payload.putTlv(HWSTATE_TAG_L3_NHG, w.buf);
    }

    // 8. L3 Neighbor -> egress
    {
        WbWriter w;
        w.putU32(static_cast<uint32_t>(m_neighborEgr.size()));
        for (const auto &kv : m_neighborEgr)
        {
            w.putStr(kv.first);
            w.putI32(kv.second);
        }
        payload.putTlv(HWSTATE_TAG_L3_NEIGH, w.buf);
    }

    // 9. L3 Route -> egress/ecmp
    {
        WbWriter w;
        w.putU32(static_cast<uint32_t>(m_routeEgr.size()));
        for (const auto &kv : m_routeEgr)
        {
            w.putStr(kv.first);
            w.putI32(kv.second);
        }
        payload.putTlv(HWSTATE_TAG_L3_ROUTE, w.buf);
    }

    // 10. Hash settings (RTAG7 fields & seeds + switch hash OIDs)
    {
        hwbcm_hash_wb_state_t hs;
        hwbcm_hash_wb_get(&hs);
        WbWriter w;
        w.putU64(static_cast<uint64_t>(m_ecmp_hash_id));
        w.putU64(static_cast<uint64_t>(m_lag_hash_id));
        w.putU32(hs.fields[0]);
        w.putU32(hs.fields[1]);
        w.putU32(hs.seed[0]);
        w.putU32(hs.seed[1]);
        payload.putTlv(HWSTATE_TAG_HASH, w.buf);
    }

    // 11. VXLAN globals (C++ local VTEP state + hwbcm_vxlan.c globals)
    {
        int cInited = 0, cUdpPort = 4789;
        hwbcm_vxlan_wb_get_globals(&cInited, &cUdpPort);
        WbWriter w;
        w.putU8(m_vxlanInited ? 1u : 0u);
        w.putI32(m_vxlanStationId);
        w.putU64(static_cast<uint64_t>(m_vtepTunnel));
        w.putU8(m_vtepSrcValid ? 1u : 0u);
        w.putBytes(m_vtepSrcIp4, 4);
        w.putI32(m_vtepTtl);
        w.putI32(m_vtepDscp);
        w.putI32(cInited);
        w.putI32(cUdpPort);
        payload.putTlv(HWSTATE_TAG_VXLAN_GLOBALS, w.buf);
    }

    // 12. VXLAN VPNs, access VPs, and hwbcm_vxlan.c SW tables (s_vp, s_acc_intf, s_vpn_l3)
    {
        WbWriter w;
        w.putU32(static_cast<uint32_t>(m_vniToVpn.size()));
        for (const auto &kv : m_vniToVpn)
        {
            w.putU32(kv.first);
            w.putI32(kv.second);
        }

        w.putU32(static_cast<uint32_t>(m_vpnAccessVps.size()));
        for (const auto &kv : m_vpnAccessVps)
        {
            w.putI32(kv.first);
            w.putU32(static_cast<uint32_t>(kv.second.size()));
            for (const auto &a : kv.second)
            {
                w.putU64(static_cast<uint64_t>(a.member));
                w.putI32(a.vp);
            }
        }

        int nvps = hwbcm_vxlan_wb_get_vps(nullptr, 0);
        std::vector<hwbcm_vxlan_vp_wb_t> vps(nvps > 0 ? static_cast<size_t>(nvps) : 0);
        if (nvps > 0) nvps = hwbcm_vxlan_wb_get_vps(vps.data(), nvps);
        if (nvps < 0) nvps = 0;
        w.putU32(static_cast<uint32_t>(nvps));
        for (int i = 0; i < nvps; i++)
        {
            const auto &v = vps[static_cast<size_t>(i)];
            w.putI32(v.vp_idx);
            w.putI32(v.is_network);
            w.putI32(v.port_gport);
            w.putI32(v.egr_id);
            w.putI32(v.intf_vid);
            w.putI32(v.vpn_id);
            w.putI32(v.uvid);
        }

        int nacc = hwbcm_vxlan_wb_get_acc_intfs(nullptr, 0);
        std::vector<hwbcm_vxlan_acc_intf_wb_t> acc(nacc > 0 ? static_cast<size_t>(nacc) : 0);
        if (nacc > 0) nacc = hwbcm_vxlan_wb_get_acc_intfs(acc.data(), nacc);
        if (nacc < 0) nacc = 0;
        w.putU32(static_cast<uint32_t>(nacc));
        for (int i = 0; i < nacc; i++)
        {
            const auto &a = acc[static_cast<size_t>(i)];
            w.putI32(a.vid);
            w.putI32(a.intf_id);
            w.putI32(a.refcnt);
        }

        int nl3 = hwbcm_vxlan_wb_get_vpn_l3(nullptr, 0);
        std::vector<hwbcm_vxlan_vpn_l3_wb_t> vl3(nl3 > 0 ? static_cast<size_t>(nl3) : 0);
        if (nl3 > 0) nl3 = hwbcm_vxlan_wb_get_vpn_l3(vl3.data(), nl3);
        if (nl3 < 0) nl3 = 0;
        w.putU32(static_cast<uint32_t>(nl3));
        for (int i = 0; i < nl3; i++)
        {
            const auto &l = vl3[static_cast<size_t>(i)];
            w.putI32(l.vfi);
            w.putI32(l.ovl_intf);
            w.putBytes(l.mac, 6);
        }
        payload.putTlv(HWSTATE_TAG_VXLAN_VPNS, w.buf);
    }

    // 13. VXLAN tunnels and remote VTEP state
    {
        WbWriter w;
        w.putU32(static_cast<uint32_t>(m_tunnelInit.size()));
        for (const auto &kv : m_tunnelInit)
        {
            w.putU64(static_cast<uint64_t>(kv.first));
            w.putI32(kv.second);
        }
        w.putU32(static_cast<uint32_t>(m_tunnelTerm.size()));
        for (const auto &kv : m_tunnelTerm)
        {
            w.putU64(static_cast<uint64_t>(kv.first));
            w.putI32(kv.second);
        }
        w.putU32(static_cast<uint32_t>(m_vtepByIp.size()));
        for (const auto &kv : m_vtepByIp)
        {
            const auto &v = kv.second;
            w.putStr(kv.first);
            w.putBytes(v.ip4, 4);
            w.putI32(v.tunnelInit);
            w.putI32(v.tunnelTerm);
            w.putI32(v.egr);
            w.putI32(v.srcEgr);
            w.putI32(v.vp);
            w.putI32(v.egrMc);
            w.putI32(v.vpMc);
            w.putU8(v.vpReady ? 1u : 0u);
            w.putI32(v.vpVpn);
            w.putI32(v.port);
            w.putU32(static_cast<uint32_t>(v.floodVpns.size()));
            for (int fvpn : v.floodVpns)
            {
                w.putI32(fvpn);
            }
        }
        payload.putTlv(HWSTATE_TAG_VXLAN_VTEPS, w.buf);
    }

    // 14. ACL tables (FP groups)
    {
        WbWriter w;
        w.putU32(static_cast<uint32_t>(m_aclTables.size()));
        for (const auto &kv : m_aclTables)
        {
            const auto &t = kv.second;
            w.putU64(static_cast<uint64_t>(kv.first));
            w.putI32(t.saiStage);
            w.putI32(t.hwStage);
            w.putU32(t.quals);
            w.putI32(t.gid);
            w.putI32(t.prio);
            w.putU8(t.matchesPorts ? 1u : 0u);
        }
        payload.putTlv(HWSTATE_TAG_ACL_TABLES, w.buf);
    }

    // 15. ACL entries (variants + stat ids)
    {
        WbWriter w;
        w.putU32(static_cast<uint32_t>(m_aclEntries.size()));
        for (const auto &kv : m_aclEntries)
        {
            const auto &e = kv.second;
            w.putU64(static_cast<uint64_t>(kv.first));
            w.putU64(static_cast<uint64_t>(e.table));
            w.putU64(static_cast<uint64_t>(e.counter));
            w.putU32(static_cast<uint32_t>(e.hw.size()));
            for (const auto &h : e.hw)
            {
                w.putI32(h.eid);
                w.putI32(h.stat);
            }
        }
        payload.putTlv(HWSTATE_TAG_ACL_ENTRIES, w.buf);
    }

    // 16. ACL counters (software offsets)
    {
        WbWriter w;
        w.putU32(static_cast<uint32_t>(m_aclCounters.size()));
        for (const auto &kv : m_aclCounters)
        {
            const auto &c = kv.second;
            w.putU64(static_cast<uint64_t>(kv.first));
            w.putU64(static_cast<uint64_t>(c.table));
            w.putU64(c.pktOffset);
            w.putU64(c.byteOffset);
        }
        payload.putTlv(HWSTATE_TAG_ACL_COUNTERS, w.buf);
    }

    // 17. ACL ranges
    {
        WbWriter w;
        w.putU32(static_cast<uint32_t>(m_aclRanges.size()));
        for (const auto &kv : m_aclRanges)
        {
            w.putU64(static_cast<uint64_t>(kv.first));
            w.putI32(kv.second);
        }
        payload.putTlv(HWSTATE_TAG_ACL_RANGES, w.buf);
    }

    // 18. CoPP (C++ maps + hwbcm_copp.c group/policers/slots/pending to-me traps)
    {
        hwbcm_copp_wb_globals_t cg;
        std::vector<hwbcm_copp_policer_wb_t> cpols(HWBCM_COPP_MAX_POLICERS);
        std::vector<hwbcm_copp_slot_wb_t> cslots(HWBCM_COPP_MAX_TRAPS);
        int npols = 0, nslots = 0;
        hwbcm_copp_wb_get(&cg, cpols.data(), HWBCM_COPP_MAX_POLICERS, &npols,
                          cslots.data(), HWBCM_COPP_MAX_TRAPS, &nslots);

        WbWriter w;
        w.putI32(m_coppLocalEgress);
        w.putU32(static_cast<uint32_t>(m_coppPolicer.size()));
        for (const auto &kv : m_coppPolicer)
        {
            w.putU64(static_cast<uint64_t>(kv.first));
            w.putI32(kv.second);
        }
        w.putU32(static_cast<uint32_t>(m_coppTrapHandle.size()));
        for (const auto &kv : m_coppTrapHandle)
        {
            w.putU64(static_cast<uint64_t>(kv.first));
            w.putI32(kv.second);
        }

        w.putI32(cg.group_valid);
        w.putI32(cg.group);
        w.putI32(cg.have_to_me);
        w.putI32(cg.have_icmp);
        w.putI32(cg.have_stats);
        w.putI32(cg.local_egr);

        w.putU32(static_cast<uint32_t>(npols));
        for (int i = 0; i < npols; i++)
        {
            const auto &p = cpols[static_cast<size_t>(i)];
            w.putI32(p.slot_idx);
            w.putI32(p.pid);
            w.putI32(p.cfg.packets);
            w.putI32(p.cfg.mode);
            w.putI32(p.cfg.color_blind);
            w.putU64(p.cfg.cir);
            w.putU64(p.cfg.cbs);
            w.putU64(p.cfg.pir);
            w.putU64(p.cfg.pbs);
        }

        w.putU32(static_cast<uint32_t>(nslots));
        for (int i = 0; i < nslots; i++)
        {
            const auto &s = cslots[static_cast<size_t>(i)];
            w.putI32(s.slot_idx);
            w.putI32(s.cfg.trap);
            w.putI32(s.cfg.action);
            w.putU32(s.cfg.priority);
            w.putI32(s.cfg.queue);
            w.putI32(s.cfg.policer_id);
            w.putI32(s.cfg.drop_green);
            w.putI32(s.cfg.drop_yellow);
            w.putI32(s.cfg.drop_red);
            int ne = s.neid;
            if (ne < 0) ne = 0;
            if (ne > HWBCM_COPP_MAX_RULES) ne = HWBCM_COPP_MAX_RULES;
            w.putI32(ne);
            for (int j = 0; j < ne; j++)
            {
                w.putI32(s.eid[j]);
                w.putI32(s.stat_id[j]);
            }
            w.putU64(s.base_nr_pkts);
            w.putU64(s.base_nr_bytes);
            w.putU64(s.base_red_pkts);
        }
        payload.putTlv(HWSTATE_TAG_COPP, w.buf);
    }

    // 19. QoS hardware maps cache + per-port map keys / pfcTouched
    {
        WbWriter w;
        w.putU32(static_cast<uint32_t>(m_qosHwMaps.size()));
        for (const auto &kv : m_qosHwMaps)
        {
            w.putStr(kv.first);
            w.putI32(kv.second.id);
            w.putI32(kv.second.refs);
        }
        uint32_t nports = 0;
        for (const auto &kv : m_qosPort)
        {
            const auto &ps = kv.second;
            if (!ps.hwIngDscpKey.empty() || !ps.hwIngDot1pKey.empty() ||
                !ps.hwEgrDscpKey.empty() || ps.pfcTouched)
            {
                nports++;
            }
        }
        w.putU32(nports);
        for (const auto &kv : m_qosPort)
        {
            const auto &ps = kv.second;
            if (!ps.hwIngDscpKey.empty() || !ps.hwIngDot1pKey.empty() ||
                !ps.hwEgrDscpKey.empty() || ps.pfcTouched)
            {
                w.putU64(static_cast<uint64_t>(kv.first));
                w.putStr(ps.hwIngDscpKey);
                w.putStr(ps.hwIngDot1pKey);
                w.putStr(ps.hwEgrDscpKey);
                w.putU8(ps.pfcTouched ? 1u : 0u);
            }
        }
        payload.putTlv(HWSTATE_TAG_QOS_MAPS, w.buf);
    }

    // 20. QoS buffer pools, SP usage, pending PGs
    {
        WbWriter w;
        for (int i = 0; i < 4; i++) w.putU8(m_qosIngressSpUsed[i] ? 1u : 0u);
        for (int i = 0; i < 4; i++) w.putU8(m_qosEgressSpUsed[i] ? 1u : 0u);
        w.putU32(static_cast<uint32_t>(m_qosPools.size()));
        for (const auto &kv : m_qosPools)
        {
            const auto &p = kv.second;
            w.putU64(static_cast<uint64_t>(kv.first));
            w.putI32(p.type);
            w.putI32(p.ingressSp);
            w.putI32(p.egressSp);
            w.putU8(p.dynamic ? 1u : 0u);
            w.putU64(p.size);
        }
        w.putU32(static_cast<uint32_t>(m_qosPgPending.size()));
        for (auto pgOid : m_qosPgPending)
        {
            w.putU64(static_cast<uint64_t>(pgOid));
        }
        payload.putTlv(HWSTATE_TAG_QOS_POOLS, w.buf);
    }

    // 21. QoS first-touch MMU PG/queue snapshots (hwbcm_qos.c)
    {
        int npgs = hwbcm_qos_wb_get_pg_snaps(nullptr, 0);
        std::vector<hwbcm_qos_pg_snap_wb_t> pgs(npgs > 0 ? static_cast<size_t>(npgs) : 0);
        if (npgs > 0) npgs = hwbcm_qos_wb_get_pg_snaps(pgs.data(), npgs);
        if (npgs < 0) npgs = 0;

        int nqs = hwbcm_qos_wb_get_q_snaps(nullptr, 0);
        std::vector<hwbcm_qos_q_snap_wb_t> qs(nqs > 0 ? static_cast<size_t>(nqs) : 0);
        if (nqs > 0) nqs = hwbcm_qos_wb_get_q_snaps(qs.data(), nqs);
        if (nqs < 0) nqs = 0;

        WbWriter w;
        w.putU32(static_cast<uint32_t>(npgs));
        for (int i = 0; i < npgs; i++)
        {
            const auto &s = pgs[static_cast<size_t>(i)];
            w.putI32(s.port);
            w.putI32(s.pg);
            w.putU32(s.min);
            w.putU32(s.hdrm);
            w.putU32(s.dyn);
            w.putU32(s.limit);
            w.putU32(s.floor);
            w.putU32(s.offset);
            w.putI32(s.spid);
        }
        w.putU32(static_cast<uint32_t>(nqs));
        for (int i = 0; i < nqs; i++)
        {
            const auto &s = qs[static_cast<size_t>(i)];
            w.putI32(s.port);
            w.putI32(s.mcast);
            w.putI32(s.cos);
            w.putI32(s.have_min);
            w.putI32(s.have_dyn);
            w.putI32(s.have_shared);
            w.putI32(s.have_limit_en);
            w.putI32(s.have_pool);
            w.putI32(s.min_bytes);
            w.putI32(s.dyn);
            w.putI32(s.shared_bytes);
            w.putI32(s.limit_en);
            w.putI32(s.pool);
            w.putU32(s.alpha);
        }
        payload.putTlv(HWSTATE_TAG_QOS_SNAPS, w.buf);

        // 21b. Optional UC qgroup-min boot snapshots (TD2+ mmu_lossless=0)
        int nqgrp = hwbcm_qos_wb_get_qgrp_snaps(nullptr, 0);
        if (nqgrp > 0)
        {
            std::vector<hwbcm_qos_qgrp_wb_t> qgrps(static_cast<size_t>(nqgrp));
            nqgrp = hwbcm_qos_wb_get_qgrp_snaps(qgrps.data(), nqgrp);
            if (nqgrp > 0)
            {
                WbWriter qw;
                qw.putU32(static_cast<uint32_t>(nqgrp));
                for (int i = 0; i < nqgrp; i++)
                {
                    const auto &g = qgrps[static_cast<size_t>(i)];
                    qw.putI32(g.port);
                    qw.putI32(g.cos);
                    qw.putI32(g.qgrp_min_en);
                }
                payload.putTlv(HWSTATE_TAG_QOS_QGRP_MIN, qw.buf);
            }
        }
    }

    // 22. Mirror sessions
    {
        WbWriter w;
        w.putU32(static_cast<uint32_t>(m_mirrorSessions.size()));
        for (const auto &kv : m_mirrorSessions)
        {
            w.putU64(static_cast<uint64_t>(kv.first));
            w.putI32(kv.second.destGport);
        }
        payload.putTlv(HWSTATE_TAG_MIRROR, w.buf);
    }

    // 23. Stats READ_AND_CLEAR baselines (sparse non-zero entries)
    {
        int nq = hwbcm_stats_wb_get_qbase(nullptr, 0);
        std::vector<hwbcm_qbase_wb_t> qb(nq > 0 ? static_cast<size_t>(nq) : 0);
        if (nq > 0) nq = hwbcm_stats_wb_get_qbase(qb.data(), nq);
        if (nq < 0) nq = 0;

        int nx = hwbcm_stats_wb_get_xbase(nullptr, 0);
        std::vector<hwbcm_xbase_wb_t> xb(nx > 0 ? static_cast<size_t>(nx) : 0);
        if (nx > 0) nx = hwbcm_stats_wb_get_xbase(xb.data(), nx);
        if (nx < 0) nx = 0;

        WbWriter w;
        w.putU32(static_cast<uint32_t>(nq));
        for (int i = 0; i < nq; i++)
        {
            const auto &e = qb[static_cast<size_t>(i)];
            w.putU8(e.port);
            w.putU8(e.mc);
            w.putU8(e.queue);
            w.putU8(e.which);
            w.putU64(e.value);
        }
        w.putU32(static_cast<uint32_t>(nx));
        for (int i = 0; i < nx; i++)
        {
            const auto &e = xb[static_cast<size_t>(i)];
            w.putU8(e.port);
            w.putU8(e.id);
            w.putU64(e.value);
        }
        payload.putTlv(HWSTATE_TAG_STATS_BASE, w.buf);
    }

    uint32_t payloadCrc = wbCrc32Buf(payload.buf.data(), payload.buf.size());
    uint32_t cfgCrc = 0u;
    (void)wbCrc32File(m_configBcm, cfgCrc);
    uint64_t tsNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

    WbWriter file;
    file.putU32(HWSTATE_MAGIC);
    file.putU32(HWSTATE_VERSION);
    file.putU32(HWSTATE_HDR_SZ);
    file.putU32(static_cast<uint32_t>(payload.buf.size()));
    file.putU32(payloadCrc);
    file.putU32(cfgCrc);
    file.putU64(tsNs);
    file.putBytes(payload.buf.data(), payload.buf.size());

    if (!wbWriteFileAtomic(path, file.buf.data(), file.buf.size()))
    {
        return SAI_STATUS_FAILURE;
    }

    SWSS_LOG_NOTICE("wb hwstate: saved %zu bytes (lags=%zu vrs=%zu rifs=%zu nhs=%zu nhgs=%zu neighs=%zu routes=%zu "
            "vpns=%zu vteps=%zu aclT=%zu aclE=%zu aclC=%zu aclR=%zu coppT=%zu coppP=%zu qosM=%zu qosP=%zu mirrors=%zu) -> %s",
            file.buf.size(), m_lagToTrunk.size(), m_vrToVrf.size(), m_rifToIntf.size(),
            m_nhToEgr.size(), m_nhgToEcmp.size(), m_neighborEgr.size(), m_routeEgr.size(),
            m_vniToVpn.size(), m_vtepByIp.size(), m_aclTables.size(), m_aclEntries.size(),
            m_aclCounters.size(), m_aclRanges.size(), m_coppTrapHandle.size(), m_coppPolicer.size(),
            m_qosHwMaps.size(), m_qosPools.size(), m_mirrorSessions.size(),
            path.c_str());
    return SAI_STATUS_SUCCESS;
}

bool SwitchBCM56860HW::hwStateValidateFile(
        _In_ const std::string &path) const
{
    SWSS_LOG_ENTER();

    std::vector<uint8_t> raw;
    if (!wbReadWholeFile(path, raw))
    {
        SWSS_LOG_WARN("wb hwstate: cannot read %s", path.c_str());
        return false;
    }
    WbReader r(raw.data(), raw.size());
    uint32_t magic = 0, version = 0, hdrSz = 0, paySz = 0, payCrc = 0, cfgCrc = 0;
    uint64_t tsNs = 0;
    if (!r.getU32(magic) || !r.getU32(version) || !r.getU32(hdrSz) ||
        !r.getU32(paySz) || !r.getU32(payCrc) || !r.getU32(cfgCrc) || !r.getU64(tsNs))
    {
        SWSS_LOG_ERROR("wb hwstate: truncated header in %s", path.c_str());
        return false;
    }
    if (magic != HWSTATE_MAGIC || version != HWSTATE_VERSION || hdrSz != HWSTATE_HDR_SZ)
    {
        SWSS_LOG_ERROR("wb hwstate: bad header in %s (magic=0x%08x ver=%u hdr=%u)",
                path.c_str(), magic, version, hdrSz);
        return false;
    }
    if (static_cast<size_t>(hdrSz) + static_cast<size_t>(paySz) != raw.size())
    {
        SWSS_LOG_ERROR("wb hwstate: length mismatch in %s (%u+%u != %zu)",
                path.c_str(), hdrSz, paySz, raw.size());
        return false;
    }
    uint32_t actualCrc = wbCrc32Buf(raw.data() + hdrSz, paySz);
    if (actualCrc != payCrc)
    {
        SWSS_LOG_ERROR("wb hwstate: CRC32 mismatch in %s (got 0x%08x expected 0x%08x)",
                path.c_str(), actualCrc, payCrc);
        return false;
    }

    std::string cfgPath = resolveConfigBcm();
    uint32_t curCfgCrc = 0u;
    if (cfgCrc != 0u && wbCrc32File(cfgPath, curCfgCrc) && curCfgCrc != cfgCrc)
    {
        SWSS_LOG_ERROR("wb hwstate: config.bcm changed since warm sync (saved crc=0x%08x current=0x%08x); rejecting warm boot",
                cfgCrc, curCfgCrc);
        return false;
    }
    return true;
}

sai_status_t SwitchBCM56860HW::hwStateLoad(
        _In_ const std::string &path)
{
    SWSS_LOG_ENTER();

    std::vector<uint8_t> raw;
    if (!wbReadWholeFile(path, raw))
    {
        SWSS_LOG_ERROR("wb hwstate: failed to read %s", path.c_str());
        return SAI_STATUS_FAILURE;
    }

    WbReader r(raw.data(), raw.size());
    uint32_t magic = 0, version = 0, hdrSz = 0, paySz = 0, payCrc = 0, cfgCrc = 0;
    uint64_t tsNs = 0;
    if (!r.getU32(magic) || !r.getU32(version) || !r.getU32(hdrSz) ||
        !r.getU32(paySz) || !r.getU32(payCrc) || !r.getU32(cfgCrc) || !r.getU64(tsNs) ||
        magic != HWSTATE_MAGIC || version != HWSTATE_VERSION || hdrSz != HWSTATE_HDR_SZ ||
        static_cast<size_t>(hdrSz) + static_cast<size_t>(paySz) != raw.size() ||
        wbCrc32Buf(raw.data() + hdrSz, paySz) != payCrc)
    {
        SWSS_LOG_ERROR("wb hwstate: corrupt file %s", path.c_str());
        return SAI_STATUS_FAILURE;
    }

    m_lagToTrunk.clear();
    m_vrToVrf.clear();
    m_rifToIntf.clear();
    m_rifToVid.clear();
    m_nhToEgr.clear();
    m_nhgToEcmp.clear();
    m_neighborEgr.clear();
    m_routeEgr.clear();

    // Clear Phase 3 SW handle tables before loading
    m_vxlanInited = false;
    m_vxlanStationId = -1;
    m_vtepTunnel = SAI_NULL_OBJECT_ID;
    m_vtepSrcValid = false;
    memset(m_vtepSrcIp4, 0, sizeof(m_vtepSrcIp4));
    m_vtepTtl = 64;
    m_vtepDscp = -1;
    m_vniToVpn.clear();
    m_vpnAccessVps.clear();
    m_tunnelInit.clear();
    m_tunnelTerm.clear();
    m_vtepByIp.clear();

    m_aclTables.clear();
    m_aclEntries.clear();
    m_aclCounters.clear();
    m_aclRanges.clear();
    m_aclGroups.clear();
    m_aclGroupMembers.clear();
    m_aclBindings.clear();
    m_aclPrioUsed.clear();
    m_aclMirrorDest.clear();

    m_coppLocalEgress = -1;
    m_coppPolicer.clear();
    m_coppTrapHandle.clear();

    m_qosHwMaps.clear();
    m_qosPort.clear();
    m_qosPools.clear();
    for (int i = 0; i < 4; i++)
    {
        m_qosIngressSpUsed[i] = false;
        m_qosEgressSpUsed[i] = false;
    }
    m_qosPgPending.clear();

    m_mirrorSessions.clear();
    m_portIngMirrors.clear();
    m_portEgrMirrors.clear();
    m_statPoolUser.clear();

    bool haveCore = false;
    int cpuEgr = -1, dropEgr = -1;

    bool haveHash = false;
    hwbcm_hash_wb_state_t hashState;
    memset(&hashState, 0, sizeof(hashState));

    bool haveVxlan = false;
    int vxlanCInited = 0, vxlanCUdpPort = 4789;
    std::vector<hwbcm_vxlan_vp_wb_t> vxlanVps;
    std::vector<hwbcm_vxlan_acc_intf_wb_t> vxlanAccIntfs;
    std::vector<hwbcm_vxlan_vpn_l3_wb_t> vxlanVpnL3;

    bool haveCopp = false;
    hwbcm_copp_wb_globals_t coppGlobals;
    memset(&coppGlobals, 0, sizeof(coppGlobals));
    coppGlobals.local_egr = -1;
    std::vector<hwbcm_copp_policer_wb_t> coppPols;
    std::vector<hwbcm_copp_slot_wb_t> coppSlots;

    bool haveQosSnaps = false;
    std::vector<hwbcm_qos_pg_snap_wb_t> qosPgSnaps;
    std::vector<hwbcm_qos_q_snap_wb_t> qosQSnaps;
    std::vector<hwbcm_qos_qgrp_wb_t> qosQgrpSnaps;

    bool haveStatsBase = false;
    std::vector<hwbcm_qbase_wb_t> statsQBase;
    std::vector<hwbcm_xbase_wb_t> statsXBase;

    while (r.remaining() >= 8)
    {
        uint32_t tag = 0, len = 0;
        if (!r.getU32(tag) || !r.getU32(len) || r.remaining() < len)
        {
            SWSS_LOG_ERROR("wb hwstate: truncated TLV in %s", path.c_str());
            return SAI_STATUS_FAILURE;
        }
        WbReader tr(r.data + r.pos, len);
        r.pos += len;

        switch (tag)
        {
            case HWSTATE_TAG_CORE:
                {
                    hwbcm_core_wb_state_t cs;
                    memset(&cs, 0, sizeof(cs));
                    for (int i = 0; i <= HWBCM_TRAP_STP; i++) cs.trap_l2cache_idx[i] = -1;
                    int32_t macValid = 0, ip2me = -1, vfi = -1, wanted = 0;
                    uint32_t ntraps = 0;
                    if (!tr.getBytes(cs.switch_mac, 6) || !tr.getI32(macValid) ||
                        !tr.getI32(ip2me) || !tr.getI32(vfi) || !tr.getI32(wanted) ||
                        !tr.getU32(ntraps))
                    {
                        return SAI_STATUS_FAILURE;
                    }
                    cs.switch_mac_valid = macValid;
                    cs.ip2me_station_id = ip2me;
                    cs.ip2me_vfi_station_id = vfi;
                    cs.ip2me_wanted = wanted;
                    for (uint32_t i = 0; i < ntraps; i++)
                    {
                        int32_t idx = -1;
                        if (!tr.getI32(idx)) return SAI_STATUS_FAILURE;
                        if (i <= static_cast<uint32_t>(HWBCM_TRAP_STP))
                        {
                            cs.trap_l2cache_idx[i] = idx;
                        }
                    }
                    hwbcm_core_wb_restore(&cs);
                    haveCore = true;
                }
                break;

            case HWSTATE_TAG_LAG_TRUNKS:
                {
                    uint32_t n = 0;
                    if (!tr.getU32(n)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < n; i++)
                    {
                        uint64_t oid = 0;
                        int32_t tid = -1;
                        if (!tr.getU64(oid) || !tr.getI32(tid)) return SAI_STATUS_FAILURE;
                        m_lagToTrunk[static_cast<sai_object_id_t>(oid)] = tid;
                        int mc = hwbcm_trunk_member_count_get(tid);
                        if (mc < 0)
                        {
                            SWSS_LOG_WARN("wb: restored lag %s -> trunk %d not found in SDK (rv=%d)",
                                    sai_serialize_object_id(static_cast<sai_object_id_t>(oid)).c_str(), tid, mc);
                        }
                    }
                }
                break;

            case HWSTATE_TAG_L3_GLOBALS:
                {
                    int32_t c = -1, d = -1;
                    if (!tr.getI32(c) || !tr.getI32(d)) return SAI_STATUS_FAILURE;
                    cpuEgr = c;
                    dropEgr = d;
                }
                break;

            case HWSTATE_TAG_L3_VR:
                {
                    uint32_t n = 0;
                    if (!tr.getU32(n)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < n; i++)
                    {
                        uint64_t oid = 0;
                        int32_t vrf = 0;
                        if (!tr.getU64(oid) || !tr.getI32(vrf)) return SAI_STATUS_FAILURE;
                        m_vrToVrf[static_cast<sai_object_id_t>(oid)] = vrf;
                    }
                }
                break;

            case HWSTATE_TAG_L3_RIF:
                {
                    uint32_t n = 0;
                    if (!tr.getU32(n)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < n; i++)
                    {
                        uint64_t oid = 0;
                        int32_t intf = -1, vid = -1;
                        if (!tr.getU64(oid) || !tr.getI32(intf) || !tr.getI32(vid)) return SAI_STATUS_FAILURE;
                        if (intf >= 0) m_rifToIntf[static_cast<sai_object_id_t>(oid)] = intf;
                        if (vid >= 0)  m_rifToVid[static_cast<sai_object_id_t>(oid)] = vid;
                    }
                }
                break;

            case HWSTATE_TAG_L3_NH:
                {
                    uint32_t n = 0;
                    if (!tr.getU32(n)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < n; i++)
                    {
                        uint64_t oid = 0;
                        int32_t egr = -1;
                        if (!tr.getU64(oid) || !tr.getI32(egr)) return SAI_STATUS_FAILURE;
                        m_nhToEgr[static_cast<sai_object_id_t>(oid)] = egr;
                    }
                }
                break;

            case HWSTATE_TAG_L3_NHG:
                {
                    uint32_t n = 0;
                    if (!tr.getU32(n)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < n; i++)
                    {
                        uint64_t oid = 0;
                        int32_t ecmp = -1;
                        if (!tr.getU64(oid) || !tr.getI32(ecmp)) return SAI_STATUS_FAILURE;
                        m_nhgToEcmp[static_cast<sai_object_id_t>(oid)] = ecmp;
                    }
                }
                break;

            case HWSTATE_TAG_L3_NEIGH:
                {
                    uint32_t n = 0;
                    if (!tr.getU32(n)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < n; i++)
                    {
                        std::string key;
                        int32_t egr = -1;
                        if (!tr.getStr(key) || !tr.getI32(egr)) return SAI_STATUS_FAILURE;
                        m_neighborEgr[key] = egr;
                    }
                }
                break;

            case HWSTATE_TAG_L3_ROUTE:
                {
                    uint32_t n = 0;
                    if (!tr.getU32(n)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < n; i++)
                    {
                        std::string key;
                        int32_t egr = -1;
                        if (!tr.getStr(key) || !tr.getI32(egr)) return SAI_STATUS_FAILURE;
                        m_routeEgr[key] = egr;
                    }
                }
                break;

            case HWSTATE_TAG_HASH:
                {
                    uint64_t ecmpOid = 0, lagOid = 0;
                    if (!tr.getU64(ecmpOid) || !tr.getU64(lagOid) ||
                        !tr.getU32(hashState.fields[0]) || !tr.getU32(hashState.fields[1]) ||
                        !tr.getU32(hashState.seed[0]) || !tr.getU32(hashState.seed[1]))
                    {
                        return SAI_STATUS_FAILURE;
                    }
                    if (ecmpOid != static_cast<uint64_t>(SAI_NULL_OBJECT_ID) &&
                        m_ecmp_hash_id == SAI_NULL_OBJECT_ID)
                    {
                        m_ecmp_hash_id = static_cast<sai_object_id_t>(ecmpOid);
                    }
                    if (lagOid != static_cast<uint64_t>(SAI_NULL_OBJECT_ID) &&
                        m_lag_hash_id == SAI_NULL_OBJECT_ID)
                    {
                        m_lag_hash_id = static_cast<sai_object_id_t>(lagOid);
                    }
                    haveHash = true;
                }
                break;

            case HWSTATE_TAG_VXLAN_GLOBALS:
                {
                    uint8_t inited = 0, srcValid = 0;
                    int32_t stationId = -1, ttl = 64, dscp = -1, cInited = 0, cUdp = 4789;
                    uint64_t vtepTun = 0;
                    if (!tr.getU8(inited) || !tr.getI32(stationId) || !tr.getU64(vtepTun) ||
                        !tr.getU8(srcValid) || !tr.getBytes(m_vtepSrcIp4, 4) ||
                        !tr.getI32(ttl) || !tr.getI32(dscp) ||
                        !tr.getI32(cInited) || !tr.getI32(cUdp))
                    {
                        return SAI_STATUS_FAILURE;
                    }
                    m_vxlanInited = (inited != 0);
                    m_vxlanStationId = stationId;
                    m_vtepTunnel = static_cast<sai_object_id_t>(vtepTun);
                    m_vtepSrcValid = (srcValid != 0);
                    m_vtepTtl = ttl;
                    m_vtepDscp = dscp;
                    vxlanCInited = cInited;
                    vxlanCUdpPort = cUdp;
                    haveVxlan = true;
                }
                break;

            case HWSTATE_TAG_VXLAN_VPNS:
                {
                    uint32_t nvni = 0;
                    if (!tr.getU32(nvni)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < nvni; i++)
                    {
                        uint32_t vni = 0;
                        int32_t vpn = -1;
                        if (!tr.getU32(vni) || !tr.getI32(vpn)) return SAI_STATUS_FAILURE;
                        m_vniToVpn[vni] = vpn;
                    }

                    uint32_t naccMap = 0;
                    if (!tr.getU32(naccMap)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < naccMap; i++)
                    {
                        int32_t vpn = -1;
                        uint32_t cnt = 0;
                        if (!tr.getI32(vpn) || !tr.getU32(cnt)) return SAI_STATUS_FAILURE;
                        auto &vec = m_vpnAccessVps[vpn];
                        vec.reserve(cnt);
                        for (uint32_t j = 0; j < cnt; j++)
                        {
                            uint64_t mem = 0;
                            int32_t vp = -1;
                            if (!tr.getU64(mem) || !tr.getI32(vp)) return SAI_STATUS_FAILURE;
                            VxlanAccessVp a;
                            a.member = static_cast<sai_object_id_t>(mem);
                            a.vp = vp;
                            vec.push_back(a);
                        }
                    }

                    uint32_t nvps = 0;
                    if (!tr.getU32(nvps)) return SAI_STATUS_FAILURE;
                    vxlanVps.resize(nvps);
                    for (uint32_t i = 0; i < nvps; i++)
                    {
                        auto &v = vxlanVps[i];
                        if (!tr.getI32(v.vp_idx) || !tr.getI32(v.is_network) ||
                            !tr.getI32(v.port_gport) || !tr.getI32(v.egr_id) ||
                            !tr.getI32(v.intf_vid) || !tr.getI32(v.vpn_id) ||
                            !tr.getI32(v.uvid))
                        {
                            return SAI_STATUS_FAILURE;
                        }
                    }

                    uint32_t nacc = 0;
                    if (!tr.getU32(nacc)) return SAI_STATUS_FAILURE;
                    vxlanAccIntfs.resize(nacc);
                    for (uint32_t i = 0; i < nacc; i++)
                    {
                        auto &a = vxlanAccIntfs[i];
                        if (!tr.getI32(a.vid) || !tr.getI32(a.intf_id) || !tr.getI32(a.refcnt))
                        {
                            return SAI_STATUS_FAILURE;
                        }
                    }

                    uint32_t nl3 = 0;
                    if (!tr.getU32(nl3)) return SAI_STATUS_FAILURE;
                    vxlanVpnL3.resize(nl3);
                    for (uint32_t i = 0; i < nl3; i++)
                    {
                        auto &l = vxlanVpnL3[i];
                        if (!tr.getI32(l.vfi) || !tr.getI32(l.ovl_intf) || !tr.getBytes(l.mac, 6))
                        {
                            return SAI_STATUS_FAILURE;
                        }
                    }
                    haveVxlan = true;
                }
                break;

            case HWSTATE_TAG_VXLAN_VTEPS:
                {
                    uint32_t ninit = 0;
                    if (!tr.getU32(ninit)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < ninit; i++)
                    {
                        uint64_t oid = 0;
                        int32_t id = -1;
                        if (!tr.getU64(oid) || !tr.getI32(id)) return SAI_STATUS_FAILURE;
                        m_tunnelInit[static_cast<sai_object_id_t>(oid)] = id;
                    }

                    uint32_t nterm = 0;
                    if (!tr.getU32(nterm)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < nterm; i++)
                    {
                        uint64_t oid = 0;
                        int32_t id = -1;
                        if (!tr.getU64(oid) || !tr.getI32(id)) return SAI_STATUS_FAILURE;
                        m_tunnelTerm[static_cast<sai_object_id_t>(oid)] = id;
                    }

                    uint32_t nvteps = 0;
                    if (!tr.getU32(nvteps)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < nvteps; i++)
                    {
                        VxlanRemoteVtep v;
                        uint8_t ready = 0;
                        uint32_t nflood = 0;
                        if (!tr.getStr(v.ip) || !tr.getBytes(v.ip4, 4) ||
                            !tr.getI32(v.tunnelInit) || !tr.getI32(v.tunnelTerm) ||
                            !tr.getI32(v.egr) || !tr.getI32(v.srcEgr) ||
                            !tr.getI32(v.vp) || !tr.getI32(v.egrMc) || !tr.getI32(v.vpMc) ||
                            !tr.getU8(ready) || !tr.getI32(v.vpVpn) || !tr.getI32(v.port) ||
                            !tr.getU32(nflood))
                        {
                            return SAI_STATUS_FAILURE;
                        }
                        v.vpReady = (ready != 0);
                        for (uint32_t j = 0; j < nflood; j++)
                        {
                            int32_t fvpn = -1;
                            if (!tr.getI32(fvpn)) return SAI_STATUS_FAILURE;
                            v.floodVpns.insert(fvpn);
                        }
                        std::string key = v.ip;
                        m_vtepByIp[key] = std::move(v);
                    }
                }
                break;

            case HWSTATE_TAG_ACL_TABLES:
                {
                    uint32_t n = 0;
                    if (!tr.getU32(n)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < n; i++)
                    {
                        uint64_t oid = 0;
                        AclTableHw t;
                        uint8_t mp = 0;
                        if (!tr.getU64(oid) || !tr.getI32(t.saiStage) || !tr.getI32(t.hwStage) ||
                            !tr.getU32(t.quals) || !tr.getI32(t.gid) || !tr.getI32(t.prio) ||
                            !tr.getU8(mp))
                        {
                            return SAI_STATUS_FAILURE;
                        }
                        t.matchesPorts = (mp != 0);
                        m_aclTables[static_cast<sai_object_id_t>(oid)] = std::move(t);
                    }
                }
                break;

            case HWSTATE_TAG_ACL_ENTRIES:
                {
                    uint32_t n = 0;
                    if (!tr.getU32(n)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < n; i++)
                    {
                        uint64_t oid = 0, tid = 0, cid = 0;
                        uint32_t nhw = 0;
                        if (!tr.getU64(oid) || !tr.getU64(tid) || !tr.getU64(cid) || !tr.getU32(nhw))
                        {
                            return SAI_STATUS_FAILURE;
                        }
                        AclEntryHw e;
                        e.table = static_cast<sai_object_id_t>(tid);
                        e.counter = static_cast<sai_object_id_t>(cid);
                        e.hw.resize(nhw);
                        for (uint32_t j = 0; j < nhw; j++)
                        {
                            if (!tr.getI32(e.hw[j].eid) || !tr.getI32(e.hw[j].stat))
                            {
                                return SAI_STATUS_FAILURE;
                            }
                        }
                        m_aclEntries[static_cast<sai_object_id_t>(oid)] = std::move(e);
                    }
                }
                break;

            case HWSTATE_TAG_ACL_COUNTERS:
                {
                    uint32_t n = 0;
                    if (!tr.getU32(n)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < n; i++)
                    {
                        uint64_t oid = 0, tid = 0, pOff = 0, bOff = 0;
                        if (!tr.getU64(oid) || !tr.getU64(tid) || !tr.getU64(pOff) || !tr.getU64(bOff))
                        {
                            return SAI_STATUS_FAILURE;
                        }
                        AclCounterHw c;
                        c.table = static_cast<sai_object_id_t>(tid);
                        c.pktOffset = pOff;
                        c.byteOffset = bOff;
                        m_aclCounters[static_cast<sai_object_id_t>(oid)] = std::move(c);
                    }
                }
                break;

            case HWSTATE_TAG_ACL_RANGES:
                {
                    uint32_t n = 0;
                    if (!tr.getU32(n)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < n; i++)
                    {
                        uint64_t oid = 0;
                        int32_t rid = -1;
                        if (!tr.getU64(oid) || !tr.getI32(rid)) return SAI_STATUS_FAILURE;
                        m_aclRanges[static_cast<sai_object_id_t>(oid)] = rid;
                    }
                }
                break;

            case HWSTATE_TAG_COPP:
                {
                    uint32_t np = 0, nt = 0;
                    if (!tr.getI32(m_coppLocalEgress) || !tr.getU32(np)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < np; i++)
                    {
                        uint64_t oid = 0;
                        int32_t pid = -1;
                        if (!tr.getU64(oid) || !tr.getI32(pid)) return SAI_STATUS_FAILURE;
                        m_coppPolicer[static_cast<sai_object_id_t>(oid)] = pid;
                    }
                    if (!tr.getU32(nt)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < nt; i++)
                    {
                        uint64_t oid = 0;
                        int32_t h = -1;
                        if (!tr.getU64(oid) || !tr.getI32(h)) return SAI_STATUS_FAILURE;
                        m_coppTrapHandle[static_cast<sai_object_id_t>(oid)] = h;
                    }

                    if (!tr.getI32(coppGlobals.group_valid) || !tr.getI32(coppGlobals.group) ||
                        !tr.getI32(coppGlobals.have_to_me) || !tr.getI32(coppGlobals.have_icmp) ||
                        !tr.getI32(coppGlobals.have_stats) || !tr.getI32(coppGlobals.local_egr))
                    {
                        return SAI_STATUS_FAILURE;
                    }

                    uint32_t ncpol = 0;
                    if (!tr.getU32(ncpol) || ncpol > HWBCM_COPP_MAX_POLICERS) return SAI_STATUS_FAILURE;
                    coppPols.resize(ncpol);
                    for (uint32_t i = 0; i < ncpol; i++)
                    {
                        auto &p = coppPols[i];
                        if (!tr.getI32(p.slot_idx) || !tr.getI32(p.pid) ||
                            !tr.getI32(p.cfg.packets) || !tr.getI32(p.cfg.mode) ||
                            !tr.getI32(p.cfg.color_blind) || !tr.getU64(p.cfg.cir) ||
                            !tr.getU64(p.cfg.cbs) || !tr.getU64(p.cfg.pir) || !tr.getU64(p.cfg.pbs))
                        {
                            return SAI_STATUS_FAILURE;
                        }
                    }

                    uint32_t ncslots = 0;
                    if (!tr.getU32(ncslots) || ncslots > HWBCM_COPP_MAX_TRAPS) return SAI_STATUS_FAILURE;
                    coppSlots.resize(ncslots);
                    for (uint32_t i = 0; i < ncslots; i++)
                    {
                        auto &s = coppSlots[i];
                        memset(&s, 0, sizeof(s));
                        for (int j = 0; j < HWBCM_COPP_MAX_RULES; j++)
                        {
                            s.eid[j] = -1;
                            s.stat_id[j] = -1;
                        }
                        if (!tr.getI32(s.slot_idx) || !tr.getI32(s.cfg.trap) ||
                            !tr.getI32(s.cfg.action) || !tr.getU32(s.cfg.priority) ||
                            !tr.getI32(s.cfg.queue) || !tr.getI32(s.cfg.policer_id) ||
                            !tr.getI32(s.cfg.drop_green) || !tr.getI32(s.cfg.drop_yellow) ||
                            !tr.getI32(s.cfg.drop_red) || !tr.getI32(s.neid))
                        {
                            return SAI_STATUS_FAILURE;
                        }
                        if (s.neid < 0 || s.neid > HWBCM_COPP_MAX_RULES)
                        {
                            return SAI_STATUS_FAILURE;
                        }
                        for (int j = 0; j < s.neid; j++)
                        {
                            if (!tr.getI32(s.eid[j]) || !tr.getI32(s.stat_id[j]))
                            {
                                return SAI_STATUS_FAILURE;
                            }
                        }
                        if (!tr.getU64(s.base_nr_pkts) || !tr.getU64(s.base_nr_bytes) ||
                            !tr.getU64(s.base_red_pkts))
                        {
                            return SAI_STATUS_FAILURE;
                        }
                    }
                    haveCopp = true;
                }
                break;

            case HWSTATE_TAG_QOS_MAPS:
                {
                    uint32_t nm = 0;
                    if (!tr.getU32(nm)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < nm; i++)
                    {
                        std::string key;
                        QosHwMap hm;
                        if (!tr.getStr(key) || !tr.getI32(hm.id) || !tr.getI32(hm.refs))
                        {
                            return SAI_STATUS_FAILURE;
                        }
                        if (hm.id >= 0 && !hwbcm_qos_map_exists(hm.id))
                        {
                            SWSS_LOG_WARN("wb qos: restored bcm map id 0x%x not found in SDK", hm.id);
                        }
                        m_qosHwMaps[key] = hm;
                    }

                    uint32_t nports = 0;
                    if (!tr.getU32(nports)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < nports; i++)
                    {
                        uint64_t poid = 0;
                        std::string kDscp, kDot1p, kEgr;
                        uint8_t pfcT = 0;
                        if (!tr.getU64(poid) || !tr.getStr(kDscp) || !tr.getStr(kDot1p) ||
                            !tr.getStr(kEgr) || !tr.getU8(pfcT))
                        {
                            return SAI_STATUS_FAILURE;
                        }
                        auto &ps = m_qosPort[static_cast<sai_object_id_t>(poid)];
                        ps.hwIngDscpKey = std::move(kDscp);
                        ps.hwIngDot1pKey = std::move(kDot1p);
                        ps.hwEgrDscpKey = std::move(kEgr);
                        ps.pfcTouched = (pfcT != 0);
                    }
                }
                break;

            case HWSTATE_TAG_QOS_POOLS:
                {
                    for (int i = 0; i < 4; i++)
                    {
                        uint8_t u = 0;
                        if (!tr.getU8(u)) return SAI_STATUS_FAILURE;
                        m_qosIngressSpUsed[i] = (u != 0);
                    }
                    for (int i = 0; i < 4; i++)
                    {
                        uint8_t u = 0;
                        if (!tr.getU8(u)) return SAI_STATUS_FAILURE;
                        m_qosEgressSpUsed[i] = (u != 0);
                    }
                    uint32_t np = 0;
                    if (!tr.getU32(np)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < np; i++)
                    {
                        uint64_t oid = 0;
                        QosPool p;
                        uint8_t dyn = 0;
                        if (!tr.getU64(oid) || !tr.getI32(p.type) || !tr.getI32(p.ingressSp) ||
                            !tr.getI32(p.egressSp) || !tr.getU8(dyn) || !tr.getU64(p.size))
                        {
                            return SAI_STATUS_FAILURE;
                        }
                        p.dynamic = (dyn != 0);
                        m_qosPools[static_cast<sai_object_id_t>(oid)] = p;
                    }
                    uint32_t npg = 0;
                    if (!tr.getU32(npg)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < npg; i++)
                    {
                        uint64_t pgOid = 0;
                        if (!tr.getU64(pgOid)) return SAI_STATUS_FAILURE;
                        m_qosPgPending.insert(static_cast<sai_object_id_t>(pgOid));
                    }
                }
                break;

            case HWSTATE_TAG_QOS_SNAPS:
                {
                    uint32_t npgs = 0;
                    if (!tr.getU32(npgs)) return SAI_STATUS_FAILURE;
                    qosPgSnaps.resize(npgs);
                    for (uint32_t i = 0; i < npgs; i++)
                    {
                        auto &s = qosPgSnaps[i];
                        if (!tr.getI32(s.port) || !tr.getI32(s.pg) || !tr.getU32(s.min) ||
                            !tr.getU32(s.hdrm) || !tr.getU32(s.dyn) || !tr.getU32(s.limit) ||
                            !tr.getU32(s.floor) || !tr.getU32(s.offset) || !tr.getI32(s.spid))
                        {
                            return SAI_STATUS_FAILURE;
                        }
                    }
                    uint32_t nqs = 0;
                    if (!tr.getU32(nqs)) return SAI_STATUS_FAILURE;
                    qosQSnaps.resize(nqs);
                    for (uint32_t i = 0; i < nqs; i++)
                    {
                        auto &s = qosQSnaps[i];
                        if (!tr.getI32(s.port) || !tr.getI32(s.mcast) || !tr.getI32(s.cos) ||
                            !tr.getI32(s.have_min) || !tr.getI32(s.have_dyn) ||
                            !tr.getI32(s.have_shared) || !tr.getI32(s.have_limit_en) ||
                            !tr.getI32(s.have_pool) || !tr.getI32(s.min_bytes) ||
                            !tr.getI32(s.dyn) || !tr.getI32(s.shared_bytes) ||
                            !tr.getI32(s.limit_en) || !tr.getI32(s.pool) || !tr.getU32(s.alpha))
                        {
                            return SAI_STATUS_FAILURE;
                        }
                    }
                    haveQosSnaps = true;
                }
                break;

            case HWSTATE_TAG_QOS_QGRP_MIN:
                {
                    uint32_t ng = 0;
                    if (!tr.getU32(ng)) return SAI_STATUS_FAILURE;
                    qosQgrpSnaps.resize(ng);
                    for (uint32_t i = 0; i < ng; i++)
                    {
                        auto &g = qosQgrpSnaps[i];
                        if (!tr.getI32(g.port) || !tr.getI32(g.cos) || !tr.getI32(g.qgrp_min_en))
                        {
                            return SAI_STATUS_FAILURE;
                        }
                    }
                }
                break;

            case HWSTATE_TAG_MIRROR:
                {
                    uint32_t n = 0;
                    if (!tr.getU32(n)) return SAI_STATUS_FAILURE;
                    for (uint32_t i = 0; i < n; i++)
                    {
                        uint64_t oid = 0;
                        int32_t dg = -1;
                        if (!tr.getU64(oid) || !tr.getI32(dg)) return SAI_STATUS_FAILURE;
                        MirrorSessionHw hw;
                        hw.destGport = dg;
                        m_mirrorSessions[static_cast<sai_object_id_t>(oid)] = hw;
                    }
                }
                break;

            case HWSTATE_TAG_STATS_BASE:
                {
                    uint32_t nq = 0;
                    if (!tr.getU32(nq)) return SAI_STATUS_FAILURE;
                    statsQBase.resize(nq);
                    for (uint32_t i = 0; i < nq; i++)
                    {
                        auto &e = statsQBase[i];
                        if (!tr.getU8(e.port) || !tr.getU8(e.mc) || !tr.getU8(e.queue) ||
                            !tr.getU8(e.which) || !tr.getU64(e.value))
                        {
                            return SAI_STATUS_FAILURE;
                        }
                    }
                    uint32_t nx = 0;
                    if (!tr.getU32(nx)) return SAI_STATUS_FAILURE;
                    statsXBase.resize(nx);
                    for (uint32_t i = 0; i < nx; i++)
                    {
                        auto &e = statsXBase[i];
                        if (!tr.getU8(e.port) || !tr.getU8(e.id) || !tr.getU64(e.value))
                        {
                            return SAI_STATUS_FAILURE;
                        }
                    }
                    haveStatsBase = true;
                }
                break;

            default:
                // Forward-compatible: ignore unknown future TLV tags.
                SWSS_LOG_INFO("wb hwstate: skipping unknown TLV tag %u (%u bytes)", tag, len);
                break;
        }
    }

    // If core MAC was not set in hwstate, restore SW copy from m_objectHash without writing HW.
    if (!haveCore)
    {
        sai_attribute_t sattr;
        if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_SRC_MAC_ADDRESS, sattr))
        {
            hwbcm_core_wb_state_t cs;
            hwbcm_core_wb_get(&cs);
            memcpy(cs.switch_mac, sattr.value.mac, 6);
            cs.switch_mac_valid = 1;
            hwbcm_core_wb_restore(&cs);
        }
    }

    hwL3RestoreSharedEgress(cpuEgr, dropEgr);

    // Restore hwbcm_l3.c SW state (L3 mode probe + VRF bitmap + ECMP list, zero HW writes).
    std::vector<int> vrfs;
    for (const auto &kv : m_vrToVrf)
    {
        if (kv.second > 0)
        {
            vrfs.push_back(kv.second);
        }
    }
    std::vector<int> ecmps;
    for (const auto &kv : m_nhgToEcmp)
    {
        ecmps.push_back(kv.second);
    }
    (void)hwbcm_l3_warm_restore(
            vrfs.empty() ? NULL : vrfs.data(), static_cast<int>(vrfs.size()),
            ecmps.empty() ? NULL : ecmps.data(), static_cast<int>(ecmps.size()));

    hwWarmRebuildLagAndL3FromHash();

    // --- Phase 3 warm restore (all zero HW writes) ---
    if (haveHash)
    {
        hwbcm_hash_wb_restore(&hashState);
    }

    if (haveVxlan)
    {
        (void)hwbcm_vxlan_warm_restore(
                vxlanCInited, vxlanCUdpPort,
                vxlanVps.empty() ? NULL : vxlanVps.data(), static_cast<int>(vxlanVps.size()),
                vxlanAccIntfs.empty() ? NULL : vxlanAccIntfs.data(), static_cast<int>(vxlanAccIntfs.size()),
                vxlanVpnL3.empty() ? NULL : vxlanVpnL3.data(), static_cast<int>(vxlanVpnL3.size()));
    }
    else
    {
        hwbcm_vxlan_warm_reset();
    }
    hwWarmRebuildVxlanFromHash();

    // Mirror before ACL so m_aclMirrorDest is populated if ACL needs it
    hwWarmRebuildMirrorFromHash();

    hwbcm_acl_warm_reset();
    for (const auto &kv : m_aclTables)
    {
        if (kv.second.gid >= 0)
        {
            int rv = hwbcm_acl_group_restore(kv.second.gid, kv.second.hwStage, kv.second.quals);
            if (rv < 0)
            {
                SWSS_LOG_WARN("wb acl: group %d restore failed (rv=%d)", kv.second.gid, rv);
            }
        }
    }
    hwWarmRebuildAclFromHash();

    if (haveCopp)
    {
        (void)hwbcm_copp_warm_restore(
                &coppGlobals,
                coppPols.empty() ? NULL : coppPols.data(), static_cast<int>(coppPols.size()),
                coppSlots.empty() ? NULL : coppSlots.data(), static_cast<int>(coppSlots.size()));
    }
    else
    {
        hwbcm_copp_warm_reset();
    }

    if (haveQosSnaps)
    {
        hwbcm_qos_warm_restore(
                qosPgSnaps.empty() ? NULL : qosPgSnaps.data(), static_cast<int>(qosPgSnaps.size()),
                qosQSnaps.empty() ? NULL : qosQSnaps.data(), static_cast<int>(qosQSnaps.size()));
        if (!qosQgrpSnaps.empty())
        {
            hwbcm_qos_warm_restore_qgrp(qosQgrpSnaps.data(), static_cast<int>(qosQgrpSnaps.size()));
        }
    }
    else
    {
        hwbcm_qos_warm_reset();
    }

    if (haveStatsBase)
    {
        hwbcm_stats_warm_restore(
                statsQBase.empty() ? NULL : statsQBase.data(), static_cast<int>(statsQBase.size()),
                statsXBase.empty() ? NULL : statsXBase.data(), static_cast<int>(statsXBase.size()));
    }

    SWSS_LOG_NOTICE("wb hwstate: restored lags=%zu lagMembers=%zu vrs=%zu rifs=%zu nhs=%zu nhgs=%zu nhgMembers=%zu "
            "neighs=%zu routes=%zu cpuEgr=%d dropEgr=%d vpns=%zu vteps=%zu aclT=%zu aclE=%zu aclC=%zu aclR=%zu "
            "coppT=%zu coppP=%zu qosM=%zu qosP=%zu qosSnap=%zu/%zu mirrors=%zu statsBase=%zu/%zu",
            m_lagToTrunk.size(), m_lagMemberInfo.size(), m_vrToVrf.size(), m_rifToIntf.size(),
            m_nhToEgr.size(), m_nhgToEcmp.size(), m_nhgMemberToNh.size(),
            m_neighborEgr.size(), m_routeEgr.size(), cpuEgr, dropEgr,
            m_vniToVpn.size(), m_vtepByIp.size(), m_aclTables.size(), m_aclEntries.size(),
            m_aclCounters.size(), m_aclRanges.size(), m_coppTrapHandle.size(), m_coppPolicer.size(),
            m_qosHwMaps.size(), m_qosPools.size(), qosPgSnaps.size(), qosQSnaps.size(),
            m_mirrorSessions.size(), statsQBase.size(), statsXBase.size());
    return SAI_STATUS_SUCCESS;
}

void SwitchBCM56860HW::hwWarmRebuildLagAndL3FromHash()
{
    SWSS_LOG_ENTER();

    // LAG members from m_objectHash (zero HW writes)
    m_lagMembers.clear();
    m_lagMemberInfo.clear();
    for (const auto &kv : m_lagToTrunk)
    {
        m_lagMembers[kv.first] = {};
    }
    auto lmit = m_objectHash.find(SAI_OBJECT_TYPE_LAG_MEMBER);
    if (lmit != m_objectHash.end())
    {
        for (const auto &kv : lmit->second)
        {
            sai_object_id_t memberOid = SAI_NULL_OBJECT_ID;
            sai_deserialize_object_id(kv.first, memberOid);
            sai_attribute_t lagAttr, portAttr;
            if (getStoredAttr(SAI_OBJECT_TYPE_LAG_MEMBER, memberOid, SAI_LAG_MEMBER_ATTR_LAG_ID, lagAttr) &&
                getStoredAttr(SAI_OBJECT_TYPE_LAG_MEMBER, memberOid, SAI_LAG_MEMBER_ATTR_PORT_ID, portAttr))
            {
                m_lagMembers[lagAttr.value.oid].push_back(portAttr.value.oid);
                m_lagMemberInfo[memberOid] = std::make_pair(lagAttr.value.oid, portAttr.value.oid);
            }
        }
    }

    // NHG members from m_objectHash (zero HW writes)
    m_nhgMemberToNh.clear();
    m_nhgMemberToNhg.clear();
    auto nmit = m_objectHash.find(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER);
    if (nmit != m_objectHash.end())
    {
        for (const auto &kv : nmit->second)
        {
            sai_object_id_t memberOid = SAI_NULL_OBJECT_ID;
            sai_deserialize_object_id(kv.first, memberOid);
            sai_attribute_t nhgAttr, nhAttr;
            if (getStoredAttr(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER, memberOid, SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID, nhgAttr) &&
                getStoredAttr(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER, memberOid, SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID, nhAttr))
            {
                m_nhgMemberToNhg[memberOid] = nhgAttr.value.oid;
                m_nhgMemberToNh[memberOid] = nhAttr.value.oid;
            }
        }
    }
}

void SwitchBCM56860HW::hwWarmRestoreSwitchOids()
{
    SWSS_LOG_ENTER();

    sai_attribute_t a;
    if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_DEFAULT_VLAN_ID, a) &&
        a.value.oid != SAI_NULL_OBJECT_ID)
    {
        m_default_vlan_id = a.value.oid;
    }
    if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_DEFAULT_1Q_BRIDGE_ID, a) &&
        a.value.oid != SAI_NULL_OBJECT_ID)
    {
        m_default_1q_bridge = a.value.oid;
    }
    if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_ECMP_HASH, a) &&
        a.value.oid != SAI_NULL_OBJECT_ID)
    {
        m_ecmp_hash_id = a.value.oid;
    }
    if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_LAG_HASH, a) &&
        a.value.oid != SAI_NULL_OBJECT_ID)
    {
        m_lag_hash_id = a.value.oid;
    }
    SWSS_LOG_NOTICE("wb: restored base switch OIDs (defVlan=%s def1QBridge=%s ecmpHash=%s lagHash=%s)",
            sai_serialize_object_id(m_default_vlan_id).c_str(),
            sai_serialize_object_id(m_default_1q_bridge).c_str(),
            sai_serialize_object_id(m_ecmp_hash_id).c_str(),
            sai_serialize_object_id(m_lag_hash_id).c_str());
}

void SwitchBCM56860HW::hwWarmRebuildVxlanFromHash()
{
    SWSS_LOG_ENTER();

    m_tunnelMapType.clear();
    m_mapEntryVidVni.clear();
    m_vniRefs.clear();
    m_vidToVni.clear();
    m_tunnelRemoteIp.clear();
    m_termEntryTunnel.clear();
    m_bridgeToVpn.clear();
    m_bridgePortRemoteIp.clear();
    m_bridgePortToVp.clear();
    for (auto &kv : m_vtepByIp)
    {
        kv.second.tunnels.clear();
        kv.second.bridgePorts.clear();
        kv.second.macs.clear();
    }

    // 1. Tunnel maps
    auto tmit = m_objectHash.find(SAI_OBJECT_TYPE_TUNNEL_MAP);
    if (tmit != m_objectHash.end())
    {
        for (const auto &kv : tmit->second)
        {
            sai_object_id_t oid = SAI_NULL_OBJECT_ID;
            sai_deserialize_object_id(kv.first, oid);
            sai_attribute_t a;
            m_tunnelMapType[oid] = getStoredAttr(SAI_OBJECT_TYPE_TUNNEL_MAP, oid, SAI_TUNNEL_MAP_ATTR_TYPE, a)
                                   ? a.value.s32 : -1;
        }
    }

    // 2. Tunnel map entries -> m_mapEntryVidVni, m_vidToVni, m_vniRefs
    auto tmeit = m_objectHash.find(SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY);
    if (tmeit != m_objectHash.end())
    {
        for (const auto &kv : tmeit->second)
        {
            sai_object_id_t oid = SAI_NULL_OBJECT_ID;
            sai_deserialize_object_id(kv.first, oid);
            int32_t type = -1;
            sai_attribute_t a;
            if (getStoredAttr(SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, oid, SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE, a))
            {
                type = a.value.s32;
            }
            else if (getStoredAttr(SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, oid, SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP, a))
            {
                auto mit = m_tunnelMapType.find(a.value.oid);
                if (mit != m_tunnelMapType.end()) type = mit->second;
            }
            int vid = -1;
            uint32_t vni = 0;
            if (type == SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID)
            {
                sai_attribute_t k, v;
                if (getStoredAttr(SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, oid, SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY, k) &&
                    getStoredAttr(SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, oid, SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE, v))
                {
                    vni = k.value.u32;
                    vid = v.value.u16;
                }
            }
            else if (type == SAI_TUNNEL_MAP_TYPE_VLAN_ID_TO_VNI)
            {
                sai_attribute_t k, v;
                if (getStoredAttr(SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, oid, SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_KEY, k) &&
                    getStoredAttr(SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, oid, SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE, v))
                {
                    vid = k.value.u16;
                    vni = v.value.u32;
                }
            }
            if (vid > 0 && vid <= 4095 && vni <= 0xFFFFFFu && m_vniToVpn.count(vni))
            {
                m_mapEntryVidVni[oid] = std::make_pair(vid, vni);
                m_vidToVni[vid] = vni;
                m_vniRefs[vni]++;
            }
        }
    }

    // 3. Tunnels -> m_tunnelRemoteIp, VxlanRemoteVtep::tunnels, P2MP fallback
    auto tunit = m_objectHash.find(SAI_OBJECT_TYPE_TUNNEL);
    if (tunit != m_objectHash.end())
    {
        for (const auto &kv : tunit->second)
        {
            sai_object_id_t tid = SAI_NULL_OBJECT_ID;
            sai_deserialize_object_id(kv.first, tid);
            sai_attribute_t typeAttr;
            if (!getStoredAttr(SAI_OBJECT_TYPE_TUNNEL, tid, SAI_TUNNEL_ATTR_TYPE, typeAttr) ||
                typeAttr.value.s32 != SAI_TUNNEL_TYPE_VXLAN)
            {
                continue;
            }
            sai_attribute_t peerAttr, dstAttr;
            bool p2p = getStoredAttr(SAI_OBJECT_TYPE_TUNNEL, tid, SAI_TUNNEL_ATTR_PEER_MODE, peerAttr) &&
                       peerAttr.value.s32 == SAI_TUNNEL_PEER_MODE_P2P &&
                       getStoredAttr(SAI_OBJECT_TYPE_TUNNEL, tid, SAI_TUNNEL_ATTR_ENCAP_DST_IP, dstAttr);
            if (!p2p)
            {
                if (m_vtepTunnel == SAI_NULL_OBJECT_ID)
                {
                    m_vtepTunnel = tid;
                    sai_attribute_t srcAttr;
                    if (!m_vtepSrcValid &&
                        getStoredAttr(SAI_OBJECT_TYPE_TUNNEL, tid, SAI_TUNNEL_ATTR_ENCAP_SRC_IP, srcAttr) &&
                        vxlanIp4Of(srcAttr.value.ipaddr, m_vtepSrcIp4))
                    {
                        m_vtepSrcValid = true;
                    }
                    sai_attribute_t ttlM, ttlV;
                    if (getStoredAttr(SAI_OBJECT_TYPE_TUNNEL, tid, SAI_TUNNEL_ATTR_ENCAP_TTL_MODE, ttlM) &&
                        ttlM.value.s32 == SAI_TUNNEL_TTL_MODE_PIPE_MODEL &&
                        getStoredAttr(SAI_OBJECT_TYPE_TUNNEL, tid, SAI_TUNNEL_ATTR_ENCAP_TTL_VAL, ttlV) &&
                        ttlV.value.u8 > 0)
                    {
                        m_vtepTtl = ttlV.value.u8;
                    }
                    sai_attribute_t dscpM, dscpV;
                    if (getStoredAttr(SAI_OBJECT_TYPE_TUNNEL, tid, SAI_TUNNEL_ATTR_ENCAP_DSCP_MODE, dscpM) &&
                        dscpM.value.s32 == SAI_TUNNEL_DSCP_MODE_PIPE_MODEL &&
                        getStoredAttr(SAI_OBJECT_TYPE_TUNNEL, tid, SAI_TUNNEL_ATTR_ENCAP_DSCP_VAL, dscpV))
                    {
                        m_vtepDscp = dscpV.value.u8;
                    }
                }
                continue;
            }

            uint8_t dst4[4] = {0, 0, 0, 0};
            if (!vxlanIp4Of(dstAttr.value.ipaddr, dst4))
            {
                continue;
            }
            std::string key = sai_serialize_ip_address(dstAttr.value.ipaddr);
            auto vt = m_vtepByIp.find(key);
            if (vt != m_vtepByIp.end())
            {
                m_tunnelRemoteIp[tid] = key;
                vt->second.tunnels.insert(tid);
            }
        }
    }

    // 4. Tunnel term table entries -> m_termEntryTunnel
    auto ttit = m_objectHash.find(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY);
    if (ttit != m_objectHash.end())
    {
        for (const auto &kv : ttit->second)
        {
            sai_object_id_t termId = SAI_NULL_OBJECT_ID;
            sai_deserialize_object_id(kv.first, termId);
            sai_attribute_t ttAttr, actAttr;
            if (getStoredAttr(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY, termId, SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE, ttAttr) &&
                ttAttr.value.s32 != SAI_TUNNEL_TYPE_VXLAN)
            {
                continue;
            }
            sai_object_id_t tun = SAI_NULL_OBJECT_ID;
            if (getStoredAttr(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY, termId, SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID, actAttr))
            {
                tun = actAttr.value.oid;
            }
            m_termEntryTunnel[termId] = tun;
        }
    }

    // 5. 1D bridges -> m_bridgeToVpn
    auto brit = m_objectHash.find(SAI_OBJECT_TYPE_BRIDGE);
    if (brit != m_objectHash.end())
    {
        for (const auto &kv : brit->second)
        {
            sai_object_id_t bid = SAI_NULL_OBJECT_ID;
            sai_deserialize_object_id(kv.first, bid);
            sai_attribute_t typeAttr;
            if (getStoredAttr(SAI_OBJECT_TYPE_BRIDGE, bid, SAI_BRIDGE_ATTR_TYPE, typeAttr) &&
                typeAttr.value.s32 == SAI_BRIDGE_TYPE_1D)
            {
                m_bridgeToVpn[bid] = -1;
            }
        }
    }

    // 6. Tunnel bridge ports -> m_bridgePortRemoteIp, m_bridgePortToVp, VxlanRemoteVtep::bridgePorts
    auto bpit = m_objectHash.find(SAI_OBJECT_TYPE_BRIDGE_PORT);
    if (bpit != m_objectHash.end())
    {
        for (const auto &kv : bpit->second)
        {
            sai_object_id_t bpId = SAI_NULL_OBJECT_ID;
            sai_deserialize_object_id(kv.first, bpId);
            sai_attribute_t typeAttr, tunAttr;
            if (!getStoredAttr(SAI_OBJECT_TYPE_BRIDGE_PORT, bpId, SAI_BRIDGE_PORT_ATTR_TYPE, typeAttr) ||
                typeAttr.value.s32 != SAI_BRIDGE_PORT_TYPE_TUNNEL ||
                !getStoredAttr(SAI_OBJECT_TYPE_BRIDGE_PORT, bpId, SAI_BRIDGE_PORT_ATTR_TUNNEL_ID, tunAttr))
            {
                continue;
            }
            auto rt = m_tunnelRemoteIp.find(tunAttr.value.oid);
            if (rt == m_tunnelRemoteIp.end())
            {
                continue;
            }
            const std::string &key = rt->second;
            auto vt = m_vtepByIp.find(key);
            if (vt == m_vtepByIp.end())
            {
                continue;
            }
            m_bridgePortRemoteIp[bpId] = key;
            vt->second.bridgePorts.insert(bpId);
            if (vt->second.vpReady)
            {
                m_bridgePortToVp[bpId] = vt->second.vp;
            }
        }
    }

    // 7. Remote FDB entries on tunnel bridge ports -> VxlanRemoteVtep::macs
    auto fdbit = m_objectHash.find(SAI_OBJECT_TYPE_FDB_ENTRY);
    if (fdbit != m_objectHash.end())
    {
        for (const auto &kv : fdbit->second)
        {
            sai_attribute_t bpAttr;
            if (!getStoredAttr(SAI_OBJECT_TYPE_FDB_ENTRY, kv.first, SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID, bpAttr))
            {
                continue;
            }
            auto rmit = m_bridgePortRemoteIp.find(bpAttr.value.oid);
            if (rmit == m_bridgePortRemoteIp.end())
            {
                continue;
            }
            auto vt = m_vtepByIp.find(rmit->second);
            if (vt == m_vtepByIp.end())
            {
                continue;
            }
            sai_fdb_entry_t fdb;
            memset(&fdb, 0, sizeof(fdb));
            try
            {
                sai_deserialize_fdb_entry(kv.first, fdb);
            }
            catch (const std::exception &)
            {
                continue;
            }
            int vid = vidOf(fdb.bv_id);
            if (vid < 0)
            {
                continue;
            }
            VxlanMac m;
            memcpy(m.mac, fdb.mac_address, sizeof(sai_mac_t));
            m.vid = vid;
            vt->second.macs[sai_serialize_fdb_entry(fdb)] = m;
        }
    }
}

void SwitchBCM56860HW::hwWarmRebuildAclFromHash()
{
    SWSS_LOG_ENTER();

    m_aclPrioUsed.clear();
    for (auto &kv : m_aclTables)
    {
        kv.second.entries.clear();
        if (kv.second.prio >= 0)
        {
            m_aclPrioUsed[kv.second.hwStage].insert(kv.second.prio);
        }
    }

    // Backfill any ACL counters present in m_objectHash that were not in .hwstate
    auto citHash = m_objectHash.find(SAI_OBJECT_TYPE_ACL_COUNTER);
    if (citHash != m_objectHash.end())
    {
        for (const auto &kv : citHash->second)
        {
            sai_object_id_t cid = SAI_NULL_OBJECT_ID;
            sai_deserialize_object_id(kv.first, cid);
            if (m_aclCounters.find(cid) == m_aclCounters.end())
            {
                AclCounterHw c;
                sai_attribute_t a;
                if (getStoredAttr(SAI_OBJECT_TYPE_ACL_COUNTER, cid, SAI_ACL_COUNTER_ATTR_TABLE_ID, a))
                {
                    c.table = a.value.oid;
                }
                if (getStoredAttr(SAI_OBJECT_TYPE_ACL_COUNTER, cid, SAI_ACL_COUNTER_ATTR_PACKETS, a))
                {
                    c.pktOffset = a.value.u64;
                }
                if (getStoredAttr(SAI_OBJECT_TYPE_ACL_COUNTER, cid, SAI_ACL_COUNTER_ATTR_BYTES, a))
                {
                    c.byteOffset = a.value.u64;
                }
                m_aclCounters[cid] = c;
            }
        }
    }

    for (auto &kv : m_aclCounters)
    {
        kv.second.entries.clear();
    }

    for (const auto &kv : m_aclEntries)
    {
        auto tit = m_aclTables.find(kv.second.table);
        if (tit != m_aclTables.end())
        {
            tit->second.entries.insert(kv.first);
        }
        if (kv.second.counter != SAI_NULL_OBJECT_ID)
        {
            auto cit = m_aclCounters.find(kv.second.counter);
            if (cit != m_aclCounters.end())
            {
                cit->second.entries.insert(kv.first);
            }
        }
    }

    // ACL table groups
    m_aclGroups.clear();
    auto gitHash = m_objectHash.find(SAI_OBJECT_TYPE_ACL_TABLE_GROUP);
    if (gitHash != m_objectHash.end())
    {
        for (const auto &kv : gitHash->second)
        {
            sai_object_id_t gid = SAI_NULL_OBJECT_ID;
            sai_deserialize_object_id(kv.first, gid);
            AclGroupHw g;
            sai_attribute_t a;
            if (getStoredAttr(SAI_OBJECT_TYPE_ACL_TABLE_GROUP, gid, SAI_ACL_TABLE_GROUP_ATTR_ACL_STAGE, a))
            {
                g.saiStage = a.value.s32;
            }
            m_aclGroups[gid] = g;
        }
    }

    // ACL table group members
    m_aclGroupMembers.clear();
    auto gmitHash = m_objectHash.find(SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER);
    if (gmitHash != m_objectHash.end())
    {
        for (const auto &kv : gmitHash->second)
        {
            sai_object_id_t mid = SAI_NULL_OBJECT_ID;
            sai_deserialize_object_id(kv.first, mid);
            sai_attribute_t ga, ta;
            if (getStoredAttr(SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER, mid, SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_GROUP_ID, ga) &&
                getStoredAttr(SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER, mid, SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_ID, ta))
            {
                m_aclGroupMembers[mid] = std::make_pair(ga.value.oid, ta.value.oid);
                auto git = m_aclGroups.find(ga.value.oid);
                if (git != m_aclGroups.end())
                {
                    git->second.members.insert(mid);
                }
            }
        }
    }

    // ACL bind points across PORT / LAG / VLAN / ROUTER_INTERFACE / SWITCH
    m_aclBindings.clear();
    struct BindSpec
    {
        sai_object_type_t ot;
        sai_attr_id_t attrId;
        int32_t stage;
    };
    static const BindSpec specs[] = {
        { SAI_OBJECT_TYPE_PORT,             SAI_PORT_ATTR_INGRESS_ACL,             SAI_ACL_STAGE_INGRESS },
        { SAI_OBJECT_TYPE_PORT,             SAI_PORT_ATTR_EGRESS_ACL,              SAI_ACL_STAGE_EGRESS },
        { SAI_OBJECT_TYPE_LAG,              SAI_LAG_ATTR_INGRESS_ACL,              SAI_ACL_STAGE_INGRESS },
        { SAI_OBJECT_TYPE_LAG,              SAI_LAG_ATTR_EGRESS_ACL,               SAI_ACL_STAGE_EGRESS },
        { SAI_OBJECT_TYPE_VLAN,             SAI_VLAN_ATTR_INGRESS_ACL,             SAI_ACL_STAGE_INGRESS },
        { SAI_OBJECT_TYPE_VLAN,             SAI_VLAN_ATTR_EGRESS_ACL,              SAI_ACL_STAGE_EGRESS },
        { SAI_OBJECT_TYPE_ROUTER_INTERFACE, SAI_ROUTER_INTERFACE_ATTR_INGRESS_ACL, SAI_ACL_STAGE_INGRESS },
        { SAI_OBJECT_TYPE_ROUTER_INTERFACE, SAI_ROUTER_INTERFACE_ATTR_EGRESS_ACL,  SAI_ACL_STAGE_EGRESS },
        { SAI_OBJECT_TYPE_SWITCH,           SAI_SWITCH_ATTR_INGRESS_ACL,           SAI_ACL_STAGE_INGRESS },
        { SAI_OBJECT_TYPE_SWITCH,           SAI_SWITCH_ATTR_EGRESS_ACL,            SAI_ACL_STAGE_EGRESS },
        { SAI_OBJECT_TYPE_SWITCH,           SAI_SWITCH_ATTR_PRE_INGRESS_ACL,       SAI_ACL_STAGE_PRE_INGRESS },
    };

    for (const auto &sp : specs)
    {
        auto oit = m_objectHash.find(sp.ot);
        if (oit == m_objectHash.end())
        {
            continue;
        }
        for (const auto &kv : oit->second)
        {
            sai_object_id_t oid = SAI_NULL_OBJECT_ID;
            sai_deserialize_object_id(kv.first, oid);
            sai_attribute_t a;
            if (getStoredAttr(sp.ot, oid, sp.attrId, a) && a.value.oid != SAI_NULL_OBJECT_ID)
            {
                AclBinding b;
                b.ot = sp.ot;
                b.acl = a.value.oid;
                m_aclBindings[std::make_pair(oid, sp.stage)] = b;
            }
        }
    }
}

void SwitchBCM56860HW::hwWarmRebuildMirrorFromHash()
{
    SWSS_LOG_ENTER();

    m_aclMirrorDest.clear();
    m_portIngMirrors.clear();
    m_portEgrMirrors.clear();

    for (auto &kv : m_mirrorSessions)
    {
        if (kv.second.destGport != -1 && !hwbcm_mirror_dest_exists(kv.second.destGport))
        {
            SWSS_LOG_WARN("wb mirror: session %s dest gport 0x%x not found in SDK",
                    sai_serialize_object_id(kv.first).c_str(), kv.second.destGport);
        }
        MirrorCfgSnapshot cfg;
        if (mirrorReadConfig(kv.first, cfg) == SAI_STATUS_SUCCESS)
        {
            kv.second.cfg = cfg;
        }
        if (kv.second.destGport != -1)
        {
            m_aclMirrorDest[kv.first] = kv.second.destGport;
        }
    }

    auto pit = m_objectHash.find(SAI_OBJECT_TYPE_PORT);
    if (pit != m_objectHash.end())
    {
        for (const auto &kv : pit->second)
        {
            sai_object_id_t poid = SAI_NULL_OBJECT_ID;
            sai_deserialize_object_id(kv.first, poid);
            sai_attribute_t a;
            if (getStoredAttr(SAI_OBJECT_TYPE_PORT, poid, SAI_PORT_ATTR_INGRESS_MIRROR_SESSION, a) &&
                a.value.objlist.list != NULL)
            {
                for (uint32_t i = 0; i < a.value.objlist.count; i++)
                {
                    if (m_mirrorSessions.count(a.value.objlist.list[i]))
                    {
                        m_portIngMirrors[poid].insert(a.value.objlist.list[i]);
                    }
                }
            }
            if (getStoredAttr(SAI_OBJECT_TYPE_PORT, poid, SAI_PORT_ATTR_EGRESS_MIRROR_SESSION, a) &&
                a.value.objlist.list != NULL)
            {
                for (uint32_t i = 0; i < a.value.objlist.count; i++)
                {
                    if (m_mirrorSessions.count(a.value.objlist.list[i]))
                    {
                        m_portEgrMirrors[poid].insert(a.value.objlist.list[i]);
                    }
                }
            }
        }
    }
}

sai_status_t SwitchBCM56860HW::hwWarmRecoverKnet()
{
    SWSS_LOG_ENTER();

    m_hostifToNetif.clear();

    std::vector<hwbcm_knet_info_t> live(256);
    int nlive = hwbcm_knet_warm_recover(live.data(), static_cast<int>(live.size()));
    if (nlive < 0)
    {
        SWSS_LOG_WARN("wb knet: hwbcm_knet_warm_recover returned %d", nlive);
        nlive = 0;
    }

    auto hit = m_objectHash.find(SAI_OBJECT_TYPE_HOSTIF);
    if (hit == m_objectHash.end() || hit->second.empty())
    {
        SWSS_LOG_NOTICE("wb knet: no SAI_OBJECT_TYPE_HOSTIF objects in WarmBootState (live=%d)", nlive);
        return SAI_STATUS_SUCCESS;
    }

    // The SDK-side KNET bring-up was decided before soc_init (hwbcm_warm_knet_probe);
    // the netif recovery follows the live table: netifs present -> attach to them
    // (destroying them would drop the Linux-side config swss does not re-apply on a
    // syncd-only restart), none -> recreate (Case B).
    int knetState = hwbcm_warm_knet_state();
    SWSS_LOG_NOTICE("wb knet: verdict %d (0 live, 1 dma-reset, 2 fresh, 3 unknown), %d live netif(s)",
            knetState, nlive);
    if (knetState == HWBCM_KNET_FRESH && nlive > 0)
    {
        SWSS_LOG_ERROR("wb knet: verdict was FRESH (0 netifs before soc_init) but %d netif(s) are live now; "
                "attaching to them (Case A)", nlive);
    }
    else if ((knetState == HWBCM_KNET_LIVE || knetState == HWBCM_KNET_DMA_RESET) && nlive == 0)
    {
        SWSS_LOG_ERROR("wb knet: verdict %d expected live netifs but the table is empty; recreating (Case B)",
                knetState);
    }

    if (nlive > 0)
    {
        // Case A: syncd restart — kernel kept KNET netifs/filters alive.
        // Match by name -> rebuild m_hostifToNetif with ZERO KNET mutations.
        std::map<std::string, int> nameToId;
        int ncap = nlive < static_cast<int>(live.size()) ? nlive : static_cast<int>(live.size());
        for (int i = 0; i < ncap; i++)
        {
            nameToId[std::string(live[static_cast<size_t>(i)].name)] = live[static_cast<size_t>(i)].netif_id;
        }

        size_t matched = 0, missing = 0;
        for (const auto &kv : hit->second)
        {
            sai_object_id_t hostifOid = SAI_NULL_OBJECT_ID;
            sai_deserialize_object_id(kv.first, hostifOid);
            sai_attribute_t typeAttr, nameAttr;
            if (!getStoredAttr(SAI_OBJECT_TYPE_HOSTIF, hostifOid, SAI_HOSTIF_ATTR_TYPE, typeAttr) ||
                typeAttr.value.s32 != SAI_HOSTIF_TYPE_NETDEV)
            {
                continue;
            }
            if (!getStoredAttr(SAI_OBJECT_TYPE_HOSTIF, hostifOid, SAI_HOSTIF_ATTR_NAME, nameAttr))
            {
                continue;
            }
            char ifname[SAI_HOSTIF_NAME_SIZE + 1];
            memset(ifname, 0, sizeof(ifname));
            memcpy(ifname, nameAttr.value.chardata, SAI_HOSTIF_NAME_SIZE);

            auto nit = nameToId.find(ifname);
            if (nit != nameToId.end())
            {
                m_hostifToNetif[hostifOid] = nit->second;
                matched++;
            }
            else
            {
                SWSS_LOG_WARN("wb knet: hostif %s (%s) not found in live kernel KNET table",
                        kv.first.c_str(), ifname);
                missing++;
            }
        }
        SWSS_LOG_NOTICE("wb knet: Case A (syncd restart) matched %zu hostif(s) to live KNET netifs (missing=%zu, live=%d)",
                matched, missing, nlive);
        return SAI_STATUS_SUCCESS;
    }

    // Case B: host kexec — kernel restarted fresh with 0 KNET netifs while
    // ASIC data plane is still forwarding. Recreate netifs from m_objectHash.
    SWSS_LOG_NOTICE("wb knet: Case B (0 live netifs, %zu HOSTIF in WarmBootState): recreating KNET netdevs",
            hit->second.size());

    uint8_t mac[6] = { 0 };
    sai_attribute_t sattr;
    if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_SRC_MAC_ADDRESS, sattr))
    {
        memcpy(mac, sattr.value.mac, sizeof(mac));
    }

    size_t recreated = 0, expected = 0;
    for (const auto &kv : hit->second)
    {
        sai_object_id_t hostifOid = SAI_NULL_OBJECT_ID;
        sai_deserialize_object_id(kv.first, hostifOid);
        sai_attribute_t typeAttr, objAttr, nameAttr;
        if (!getStoredAttr(SAI_OBJECT_TYPE_HOSTIF, hostifOid, SAI_HOSTIF_ATTR_TYPE, typeAttr) ||
            typeAttr.value.s32 != SAI_HOSTIF_TYPE_NETDEV)
        {
            continue;
        }
        if (!getStoredAttr(SAI_OBJECT_TYPE_HOSTIF, hostifOid, SAI_HOSTIF_ATTR_OBJ_ID, objAttr) ||
            !getStoredAttr(SAI_OBJECT_TYPE_HOSTIF, hostifOid, SAI_HOSTIF_ATTR_NAME, nameAttr))
        {
            continue;
        }
        if (objectTypeQuery(objAttr.value.oid) != SAI_OBJECT_TYPE_PORT)
        {
            continue;
        }
        int bcm = bcmPortOf(objAttr.value.oid);
        if (bcm < 0)
        {
            continue;
        }
        char ifname[SAI_HOSTIF_NAME_SIZE + 1];
        memset(ifname, 0, sizeof(ifname));
        memcpy(ifname, nameAttr.value.chardata, SAI_HOSTIF_NAME_SIZE);

        expected++;
        int netifId = 0;
        int rv = hwbcm_knet_netif_create(bcm, ifname, mac, &netifId);
        if (rv == 0)
        {
            m_hostifToNetif[hostifOid] = netifId;
            recreated++;
            if (recreated <= 4)
            {
                SWSS_LOG_NOTICE("wb knet: Case B hostif %s %s -> bcm port %d netif %d",
                        kv.first.c_str(), ifname, bcm, netifId);
            }
        }
        else
        {
            SWSS_LOG_ERROR("wb knet: Case B recreate(%s, port %d) failed: %d", ifname, bcm, rv);
        }
    }
    if (recreated < expected)
    {
        // No fallback to a cold SDK init: that would reset the data plane.
        SWSS_LOG_ERROR("wb knet: Case B recreated %zu/%zu KNET netdev(s) (%zu FAILED, CPU path of those ports is dead)",
                recreated, expected, expected - recreated);
    }
    else
    {
        SWSS_LOG_NOTICE("wb knet: Case B recreated %zu/%zu KNET netdev(s)", recreated, expected);
    }
    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwSetBridgePort(
        _In_ sai_object_id_t bp_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    if (attr == NULL || attr->id != SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE)
    {
        return SAI_STATUS_SUCCESS;
    }

    int learnEnable = 1;
    switch (attr->value.s32)
    {
        case SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DISABLE:
        case SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DROP:
        case SAI_BRIDGE_PORT_FDB_LEARNING_MODE_CPU_TRAP:
            learnEnable = 0;
            break;
        default:
            learnEnable = 1;
            break;
    }

    sai_object_id_t target = bridgePortTarget(bp_id);
    if (target == SAI_NULL_OBJECT_ID)
    {
        return SAI_STATUS_SUCCESS;
    }

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
            for (auto p : it->second)
            {
                int bcm = bcmPortOf(p);
                if (bcm >= 0)
                {
                    ports.push_back(bcm);
                }
            }
        }
    }

    for (int bcm : ports)
    {
        int rv = hwbcm_port_learn_set(bcm, learnEnable);
        if (rv < 0)
        {
            SWSS_LOG_WARN("hwbcm_port_learn_set(%d, %d) for bridge port %s failed: %d",
                    bcm, learnEnable, sai_serialize_object_id(bp_id).c_str(), rv);
        }
    }
    return SAI_STATUS_SUCCESS;
}
