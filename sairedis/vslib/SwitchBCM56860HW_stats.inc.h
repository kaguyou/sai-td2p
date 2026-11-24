// SwitchBCM56860HW_stats.inc.h — member declarations for
// SwitchBCM56860HW_stats.cpp (queue / priority group / buffer pool / extra
// port statistics and queryStatsCapability).
//
// #include this INSIDE the class body of SwitchBCM56860HW (SwitchBCM56860HW.h),
// as the last thing before the closing "};". It sets its own access
// specifiers and leaves the class in "protected:".

        // --- statistics (SwitchBCM56860HW_stats.cpp) ----------------------

        public:

            /* Advertise exactly what the ASIC provides for PORT, QUEUE,
             * INGRESS_PRIORITY_GROUP and BUFFER_POOL. syncd's FlexCounter
             * filters every requested counter through this list
             * (sonic-sairedis/syncd/FlexCounter.cpp:1497, 1504-1557), so a
             * counter missing here is never polled and shows as N/A.
             * Other object types: base class. */
            virtual sai_status_t queryStatsCapability(
                    _In_ sai_object_id_t switchId,
                    _In_ sai_object_type_t objectType,
                    _Inout_ sai_stat_capability_list_t *stats_capability) override;

        protected:

            /* Called first from getStatsExt. Returns true when the object
             * type is QUEUE / INGRESS_PRIORITY_GROUP / BUFFER_POOL and the
             * hardware answered (status holds the SAI result); false = keep
             * the vslib behaviour (HW not ready, unit-test "set counters"
             * call, other object type, object not bound to a HW port). */
            bool hwGetObjectStats(
                    _In_ sai_object_type_t object_type,
                    _In_ sai_object_id_t object_id,
                    _In_ uint32_t number_of_counters,
                    _In_ const sai_stat_id_t *counter_ids,
                    _In_ sai_stats_mode_t mode,
                    _Out_ uint64_t *counters,
                    _Out_ sai_status_t &status);

            /* Port counters that are not in the bcm_stat_get table of
             * hwbcm_core.c (MMU / WRED drops, EtherStats >1518 buckets, IP
             * counters). Call after hwbcm_port_stat_multi_get succeeded: it
             * overwrites only the ids it owns. */
            sai_status_t hwPortStatsOverlay(
                    _In_ int bcm_port,
                    _In_ uint32_t number_of_counters,
                    _In_ const sai_stat_id_t *counter_ids,
                    _In_ sai_stats_mode_t mode,
                    _Inout_ uint64_t *counters);

            /* SAI_PORT_STAT_* -> HWBCM_PSTAT_* of the existing table
             * (0 = HWBCM_PSTAT_NONE). Defined in SwitchBCM56860HW.cpp as a
             * wrapper of its static hwPstatOf() (see notes/integration_stats.md). */
            static int hwPortPstatOf(_In_ sai_stat_id_t id);

            /* QUEUE oid -> bcm port (CPU = hwbcm_stats_cpu_port()), hardware
             * cos (SAI MC index 10..19 -> 0..9), multicast flag. */
            bool hwStatsQueueRef(_In_ sai_object_id_t queue_oid, _Out_ int &bcm_port, _Out_ int &cos, _Out_ bool &mc);

            /* INGRESS_PRIORITY_GROUP oid -> bcm port, PG index. */
            bool hwStatsPgRef(_In_ sai_object_id_t pg_oid, _Out_ int &bcm_port, _Out_ int &pg);

            /* BUFFER_POOL oid -> direction and one PG (ingress) / queue
             * (egress) whose buffer profile uses the pool; false if no
             * such user exists (the pool holds nothing). */
            bool hwStatsPoolRef(_In_ sai_object_id_t pool_oid, _Out_ bool &egress, _Out_ int &bcm_port, _Out_ int &cos, _Out_ bool &mc);

            /* pool oid -> PG / queue oid last used as its representative */
            std::map<sai_object_id_t, sai_object_id_t> m_statPoolUser;
