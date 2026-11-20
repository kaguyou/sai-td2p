#include <stdio.h>
#include <sal/core/boot.h>
#include <sal/core/libc.h>
#include <sal/core/thread.h>
#include <ibde.h>
#include <linux-bde.h>
int soc_ndev = 0; int soc_eth_ndev = 0;
int main(void) {
    linux_bde_bus_t bus = {0,0,0};
    ibde_t *bde = NULL;
    sal_core_init();
    int rv = linux_bde_create(&bus, &bde);
    printf("linux_bde_create rv=%d bde=%p\n", rv, (void*)bde);
    if (!bde) return 1;
    int n = bde->num_devices(BDE_ALL_DEVICES);
    printf("num_devices(all)=%d switch=%d\n", n, bde->num_devices(BDE_SWITCH_DEVICES));
    for (int i = 0; i < n; i++) { const ibde_dev_t *d = bde->get_dev(i); printf("dev %d: device=0x%04x rev=0x%02x base=0x%llx\n", i, d->device, d->rev, (unsigned long long)d->base_address); }
    return 0;
}
