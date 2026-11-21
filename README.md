# sai-td2p — 自作 SAI で Edgecore AS5812-54T (Trident2+) 上の SONiC を動かす

Broadcom 純正 SAI は BCM56864 (Trident2+) を一度も対応していない。本リポは **OpenBCM sdk-6.5.27 を自前ビルドし、
sonic-sairedis の仮想スイッチ SAI (vslib = libsaivs) に実機クラスを足して、syncd が読む libsai.so.1 を置き換える** 路線。
対象 NOS は SONiC 202505 (Debian 12)。

状態 (2026-11-21 夜・ビルド 15): 実機で syncd / orchagent が自作 libsai (`fc335781…`) で稼働。72 ポート、リンク反映、L2 学習、
ルーテッド/VLAN の ping、FRR の BGP 経路の ASIC 投入、VXLAN/EVPN(VTEP・リモート MAC・カプセル化/復号・外側 VLAN タグの untagged 化)まで実機確認済み。
1 本のケーブルで ASIC のポート→ポート転送を試すため、未接続ポートの MAC ループバックをアクセス側ホストの代役にしている。
残: 2 ポート転送/ECMP/VXLAN 往復の完走(2 本目のケーブル)、BUM 二重送信の整理、CPLD ドライバの probe 失敗、
RTC なしの時計。日々の到達点は `notes/` の議事録を参照。

## 中身

| path | 内容 |
|---|---|
| `DESIGN.md` | 設計書。構成、SDK 初期化手順、役割分担、実機で判明した癖 |
| `sairedis/` | sonic-sairedis への差分。`vslib.patch` (既存ファイルの変更)、`vslib/` (新規ファイル: `SwitchBCM56860HW*` C++ 層、`hwbcm/` OpenBCM を包む C 層)、`BASE_COMMIT`、再生成スクリプト |
| `deploy/` | saibuild コンテナでのビルド、機体へのインストール/巻き戻し、`lanemap.ini` 生成、機体から取った参照ファイル (`ref/`) |
| `hwtest/` | SDK 単体の静的テストバイナリのソース (init → ポート確認) |
| `tools/bdeshim/` | 初期の試行 (BDE ioctl で device id 偽装 → S-channel timeout で失敗) と、シリアル/SSH 経由で bcm.user を叩く補助スクリプト |
| `notes/` | 議事録 (2026-11-13 導入、2026-11-20 偽装の試行と方針決定) |
| `sdk_cflags.txt` ほか | SDK ビルドのフラグ/インクルードの生キャプチャ (末尾の `-f` と `-DBCM_HIDE_DISPATCHABLE` は使う側で除去) |

ビルド成果物 (`deploy/build/*.a`, `*.so`, `hwtest/hwtest`) は .gitignore。`deploy/build.sh` / `hwtest/build.sh` で再生成できる。

## 適用の仕方

```
git clone -b 202505 https://github.com/sonic-net/sonic-sairedis && cd sonic-sairedis
git checkout $(cat ../sai-td2p/sairedis/BASE_COMMIT)
git apply ../sai-td2p/sairedis/vslib.patch
cp -r ../sai-td2p/sairedis/vslib/* vslib/
```

OpenBCM 側: sdk-6.5.27 を `make/Make.local` に `BCM_PTL_SPT=1 BCM_56860_A0=1`、`CFGFLAGS += -DALPM_ENABLE -fPIC`、
Make.config で BCM_56860_A0 を DEPRECATED から ESW に戻してビルド (詳細は DESIGN.md)。

機体への接続は `SWITCH_PASS` を環境変数で渡す (リポには書かない)。

## 関連

同じ機体で、stock の libsaibcm 7.1.73.4 (SONiC 202205) を LD_PRELOAD shim 3 つ (device id 偽装 / RX LOS / 8481 系 PHY driver 後付け)
で通す路線が別リポ (UnrealKinoko/edgecore-as5812-54t-sonic) で進んでいる。向こうは SAI 単体でリンクと疎通まで、こちらは
syncd/orchagent 起動まで、と段階が逆で相補関係。向こうで判明した「TD2+ は LLS L0 が 259 本で 1 pipe 35 port 上限」は
こちらの 72 port 構成でも要確認。
