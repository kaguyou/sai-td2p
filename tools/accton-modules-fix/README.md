# accton-modules-fix — AS5812-54T の CPLD/PSU カーネルモジュールを kernel 6.1 で動かす

症状: SONiC 202505(kernel 6.1.0-29-2)で `as5812_54t_cpld: probe of 1-0060 failed with error -22`、PSU も同じ。
ファン duty の sysfs が空になり、付属の温度監視が CPLD に書けない(td2p-fan.py で回避していた)。

原因: 上流 sonic-buildimage の accton ドライバが `hwmon_device_register_with_info(dev, name, NULL, NULL, NULL)` を
呼んでいる。kernel 5.x 以降この関数は chip 情報が NULL だと -EINVAL を返す。

修正: `hwmon_device_register_with_groups(dev, name, NULL, NULL)` に置き換え(cpld.c と psu.c)。src/ は
sonic-buildimage 202505 の platform/broadcom/sonic-platform-modules-accton/as5812-54t/modules から取得して 1 行ずつ変更。

ビルド(Debian 12 の saibuild コンテナ): SONiC のカーネルヘッダ deb を
https://sonic-build.azurewebsites.net/api/sonic/artifacts?branchName=202505&platform=broadcom&target=target/debs/bookworm/linux-headers-6.1.0-29-2-{amd64_6.1.123-1_amd64,common_6.1.123-1_all}.deb
から取り、`dpkg -i --force-depends`(+ apt の linux-kbuild-6.1)→ `make -C /usr/src/linux-headers-6.1.0-29-2-amd64 M=$PWD modules`。
vermagic は 6.1.0-29-2-amd64 で一致。

機体: /lib/modules/$(uname -r)/extra/ の 2 本を差し替え(元は /home/admin/backup-modules/)、depmod、
leds/fan/psu/cpld を rmmod → modprobe。2026-11-21 19:30 適用、probe 成功を確認。
