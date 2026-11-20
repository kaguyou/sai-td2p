# sai-td2p — SONiC 用 SAI(Trident2+ 実機) 設計書

目的: Edgecore AS5812-54T(Broadcom BCM56864 = Trident2+)で SONiC のデータプレーンを動かす。
Broadcom 純正 SAI(閉源)は Trident2+ 非対応なので、**OpenBCM(sdk-6.5.27、公開ソース)を自前ビルドし、
sonic-sairedis の仮想スイッチ SAI(vslib = libsaivs)に実機クラスを足して、syncd が読む libsai.so.1 を置き換える。**

## 既に確認済みの事実(2026-11-20)
- OpenBCM 6.5.27 を BCM_56860_A0 有効でビルドした bcm.user で、機体の ASIC は init soc/misc/mmu/bcm 全段成功。
  72 ポート(xe0-71)認識、BCM84858 PHY 48 本初期化、前面ポート間 10G で双方向フレーム通過・L2 学習確認。
- ビルド木: `~/homelab/openbcm-src/sdk-6.5.27`(make/Make.local: BCM_PTL_SPT=1, BCM_56860_A0=1,
  FEATURE_LIST=L3 I2C MEM_SCAN EDITLINE CUSTOMER CINT TEST KNET, CFGFLAGS += -DALPM_ENABLE -fPIC)。
  静的ライブラリは `build/unix-user/x86-smp_generic_64-2_6/*.a`(-fPIC 済)。リンク順は
  `~/homelab/as5812-bdeshim/sdk_link_list.txt`(bcm.user のリンク行から抽出、--start-group ... --end-group)。
- 機体側: SONiC 202505(Debian 12, glibc 2.36)。syncd は docker `docker-syncd-brcm`、中で `/usr/lib/libsai.so.1 -> libsai.so.1.0`(純正)。
  カーネルモジュール linux-kernel-bde / linux-user-bde / linux-bcm-knet は SONiC 同梱(SDK 6.5.32 由来)をそのまま使う。
  `/dev/linux-user-bde` は無いので `mknod /dev/linux-user-bde c 126 0` が毎起動必要(126 は /proc/devices で確認)。
- config.bcm: `/usr/share/sonic/device/x86_64-accton_as5812_54t-r0/Accton-AS5812-54T/td2-as5812t-72x10G.config.bcm`
  (コンテナ内では /usr/share/sonic/hwsku/)。portmap_<logical>=<phys>:10 が 72 本。l3_alpm_enable=2(SDK は ALPM_ENABLE で再ビルド済)。
- SONiC の port_config.ini(同ディレクトリ)の lanes = 物理ポート番号(portmap の右側)。SAI の HW_LANE_LIST とは
  portmap で突合して bcm logical port を得る。
- SDK 初期化の手順(bcm.user の main と diag shell を再現):
  1. `sal_config_file_set(config_bcm, tmp)` 2. `sal_core_init()` 3. `sal_appl_init()` 4. `knet_kcom_config()`相当
  (`soc_knet_config(&knet_vect_bcm_knet)`) 5. `sysconf_init()` 6. `sysconf_probe()` 7. `sysconf_attach(0)`
  8. `sh_process_command(0, "init soc")` → "init misc" → "init mmu" → "init bcm" 9. `bcm_linkscan_enable_set(0, 250000)`
  アプリ側で必要なグローバル: `ibde_t *bde;` と `int bde_create(void)`(linux_bde_create を呼ぶ。systems/linux/user/common/socdiag.c 参照)。
- 既知の癖: オートネゴで 1G にリンクした直後、内部 SerDes が 10G のまま残りフレームが流れない。`port <n> if=sgmii` を再適用
  (bcm_port_interface_set(unit, port, BCM_PORT_IF_SGMII))すると流れる。10G は問題なし。
- CPU からの tx は速度変更後に `dv_vcnt = 0` で失敗することがある(要調査、KNET 経由では別経路)。

## 全体構成

```
syncd (SONiC, C++) --dlopen--> libsai.so.1  (= 改造 libsaivs.so)
                                 ├─ vslib (sonic-sairedis 202505, C++)  SAI オブジェクトの帳簿・属性・通知の骨格
                                 ├─ SwitchBCM56860HW (新規 C++)       SwitchBCM56850 を継承、create/set/remove を横取りして HW へ
                                 └─ hwbcm (新規 C)                    OpenBCM の bcm_* API を薄く包む。SDK 静的ライブラリをここで抱える
```

- vslib の `SwitchConfig::m_switchType` に `SAI_VS_SWITCH_TYPE_BCM56860HW` を追加。sai.profile の
  `SAI_VS_SWITCH_TYPE=SAI_VS_SWITCH_TYPE_BCM56860HW` で選択(Sai.cpp / SwitchConfig.cpp / VirtualSwitchSaiInterface.cpp の
  switch 文に 1 行ずつ足す)。
- hwbcm の C API は `vslib/hwbcm/hwbcm.h` で固定。C++ 側はこのヘッダだけに依存する。SDK ヘッダは hwbcm/*.c の中だけで include する
  (SDK ヘッダは C++ から include しない。名前衝突と -Werror 地獄を避ける)。
- SAI オブジェクト ID → bcm の ID の対応表は C++ 側(SwitchBCM56860HW)が持つ。hwbcm は bcm の ID だけを扱う。
- vslib 本来の「ソフトで動く」振る舞いは残す: HW 呼び出しが成功したら必ず基底クラスの処理も呼んで帳簿を更新する。
  HW 側で未対応のオブジェクト種は基底クラスに丸投げ(= 従来の仮想スイッチ動作)。これで syncd/orchagent を落とさない。

## 役割分担(並行作業)
| 担当 | 成果物 | 依存 |
|---|---|---|
| A1 hwbcm コア+L2 | `vslib/hwbcm/hwbcm_core.c`(init, port, linkscan, knet), `hwbcm_l2.c`(vlan, l2) | hwbcm.h |
| A2 SAI コア+L2 | `vslib/SwitchBCM56860HW.cpp`(構築, initialize, ports, link 通知, hostif, VLAN/VLAN_MEMBER/FDB/BRIDGE_PORT) + SwitchConfig/Sai/VirtualSwitchSaiInterface への登録 + Makefile.am | hwbcm.h, SwitchBCM56860HW.h |
| A3 L3 | `vslib/hwbcm/hwbcm_l3.c`, `vslib/SwitchBCM56860HW_l3.cpp`(RIF, NEIGHBOR, NEXT_HOP, NEXT_HOP_GROUP(+MEMBER), ROUTE) | 同上 |
| A4 VXLAN/EVPN | `vslib/hwbcm/hwbcm_vxlan.c`, `vslib/SwitchBCM56860HW_vxlan.cpp`(TUNNEL, TUNNEL_MAP(+ENTRY), TUNNEL_TERM_TABLE_ENTRY, BRIDGE(1D)/BRIDGE_PORT(tunnel), FDB on tunnel) | 同上 |
| A5 配備 | `~/homelab/sai-td2p/deploy/`: ビルドスクリプト(saibuild コンテナ)、lanemap.ini 生成、sai.profile、機体へのインストール/巻き戻しスクリプト、syncd 起動確認手順 | — |

## コーディング規約(全員)
- C: C99、`-fPIC`、警告は SDK ヘッダ由来は無視可だが自分のコードは -Wall クリーン。返り値は 0 成功、負は `BCM_E_*` をそのまま返す。
  すべての関数は `hwbcm_` 接頭辞。ログは `hwbcm_log(level, fmt, ...)`(hwbcm_core.c で syslog + stderr)。
- C++: vslib の流儀(SWSS_LOG_ENTER、SWSS_LOG_*、CHECK_STATUS、`_In_`/`_Out_`)に合わせる。-Werror -Wconversion が厳しいので
  キャストは明示。名前空間 `saivs`。
- 未対応の属性・オブジェクトは **黙って基底クラスに委譲** し、WARN ログだけ出す。例外を投げない。
- HW 呼び出しの失敗は SAI_STATUS_FAILURE を返す(orchagent が再試行/中断を判断する)。ただし「存在しない」系は SAI_STATUS_ITEM_NOT_FOUND。
- bcm の unit は常に 0。
- 自分の担当ファイル以外は編集しない(衝突回避)。共有ヘッダ(hwbcm.h, SwitchBCM56860HW.h)に追加が必要なら、
  **末尾の自分用セクション(コメントで区切ってある)にだけ追記**する。

## 試験順序(実機は 1 人ずつ、シリアル経由)
1. libsaivs.so を syncd コンテナに入れて syncd が起動し `show interfaces status` にポートが出る。
2. 前面ポートにリンク → oper up が SONiC に反映。
3. VLAN 100 に 2 ポート入れて L2 疎通。
4. Ethernet0 のネットデバイス(KNET)に IP を付けて ping。
5. FRR で BGP、経路が ASIC に入る(M2)。
6. VXLAN トンネル(M3)。
