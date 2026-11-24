/*
 * SwitchBCM56860HW_wb.inc.h — Warm Boot members of SwitchBCM56860HW.
 *
 * #include this file INSIDE the class body of SwitchBCM56860HW.
 * Implementation: SwitchBCM56860HW_wb.inc.cpp (included by SwitchBCM56860HW.cpp).
 */

        // --- Warm Boot (Phase 1 & Phase 2) --------------------------------

        protected:

            /* Derive sibling .scache / .hwstate file paths from SwitchConfig's
             * warm-boot file (fallback /var/warmboot/sai-warmboot.bin). */
            std::string warmBootBasePath(_In_ bool forWrite) const;
            std::string warmBootScachePath(_In_ bool forWrite) const;
            std::string warmBootHwStatePath(_In_ bool forWrite) const;

            /* Sync SDK Level 2 scache + write versioned .hwstate atomically. */
            sai_status_t hwWarmSync();

            /* Validate .hwstate header/CRC32/config.bcm CRC before warm attach. */
            bool hwStateValidateFile(_In_ const std::string &path) const;

            /* Persist / restore Phase 2 SW+HW handle tables in .hwstate. */
            sai_status_t hwStateSave(_In_ const std::string &path) const;
            sai_status_t hwStateLoad(_In_ const std::string &path);

            /* Warm-boot KNET recovery: Case A attaches to surviving netifs/filters
             * with zero mutations; Case B (kexec) recreates netifs from m_objectHash. */
            sai_status_t hwWarmRecoverKnet();

            /* Rebuild non-persisted LAG / L3 NHG-member bookkeeping from m_objectHash. */
            void hwWarmRebuildLagAndL3FromHash();

            /* SAI_OBJECT_TYPE_BRIDGE_PORT set handler (FDB learning mode freeze/unfreeze). */
            sai_status_t hwSetBridgePort(_In_ sai_object_id_t bp_id, _In_ const sai_attribute_t *attr);

            /* Accessors for anonymous-namespace shared L3 egresses in _l3.cpp. */
            void hwL3GetSharedEgress(_Out_ int &cpuEgr, _Out_ int &dropEgr) const;
            void hwL3RestoreSharedEgress(_In_ int cpuEgr, _In_ int dropEgr);

            bool m_restartWarm = false;
            bool m_uninitDataPlaneOnRemoval = false;
            bool m_warmSynced = false;
            bool m_isWarmBoot = false;
