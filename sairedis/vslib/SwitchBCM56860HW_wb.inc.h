/*
 * SwitchBCM56860HW_wb.inc.h — Warm Boot members of SwitchBCM56860HW.
 *
 * #include this file INSIDE the class body of SwitchBCM56860HW.
 * Implementation: SwitchBCM56860HW_wb.inc.cpp (included by SwitchBCM56860HW.cpp).
 */

        // --- Warm Boot (Phase 1, Phase 2 & Phase 3) -----------------------

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

            /* Restore base switch OIDs (default VLAN, 1Q bridge, ECMP/LAG hash) from m_objectHash. */
            void hwWarmRestoreSwitchOids();

            /* Clear stale function-pointer callbacks restored from sai-warmboot.bin
             * before warm_boot_initialize_objects() runs hardware init or fallback
             * replay; VirtualSwitchSaiInterface::create() installs the new process's
             * live callbacks afterwards via SwitchBCM56860HW::create(SWITCH). */
            void hwWarmScrubStaleCallbacks();

            /* Rebuild non-persisted Phase 3 bookkeeping from m_objectHash (zero HW writes). */
            void hwWarmRebuildVxlanFromHash();
            void hwWarmRebuildAclFromHash();
            void hwWarmRebuildMirrorFromHash();
            void hwWarmRebuildFdbInfoFromHash();

            /* SAI_OBJECT_TYPE_BRIDGE_PORT set handler (FDB learning mode freeze/unfreeze). */
            sai_status_t hwSetBridgePort(_In_ sai_object_id_t bp_id, _In_ const sai_attribute_t *attr);

            /* Warm-boot fallback helpers: tear down a partially recovered warm
             * attach cleanly, then replay every restored SAI object from
             * m_objectHash into a freshly cold-initialized ASIC. */
            void hwTeardownForColdFallback();
            sai_status_t hwColdFallbackReplay();

            /* Accessors for anonymous-namespace shared L3 egresses in _l3.cpp. */
            void hwL3GetSharedEgress(_Out_ int &cpuEgr, _Out_ int &dropEgr) const;
            void hwL3RestoreSharedEgress(_In_ int cpuEgr, _In_ int dropEgr);

            bool m_restartWarm = false;
            bool m_uninitDataPlaneOnRemoval = false;
            bool m_warmSynced = false;
            /* Phase 4: PRE_SHUTDOWN=true (warm-reboot/fast-reboot only, a
             * syncd-only warm restart never sends it) always forces kexec-mode
             * warm deinit (KNET DMA aborted), even if HWBCM_WB_KEEP_KNET=1. */
            bool m_preShutdown = false;

            /* kexec/dma-reset warm deinit wanted: always true for PRE_SHUTDOWN,
             * HWBCM_WB_KEXEC=1, or <warmboot dir>/kexec-pending; also true by
             * default for plain syncd warm restarts unless HWBCM_WB_KEEP_KNET=1
             * opts back into live KNET rings. */
            bool warmDeinitForKexec() const;
            bool m_isWarmBoot = false;
            bool m_inWarmBootInit = false;
            bool m_pendingVxlanOperNotify = false;
