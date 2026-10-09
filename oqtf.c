#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define OQTF_SWEEP_TRY  50U
#define OQTF_GRACE_MS 2000U

#define OQTF_SCAN_MIN_MS   50U
#define OQTF_SCAN_MAX_MS 1000U

#define OQTF_STAT_STATE  3
#define OQTF_STAT_START 22

#define OQTF_COUNT(a) (sizeof(a) / sizeof((a)[0]))

#define OQTF_RETRY(expr) __extension__({ \
    __typeof__(expr) r_;                 \
    do r_ = (expr);                      \
    while (r_ < 0 && errno == EINTR);    \
    r_; })

struct oqtf_path {
    char b[PATH_MAX];
};

static struct {
    pid_t self;
    pid_t cmd;
    double cmd_start;
    unsigned long long timeout;
    int shell;
    int fired;
} g;

static void
oqtf_help(const char *prog)
{
    printf("Usage: %s TIME [-s|--shell] [CMD [ARG...]]"                 "\n"
                                                                        "\n"
           "Run CMD. When TIME runs out, gently bring it to exit,"      "\n"
           "with everything it spawned. Nothing is left behind."        "\n"
                                                                        "\n"
           "  oqtf 1m make          a build that can not hang forever"  "\n"
           "  oqtf 500ms curl api   a request that can not get stuck"   "\n"
           "  oqtf 30m -s           your shell: thirty minutes per"     "\n"
           "                        command, forever"                   "\n"
           , prog);
}

static _Noreturn void
oqtf_fatal(int code, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (len >= 0) {
        if ((size_t)len >= sizeof(buf))
            len = (int)sizeof(buf) - 1;

        buf[len++] = '\n';
        len = OQTF_RETRY(write(2, buf, (size_t)len));
    }
    _exit(code);
}

static struct oqtf_path
oqtf_path(const char *fmt, ...)
{
    struct oqtf_path p;
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(p.b, sizeof(p.b), fmt, ap);
    va_end(ap);

    if (len < 0 || (size_t)len >= sizeof(p.b))
        oqtf_fatal(1, "path too long");

    return p;
}

static ssize_t
oqtf_read_file(const char *path, char *data, size_t size)
{
    int fd = OQTF_RETRY(open(path, O_RDONLY));

    if (fd < 0)
        return -1;

    ssize_t n = OQTF_RETRY(read(fd, data, size));
    close(fd);
    return n;
}

static void
oqtf_write_file_fmt(const char *path, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (len < 0 || (size_t)len >= sizeof(buf))
        oqtf_fatal(1, "%s: value too long", path);

    int fd = OQTF_RETRY(open(path, O_WRONLY));

    if (fd < 0)
        oqtf_fatal(1, "%s: %s", path, strerror(errno));

    ssize_t n = OQTF_RETRY(write(fd, buf, (size_t)len));

    if (n < 0)
        oqtf_fatal(1, "%s: %s", path, strerror(errno));

    if (n != len)
        oqtf_fatal(1, "%s: partial write", path);

    close(fd);
}

static struct oqtf_path
oqtf_self_cg(void)
{
    char buf[PATH_MAX] = {0};
    ssize_t n = oqtf_read_file("/proc/self/cgroup", buf, sizeof(buf) - 1U);

    if (n <= 0)
        oqtf_fatal(1, "Unable to read /proc/self/cgroup");

    *strchrnul(buf, '\n') = '\0';

    if (n < 3 || memcmp(buf, "0::", 3U))
        oqtf_fatal(1, "cgroups v2 not available");

    return oqtf_path("%s", buf + 3);
}

static double
oqtf_uptime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static double
oqtf_start_time(pid_t pid)
{
    char buf[PATH_MAX] = {0};
    struct oqtf_path path = oqtf_path("/proc/%d/stat", pid);
    ssize_t n = oqtf_read_file(path.b, buf, sizeof(buf) - 1U);

    if (n <= 0)
        return -1.0;

    char *p = strrchr(buf, ')');

    if (!p)
        return -1.0;

    p += 2; // ") "

    for (int i = OQTF_STAT_STATE; i < OQTF_STAT_START; i++) {
        if (!(p = strchr(p, ' ')))
            return -1.0;
        p++;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long ticks = strtoull(p, &end, 10);

    if (errno || end == p)
        return -1.0;

    long clk = sysconf(_SC_CLK_TCK);

    if (clk <= 0)
        return -1.0;

    return (double)ticks / (double)clk;
}

static int
oqtf_time(const char *s, unsigned long long *out)
{
    unsigned long long total = 0;
    char *end = NULL;
    int first = 1;

    for (;;) {
        errno = 0;
        unsigned long long v = strtoull(s, &end, 10);

        if (errno || end == s)
            return -1;

        s = end;

        if (!s[0] && !first)
            return -1;

        unsigned long long mult = 1;

        if (s[0] == 'm' && s[1] == 's') {
            s += 2;
        } else {
            switch (s[0]) {
                case 'w': mult *= 7;    /* FALLTHRU */
                case 'd': mult *= 24;   /* FALLTHRU */
                case 'h': mult *= 60;   /* FALLTHRU */
                case 'm': mult *= 60;   /* FALLTHRU */
                case  0 :               /* FALLTHRU */
                case 's': mult *= 1000; break;
                default : return -1;
            }
            if (s[0])
                s++;
        }
        if (!v || v > ULLONG_MAX / mult || total > ULLONG_MAX - v * mult)
            return -1;

        total += v * mult;
        first = 0;

        if (!s[0])
            break;
    }
    *out = total;
    return total ? 0 : -1;
}

static void
oqtf_msleep(unsigned long long ms)
{
    struct timespec ts = {
        .tv_sec = (time_t)(ms / 1000U),
        .tv_nsec = (long)((ms % 1000U) * 1000000U),
    };
    nanosleep(&ts, NULL);
}

struct oqtf_pids {
    int fd;
    size_t pos;
    size_t len;
    char buf[512];
};

static int
oqtf_pids_open(struct oqtf_pids *it, const char *path)
{
    it->fd = OQTF_RETRY(open(path, O_RDONLY));
    it->pos = 0U;
    it->len = 0U;
    return it->fd < 0;
}

static int
oqtf_pids_next(struct oqtf_pids *it, pid_t *pid)
{
    unsigned long long value = 0ULL;
    int have = 0;

    for (;;) {
        if (it->pos == it->len) {
            ssize_t n = OQTF_RETRY(read(it->fd, it->buf, sizeof(it->buf)));

            if (n <= 0) {
                if (!have)
                    return 0;

                *pid = (pid_t)value;
                return 1;
            }
            it->pos = 0U;
            it->len = (size_t)n;
        }
        char c = it->buf[it->pos++];

        if (c >= '0' && c <= '9') {
            value = value * 10ULL + (unsigned long long)(c - '0');
            have = 1;
        } else if (have) {
            *pid = (pid_t)value;
            return 1;
        }
    }
}

static void
oqtf_cull(const char *procs)
{
    double now = oqtf_uptime();
    double elapsed = now - g.cmd_start;
    double limit = g.timeout / 1000.0;

    int term_all = !g.shell && !g.fired && elapsed >= limit;

    if (term_all) {
        kill(g.cmd, SIGTERM);
        g.fired = 1;
    }
    int kill_all = g.fired && elapsed >= limit + OQTF_GRACE_MS / 1000.0;

    if (kill_all)
        kill(g.cmd, SIGKILL);

    struct oqtf_pids it;

    if (oqtf_pids_open(&it, procs))
        return;

    pid_t pid;

    while (oqtf_pids_next(&it, &pid)) {
        if (pid == g.self || pid == g.cmd)
            continue;

        if (term_all)
            kill(pid, SIGTERM);

        if (kill_all)
            kill(pid, SIGKILL);

        double start = oqtf_start_time(pid);

        if (start >= 0.0 && now - start >= limit)
            kill(pid, SIGKILL);
    }
    close(it.fd);
}

static void
oqtf_sweep(const char *procs)
{
    for (size_t i = 0U; i < OQTF_SWEEP_TRY; i++) {
        struct oqtf_pids it;
        int left = 0;

        if (oqtf_pids_open(&it, procs))
            return;

        pid_t pid;

        while (oqtf_pids_next(&it, &pid)) {
            if (pid == g.self)
                continue;

            kill(pid, SIGKILL);
            left = 1;
        }
        close(it.fd);

        if (!left)
            return;

        oqtf_msleep(100U);
    }
}

int
main(int argc, char **argv)
{
    int a = 1;

    while (a < argc) {
        char *t = argv[a];

        if (!strcmp(t, "-h") || !strcmp(t, "--help")) {
            oqtf_help(argv[0]);
            return 0;
        }
        if (!strcmp(t, "--")) {
            a++;
            break;
        }
        if (!strcmp(t, "-s") || !strcmp(t, "--shell")) {
            g.shell = 1;
            a++;
            continue;
        }
        if (t[0] == '-' && t[1])
            oqtf_fatal(2, "unknown option %s", t);

        if (!g.timeout && !oqtf_time(t, &g.timeout)) {
            a++;
            continue;
        }
        break;
    }
    if (!g.timeout)
        oqtf_fatal(2, "Missing time");

    char *shell_argv[2] = {0};
    char **args = NULL;

    if (argc == a) {
        char *sh = getenv("SHELL");

        if (!sh || !sh[0])
            oqtf_fatal(1, "Missing env SHELL, nothing to exec!");

        shell_argv[0] = sh;
        args = shell_argv;
        g.shell = 1;
    } else {
        args = argv + a;
    }
    g.self = getpid();

    struct oqtf_path self_cg = oqtf_self_cg();
    struct oqtf_path cg_dir = oqtf_path("/sys/fs/cgroup%s/oqtf-%d", self_cg.b, g.self);
    struct oqtf_path cg_procs = oqtf_path("%s/cgroup.procs", cg_dir.b);
    struct oqtf_path self_procs = oqtf_path("/sys/fs/cgroup%s/cgroup.procs", self_cg.b);

    rmdir(cg_dir.b);

    if (mkdir(cg_dir.b, 0755U) < 0)
        oqtf_fatal(1, "%s: %s", cg_dir.b, strerror(errno));

    oqtf_write_file_fmt(cg_procs.b, "%d\n", g.self);

    pid_t pid = fork();

    if (pid < 0)
        oqtf_fatal(1, "fork: %s", strerror(errno));

    if (!pid) {
        unshare(CLONE_NEWCGROUP);
        execvp(args[0], args);
        oqtf_fatal(127, "%s: %s", args[0], strerror(errno));
    }
    struct sigaction sa = {
        .sa_handler = SIG_IGN,
    };
    int sig[] = {SIGHUP, SIGINT, SIGQUIT, SIGPIPE, SIGTERM};

    for (size_t i = 0U; i < OQTF_COUNT(sig); i++)
        sigaction(sig[i], &sa, NULL);

    g.cmd = pid;
    g.cmd_start = oqtf_uptime();

    unsigned long long scan = g.timeout / 4U;

    if (scan < OQTF_SCAN_MIN_MS)
        scan = OQTF_SCAN_MIN_MS;

    if (scan > OQTF_SCAN_MAX_MS)
        scan = OQTF_SCAN_MAX_MS;

    int status = 0;

    for (;;) {
        pid = OQTF_RETRY(waitpid(g.cmd, &status, WNOHANG));

        if (pid == g.cmd || pid < 0)
            break;

        oqtf_cull(cg_procs.b);
        oqtf_msleep(scan);
    }
    oqtf_sweep(cg_procs.b);
    oqtf_write_file_fmt(self_procs.b, "%d\n", g.self);

    if (rmdir(cg_dir.b) < 0)
        oqtf_fatal(1, "%s: %s", cg_dir.b, strerror(errno));

    if (g.fired)
        return 124;

    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);

    return 1;
}
