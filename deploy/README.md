# sai-td2p deploy: build, install, roll back

Scripts to build the Trident2+ SAI (`libsaivs.so.0.0.0` = sonic-sairedis vslib + hwbcm + static
OpenBCM 6.5.27) in the `saibuild` podman container and to make it the `libsai.so.1` of the
`syncd` container on the Edgecore AS5812-54T running SONiC 202505.

All scripts run **on the PC**. Nothing here starts syncd; the start commands are printed at the end.

| file | what |
|---|---|
| `build_wb.sh` | **default going forward**: same as build.sh but against the warm-boot SDK `openbcm-src/sdk-6.5.27-wb` (`sdk_*_wb.txt`, `sdk_link_list_wb.txt`, `libopenbcm_td2p_wb.a`) plus warm-boot symbol checks; writes the tracked `nm-*.txt` |
| `build.sh` | cold-only variant against `openbcm-src/sdk-6.5.27`: compile in `saibuild`, explicit `g++ -shared` link with the full SDK group, nm/ldd checks (nm lists go to untracked `nm-*-cold.txt`) |
| `install.sh` | scp + install on the switch (device node, load test, backup, symlink, lanemap.ini, sai.profile) |
| `rollback.sh` | restore the vendor `libsai.so.1` and `sai.profile.vendor` |
| `gen_lanemap.py` | `port_config.ini` -> `lanemap.ini` (also run on the switch by install.sh) |
| `sai.profile.td2p.extra` | the extra profile keys appended to the vendor sai.profile |
| `common.sh` | shared ssh/scp helpers (target probing IPv4 -> IPv6 link-local) |
| `remote_install.sh`, `remote_rollback.sh` | the switch-side halves, streamed over ssh (`bash -s`) |
| `platform/warm-reboot_plugin` | kexec DMA guard, installed by `install.sh` as `/usr/share/sonic/device/x86_64-accton_as5812_54t-r0/{warm,fast}-reboot_plugin` (see "SONiC warm-reboot" below) |
| `ref/` | read-only copies fetched from the switch: `port_config.ini`, `sai.profile`, `td2-as5812t-72x10G.config.bcm`, `syncd_init_common.sh`, `syncd_container_start.sh`, `switch_host_scripts.txt` (host syncd.sh, opennsl-modules, supervisor conf), and `lanemap.ini.expected` (generator output) |
| `build/` | outputs: `libsaivs.so.0.0.0` (+ `libsaivs.so`, `libsaivs.so.0` symlinks), `.sha256`, `.variant` (which script/SDK built it: both scripts write the same `libsaivs.so.0.0.0`, last one wins), `libopenbcm_td2p{,_wb}.a` (merged SDK), `nm-undefined.txt`, `nm-defined.txt` (from build_wb.sh), `dry-run.log` |

Both build scripts take `SAI_TD2P_DIR` (default: this checkout), `SAIREDIS_DIR` (default `~/homelab/sonic-sairedis`), `SDK_DIR` and `LINK_LIST` from the environment (PC paths under `~/homelab`, mounted as `/w` in `saibuild`). They may share one sonic-sairedis tree: a stamp (`vslib/hwbcm/.sdk_cflags_stamp`) forces the hwbcm objects to be recompiled when the SDK flags change.

Connection defaults (override via env): `SWITCH_V4=192.168.0.2`, `SWITCH_V6=fe80::aa2b:b5ff:fe3a:3e8e%enp2s0`,
`SWITCH_USER=admin`, `SWITCH_PASS`. If both fail: `ip -br addr show enp2s0` and `nmcli con up sonic-mgmt`
(NetworkManager tends to fall back to the DHCP profile `wired-dhcp`, which drops all addresses after 45 s).

## Order of operations

### 1. Build

```
deploy/build.sh                 # -j12, meta + vslib compile + link + checks
deploy/build.sh --dry-run       # make -n for the compile, print the link command, build nothing
deploy/build.sh --check-only    # re-run the checks on vslib/.libs/libsaivs.so.0.0.0 (copied to build/)
deploy/build.sh --force-archive # rebuild the merged SDK archive even if it looks current
C_STD=-std=gnu11 deploy/build.sh   # override the extra C flag (default -std=gnu99)
```

What it runs inside the container:

```
ar -M   # CREATE build/libopenbcm_td2p.a; ADDLIB <each .a of as5812-bdeshim/sdk_link_list.txt>; SAVE
make -C /w/sonic-sairedis/meta -j12
make -C /w/sonic-sairedis/vslib -j12 libsaivs.la CFLAGS="-g -O2 -std=gnu99" \
     LIBS="-Wl,<sdk>/platform_defines.o -Wl,/w/sai-td2p/deploy/build/libopenbcm_td2p.a \
           -Wl,-lm -Wl,-lrt -Wl,-ldl -Wl,-lpthread libSaiVS.a -lpthread -lrt -lm -ldl"
cp vslib/.libs/libsaivs.so.0.0.0 deploy/build/libsaivs.so.0.0.0
```

`<sdk>` = `openbcm-src/sdk-6.5.27/build/unix-user/x86-smp_generic_64-2_6`; the `.a` list and order come
from `as5812-bdeshim/sdk_link_list.txt` (100 archives, all checked for existence first). `CFLAGS` on the
make command line only adds `-std=gnu99` to the hwbcm C objects: automake's per-target rule is
`$(libSaiVS_a_CFLAGS) $(CFLAGS)` and A2's `libSaiVS_a_CFLAGS` carries the SDK defines/includes.

Why this shape (all three points verified on real builds):

* `Makefile.am` never defines `libsaivs_la_LDFLAGS`, so only `LIBS=` can be overridden from the make
  command line; it lands after `libsaivs_la_LIBADD`, i.e. after `libSaiVS.a`.
* libtool moves every `-Wl,*` flag behind all libraries, so `-Wl,--start-group ... --end-group` would be an
  empty group. The SDK archives have circular dependencies, hence one merged archive: GNU ld rescans a
  single archive until closure, which is group semantics.
* libtool silently drops a bare `/path/lib.a` from `LIBS` when building a shared library (first real build:
  0 `bcm_*` defined). Prefixed as `-Wl,/path/lib.a` it is passed through untouched. Because those `-Wl,`
  items end up after libtool's own `-lm`, the SDK's libm references stayed unbound (unversioned
  `exp/log/pow/sqrt`, no `libm.so.6` in NEEDED) until `-Wl,-lm ...` was appended after the archive.
* The `tests` program in `vslib/Makefile.am` would need the SDK too; it is never built.

Checks performed (script exits 1 on FAIL, the .so is still left in place):

* no strong undefined `bcm_* soc_* sal_* bde_* hwbcm_* var_set ...` symbols (SDK really linked in)
* every other undefined symbol is versioned glibc/libstdc++, C++ mangled (swsscommon, saimeta,
  resolved by the syncd process), hiredis, `sai_metadata_*` or libnl `rtnl_*`; anything else is a WARN
* max version tags: `GLIBC_` <= 2.36, `GLIBCXX_` <= 3.4.30, `CXXABI_` <= 1.3.13 (switch = Debian 12)
* exported: `sai_api_initialize`, `sai_api_query`, `sai_api_uninitialize`, `sai_log_set`;
  `bcm_*` defined count > 0; `hwbcm_*` and `bde_create` defined (WARN if not)
* `ldd` has no "not found"

Result: `/home/kaguyou/homelab/sai-td2p/deploy/build/libsaivs.so.0.0.0` (path, size, sha256, SONAME printed).
First passing build (2026-11-20): 136 MB, 32449 `bcm_*` and 87 `hwbcm_*` defined, 427 undefined (all
versioned glibc/libstdc++/libm, C++ from swsscommon/saimeta, `sai_metadata_*`, `rtnl_link_*`), GLIBC max
2.34, NEEDED = libswsscommon.so.0 libstdc++.so.6 libc.so.6 libgcc_s.so.1 libm.so.6.

### 2. Install

```
deploy/install.sh                 # refuses if syncd is running
deploy/install.sh --stop          # stops swss+syncd first
deploy/install.sh --lib /path/to/other.so
deploy/install.sh --devnode-only  # after a reboot: just recreate /dev/linux-user-bde
```

Switch-side sequence (idempotent, re-runnable):

1. `/dev/linux-user-bde`: `mknod c <major from /proc/devices> 0; chmod 666`. The SONiC `opennsl-modules`
   service only creates `/dev/linux-bcm-bde` (same major 126), not the name the 6.5.27 user BDE opens.
   `/dev` is volatile: **after every reboot run `install.sh --devnode-only` before `systemctl start swss`.**
   The syncd container is `--privileged` (host `/dev` is populated into it at `docker start`), so the node
   must exist before the container starts.
2. `syncd` container must exist and be stopped (`docker inspect`), otherwise abort (or `--stop`).
3. Load test: `docker run --rm docker-syncd-brcm` with our library copied over `libsai.so.1`'s target,
   `ldd` must resolve, then `LD_BIND_NOW=1 syncd -h`. A loader error (`symbol lookup error`,
   `undefined symbol`, ...) aborts the install; a redis error afterwards is expected (syncd opens the
   DB before parsing `-h`) and counts as pass. `--skip-load-test` to skip.
4. First run only: `docker cp -L syncd:/usr/lib/libsai.so.1` -> `syncd:/usr/lib/libsai.so.1.vendor`
   and `/home/admin/sai-td2p/backup/libsai.so.1.vendor` on the host; the original shape of
   `libsai.so.1` (symlink target, normally `libsai.so.1.0`) is recorded in `backup/libsai.so.1.orig_link`.
   Never deleted.
5. One tar stream into `/usr/lib` of the stopped container (`docker cp - syncd:/usr/lib`):
   `libsaivs.so.0.0.0` (our file) and the symlink `libsai.so.1 -> libsaivs.so.0.0.0`. docker's untar
   removes an existing entry before creating it, so the old symlink is replaced, not followed. The
   vendor `libsai.so.1.0` stays untouched. Verified by reading back: `libsai.so.1` must be a symlink to
   `libsaivs.so.0.0.0` and the content behind it must have the uploaded sha256.
6. hwsku dir on the host `/usr/share/sonic/device/x86_64-accton_as5812_54t-r0/Accton-AS5812-54T/`
   (root-owned, mounted read-only into the container at `/usr/share/sonic/hwsku`):
   * `sai.profile` -> `sai.profile.vendor` (once)
   * `lanemap.ini` generated from the switch's own `port_config.ini`, cross-checked against the
     `portmap_` lines of the config.bcm named in the profile (72 ports, `Ethernet0:14` ...)
   * `sai.profile` = vendor profile minus any `SAI_VS_*` lines + `sai.profile.td2p.extra`:

```
SAI_INIT_CONFIG_FILE=/usr/share/sonic/hwsku/td2-as5812t-72x10G.config.bcm
SAI_NUM_ECMP_MEMBERS=64
SAI_VS_SWITCH_TYPE=SAI_VS_SWITCH_TYPE_BCM56860HW
SAI_VS_INTERFACE_LANE_MAP_FILE=/usr/share/sonic/platform/Accton-AS5812-54T/lanemap.ini
SAI_VS_HOSTIF_USE_TAP_DEVICE=false
```

   The lane map is referenced through the `/usr/share/sonic/platform` mount (same host file) on purpose:
   `syncd_init_common.sh`'s Broadcom branch does `sed 's+/usr/share/sonic/hwsku+/tmp+g'` on the profile and
   copies only `*.bcm` to `/tmp` when `<platform>/common_config_support` exists. It does not exist on this
   box today, but the platform path survives either way. The container's `start.sh` copies the hwsku
   `sai.profile` to `/etc/sai.d/sai.profile` at every start and syncd runs with `-p /etc/sai.d/sai.profile`
   (plus `-b /tmp/break_before_make_objects` for ACL_TABLE and TUNNEL).

### 3. Start and watch

```
sudo systemctl reset-failed swss syncd
sudo systemctl start swss
# other terminal
sudo tail -f /var/log/syslog | grep -E 'syncd|orchagent'
docker logs -f syncd
```

### 4. Checks

```
show interfaces status                                            # 72 ports listed
docker exec syncd cat /etc/sai.d/sai.profile                      # our keys present
docker exec syncd ls -la /usr/lib/libsai.so.1* /usr/lib/libsaivs*  # libsai.so.1 -> libsaivs.so.0.0.0, .1.0 vendor, .1.vendor
redis-cli -n 1 keys 'ASIC_STATE:SAI_OBJECT_TYPE_PORT*' | wc -l    # 72 (+1 CPU port = 73)
redis-cli -n 1 keys 'ASIC_STATE:SAI_OBJECT_TYPE_SWITCH*'
redis-cli -n 6 hgetall 'PORT_TABLE|Ethernet0'                     # STATE_DB oper status
docker exec syncd grep -c . /usr/share/sonic/platform/Accton-AS5812-54T/lanemap.ini
```

Failure signatures:

* `syncd` exits immediately, syslog `failed to sai_api_initialize` / `SAI_VS_SWITCH_TYPE ... not supported`:
  the profile keys did not reach syncd or A2's switch type is not registered. Check `docker exec syncd cat /etc/sai.d/sai.profile`.
* `linux-user-bde: open /dev/linux-user-bde failed` / `No such device`: node missing at container start.
  Run `install.sh --devnode-only`, then `sudo systemctl restart syncd` (restart recreates the container's /dev).
* `symbol lookup error` in `docker logs syncd`: the build's symbol check was skipped or the switch's
  libswsscommon/libsaimeta differ from the build container's; `install.sh` load test catches this.
* `show interfaces status` empty while syncd runs: lane map not read (syslog `failed to open lane map file`)
  or lanes do not match `port_config.ini`.

### 5. Roll back

```
deploy/rollback.sh [--stop] [--remove-lanemap]
```

Puts `/usr/lib/libsai.so.1` back into its recorded original shape (symlink -> `libsai.so.1.0`) via a tar
stream, checks that the content behind it is the vendor library (restoring it from
`/usr/lib/libsai.so.1.vendor`, fallback `/home/admin/sai-td2p/backup/libsai.so.1.vendor`, if not), and
restores `sai.profile` from `sai.profile.vendor`. The `.vendor` files stay, so rollback and install can
alternate freely. `/usr/lib/libsaivs.so.0.0.0` stays in the container (unused; files cannot be deleted in a
stopped container with docker cp). `/dev/linux-user-bde` is left alone (harmless for the vendor SAI).

## SONiC warm-reboot (kexec), phase 4

`sudo warm-reboot` kexecs into the same image without resetting the ASIC. What our library and the
plugin do (details: `notes/plan_warmboot_phase4.md`, test steps: `notes/runbook_warmboot_phase4.md`):

* `PRE_SHUTDOWN` (sent only by warm-reboot/fast-reboot, never by a syncd-only warm restart) puts the
  final warm deinit into kexec mode: KNET netifs/filters are kept, but every KNET DMA channel is
  aborted (`KCOM_M_HW_RESET`), the CMIC DMA engines are read back and the verdict goes to
  `/host/warmboot/sai-warmboot.bin.dma_idle`. Test overrides: `touch /host/warmboot/kexec-pending`
  or `HWBCM_WB_KEXEC=1`.
* The plugin runs after syncd/docker are gone, right before `kexec -e`: KNET netdevs down, register
  check through BAR0; still active -> `rmmod linux-knet-cb linux-bcm-knet`; still active -> `kexec -u`
  and a cold reboot. `warm-reboot_plugin --check` only reports. Log: `/host/warmboot/plugin.log`.
* Warm init picks the KNET bring-up from `/proc/bcm/knet/link`, `sai-warmboot.bin.bootid` and the
  `.dma_idle` marker (`wb: knet fresh=... state=...` in syslog): `live` = syncd restart as before;
  `fresh` (0 netifs) = KCOM HW_INIT + DefaultRxAPI and the netdevs are recreated from the hostif
  objects; `dma-reset`/`unknown` = HW_INIT, netifs kept.
* Same image only: a different image has its own docker dir, i.e. the vendor libsai.
* `warm-reboot` refuses while `WARM_RESTART_ENABLE_TABLE|syncd` is enabled: `config warm_restart disable syncd` first.
* config.bcm lists `stable_size` twice; the SDK keeps the first (32 MB). Do not change it between a
  cold boot and a warm one.
* Builds: `nice -n 19 deploy/build_wb.sh ...` now really runs niced (the niceness is passed into `saibuild`).

## Caveats

* The replacement lives in the container's writable layer. If SONiC recreates the container
  (`syncd.sh` does that when the HWSKU changes, and image upgrades do), the vendor library is back:
  rerun `install.sh`. `installed.txt` in `/home/admin/sai-td2p/` records what is installed.
* `install.sh` and `rollback.sh` never start services; `--stop` is the only service action they take.
* The password is read from `SWITCH_PASS` (default: the switch's current admin password); the scripts use
  `sshpass -e`, so it never appears on a command line.
* `gen_lanemap.py` is stdlib-only and is run on the switch with `python3 -I`.
* Not yet exercised against the switch: the tar-stream replacement of the symlink and the throw-away
  load test (written from docker's documented behaviour; install.sh verifies the result and aborts
  with a rollback hint if the shape or sha256 is wrong).
