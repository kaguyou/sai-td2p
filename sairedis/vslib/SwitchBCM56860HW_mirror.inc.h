/*
 * SwitchBCM56860HW_mirror.inc.h — port mirroring (SPAN / RSPAN / ERSPAN)
 * members of SwitchBCM56860HW.
 *
 * #include this file INSIDE the class body of SwitchBCM56860HW (last line of
 * the class, after SwitchBCM56860HW_qos.inc.h). It switches access itself and
 * leaves the class in "protected:" state at the end.
 * Implementation: SwitchBCM56860HW_mirror.cpp.
 * Wiring: sai-td2p/notes/integration_mirror.md.
 *
 * SDK-free: only plain C++ / SAI types here (no hwbcm_*.h include).
 */

        // --- Mirror (SwitchBCM56860HW_mirror.cpp) --------------------------

        public:

            /* Plain snapshot of the hardware-programmed mirror session fields
             * (SDK/hwbcm-free). */
            struct MirrorCfgSnapshot
            {
                int type = 0;               /* HWBCM_MIRROR_TYPE_* */
                int port = -1;              /* bcm logical port, -1 if trunk */
                int trunkId = -1;           /* bcm trunk id, -1 if port */
                int tc = -1;                /* 0..15, or -1 if unset */
                int vlanValid = 0;
                uint16_t vlanTpid = 0x8100;
                uint16_t vlanId = 0;
                uint8_t vlanPri = 0;
                uint8_t vlanCfi = 0;
                uint32_t srcIp = 0;         /* host byte order */
                uint32_t dstIp = 0;         /* host byte order */
                uint8_t srcMac[6] = {0, 0, 0, 0, 0, 0};
                uint8_t dstMac[6] = {0, 0, 0, 0, 0, 0};
                uint8_t tos = 0;
                uint8_t ttl = 255;
                uint16_t greProto = 0x88be;
                int payloadUntagged = 0;

                bool sameAs(const MirrorCfgSnapshot &o) const;
            };

            /* Port attributes owned by mirror: hwSetPort forwards
             * SAI_PORT_ATTR_INGRESS_MIRROR_SESSION and
             * SAI_PORT_ATTR_EGRESS_MIRROR_SESSION to mirrorHwSetPort. */
            static bool mirrorIsPortAttr(_In_ sai_attr_id_t id);

            sai_status_t mirrorHwSetPort(
                    _In_ sai_object_id_t port_id,
                    _In_ const sai_attribute_t *attr);

            /* Called from hwRemovePort (before unmapPort) to detach any port
             * mirroring bindings on the port being removed. */
            void mirrorOnPortRemoved(_In_ sai_object_id_t port_id);

        protected:

            /* create / set / remove dispatch handlers (SwitchBCM56860HW.cpp) */
            sai_status_t hwCreateMirrorSession(
                    _In_ sai_object_id_t session_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);

            sai_status_t hwSetMirrorSession(
                    _In_ sai_object_id_t session_id,
                    _In_ const sai_attribute_t *attr);

            sai_status_t hwRemoveMirrorSession(
                    _In_ sai_object_id_t session_id);

            /* --- state --- */

            struct MirrorSessionHw
            {
                int destGport = -1;         /* bcm_gport_t (BCM_GPORT_MIRROR_*) */
                MirrorCfgSnapshot cfg;
            };

            /* session oid -> hardware mirror destination */
            std::map<sai_object_id_t, MirrorSessionHw> m_mirrorSessions;

            /* source port oid -> currently attached mirror session oids */
            std::map<sai_object_id_t, std::set<sai_object_id_t>> m_portIngMirrors;
            std::map<sai_object_id_t, std::set<sai_object_id_t>> m_portEgrMirrors;

            /* --- internals --- */
            sai_status_t mirrorReadConfig(
                    _In_ sai_object_id_t session_id,
                    _Out_ MirrorCfgSnapshot &out);

        protected:
