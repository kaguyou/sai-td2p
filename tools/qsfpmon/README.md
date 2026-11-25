# qsfpmon

AS5812-54T (SONiC 202505 / 自作 SAI, BCM56864) の QSFP+ 6 ケージ (49-54) を `top` 風にリアルタイム監視する CLI。
読み取り専用・標準ライブラリのみ (python3.11)。スイッチ上で root で実行する。

    sudo qsfpmon                 # ライブ表示 (1 秒更新)
    sudo qsfpmon -i 0.5          # 更新間隔
    sudo qsfpmon --once          # 1 回だけ出力 (スクリプト/テスト用, 色は tty の時だけ)
    sudo qsfpmon --no-serdes     # 診断シェル (SD/LCK) を使わない
    sudo qsfpmon --serdes-interval 5

キー: `q` 終了 / `s` SerDes 切替 / `p` 一時停止 / `c` ログ消去 / `+` `-` 間隔変更。

## 表示
- ケージ毎: 挿抜、ベンダ/PN/SN/種別/長さ、温度、Vcc、LP モード、i2c バス
- レーン毎: ポート名・速度・admin/oper、RX 電力 (mW/dBm/バー)、TX bias、TX 電力、RX/TX LOS、TX fault、SD/LCK
- RX < 0.01 mW (-20 dBm) は赤い `dark`。直前の更新からの変化は反転表示
- 下部に挿抜・oper・LOS・dark・SD/LCK の変化ログ (時刻付き)

## データ源
| 項目 | 取得元 |
|---|---|
| 挿抜/LP/reset | `/sys/bus/i2c/devices/1-0060/module_{present,lp_mode,reset}_N` (CPLD) |
| EEPROM (SFF-8636) | `/sys/bus/i2c/devices/<bus>-0050/eeprom` (optoe1), 256B を 1 更新に 1 回 |
| ポート/oper/speed/lanes | APPL_DB `PORT_TABLE:Ethernet*` を sonic-db-cli eval 1 回で取得 (`index` が cage) |
| SerDes | `docker exec syncd` で診断シェルソケットへ `ps <bcm>` と `phy diag <bcm> dsc` (別スレッド, 既定 3 秒毎, 挿さっているケージのみ) |

bcm ポート番号は config.bcm の `portmap_N=<先頭レーン>:速度` を、APPL_DB の先頭レーンで引いて決める (40G/4x10G 両対応)。

## cage -> I2C bus (platform plugin sfputil.py の `_port_to_i2c_mapping`)
| cage | 49 | 50 | 51 | 52 | 53 | 54 |
|---|---|---|---|---|---|---|
| EEPROM bus | 4 | 6 | 3 | 5 | 7 | 2 |

## 注意
- LOS/TX fault はクリアオンリードのラッチ。読むと 0 に戻り次の周期で再アサートするので、直近 4 回 (--once は 2 回) の OR を現在値として表示する。
- 診断シェルでは `ps` と `phy diag N dsc` 以外を送らない (裸の `cint` は syncd が固まる)。
- syncd 停止中は SerDes 列が n/a になるだけで落ちない。
