# saiprobe-vxlan — 純正 Broadcom SAI(libsaibcm 7.1.73.4)で同じ VXLAN を組んで ASIC テーブルを見る

目的: 自作 SAI(sai-td2p)との差分で「DVP 経由の外側フレームを untagged にする設定」を特定する。

- `saiprobe.c` は UnrealKinoko/edgecore-as5812-54t-sonic の tools/saiprobe/saiprobe.c のコピーに `--vxlan LU,LA[,MAC]` を足したもの
  (`vxlan_demo.inc`: VLAN 100 untagged on lane LU + RIF + neighbor/next hop/route 10.1.0.2/32、VLAN 200 untagged on lane LA、
  tunnel map 2000<->200、P2MP decap tunnel + term 10.1.0.1、P2P tunnel -> 10.1.0.2、tunnel bridge port in VLAN 200、static remote MAC)。
- ビルド(Debian 12 の saibuild コンテナ、純正 libsai は deb から抽出して lib/ に symlink):
  gcc -O2 -I <kinoko>/tools/saiprobe/sai-inc/inc -I .../experimental -I. saiprobe.c -o saiprobe -L lib -lsai -Wl,--allow-shlib-undefined -Wl,-rpath,'$ORIGIN/lib' -ldl -lpthread
- deb の所在: https://packages.trafficmanager.net/public/sai/bcmsai/REL_7.0_202205/7.1.73.4/libsaibcm_7.1.73.4_amd64.deb (sha256 e1ac36a0…)
- 実行(機体、シリアルから。自作 syncd を止めてから):
  cd /host/saiprobe-vxlan && sudo ./run_on_switch.sh -n -K -c device-202205/td2-as5812t-72x10G.config.bcm -t 900 -V 74,97,<PC MAC> -x 'd chg EGR_DVP_ATTRIBUTE' ...
  (run_on_switch.sh は kinoko 版に -V を足したもの。-K = KNET スタブ、ポートのリンクは不要)
