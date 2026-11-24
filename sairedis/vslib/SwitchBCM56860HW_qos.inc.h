/*
 * SwitchBCM56860HW_qos.inc.h — QoS / buffer members of SwitchBCM56860HW.
 *
 * #include this file INSIDE the class body of SwitchBCM56860HW (it is not a
 * standalone header: no include guard, no includes, it opens its own access
 * sections). Implementation: SwitchBCM56860HW_qos.cpp. Integration notes:
 * sai-td2p/notes/integration_qos.md.
 *
 * Uses only SAI types and std containers already pulled in by
 * SwitchBCM56860HW.h (<map>, <set>, <string>, <vector>).
 */

        // --- QoS / buffers (A6) -------------------------------------------

        public:

            /* Hardware location of a SAI QUEUE object (built by qosBuildHwMap /
             * qosOnPortMapped from SAI_PORT_ATTR_QOS_QUEUE_LIST + the queue's
             * TYPE / INDEX). For the statistics code:
             *  - front port: bcmPort = logical port, cos = 0..9 (queue index
             *    within its type), mcast = MC queue. The bcm_cosq API reaches
             *    cos 0..7 with (bcmPort, cos) in the SDK's non-ETS mode; there
             *    are no queue gports (see notes/integration_qos.md).
             *  - CPU port: cpu = true, bcmPort = CMIC port 0, cos = 0..31. */
            struct QosQueueHw
            {
                sai_object_id_t port = SAI_NULL_OBJECT_ID;
                int bcmPort = -1;
                bool cpu = false;
                bool mcast = false;
                int cos = -1;
            };

            /* Hardware location of a SAI INGRESS_PRIORITY_GROUP: bcm port + PG 0..7. */
            struct QosPgHw
            {
                sai_object_id_t port = SAI_NULL_OBJECT_ID;
                int bcmPort = -1;
                int pg = -1;
            };

            /* Hardware node of a SAI SCHEDULER_GROUP in the vslib 13-node tree:
             * level 0 = port root, 1 = L0 node (index 0/1), 2 = L1 node
             * (index = cos 0..9, parent of UC queue cos and MC queue cos). */
            struct QosSgHw
            {
                sai_object_id_t port = SAI_NULL_OBJECT_ID;
                int bcmPort = -1;
                int level = -1;
                int index = -1;
            };

            bool qosQueueHwInfo(_In_ sai_object_id_t queue_id, _Out_ QosQueueHw &out) const;
            bool qosPgHwInfo(_In_ sai_object_id_t pg_id, _Out_ QosPgHw &out) const;
            bool qosSgHwInfo(_In_ sai_object_id_t sg_id, _Out_ QosSgHw &out) const;

        protected:

            // --- entry points (wired by A2, see integration_qos.md) ---------

            /* Build the queue / scheduler group / PG oid -> HW maps for every
             * port in m_port_list (+ CPU queues). Call at the end of
             * initialize_default_objects and warm_boot_initialize_objects. */
            sai_status_t qosBuildHwMap();

            /* A port was created (after mapPort in hwCreatePort) / is about to
             * be removed (before unmapPort in hwRemovePort). */
            void qosOnPortMapped(_In_ sai_object_id_t port_id);
            void qosOnPortRemoved(_In_ sai_object_id_t port_id);

            /* Port attributes owned by QoS: hwSetPort forwards them to qosHwSetPort. */
            static bool qosIsPortAttr(_In_ sai_attr_id_t id);
            sai_status_t qosHwSetPort(_In_ sai_object_id_t port_id, _In_ const sai_attribute_t *attr);

            /* Switch-level QoS maps / default TC (fallback for ports without a
             * port-level map): hwSetSwitch forwards them to qosHwSetSwitch. */
            static bool qosIsSwitchAttr(_In_ sai_attr_id_t id);
            sai_status_t qosHwSetSwitch(_In_ const sai_attribute_t *attr);

            sai_status_t hwCreateQosMap(_In_ sai_object_id_t map_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwSetQosMap(_In_ sai_object_id_t map_id, _In_ const sai_attribute_t *attr);
            sai_status_t hwRemoveQosMap(_In_ sai_object_id_t map_id);

            sai_status_t hwCreateScheduler(_In_ sai_object_id_t sched_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwSetScheduler(_In_ sai_object_id_t sched_id, _In_ const sai_attribute_t *attr);
            sai_status_t hwRemoveScheduler(_In_ sai_object_id_t sched_id);

            sai_status_t hwCreateWred(_In_ sai_object_id_t wred_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwSetWred(_In_ sai_object_id_t wred_id, _In_ const sai_attribute_t *attr);
            sai_status_t hwRemoveWred(_In_ sai_object_id_t wred_id);

            sai_status_t hwCreateBufferPool(_In_ sai_object_id_t pool_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwSetBufferPool(_In_ sai_object_id_t pool_id, _In_ const sai_attribute_t *attr);
            sai_status_t hwRemoveBufferPool(_In_ sai_object_id_t pool_id);

            sai_status_t hwCreateBufferProfile(_In_ sai_object_id_t profile_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwSetBufferProfile(_In_ sai_object_id_t profile_id, _In_ const sai_attribute_t *attr);
            sai_status_t hwRemoveBufferProfile(_In_ sai_object_id_t profile_id);

            sai_status_t hwSetQueue(_In_ sai_object_id_t queue_id, _In_ const sai_attribute_t *attr);
            sai_status_t hwSetSchedulerGroup(_In_ sai_object_id_t sg_id, _In_ const sai_attribute_t *attr);
            sai_status_t hwSetIngressPriorityGroup(_In_ sai_object_id_t pg_id, _In_ const sai_attribute_t *attr);

            // --- internals (SwitchBCM56860HW_qos.cpp) -----------------------

            enum QosMapSlot
            {
                QOS_SLOT_DSCP_TO_TC = 0,
                QOS_SLOT_DSCP_TO_COLOR,
                QOS_SLOT_DOT1P_TO_TC,
                QOS_SLOT_DOT1P_TO_COLOR,
                QOS_SLOT_TC_TO_QUEUE,
                QOS_SLOT_TC_TO_PG,
                QOS_SLOT_PFC_TO_QUEUE,
                QOS_SLOT_PFC_TO_PG,
                QOS_SLOT_TC_COLOR_TO_DSCP,
                QOS_SLOT_COUNT
            };

            struct QosPortState
            {
                sai_object_id_t maps[QOS_SLOT_COUNT] = {};
                int defaultTc = -1;                    /* -1: not set */
                bool pfcSeparate = false;              /* PRIORITY_FLOW_CONTROL_MODE */
                uint8_t pfcCombined = 0;
                uint8_t pfcTx = 0;
                uint8_t pfcRx = 0;
                bool pfcTouched = false;
                sai_object_id_t scheduler = SAI_NULL_OBJECT_ID;
                std::string hwIngDscpKey;              /* keys into m_qosHwMaps, "" = none */
                std::string hwIngDot1pKey;
                std::string hwEgrDscpKey;
            };

            struct QosHwMap
            {
                int id = -1;                           /* bcm qos map id */
                int refs = 0;
            };

            /* Decoded SAI profiles (read back from the object hash). */
            struct QosSched
            {
                int mode = 1;                          /* HWBCM_QOS_SCHED_* */
                int weight = 1;
                bool pps = false;
                uint64_t minRate = 0, maxRate = 0, minBurst = 0, maxBurst = 0;
            };

            struct QosBuffer
            {
                int pool = -1;
                uint32_t minBytes = 0;
                bool dynamic = true;
                int alphaExp = 0;
                uint32_t staticBytes = 0;
                uint32_t xoffBytes = 0, xonBytes = 0, xonOffsetBytes = 0;
            };

            struct QosPool
            {
                int32_t type = 0;                      /* sai_buffer_pool_type_t */
                int ingressSp = -1;
                int egressSp = -1;
                bool dynamic = true;                   /* THRESHOLD_MODE */
                uint64_t size = 0;                     /* recorded only */
            };

            void qosMapPort(_In_ sai_object_id_t port_id);
            void qosUnmapPort(_In_ sai_object_id_t port_id);

            QosPortState &qosPortState(_In_ sai_object_id_t port_id);
            sai_object_id_t qosEffectiveMap(_In_ const QosPortState &ps, _In_ int slot) const;
            bool qosMapEntries(_In_ sai_object_id_t map_id, _Out_ std::vector<sai_qos_map_t> &out);
            static int qosSlotOfPortAttr(_In_ sai_attr_id_t id);
            static int qosSlotOfSwitchAttr(_In_ sai_attr_id_t id);

            sai_status_t qosApplyPortIngressMaps(_In_ sai_object_id_t port_id);
            sai_status_t qosApplyPortEgressMap(_In_ sai_object_id_t port_id);
            sai_status_t qosApplyPortTcToQueue(_In_ sai_object_id_t port_id);
            sai_status_t qosApplyPortTcToPg(_In_ sai_object_id_t port_id);
            sai_status_t qosApplyPortPfc(_In_ sai_object_id_t port_id);
            sai_status_t qosApplyPortDefaultTc(_In_ sai_object_id_t port_id);
            sai_status_t qosApplyPortShaper(_In_ sai_object_id_t port_id);
            sai_status_t qosApplyPortAll(_In_ sai_object_id_t port_id);
            sai_status_t qosApplyPortSlot(_In_ sai_object_id_t port_id, _In_ int slot);

            /* content-keyed cache of bcm qos maps (DSCP_TABLE etc. are shared
             * profiles with few ids; ports with equal content share one id) */
            int qosHwMapAcquire(_In_ const std::string &key);
            void qosHwMapRelease(_In_ const std::string &key);

            bool qosDecodeScheduler(_In_ sai_object_id_t sched_id, _Out_ QosSched &out);
            bool qosDecodeBufferProfile(_In_ sai_object_id_t profile_id, _In_ bool ingress, _Out_ QosBuffer &out);

            sai_status_t qosApplySgScheduler(_In_ sai_object_id_t sg_id);
            sai_status_t qosApplyQueueScheduler(_In_ sai_object_id_t queue_id);
            sai_status_t qosApplyQueueWred(_In_ sai_object_id_t queue_id);
            sai_status_t qosApplyQueueBuffer(_In_ sai_object_id_t queue_id);
            sai_status_t qosApplyPgBuffer(_In_ sai_object_id_t pg_id);
            void qosRetryPendingPgs(_In_ sai_object_id_t port_id);

            // --- state ------------------------------------------------------

            std::map<sai_object_id_t, QosQueueHw> m_qosQueueHw;      /* queue oid -> HW */
            std::map<sai_object_id_t, QosPgHw> m_qosPgHw;            /* PG oid -> HW */
            std::map<sai_object_id_t, QosSgHw> m_qosSgHw;            /* scheduler group oid -> HW */

            std::map<sai_object_id_t, QosPortState> m_qosPort;       /* port oid -> bindings */
            sai_object_id_t m_qosSwitchMaps[QOS_SLOT_COUNT] = {};    /* switch-level maps */
            int m_qosSwitchDefaultTc = -1;

            std::map<sai_object_id_t, int32_t> m_qosMapType;         /* qos map oid -> sai_qos_map_type_t */
            std::map<std::string, QosHwMap> m_qosHwMaps;             /* content key -> bcm map */

            std::set<sai_object_id_t> m_qosSchedulers;               /* known scheduler oids */
            std::map<sai_object_id_t, sai_object_id_t> m_qosSgSched;    /* sg -> scheduler */
            std::map<sai_object_id_t, sai_object_id_t> m_qosQueueSched; /* queue -> scheduler */

            std::set<sai_object_id_t> m_qosWreds;                    /* known wred oids */
            std::map<sai_object_id_t, sai_object_id_t> m_qosQueueWred;  /* queue -> wred */

            std::map<sai_object_id_t, QosPool> m_qosPools;           /* buffer pool oid -> SP */
            bool m_qosIngressSpUsed[4] = {false, false, false, false};
            bool m_qosEgressSpUsed[4] = {false, false, false, false};
            std::set<sai_object_id_t> m_qosProfiles;                 /* known buffer profile oids */
            std::map<sai_object_id_t, sai_object_id_t> m_qosPgProfile;    /* PG -> buffer profile */
            std::map<sai_object_id_t, sai_object_id_t> m_qosQueueProfile; /* queue -> buffer profile */
            std::set<sai_object_id_t> m_qosPgPending;                /* PGs whose min/headroom await a TC->PG mapping */

        protected:
