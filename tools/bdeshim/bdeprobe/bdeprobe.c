#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
typedef struct { unsigned int dev, rc, d0, d1, d2, d3; unsigned long long p0; union { unsigned int dw[16]; unsigned char buf[64]; } dx; } lubde_ioctl_t;
#define LUBDE_MAGIC 'L'
#define IO(n) _IO(LUBDE_MAGIC, n)
static int fd;
static void call(const char *name, int nr, unsigned dev, unsigned d0) {
    lubde_ioctl_t io; memset(&io, 0, sizeof io); io.dev = dev; io.d0 = d0; io.rc = (unsigned)-1;
    int r = ioctl(fd, IO(nr), &io);
    printf("%-22s nr=%2d dev=%u -> ret=%d rc=%d d0=0x%08x d1=0x%08x d2=0x%08x d3=0x%08x dw0=0x%08x dw1=0x%08x\n",
           name, nr, dev, r, (int)io.rc, io.d0, io.d1, io.d2, io.d3, io.dx.dw[0], io.dx.dw[1]);
}
int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "/dev/linux-user-bde";
    fd = open(dev, O_RDWR | O_SYNC); if (fd < 0) { perror(dev); return 1; }
    call("VERSION", 0, 0, 0);
    call("GET_NUM_DEVICES(all)", 1, 0xffffffffu, 0);   /* BDE_ALL_DEVICES */
    call("GET_NUM_DEVICES(sw)", 1, 0x1, 0);            /* BDE_SWITCH_DEVICES? */
    call("GET_NUM_DEVICES(0)", 1, 0, 0);
    call("GET_DEVICE(0)", 2, 0, 0);
    call("GET_DMA_INFO", 5, 0, 0);
    call("GET_DEVICE_TYPE(0)", 12, 0, 0);
    call("GET_DEVICE_STATE(0)", 30, 0, 0);
    call("GET_BUS_FEATURES(0)", 21, 0, 0);
    return 0;
}
