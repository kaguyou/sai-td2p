/* bdeshim: LD_PRELOAD shim for Broadcom SDK user BDE ioctls.
 * Logs ioctls to /dev/linux-user-bde and, when BDE_SPOOF_FROM/BDE_SPOOF_TO are
 * set (hex device ids), rewrites the device id returned by the kernel BDE so the
 * SDK picks a different chip driver. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

typedef struct {
    unsigned int dev, rc, d0, d1, d2, d3;
    unsigned long long p0;
    union { unsigned int dw[2]; unsigned char buf[64]; } dx;
} lubde_ioctl_t;

static int (*real_ioctl)(int, unsigned long, ...) = 0;
static int logfd = -1;
static unsigned int spoof_from = 0, spoof_to = 0;
static int inited = 0;
static unsigned int hexval(const char *s){unsigned int v=0;for(;*s;s++){char c=*s;int d;if(c>='0'&&c<='9')d=c-'0';else if(c>='a'&&c<='f')d=c-'a'+10;else if(c>='A'&&c<='F')d=c-'A'+10;else break;v=v*16+d;}return v;}

static void init(void) {
    if (inited) return;
    inited = 1;
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    const char *lp = getenv("BDE_SHIM_LOG");
    if (!lp) lp = "/var/log/bdeshim.log";
    logfd = open(lp, O_WRONLY | O_CREAT | O_APPEND, 0644);
    const char *f = getenv("BDE_SPOOF_FROM"), *t = getenv("BDE_SPOOF_TO");
    if (f && t) { spoof_from = hexval(f); spoof_to = hexval(t); }
    if (logfd >= 0) {
        char b[160];
        int n = snprintf(b, sizeof b, "bdeshim loaded pid=%d spoof %04x->%04x\n", getpid(), spoof_from, spoof_to);
        write(logfd, b, n);
    }
}

static void lg(const char *tag, unsigned long req, lubde_ioctl_t *io, int rc) {
    if (logfd < 0 || !io) return;
    char b[256];
    int n = snprintf(b, sizeof b, "%s nr=%lu dev=%u rc=%u d0=%08x d1=%08x d2=%08x d3=%08x dw0=%08x dw1=%08x ret=%d\n",
                     tag, (req >> 0) & 0xff, io->dev, io->rc, io->d0, io->d1, io->d2, io->d3, io->dx.dw[0], io->dx.dw[1], rc);
    write(logfd, b, n);
}

int ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    init();
    unsigned int type = (req >> 8) & 0xff, nr = req & 0xff;
    if (type != 0x4c || nr > 63 || !arg) return real_ioctl(fd, req, arg);
    lubde_ioctl_t *io = (lubde_ioctl_t *)arg;
    int hot = (nr == 8 || nr == 9 || nr == 10 || nr == 11);
    if (!hot) lg("pre ", req, io, 0);
    int rc = real_ioctl(fd, req, arg);
    if (io && spoof_from) {
        if (nr == 2 && (io->d0 & 0xffff) == spoof_from) {          /* LUBDE_GET_DEVICE: d0=device id */
            io->d0 = (io->d0 & ~0xffffu) | spoof_to; lg("SPOOF", req, io, rc);
        } else if (nr == 4 && io->d1 == 0 && (io->d0 >> 16) == spoof_from) { /* PCI_CONFIG_GET32 off 0: d1=offset? d0=value */
            io->d0 = (spoof_to << 16) | (io->d0 & 0xffff); lg("SPOOF", req, io, rc);
        } else if (nr == 4 && io->d0 == 0 && (io->d1 >> 16) == spoof_from) {
            io->d1 = (spoof_to << 16) | (io->d1 & 0xffff); lg("SPOOF", req, io, rc);
        }
    }
    if (!hot) lg("post", req, io, rc);
    return rc;
}
