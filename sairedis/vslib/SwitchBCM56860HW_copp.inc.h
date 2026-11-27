/*
 * SwitchBCM56860HW_copp.inc.h — CoPP members of SwitchBCM56860HW.
 *
 * #include this file INSIDE the class body of SwitchBCM56860HW (last thing
 * before the closing "};"). It switches access to public:/protected: itself
 * and leaves it at protected:. Implementation: SwitchBCM56860HW_copp.cpp.
 * Uses only types SwitchBCM56860HW.h already has (<map>, <string>, SAI).
 */

        // --- CoPP (POLICER, HOSTIF_TRAP_GROUP, hostif trap policing) -----

        public:

            /* Dispatch targets (create/set/remove of SwitchBCM56860HW.cpp). */
            sai_status_t hwCreatePolicer(_In_ sai_object_id_t policer_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwSetPolicer(_In_ sai_object_id_t policer_id, _In_ const sai_attribute_t *attr);
            sai_status_t hwRemovePolicer(_In_ sai_object_id_t policer_id);

            sai_status_t hwCreateHostifTrapGroup(_In_ sai_object_id_t group_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwSetHostifTrapGroup(_In_ sai_object_id_t group_id, _In_ const sai_attribute_t *attr);
            sai_status_t hwRemoveHostifTrapGroup(_In_ sai_object_id_t group_id);

            /* Hostif trap hooks. OnTrapCreate runs after hwCreateHostifTrap
             * (which keeps doing the CPU steering); OnTrapSet re-steers on a
             * PACKET_ACTION / TRAP_GROUP change and re-applies the policing;
             * OnTrapRemove drops the trap's FP entries. Propagates real HW
             * errors while keeping unsupported trap kinds as SUCCESS. */
            sai_status_t hwCoppOnTrapCreate(_In_ sai_object_id_t trap_id);
            sai_status_t hwCoppOnTrapSet(_In_ sai_object_id_t trap_id, _In_ const sai_attribute_t *attr);
            sai_status_t hwCoppOnTrapRemove(_In_ sai_object_id_t trap_id);

            /* After a successful hwCreateRoute / hwSetRoute: an ip2me route
             * (NEXT_HOP_ID = CPU port) tells CoPP the CPU egress that "to me"
             * packets resolve to (m_routeEgr), which the IP2ME/BGP/SSH/SNMP/
             * BFD/LDP classifiers match on. */
            void hwCoppOnRoute(_In_ const std::string &serializedRoute);

            /* Re-install every CoPP entry (after SAI_SWITCH_ATTR_SRC_MAC_ADDRESS
             * changed: the ARP reply classifier matches the switch MAC). */
            void hwCoppRefresh();

            /* SAI_OBJECT_TYPE_COUNTER stats for a generic counter bound to a
             * hostif trap (SAI_HOSTIF_TRAP_ATTR_COUNTER_ID, SONiC trap flow
             * counters). PACKETS/BYTES = packets that reached the CPU path
             * (meter colour not red). Returns false if counter_id is not bound
             * to a policed trap (caller keeps the vslib behaviour). */
            bool hwCoppCounterStats(
                    _In_ sai_object_id_t counter_id,
                    _In_ uint32_t number_of_counters,
                    _In_ const sai_stat_id_t *counter_ids,
                    _In_ sai_stats_mode_t mode,
                    _Out_ uint64_t *counters);

        protected:

            struct CoppPolicerParams
            {
                int packets = 1;        /* SAI_METER_TYPE_PACKETS */
                int mode = 0;           /* HWBCM_COPP_POLICER_* */
                int colorBlind = 0;
                uint64_t cir = 0;
                uint64_t cbs = 0;
                uint64_t pir = 0;
                uint64_t pbs = 0;
                int dropGreen = 0;      /* colour action == DROP */
                int dropYellow = 0;
                int dropRed = 0;
            };

            /* Policer attributes from the object hash. False if the SAI mode
             * or meter type has no TD2+ equivalent. */
            bool coppReadPolicer(_In_ sai_object_id_t policer_id, _Out_ CoppPolicerParams &p);

            /* bcm policer of a SAI policer, created on first use by a trap
             * group. -1 on failure (with optional hwbcm rv in *rvOut). */
            int coppBcmPolicer(_In_ sai_object_id_t policer_id, _Out_opt_ int *rvOut = nullptr);

            /* Trap group of a trap (SAI default trap group when unset). */
            sai_object_id_t coppTrapGroupOf(_In_ sai_object_id_t trap_id);

            /* (Re)program / forget the FP entries of one trap from its stored
             * attributes. */
            sai_status_t coppApplyTrap(_In_ sai_object_id_t trap_id);
            void coppForgetTrap(_In_ sai_object_id_t trap_id);

            /* Every trap whose group == group_id / whose group uses policer_id. */
            sai_status_t coppApplyGroup(_In_ sai_object_id_t group_id);
            sai_status_t coppApplyPolicerUsers(_In_ sai_object_id_t policer_id);

            /* All hostif trap oids in the object hash. */
            std::vector<sai_object_id_t> coppAllTraps();

            std::map<sai_object_id_t, int> m_coppTrapHandle;   /* hostif trap oid -> hwbcm_copp handle */
            std::map<sai_object_id_t, int> m_coppPolicer;      /* SAI policer oid -> bcm policer id */
            int m_coppLocalEgress = -1;                        /* ip2me CPU egress last reported */
