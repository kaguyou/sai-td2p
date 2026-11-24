/*
 * SwitchBCM56860HW_wb.inc.cpp — Warm Boot Phase 1 & Phase 2 implementation
 * (included by SwitchBCM56860HW.cpp).
 *
 * Covers:
 *  - Sibling .scache / .hwstate path derivation from SwitchConfig
 *  - Versioned TLV binary .hwstate persistence (magic, version, payload CRC32,
 *    config.bcm CRC32, atomic .tmp + fsync + rename + dir fsync)
 *  - Core (IP2ME stations, L2 cache traps, switch MAC), LAG (m_lagToTrunk +
 *    m_lagMembers/m_lagMemberInfo), L3 (g_cpuEgr, g_dropEgr, m_vrToVrf,
 *    m_rifToIntf, m_rifToVid, m_nhToEgr, m_nhgToEcmp, m_neighborEgr,
 *    m_routeEgr, m_nhgMemberToNh/Nhg, hwbcm_l3_warm_restore)
 *  - KNET warm recovery (Case A live traverse attach; Case B kexec recreate)
 *  - Bridge port FDB learning mode wiring (orchagent warm-restart freeze)
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
        HWSTATE_TAG_CORE       = 1u,
        HWSTATE_TAG_LAG_TRUNKS = 2u,
        HWSTATE_TAG_L3_GLOBALS = 3u,
        HWSTATE_TAG_L3_VR      = 4u,
        HWSTATE_TAG_L3_RIF     = 5u,
        HWSTATE_TAG_L3_NH      = 6u,
        HWSTATE_TAG_L3_NHG     = 7u,
        HWSTATE_TAG_L3_NEIGH   = 8u,
        HWSTATE_TAG_L3_ROUTE   = 9u,
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

    SWSS_LOG_NOTICE("wb hwstate: saved %zu bytes (lags=%zu vrs=%zu rifs=%zu nhs=%zu nhgs=%zu neighs=%zu routes=%zu) -> %s",
            file.buf.size(), m_lagToTrunk.size(), m_vrToVrf.size(), m_rifToIntf.size(),
            m_nhToEgr.size(), m_nhgToEcmp.size(), m_neighborEgr.size(), m_routeEgr.size(),
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

    bool haveCore = false;
    int cpuEgr = -1, dropEgr = -1;

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

            default:
                // Forward-compatible: ignore unknown Phase 3+ TLV tags.
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

    SWSS_LOG_NOTICE("wb hwstate: restored lags=%zu lagMembers=%zu vrs=%zu rifs=%zu nhs=%zu nhgs=%zu nhgMembers=%zu neighs=%zu routes=%zu cpuEgr=%d dropEgr=%d",
            m_lagToTrunk.size(), m_lagMemberInfo.size(), m_vrToVrf.size(), m_rifToIntf.size(),
            m_nhToEgr.size(), m_nhgToEcmp.size(), m_nhgMemberToNh.size(),
            m_neighborEgr.size(), m_routeEgr.size(), cpuEgr, dropEgr);
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

    size_t recreated = 0;
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

        int netifId = 0;
        int rv = hwbcm_knet_netif_create(bcm, ifname, mac, &netifId);
        if (rv == 0)
        {
            m_hostifToNetif[hostifOid] = netifId;
            recreated++;
        }
        else
        {
            SWSS_LOG_WARN("wb knet: recreate(%s, port %d) failed: %d", ifname, bcm, rv);
        }
    }
    SWSS_LOG_NOTICE("wb knet: Case B recreated %zu KNET netdev(s)", recreated);
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
