/*
 * saiprobe - minimal SAI client for libsai 7.1.73.4 (Broadcom SAI, OCP SAI 1.10.2, SDK 6.5.24).
 *
 * Question it answers: does libsai 7.1 bring up this ASIC when the SAI layer is told it is a
 * Trident II (LD_PRELOAD=sai_td2p_shim.so)? Prints numbered stages and ONE machine-greppable
 * verdict line:   SAIPROBE-VERDICT key=value key=value ...
 *
 * Safety: never loads kernel modules (system()/popen() of insmod/rmmod/modprobe/mknod is denied),
 * refuses to start the SDK when the BDE device nodes are missing (override with --no-precheck),
 * keeps its crash/watchdog handlers (the library's signal()/sigaction() for those signals are
 * ignored), and redirects the warm-boot files into --workdir.
 *
 * build: see Makefile in this directory.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include <sai.h>
#include "status_names.h"

/* diag-shell entry points exported by libsai (not in the OCP headers; signatures from disassembly) */
extern int  sai_driver_process_command(char *cmd);
extern void sai_driver_shell(void);
/* metadata lookup, exported by libsai (prototype from saimetadata.h, struct is opaque here) */
extern const void *sai_metadata_get_attr_metadata(sai_object_type_t objecttype, sai_attr_id_t attrid);

#define NSTAGES 8
static const char *stage_name[NSTAGES + 1] = {
    "none", "precheck", "api_initialize", "api_query", "create_switch",
    "switch_attrs", "port_attrs", "diag", "cleanup"
};

/* ---------------------------------------------------------------- state */
static struct timespec t0;
static volatile int stage_entered, stage_ok;
static volatile int verdict_done;
static int saw_signal;

static struct {
    int stage;
    char what[96];
    sai_status_t status;
    int set;
} first_fail;

static char note_devid[32];   /* "0xb864" when the SAI layer logged "unsupported device id" */
static char note_misc[160];   /* first interesting SDK/SAI log line */
static const char *g_result = "RUNNING";
static pthread_t main_thread;
static int g_timeout;
static int g_have_wd;

typedef int (*sigaction_fn)(int, const struct sigaction *, struct sigaction *);
static sigaction_fn real_sigaction;

/* ---------------------------------------------------------------- output */
static double elapsed(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
}

static void stamp(char *buf, size_t n)
{
    struct timespec t;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &t);
    localtime_r(&t.tv_sec, &tm);
    snprintf(buf, n, "%02d:%02d:%02d.%03ld", tm.tm_hour, tm.tm_min, tm.tm_sec, t.tv_nsec / 1000000);
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char ts[32];
    va_list ap;
    stamp(ts, sizeof ts);
    fprintf(stderr, "%s +%7.3fs  ", ts, elapsed());
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void enter_stage(int n, const char *extra)
{
    stage_entered = n;
    say("[stage %d/%d] %s %s", n, NSTAGES, stage_name[n], extra ? extra : "");
}

static void stage_done(int n)
{
    if (!first_fail.set)
        stage_ok = n;
    say("[stage %d/%d] %s: ok", n, NSTAGES, stage_name[n]);
}

static const char *status_name(sai_status_t s)
{
    static char buf[64];
    return saiprobe_status_name(s, buf, sizeof buf);
}

static void record_fail(int stage, const char *what, sai_status_t st)
{
    if (!first_fail.set) {
        first_fail.set = 1;
        first_fail.stage = stage;
        first_fail.status = st;
        snprintf(first_fail.what, sizeof first_fail.what, "%s", what);
    }
    say("[stage %d/%d] FAIL %s -> status %d (0x%08x) %s", stage, NSTAGES, what, st, (unsigned)st, status_name(st));
}

/* ---------------------------------------------------------------- shim info */
static void shim_summary(char *buf, size_t n)
{
    unsigned long (*calls)(void) = (unsigned long (*)(void))dlsym(RTLD_DEFAULT, "sai_td2p_shim_calls");
    unsigned long (*rew)(void) = (unsigned long (*)(void))dlsym(RTLD_DEFAULT, "sai_td2p_shim_rewrites");
    uint32_t (*real)(void) = (uint32_t (*)(void))dlsym(RTLD_DEFAULT, "sai_td2p_shim_last_real_id");
    uint32_t (*rep)(void) = (uint32_t (*)(void))dlsym(RTLD_DEFAULT, "sai_td2p_shim_reported_id");
    int (*dis)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "sai_td2p_shim_disabled");
    if (!calls || !rew) {
        snprintf(buf, n, "shim=not_loaded");
        return;
    }
    snprintf(buf, n, "shim=loaded%s shim_calls=%lu shim_rewrites=%lu shim_real_id=0x%04x shim_reported_id=0x%04x",
             (dis && dis()) ? "(disabled)" : "", calls(), rew(),
             real ? real() : 0xffffu, rep ? rep() : 0xffffu);
}

/* ---------------------------------------------------------------- verdict */
static void print_verdict(void)
{
    char shim[200], line[900], dl[160] = "";
    if (__atomic_exchange_n(&verdict_done, 1, __ATOMIC_SEQ_CST))
        return;
    shim_summary(shim, sizeof shim);
    if (note_devid[0])
        snprintf(dl, sizeof dl, " sai_unsupported_device_id=%s", note_devid);
    snprintf(line, sizeof line,
             "SAIPROBE-VERDICT result=%s last_stage_entered=%d/%d:%s last_stage_ok=%d/%d:%s "
             "first_failure=%s%s%s status=%d status_name=%s%s %s elapsed=%.1fs",
             g_result, stage_entered, NSTAGES, stage_name[stage_entered], stage_ok, NSTAGES, stage_name[stage_ok],
             first_fail.set ? stage_name[first_fail.stage] : "none",
             first_fail.set ? ":" : "", first_fail.set ? first_fail.what : "",
             first_fail.set ? first_fail.status : 0,
             first_fail.set ? status_name(first_fail.status) : "SAI_STATUS_SUCCESS",
             dl, shim, elapsed());
    fprintf(stderr, "%s\n", line);
    fflush(NULL);
}

static void finish(const char *result, int rc) __attribute__((noreturn));
static void finish(const char *result, int rc)
{
    g_result = result;
    print_verdict();
    fflush(NULL);
    _exit(rc);
}

/* ---------------------------------------------------------------- crash / exit / watchdog */
static void read_line(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (f) {
        if (fgets(out, (int)n, f))
            out[strcspn(out, "\n")] = 0;
        fclose(f);
    }
}

static void dump_threads(void)
{
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    fprintf(stderr, "---- threads (tid comm wchan syscall) ----\n");
    if (!d)
        return;
    while ((e = readdir(d))) {
        char path[128], comm[64], w[64], sc[128];
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "/proc/self/task/%s/comm", e->d_name);    read_line(path, comm, sizeof comm);
        snprintf(path, sizeof path, "/proc/self/task/%s/wchan", e->d_name);   read_line(path, w, sizeof w);
        snprintf(path, sizeof path, "/proc/self/task/%s/syscall", e->d_name); read_line(path, sc, sizeof sc);
        fprintf(stderr, "  %s %-16s wchan=%s syscall=%s\n", e->d_name, comm, w, sc);
    }
    closedir(d);
}

static void crash_handler(int sig)
{
    void *bt[48];
    int n;
    char res[32];
    if (saw_signal++)
        _exit(128 + sig);
    n = backtrace(bt, 48);
    fprintf(stderr, "\n==== signal %d (%s) at stage %d/%d (%s) ====\n", sig, strsignal(sig),
            stage_entered, NSTAGES, stage_name[stage_entered]);
    backtrace_symbols_fd(bt, n, 2);
    snprintf(res, sizeof res, "CRASH(sig%d)", sig);
    g_result = strdup(res);
    if (!first_fail.set) {
        first_fail.set = 1;
        first_fail.stage = stage_entered ? stage_entered : 1;
        snprintf(first_fail.what, sizeof first_fail.what, "signal_%d", sig);
        first_fail.status = SAI_STATUS_FAILURE;
    }
    print_verdict();
    _exit(128 + sig);
}

static void wd_handler(int sig)
{
    void *bt[48];
    int n;
    (void)sig;
    fprintf(stderr, "\n==== WATCHDOG: no progress after %ds, stuck in stage %d/%d (%s); main thread stack: ====\n",
            g_timeout, stage_entered, NSTAGES, stage_name[stage_entered]);
    n = backtrace(bt, 48);
    backtrace_symbols_fd(bt, n, 2);
    dump_threads();
    g_result = "TIMEOUT";
    if (!first_fail.set) {
        first_fail.set = 1;
        first_fail.stage = stage_entered ? stage_entered : 1;
        snprintf(first_fail.what, sizeof first_fail.what, "watchdog_timeout");
        first_fail.status = SAI_STATUS_FAILURE;
    }
    print_verdict();
    _exit(124);
}

static void *watchdog(void *arg)
{
    (void)arg;
    sleep((unsigned)g_timeout);
    pthread_kill(main_thread, SIGUSR2);
    sleep(5);
    /* main thread did not run the handler (signal blocked / stuck in D state): report from here */
    fprintf(stderr, "\n==== WATCHDOG: main thread unresponsive, stuck in stage %d/%d (%s) ====\n",
            stage_entered, NSTAGES, stage_name[stage_entered]);
    dump_threads();
    g_result = "TIMEOUT";
    if (!first_fail.set) {
        first_fail.set = 1;
        first_fail.stage = stage_entered ? stage_entered : 1;
        snprintf(first_fail.what, sizeof first_fail.what, "watchdog_timeout");
        first_fail.status = SAI_STATUS_FAILURE;
    }
    print_verdict();
    _exit(124);
}

static void on_exit_hook(void)
{
    /* reached only when somebody (typically the SDK) calls exit() without us having printed a verdict */
    if (!verdict_done) {
        g_result = "EXIT_IN_LIBRARY";
        if (!first_fail.set) {
            first_fail.set = 1;
            first_fail.stage = stage_entered ? stage_entered : 1;
            snprintf(first_fail.what, sizeof first_fail.what, "exit_called");
            first_fail.status = SAI_STATUS_FAILURE;
        }
        fprintf(stderr, "exit() called while in stage %d/%d (%s)\n", stage_entered, NSTAGES, stage_name[stage_entered]);
        print_verdict();
    }
}

static void install_handler(int sig, void (*h)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_ONSTACK;
    if (real_sigaction)
        real_sigaction(sig, &sa, NULL);
}

/* Keep our handlers: ignore the library's attempts to take over these signals. */
static int protected_sig(int sig)
{
    return sig == SIGSEGV || sig == SIGBUS || sig == SIGABRT || sig == SIGFPE || sig == SIGILL || sig == SIGUSR2;
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    if (!real_sigaction)
        real_sigaction = (sigaction_fn)dlsym(RTLD_NEXT, "sigaction");
    if (act && protected_sig(sig) && real_sigaction && stage_entered) {
        static int logged;
        if (logged++ < 8)
            say("note: library tried sigaction(%d); ignored to keep probe handlers", sig);
        if (old)
            memset(old, 0, sizeof *old);
        return 0;
    }
    return real_sigaction(sig, act, old);
}

typedef void (*sighandler_fn)(int);
sighandler_fn signal(int sig, sighandler_fn h)
{
    struct sigaction sa, old;
    if (protected_sig(sig) && stage_entered)
        return SIG_DFL;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(sig, &sa, &old) < 0)
        return SIG_ERR;
    return old.sa_handler;
}

/* ---------------------------------------------------------------- syslog interposer (SAI logs go here) */
static int is_dangerous_cmd(const char *c)
{
    static const char *bad[] = { "insmod", "rmmod", "modprobe", "mknod", "mount", "umount", "reboot", "poweroff", "systemctl", NULL };
    int i;
    for (i = 0; bad[i]; i++)
        if (strstr(c, bad[i]))
            return 1;
    return 0;
}

static void note_log(const char *msg)
{
    const char *p;
    if ((p = strstr(msg, "unsupported device id")) && !note_devid[0]) {
        const char *x = strstr(p, "0x");
        if (x)
            snprintf(note_devid, sizeof note_devid, "%.12s", x), note_devid[strcspn(note_devid, " \n\r,.")] = 0;
        else
            snprintf(note_devid, sizeof note_devid, "unknown");
    }
    if (!note_misc[0] && (strstr(msg, "KCOM") || strstr(msg, "version mismatch") || strstr(msg, "attach failed") ||
                          strstr(msg, "Unsupported") || strstr(msg, "unsupported")))
        snprintf(note_misc, sizeof note_misc, "%.150s", msg);
}

void vsyslog(int pri, const char *fmt, va_list ap)
{
    char msg[1024];
    char ts[32];
    vsnprintf(msg, sizeof msg, fmt, ap);
    msg[strcspn(msg, "\n")] = 0;
    note_log(msg);
    stamp(ts, sizeof ts);
    fprintf(stderr, "%s +%7.3fs  [sai-syslog p%d] %s\n", ts, elapsed(), pri, msg);
}

void syslog(int pri, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsyslog(pri, fmt, ap);
    va_end(ap);
}

int system(const char *cmd)
{
    int (*real)(const char *) = (int (*)(const char *))dlsym(RTLD_NEXT, "system");
    if (!cmd)
        return real ? real(cmd) : 0;
    if (is_dangerous_cmd(cmd)) {
        say("DENIED system(\"%s\") by probe safety policy", cmd);
        return -1;
    }
    say("system(\"%s\")", cmd);
    return real ? real(cmd) : -1;
}

FILE *popen(const char *cmd, const char *mode)
{
    FILE *(*real)(const char *, const char *) = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, "popen");
    if (is_dangerous_cmd(cmd)) {
        say("DENIED popen(\"%s\") by probe safety policy", cmd);
        errno = EPERM;
        return NULL;
    }
    return real ? real(cmd, mode) : NULL;
}

/* ---------------------------------------------------------------- profile (sai.profile) */
#define MAX_KV 128
static struct { char *k, *v; } kv[MAX_KV];
static int nkv, kv_iter, kv_logged;

static void kv_set(const char *k, const char *v, int overwrite)
{
    int i;
    for (i = 0; i < nkv; i++) {
        if (!strcmp(kv[i].k, k)) {
            if (overwrite) { free(kv[i].v); kv[i].v = strdup(v); }
            return;
        }
    }
    if (nkv < MAX_KV) {
        kv[nkv].k = strdup(k);
        kv[nkv].v = strdup(v);
        nkv++;
    }
}

static int load_profile(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f)) {
        char *s = line, *eq, *e;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\r' || !*s)
            continue;
        eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = 0;
        e = eq - 1;
        while (e >= s && (*e == ' ' || *e == '\t')) *e-- = 0;
        eq++;
        while (*eq == ' ' || *eq == '\t') eq++;
        e = eq + strlen(eq);
        while (e > eq && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        kv_set(s, eq, 1);
    }
    fclose(f);
    return 0;
}

static const char *profile_get_value(sai_switch_profile_id_t id, const char *var)
{
    int i;
    const char *val = NULL;
    for (i = 0; var && i < nkv; i++)
        if (!strcmp(kv[i].k, var)) { val = kv[i].v; break; }
    if (kv_logged < 80) {
        kv_logged++;
        say("  profile_get_value(profile=%u, \"%s\") -> %s%s%s%s", (unsigned)id, var ? var : "(null)",
            val ? "\"" : "NULL", val ? val : "", val ? "\"" : "", kv_logged == 80 ? "   [further profile logging suppressed]" : "");
    }
    return val;
}

static int profile_get_next_value(sai_switch_profile_id_t id, const char **var, const char **value)
{
    (void)id;
    if (!value) {               /* restart, same convention as sonic-sairedis syncd */
        kv_iter = 0;
        if (kv_logged < 80) { kv_logged++; say("  profile_get_next_value(restart)"); }
        return 0;
    }
    if (!var || kv_iter >= nkv) {
        kv_iter = 0;
        if (kv_logged < 80) { kv_logged++; say("  profile_get_next_value -> end(-1)"); }
        return -1;
    }
    *var = kv[kv_iter].k;
    *value = kv[kv_iter].v;
    if (kv_logged < 80) { kv_logged++; say("  profile_get_next_value -> %s=%s", *var, *value); }
    kv_iter++;
    return 0;
}

/* ---------------------------------------------------------------- notifications */
static void on_port_state(uint32_t count, const sai_port_oper_status_notification_t *d)
{
    uint32_t i;
    say("[notify] port_state_change count=%u", count);
    for (i = 0; i < count && i < 8; i++)
        say("[notify]   port 0x%llx state %d", (unsigned long long)d[i].port_id, (int)d[i].port_state);
}

static void on_fdb(uint32_t count, const sai_fdb_event_notification_data_t *d)
{
    (void)d;
    say("[notify] fdb_event count=%u", count);
}

static void on_shutdown(sai_object_id_t sw)
{
    say("[notify] switch_shutdown_request switch=0x%llx", (unsigned long long)sw);
}

/* ---------------------------------------------------------------- attribute-id check vs the library's own metadata */
static int check_attr_ids(void)
{
    static const struct { sai_object_type_t ot; sai_attr_id_t id; const char *name; } want[] = {
        { SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_INIT_SWITCH, "SAI_SWITCH_ATTR_INIT_SWITCH" },
        { SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_SRC_MAC_ADDRESS, "SAI_SWITCH_ATTR_SRC_MAC_ADDRESS" },
        { SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_FDB_EVENT_NOTIFY, "SAI_SWITCH_ATTR_FDB_EVENT_NOTIFY" },
        { SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY, "SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY" },
        { SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_SWITCH_SHUTDOWN_REQUEST_NOTIFY, "SAI_SWITCH_ATTR_SWITCH_SHUTDOWN_REQUEST_NOTIFY" },
        { SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_SWITCH_SHELL_ENABLE, "SAI_SWITCH_ATTR_SWITCH_SHELL_ENABLE" },
        { SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_NUMBER_OF_ACTIVE_PORTS, "SAI_SWITCH_ATTR_NUMBER_OF_ACTIVE_PORTS" },
        { SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_MAX_NUMBER_OF_SUPPORTED_PORTS, "SAI_SWITCH_ATTR_MAX_NUMBER_OF_SUPPORTED_PORTS" },
        { SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PORT_LIST, "SAI_SWITCH_ATTR_PORT_LIST" },
        { SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_CPU_PORT, "SAI_SWITCH_ATTR_CPU_PORT" },
        { SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_DEFAULT_VLAN_ID, "SAI_SWITCH_ATTR_DEFAULT_VLAN_ID" },
        { SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID, "SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID" },
        { SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_DEFAULT_1Q_BRIDGE_ID, "SAI_SWITCH_ATTR_DEFAULT_1Q_BRIDGE_ID" },
        { SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_SWITCH_HARDWARE_INFO, "SAI_SWITCH_ATTR_SWITCH_HARDWARE_INFO" },
        { SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_FIRMWARE_PATH_NAME, "SAI_SWITCH_ATTR_FIRMWARE_PATH_NAME" },
        { SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_FIRMWARE_MAJOR_VERSION, "SAI_SWITCH_ATTR_FIRMWARE_MAJOR_VERSION" },
        { SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_FIRMWARE_MINOR_VERSION, "SAI_SWITCH_ATTR_FIRMWARE_MINOR_VERSION" },
        { SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_TYPE, "SAI_SWITCH_ATTR_TYPE" },
        { SAI_OBJECT_TYPE_PORT, SAI_PORT_ATTR_OPER_STATUS, "SAI_PORT_ATTR_OPER_STATUS" },
        { SAI_OBJECT_TYPE_PORT, SAI_PORT_ATTR_SPEED, "SAI_PORT_ATTR_SPEED" },
        { SAI_OBJECT_TYPE_PORT, SAI_PORT_ATTR_HW_LANE_LIST, "SAI_PORT_ATTR_HW_LANE_LIST" },
    };
    size_t i;
    int bad = 0;
    for (i = 0; i < sizeof want / sizeof want[0]; i++) {
        /* sai_attr_metadata_t starts with { objecttype; attrid; const char *attridname; ... } */
        const struct { int ot; unsigned id; const char *name; } *m = sai_metadata_get_attr_metadata(want[i].ot, want[i].id);
        if (!m || (int)m->ot != (int)want[i].ot || m->id != want[i].id || !m->name || strcmp(m->name, want[i].name)) {
            say("  attr id MISMATCH: header says %s=%u, library metadata says %s", want[i].name, want[i].id,
                m ? (m->name ? m->name : "(null name)") : "(no metadata)");
            bad++;
        }
    }
    say("  attribute ids checked against libsai metadata: %zu checked, %d mismatches (headers: OCP SAI %d.%d.%d)",
        sizeof want / sizeof want[0], bad, SAI_MAJOR, SAI_MINOR, SAI_REVISION);
    return bad;
}

/* ---------------------------------------------------------------- precheck */
static int precheck(int force, int *warn_knet)
{
    struct stat st;
    int missing = 0;
    static const char *must[] = { "/dev/linux-user-bde", "/dev/linux-kernel-bde" };
    size_t i;
    FILE *f;
    char line[256];
    for (i = 0; i < 2; i++) {
        if (stat(must[i], &st) == 0)
            say("  %s: present (%s, major %u minor %u)", must[i], S_ISCHR(st.st_mode) ? "char dev" : "NOT a char dev",
                major(st.st_rdev), minor(st.st_rdev));
        else {
            say("  %s: MISSING (%s)", must[i], strerror(errno));
            missing++;
        }
    }
    if (stat("/dev/linux-bcm-knet", &st) == 0)
        say("  /dev/linux-bcm-knet: present");
    else {
        say("  /dev/linux-bcm-knet: missing (packet I/O / KNET init will fail or be skipped)");
        *warn_knet = 1;
    }
    if ((f = fopen("/proc/linux-kernel-bde", "r"))) {
        int n = 0;
        say("  /proc/linux-kernel-bde:");
        while (fgets(line, sizeof line, f) && n++ < 24) {
            line[strcspn(line, "\n")] = 0;
            say("    | %s", line);
        }
        fclose(f);
    } else
        say("  /proc/linux-kernel-bde: not readable (%s)", strerror(errno));
    if (missing && !force) {
        say("  BDE device node(s) missing; the SDK would fall back to insmod/mknod attempts or abort. Not starting it.");
        say("  (use --no-precheck to run anyway; the probe still denies insmod/modprobe/mknod)");
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- readback helpers */
static sai_switch_api_t *swapi;
static sai_port_api_t *portapi;
static sai_object_id_t g_switch;

static void show_oid_attr(const char *name, sai_attr_id_t id)
{
    sai_attribute_t a;
    sai_status_t st;
    memset(&a, 0, sizeof a);
    a.id = id;
    st = swapi->get_switch_attribute(g_switch, 1, &a);
    if (st == SAI_STATUS_SUCCESS)
        say("  %-44s = 0x%llx", name, (unsigned long long)a.value.oid);
    else
        say("  %-44s : get failed %d %s", name, st, status_name(st));
}

static void show_u32_attr(const char *name, sai_attr_id_t id)
{
    sai_attribute_t a;
    sai_status_t st;
    memset(&a, 0, sizeof a);
    a.id = id;
    st = swapi->get_switch_attribute(g_switch, 1, &a);
    if (st == SAI_STATUS_SUCCESS)
        say("  %-44s = %u", name, a.value.u32);
    else
        say("  %-44s : get failed %d %s", name, st, status_name(st));
}

static void show_s8list_attr(const char *name, sai_attr_id_t id)
{
    sai_attribute_t a;
    sai_status_t st;
    char buf[256];
    memset(&a, 0, sizeof a);
    memset(buf, 0, sizeof buf);
    a.id = id;
    a.value.s8list.count = sizeof buf - 1;
    a.value.s8list.list = (int8_t *)buf;
    st = swapi->get_switch_attribute(g_switch, 1, &a);
    if (st == SAI_STATUS_SUCCESS)
        say("  %-44s = \"%s\"", name, buf);
    else
        say("  %-44s : get failed %d %s", name, st, status_name(st));
}

static sai_object_id_t *ports;
static uint32_t nports;

static sai_status_t read_port_list(void)
{
    sai_attribute_t a;
    sai_status_t st;
    memset(&a, 0, sizeof a);
    a.id = SAI_SWITCH_ATTR_PORT_LIST;
    a.value.objlist.count = 0;
    a.value.objlist.list = NULL;
    st = swapi->get_switch_attribute(g_switch, 1, &a);
    if (st != SAI_STATUS_BUFFER_OVERFLOW && st != SAI_STATUS_SUCCESS)
        return st;
    nports = a.value.objlist.count;
    ports = calloc(nports ? nports : 1, sizeof *ports);
    a.value.objlist.list = ports;
    a.value.objlist.count = nports;
    st = swapi->get_switch_attribute(g_switch, 1, &a);
    nports = a.value.objlist.count;
    return st;
}

static const char *oper_name(int s)
{
    return s == SAI_PORT_OPER_STATUS_UP ? "up" : s == SAI_PORT_OPER_STATUS_DOWN ? "down" :
           s == SAI_PORT_OPER_STATUS_UNKNOWN ? "unknown" : "other";
}

#include "vxlan_demo.inc"

static void usage(const char *p)
{
    fprintf(stderr,
        "usage: %s --profile FILE [options]\n"
        "  --profile FILE     sai.profile-style KEY=VALUE file (required; SAI_INIT_CONFIG_FILE=<config.bcm>)\n"
        "  --config FILE      override SAI_INIT_CONFIG_FILE\n"
        "  --workdir DIR      where warm-boot files go (default: current dir)\n"
        "  --sai-log LEVEL    debug|info|notice|warn|error|critical for every SAI API (default: info)\n"
        "  --shell            enter the library's interactive diag shell after init\n"
        "  --cmd 'CMD'        run one diag-shell command via sai_driver_process_command (repeatable)\n"
        "  --no-remove        skip remove_switch / sai_api_uninitialize\n"
        "  --no-precheck      start the SDK even if /dev/linux-{user,kernel}-bde are missing\n"
        "  --selftest hang|crash|abort|exit   exercise watchdog/crash/exit reporting (no SDK involved)\n"
        "  --timeout SEC      watchdog (default 600, 0 = off; off during --shell)\n"
        "  --mac AA:BB:..     SAI_SWITCH_ATTR_SRC_MAC_ADDRESS (default 02:00:00:00:00:01)\n"
        "  --vxlan LU,LA,MAC  (sai-td2p) build the EVPN VXLAN object set: underlay port = first lane LU,\n"
        "                     access port = first lane LA, PC MAC for the 192.168.52.2 neighbor\n", p);
}

static int parse_level(const char *s)
{
    static const char *n[] = { "debug", "info", "notice", "warn", "error", "critical" };
    int i;
    for (i = 0; i < 6; i++)
        if (!strcmp(s, n[i]))
            return i;
    return -1;
}

int main(int argc, char **argv)
{
    const char *profile = NULL, *config = NULL, *workdir = ".", *mac_s = "02:00:00:00:00:01";
    const char *vxlan_s = NULL;
    const char *cmds[32];
    int ncmds = 0, shell = 0, no_remove = 0, no_precheck = 0, loglevel = SAI_LOG_LEVEL_INFO, timeout = 600;
    int c, warn_knet = 0, mism;
    const char *selftest = NULL;
    sai_status_t st;
    sai_service_method_table_t svc = { profile_get_value, profile_get_next_value };
    static const struct option lo[] = {
        { "profile", 1, 0, 'p' }, { "config", 1, 0, 'c' }, { "workdir", 1, 0, 'w' }, { "sai-log", 1, 0, 'l' },
        { "vxlan", 1, 0, 'V' }, { "shell", 0, 0, 's' }, { "cmd", 1, 0, 'x' }, { "no-remove", 0, 0, 'R' }, { "no-precheck", 0, 0, 'P' },
        { "selftest", 1, 0, 'S' }, { "timeout", 1, 0, 't' }, { "mac", 1, 0, 'm' }, { "help", 0, 0, 'h' }, { 0, 0, 0, 0 } };

    clock_gettime(CLOCK_MONOTONIC, &t0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    main_thread = pthread_self();
    real_sigaction = (sigaction_fn)dlsym(RTLD_NEXT, "sigaction");

    while ((c = getopt_long(argc, argv, "h", lo, NULL)) != -1) {
        switch (c) {
        case 'p': profile = optarg; break;
        case 'c': config = optarg; break;
        case 'w': workdir = optarg; break;
        case 'l': loglevel = parse_level(optarg); if (loglevel < 0) { usage(argv[0]); return 2; } break;
        case 's': shell = 1; break;
        case 'x': if (ncmds < 32) cmds[ncmds++] = optarg; break;
        case 'R': no_remove = 1; break;
        case 'P': no_precheck = 1; break;
        case 't': timeout = atoi(optarg); break;
        case 'S': selftest = optarg; break;
        case 'm': mac_s = optarg; break;
        case 'V': vxlan_s = optarg; break;
        default: usage(argv[0]); return 2;
        }
    }
    if (!profile) {
        usage(argv[0]);
        return 2;
    }

    {   /* alt stack so that a stack overflow can still be reported */
        static char altstack[65536];
        stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0 };
        sigaltstack(&ss, NULL);
    }
    install_handler(SIGSEGV, crash_handler);
    install_handler(SIGBUS, crash_handler);
    install_handler(SIGABRT, crash_handler);
    install_handler(SIGFPE, crash_handler);
    install_handler(SIGILL, crash_handler);
    install_handler(SIGUSR2, wd_handler);
    atexit(on_exit_hook);

    g_timeout = (shell || timeout <= 0) ? 0 : timeout;
    if (g_timeout) {
        pthread_t wd;
        if (pthread_create(&wd, NULL, watchdog, NULL) == 0) {
            pthread_detach(wd);
            g_have_wd = 1;
        }
    }

    /* ------------------------------------------------------------ stage 1 */
    enter_stage(1, "preconditions, profile, attribute ids");
    say("  saiprobe built against OCP SAI headers %d.%d.%d; pid %d; uid %d", SAI_MAJOR, SAI_MINOR, SAI_REVISION,
        (int)getpid(), (int)getuid());
    {
        Dl_info di;
        if (dladdr((void *)sai_api_initialize, &di))
            say("  libsai loaded from: %s", di.dli_fname);
        void *bi = dlsym(RTLD_DEFAULT, "bcm_info_get");
        if (bi && dladdr(bi, &di))
            say("  bcm_info_get resolves (global scope) to: %s", di.dli_fname);
        char sh[200];
        shim_summary(sh, sizeof sh);
        say("  %s", sh);
    }
    if (load_profile(profile) < 0) {
        say("  cannot read profile %s: %s", profile, strerror(errno));
        record_fail(1, "read_profile", SAI_STATUS_FAILURE);
        finish("PRECHECK_FAIL", 3);
    }
    if (config)
        kv_set("SAI_INIT_CONFIG_FILE", config, 1);
    {
        char p1[512], p2[512];
        snprintf(p1, sizeof p1, "%s/sai-warmboot.read", workdir);
        snprintf(p2, sizeof p2, "%s/sai-warmboot.write", workdir);
        kv_set("SAI_BOOT_TYPE", "0", 0);
        kv_set("SAI_WARM_BOOT_READ_FILE", p1, 0);
        kv_set("SAI_WARM_BOOT_WRITE_FILE", p2, 0);
    }
    {
        int i;
        const char *cf = NULL;
        for (i = 0; i < nkv; i++) {
            say("  profile: %s=%s", kv[i].k, kv[i].v);
            if (!strcmp(kv[i].k, "SAI_INIT_CONFIG_FILE"))
                cf = kv[i].v;
        }
        if (cf && access(cf, R_OK) != 0) {
            say("  SAI_INIT_CONFIG_FILE %s not readable: %s", cf, strerror(errno));
            record_fail(1, "config_file_unreadable", SAI_STATUS_FAILURE);
            finish("PRECHECK_FAIL", 3);
        }
        if (!cf)
            say("  warning: no SAI_INIT_CONFIG_FILE in profile");
    }
    mism = check_attr_ids();
    if (mism) {
        record_fail(1, "attribute_id_mismatch", SAI_STATUS_FAILURE);
        finish("PRECHECK_FAIL", 3);
    }
    if (precheck(no_precheck, &warn_knet) < 0) {
        record_fail(1, "bde_device_nodes_missing", SAI_STATUS_UNINITIALIZED);
        finish("PRECHECK_FAIL", 3);
    }
    stage_done(1);

    /* ------------------------------------------------------------ stage 2 */
    enter_stage(2, "sai_api_initialize(0, &services)");
    if (selftest) {   /* exercises the watchdog / crash / exit paths without hardware */
        say("  SELFTEST mode '%s'", selftest);
        if (!strcmp(selftest, "hang")) for (;;) sleep(1000);
        if (!strcmp(selftest, "crash")) *(volatile int *)0 = 1;
        if (!strcmp(selftest, "abort")) abort();
        if (!strcmp(selftest, "exit")) exit(7);
    }
    st = sai_api_initialize(0, &svc);
    if (st != SAI_STATUS_SUCCESS) {
        record_fail(2, "sai_api_initialize", st);
        finish("FAIL", 1);
    }
    stage_done(2);

    /* ------------------------------------------------------------ stage 3 */
    enter_stage(3, "sai_api_query(SWITCH, PORT), sai_log_set");
    st = sai_api_query(SAI_API_SWITCH, (void **)&swapi);
    if (st != SAI_STATUS_SUCCESS || !swapi) {
        record_fail(3, "sai_api_query(SAI_API_SWITCH)", st ? st : SAI_STATUS_FAILURE);
        finish("FAIL", 1);
    }
    st = sai_api_query(SAI_API_PORT, (void **)&portapi);
    if (st != SAI_STATUS_SUCCESS || !portapi) {
        record_fail(3, "sai_api_query(SAI_API_PORT)", st ? st : SAI_STATUS_FAILURE);
        finish("FAIL", 1);
    }
    {
        int api, okc = 0;
        for (api = SAI_API_SWITCH; api < SAI_API_MAX; api++)
            if (sai_log_set((sai_api_t)api, (sai_log_level_t)loglevel) == SAI_STATUS_SUCCESS)
                okc++;
        say("  sai_log_set(level %d) accepted for %d of %d APIs", loglevel, okc, SAI_API_MAX - 1);
    }
    stage_done(3);

    /* ------------------------------------------------------------ stage 4 */
    {
        sai_attribute_t at[8];
        uint32_t n = 0;
        unsigned m[6];
        char extra[160];
        memset(at, 0, sizeof at);
        if (sscanf(mac_s, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
            say("bad --mac %s", mac_s);
            record_fail(1, "bad_mac_argument", SAI_STATUS_INVALID_PARAMETER);
            finish("PRECHECK_FAIL", 2);
        }
        at[n].id = SAI_SWITCH_ATTR_INIT_SWITCH;               at[n++].value.booldata = true;
        at[n].id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
        { int i; for (i = 0; i < 6; i++) at[n].value.mac[i] = (uint8_t)m[i]; n++; }
        at[n].id = SAI_SWITCH_ATTR_FDB_EVENT_NOTIFY;          at[n++].value.ptr = (void *)on_fdb;
        at[n].id = SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY;  at[n++].value.ptr = (void *)on_port_state;
        at[n].id = SAI_SWITCH_ATTR_SWITCH_SHUTDOWN_REQUEST_NOTIFY; at[n++].value.ptr = (void *)on_shutdown;
        snprintf(extra, sizeof extra, "(%u attributes, INIT_SWITCH=true) - this runs the whole SDK init, may take minutes", n);
        enter_stage(4, extra);
        st = swapi->create_switch(&g_switch, n, at);
        if (st != SAI_STATUS_SUCCESS) {
            record_fail(4, "create_switch", st);
            if (note_devid[0])
                say("  SAI layer logged: unsupported device id %s  (=> baseline rejection; is the shim loaded?)", note_devid);
            if (note_misc[0])
                say("  first suspicious log line: %s", note_misc);
            goto cleanup_after_fail;
        }
        say("  create_switch returned switch id 0x%llx", (unsigned long long)g_switch);
        if (warn_knet)
            say("  note: /dev/linux-bcm-knet was missing at start");
        stage_done(4);
    }

    /* ------------------------------------------------------------ stage 5 */
    enter_stage(5, "read back switch attributes");
    show_u32_attr("NUMBER_OF_ACTIVE_PORTS", SAI_SWITCH_ATTR_NUMBER_OF_ACTIVE_PORTS);
    show_u32_attr("MAX_NUMBER_OF_SUPPORTED_PORTS", SAI_SWITCH_ATTR_MAX_NUMBER_OF_SUPPORTED_PORTS);
    show_oid_attr("CPU_PORT", SAI_SWITCH_ATTR_CPU_PORT);
    show_oid_attr("DEFAULT_VLAN_ID", SAI_SWITCH_ATTR_DEFAULT_VLAN_ID);
    show_oid_attr("DEFAULT_VIRTUAL_ROUTER_ID", SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID);
    show_oid_attr("DEFAULT_1Q_BRIDGE_ID", SAI_SWITCH_ATTR_DEFAULT_1Q_BRIDGE_ID);
    show_u32_attr("TYPE (enum)", SAI_SWITCH_ATTR_TYPE);
    show_s8list_attr("SWITCH_HARDWARE_INFO", SAI_SWITCH_ATTR_SWITCH_HARDWARE_INFO);
    show_s8list_attr("FIRMWARE_PATH_NAME", SAI_SWITCH_ATTR_FIRMWARE_PATH_NAME);
    show_u32_attr("FIRMWARE_MAJOR_VERSION", SAI_SWITCH_ATTR_FIRMWARE_MAJOR_VERSION);
    show_u32_attr("FIRMWARE_MINOR_VERSION", SAI_SWITCH_ATTR_FIRMWARE_MINOR_VERSION);
    st = read_port_list();
    if (st != SAI_STATUS_SUCCESS) {
        record_fail(5, "get PORT_LIST", st);
    } else {
        say("  PORT_LIST: %u ports", nports);
        if (nports == 0) {
            record_fail(5, "PORT_LIST empty", SAI_STATUS_FAILURE);
        }
    }
    if (!first_fail.set || first_fail.stage != 5)
        stage_done(5);

    /* ------------------------------------------------------------ stage 6 */
    enter_stage(6, "per-port oper status / speed");
    if (ports && nports) {
        uint32_t i, up = 0, fails = 0;
        for (i = 0; i < nports; i++) {
            sai_attribute_t a[2];
            sai_attribute_t lanes;
            uint32_t lane_buf[8];
            sai_status_t s1, s2, s3;
            memset(a, 0, sizeof a);
            a[0].id = SAI_PORT_ATTR_OPER_STATUS;
            s1 = portapi->get_port_attribute(ports[i], 1, &a[0]);
            a[1].id = SAI_PORT_ATTR_SPEED;
            s2 = portapi->get_port_attribute(ports[i], 1, &a[1]);
            memset(&lanes, 0, sizeof lanes);
            lanes.id = SAI_PORT_ATTR_HW_LANE_LIST;
            lanes.value.u32list.count = 8;
            lanes.value.u32list.list = lane_buf;
            s3 = portapi->get_port_attribute(ports[i], 1, &lanes);
            if (s1 || s2)
                fails++;
            if (s1 == 0 && a[0].value.s32 == SAI_PORT_OPER_STATUS_UP)
                up++;
            say("  port[%2u] 0x%llx oper=%-7s speed=%u Mb/s lanes=%s (first=%u n=%u)",
                i, (unsigned long long)ports[i],
                s1 ? status_name(s1) : oper_name(a[0].value.s32), s2 ? 0u : a[1].value.u32,
                s3 ? status_name(s3) : "ok", s3 ? 0u : lanes.value.u32list.list[0],
                s3 ? 0u : lanes.value.u32list.count);
        }
        say("  ports up: %u of %u; attribute read failures: %u", up, nports, fails);
        if (fails == nports)
            record_fail(6, "all port attribute reads failed", SAI_STATUS_FAILURE);
        else
            stage_done(6);
    } else {
        say("  skipped (no ports)");
    }

    /* ------------------------------------------------------------ stage 6b (sai-td2p) */
    if (vxlan_s) {
        unsigned lu = 0, la = 0; char macbuf[32] = "84:ba:59:0c:d5:2c";
        if (sscanf(vxlan_s, "%u,%u,%31s", &lu, &la, macbuf) < 2) {
            say("  --vxlan: expected LU,LA[,MAC]");
        } else {
            int vf;
            enter_stage(6, "vxlan demo through the stock SAI");
            vf = vxlan_demo(lu, la, macbuf);
            say("  vxlan demo: %d failing calls", vf);
        }
    }

    /* ------------------------------------------------------------ stage 7 */
    if (ncmds || shell) {
        int i;
        enter_stage(7, "diag shell");
        /* 2026-11-20, real AS5812-54T: setting SAI_SWITCH_ATTR_SWITCH_SHELL_ENABLE=true does not just
         * enable diag access, it enters the interactive "drivshell" and blocks on stdin. So --cmd goes
         * straight to the SDK's own command processor, which libsai exports. */
        {
            int (*shp)(int, char *) = (int (*)(int, char *))dlsym(RTLD_DEFAULT, "sh_process_command");
            if (!shp)
                say("  sh_process_command not found in the library; falling back to sai_driver_process_command");
            for (i = 0; i < ncmds; i++) {
                char buf[512];
                int rv;
                snprintf(buf, sizeof buf, "%s", cmds[i]);
                say("  diag> %s", cmds[i]);
                fflush(stdout);
                rv = shp ? shp(0, buf) : sai_driver_process_command(buf);
                fflush(stdout);
                say("  diag command returned %d", rv);
                if (rv && !first_fail.set)
                    record_fail(7, cmds[i], rv);
            }
        }
        if (shell) {
            say("  entering the library diag shell (stdin). Leave with 'quit' / 'exit' / Ctrl-D.");
            sai_driver_shell();
            say("  diag shell returned");
        }
        if (!first_fail.set || first_fail.stage != 7)
            stage_done(7);
    } else {
        say("[stage 7/%d] diag: skipped (no --shell / --cmd)", NSTAGES);
        stage_ok = 7;
    }

cleanup_after_fail:
    /* ------------------------------------------------------------ stage 8 */
    if (no_remove) {
        say("[stage 8/%d] cleanup: skipped (--no-remove)", NSTAGES);
    } else {
        enter_stage(8, "remove_switch, sai_api_uninitialize");
        if (g_switch) {
            st = swapi->remove_switch(g_switch);
            if (st != SAI_STATUS_SUCCESS)
                record_fail(8, "remove_switch", st);
        }
        st = sai_api_uninitialize();
        if (st != SAI_STATUS_SUCCESS)
            record_fail(8, "sai_api_uninitialize", st);
        if (!first_fail.set || first_fail.stage != 8)
            stage_done(8);
    }
    if (first_fail.set)
        finish(first_fail.stage == 4 && note_devid[0] ? "FAIL_DEVID_REJECTED" : "FAIL", 1);
    g_result = "OK";
    print_verdict();
    (void)g_have_wd;
    fflush(NULL);
    _exit(0);
}
