/*
 * SwitchBCM56860HW_acl.inc.h — ACL members of SwitchBCM56860HW.
 *
 * #include this file INSIDE the class body of SwitchBCM56860HW (after the
 * A4 section). It switches access to public itself and leaves the class in
 * "protected:" state at the end. Implementation: SwitchBCM56860HW_acl.cpp.
 * Wiring: sai-td2p/notes/integration_acl.md.
 *
 * SDK-free: only plain ints cross into hwbcm_acl.h.
 */

        // --- ACL (SwitchBCM56860HW_acl.cpp) --------------------------------

        public:

            /* True for the bind attributes this file handles:
             * PORT/LAG/VLAN/ROUTER_INTERFACE *_INGRESS_ACL / *_EGRESS_ACL and
             * SWITCH INGRESS_ACL / EGRESS_ACL / PRE_INGRESS_ACL. */
            static bool hwAclIsBindAttr(_In_ sai_object_type_t ot, _In_ sai_attr_id_t id);

            /* Bind (oid = ACL table or ACL table group) / unbind (NULL oid)
             * at one bind point. Call from set() after the base class stored
             * the attribute. */
            sai_status_t hwAclBind(_In_ sai_object_type_t ot, _In_ sai_object_id_t oid, _In_ const sai_attribute_t *attr);

            /* Same for bind attributes given at create time (PORT/LAG/VLAN/RIF/SWITCH create). */
            sai_status_t hwAclBindOnCreate(_In_ sai_object_type_t ot, _In_ sai_object_id_t oid,
                    _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);

            /* A bind point object (port, LAG, VLAN, RIF) is being removed:
             * drop its bindings and reprogram the affected tables. */
            void hwAclObjectRemoved(_In_ sai_object_id_t oid);

            /* LAG membership changed: reprogram tables bound to the LAG (their
             * InPorts / OutPort expansion follows the member ports). */
            void hwAclLagMembersChanged(_In_ sai_object_id_t lag_oid);

            /* Mirror feature hook: session oid -> bcm mirror destination gport
             * (from bcm_mirror_destination_create), or -1 when the session is
             * gone. Entries that reference the session are reprogrammed. */
            void hwAclMirrorSessionUpdated(_In_ sai_object_id_t session_oid, _In_ int mirror_dest_gport);

            /* get() hook for SAI_OBJECT_TYPE_ACL_COUNTER: overwrites
             * SAI_ACL_COUNTER_ATTR_PACKETS / BYTES in attr_list with the
             * hardware values. Call after the base get() succeeded. */
            sai_status_t hwAclCounterGet(_In_ const std::string &serializedObjectId,
                    _In_ uint32_t attr_count, _Inout_ sai_attribute_t *attr_list);

        protected:

            /* create / set / remove handlers (dispatch in SwitchBCM56860HW.cpp) */
            sai_status_t hwCreateAclTable(_In_ sai_object_id_t table_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwRemoveAclTable(_In_ sai_object_id_t table_id);
            sai_status_t hwCreateAclEntry(_In_ sai_object_id_t entry_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwSetAclEntry(_In_ sai_object_id_t entry_id, _In_ const sai_attribute_t *attr);
            sai_status_t hwRemoveAclEntry(_In_ sai_object_id_t entry_id);
            sai_status_t hwCreateAclCounter(_In_ sai_object_id_t counter_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwSetAclCounter(_In_ sai_object_id_t counter_id, _In_ const sai_attribute_t *attr);
            sai_status_t hwRemoveAclCounter(_In_ sai_object_id_t counter_id);
            sai_status_t hwCreateAclRange(_In_ sai_object_id_t range_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwRemoveAclRange(_In_ sai_object_id_t range_id);
            sai_status_t hwCreateAclTableGroup(_In_ sai_object_id_t group_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwRemoveAclTableGroup(_In_ sai_object_id_t group_id);
            sai_status_t hwCreateAclTableGroupMember(_In_ sai_object_id_t member_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);
            sai_status_t hwRemoveAclTableGroupMember(_In_ sai_object_id_t member_id);

            /* Advertise per-stage action lists the TD2+ IFP/EFP really
             * support (replaces vslib's "every action" lists). */
            virtual sai_status_t set_acl_capabilities() override;

            /* --- state --- */

            struct AclHwEntry
            {
                int eid = -1;                              /* bcm field entry */
                int stat = -1;                             /* bcm field stat, -1 = none */
            };

            struct AclTableHw
            {
                int32_t saiStage = 0;                      /* sai_acl_stage_t */
                int hwStage = 0;                           /* HWBCM_ACL_STAGE_* */
                uint32_t quals = 0;                        /* group qset (HWBCM_ACL_Q_*) */
                int gid = -1;                              /* bcm field group */
                int prio = -1;                             /* bcm group priority (10..40) */
                bool matchesPorts = false;                 /* table has IN_PORT(S) / OUT_PORT(S) fields */
                std::set<sai_object_id_t> entries;
            };

            struct AclEntryHw
            {
                sai_object_id_t table = SAI_NULL_OBJECT_ID;
                sai_object_id_t counter = SAI_NULL_OBJECT_ID; /* counter the hw stats belong to */
                std::vector<AclHwEntry> hw;                /* one per binding-scope variant */
            };

            struct AclCounterHw
            {
                sai_object_id_t table = SAI_NULL_OBJECT_ID;
                uint64_t pktOffset = 0;                    /* value = offset + sum(live stats), mod 2^64 */
                uint64_t byteOffset = 0;
                std::set<sai_object_id_t> entries;
            };

            struct AclGroupHw
            {
                int32_t saiStage = 0;
                std::set<sai_object_id_t> members;
            };

            struct AclBinding
            {
                sai_object_type_t ot = SAI_OBJECT_TYPE_NULL;
                sai_object_id_t acl = SAI_NULL_OBJECT_ID;  /* ACL table or ACL table group */
            };

            struct AclScope
            {
                bool all = false;                          /* bound to the switch */
                std::set<int> ports;                       /* bcm ports (ports + LAG members) */
                std::set<int> vlans;                       /* VLAN / RIF bindings */
            };

            std::map<sai_object_id_t, AclTableHw> m_aclTables;
            std::map<sai_object_id_t, AclEntryHw> m_aclEntries;
            std::map<sai_object_id_t, AclCounterHw> m_aclCounters;
            std::map<sai_object_id_t, int> m_aclRanges;                    /* range oid -> bcm range id */
            std::map<sai_object_id_t, AclGroupHw> m_aclGroups;
            std::map<sai_object_id_t, std::pair<sai_object_id_t, sai_object_id_t>> m_aclGroupMembers; /* member -> (group, table) */
            std::map<std::pair<sai_object_id_t, int32_t>, AclBinding> m_aclBindings; /* (bind obj, sai stage) -> acl */
            std::map<int, std::set<int>> m_aclPrioUsed;                    /* hw stage -> group priorities in use */
            std::map<sai_object_id_t, int> m_aclMirrorDest;                /* mirror session -> bcm mirror gport */
            std::vector<int32_t> m_aclIngressActions;                     /* SAI_SWITCH_ATTR_ACL_STAGE_INGRESS */
            std::vector<int32_t> m_aclEgressActions;                      /* SAI_SWITCH_ATTR_ACL_STAGE_EGRESS */

            /* --- internals --- */
            bool aclStoredAttrs(_In_ sai_object_type_t ot, _In_ sai_object_id_t oid, _Out_ std::vector<sai_attribute_t> &attrs);
            std::set<sai_object_id_t> aclTablesOf(_In_ sai_object_id_t acl) const;
            AclScope aclScopeOf(_In_ sai_object_id_t table_id);
            void aclPortsOf(_In_ sai_object_id_t port_or_lag, _Inout_ std::vector<int> &out) const;
            sai_status_t aclRefreshTable(_In_ sai_object_id_t table_id);
            sai_status_t aclProgramEntry(_In_ sai_object_id_t entry_id);
            void aclRetire(_In_ sai_object_id_t counter_id, _In_ const AclHwEntry &h);
            sai_status_t aclBindChanged(_In_ sai_object_id_t oldAcl, _In_ sai_object_id_t newAcl);

        protected:
