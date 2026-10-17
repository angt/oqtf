#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define OQTF_SWEEP_TRY  50U
#define OQTF_RMDIR_TRY  20U
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
    int hard;
    struct oqtf_path mount;
    struct oqtf_path base;
    struct oqtf_path rel;
    struct oqtf_path cg;
    struct oqtf_path parent;
} g;

static void oqtf_hardkill(void);

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

static void
oqtf_err(int code, const char *fmt, ...)
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
        OQTF_RETRY(write(2, buf, (size_t)len));
    }
    if (code)
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
        oqtf_err(1, "path too long");

    return p;
}

static ssize_t
oqtf_read_at(int dirfd, const char *name, char *data, size_t size)
{
    int fd = OQTF_RETRY(openat(dirfd, name, O_RDONLY | O_CLOEXEC));

    if (fd < 0)
        return -1;

    ssize_t len = 0;

    while ((size_t)len < size) {
        ssize_t n = OQTF_RETRY(read(fd, data + len, size - (size_t)len));

        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0)
            break;

        len += n;
    }
    close(fd);
    return len;
}

static int
oqtf_write_file(const char *path, const char *data, size_t len)
{
    int fd = OQTF_RETRY(open(path, O_WRONLY));

    if (fd < 0)
        return -1;

    ssize_t n = OQTF_RETRY(write(fd, data, len));
    int saved = errno;
    close(fd);
    errno = saved;

    if (n == (ssize_t)len)
        return 0;

    if (n >= 0)
        errno = EIO;

    return -1;
}

static int
oqtf_write_pid(const char *path, pid_t pid)
{
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", (int)pid);
    return oqtf_write_file(path, buf, (size_t)len);
}

static char *
oqtf_cg_line(char *data)
{
    if (strncmp(data, "0::", 3U))
        return NULL;

    char *rel = data + 3;
    *strchrnul(rel, '\n') = '\0';
    return rel[0] ? rel : (char *)"/";
}

static struct oqtf_path
oqtf_self_cg(void)
{
    char buf[PATH_MAX] = {0};
    ssize_t n = oqtf_read_at(AT_FDCWD, "/proc/self/cgroup", buf, sizeof(buf) - 1U);

    if (n <= 0)
        oqtf_err(1, "Unable to read /proc/self/cgroup");

    char *rel = oqtf_cg_line(buf);

    if (!rel)
        oqtf_err(1, "cgroups v2 not available");

    return oqtf_path("%s", rel);
}

static struct oqtf_path
oqtf_unescape(const char *s)
{
    struct oqtf_path p;
    size_t j = 0U;

    for (size_t i = 0U; s[i] && j + 1U < sizeof(p.b); i++) {
        if (s[i] == '\\' && s[i + 1U] >= '0' && s[i + 1U] <= '7'
                         && s[i + 2U] >= '0' && s[i + 2U] <= '7'
                         && s[i + 3U] >= '0' && s[i + 3U] <= '7') {
            p.b[j++] = (char)(((s[i + 1U] - '0') << 6) |
                              ((s[i + 2U] - '0') << 3) |
                               (s[i + 3U] - '0'));
            i += 3U;
        } else {
            p.b[j++] = s[i];
        }
    }
    p.b[j] = '\0';
    return p;
}

static struct oqtf_path
oqtf_cgroup_mount(void)
{
    char buf[16384] = {0};
    ssize_t n = oqtf_read_at(AT_FDCWD, "/proc/self/mountinfo", buf, sizeof(buf) - 1U);

    if (n <= 0)
        oqtf_err(1, "Unable to read /proc/self/mountinfo");

    struct oqtf_path found = {{0}};
    int have_cgroup2 = 0;

    for (char *line = buf; *line;) {
        char *nl = strchr(line, '\n');

        if (nl)
            *nl = '\0';

        char *root = line;

        for (int i = 0; i < 3; i++) {
            root = strchr(root, ' ');

            if (!root)
                break;

            *root++ = '\0';
        }
        char *mnt = root ? strchr(root, ' ') : NULL;

        if (mnt)
            *mnt++ = '\0';

        char *rest = mnt ? strchr(mnt, ' ') : NULL;

        if (rest) {
            *rest++ = '\0';
            char *sep = strstr(rest, " - ");

            if (sep && !strncmp(sep + 3, "cgroup2 ", 8U)) {
                have_cgroup2 = 1;
                struct oqtf_path r = oqtf_unescape(root);

                if (!strcmp(r.b, "/")) {
                    found = oqtf_unescape(mnt);
                }
            }
        }
        if (!nl)
            break;

        line = nl + 1;
    }
    if (!found.b[0]) {
        if (have_cgroup2)
            oqtf_err(1, "no cgroup2 mount for this cgroup namespace");

        oqtf_err(1, "no cgroup2 mount");
    }
    return found;
}

static void
oqtf_hide_cgroup_mounts(void)
{
    char buf[16384] = {0};
    ssize_t n = oqtf_read_at(AT_FDCWD, "/proc/self/mountinfo", buf, sizeof(buf) - 1U);

    if (n <= 0)
        oqtf_err(1, "Unable to read /proc/self/mountinfo");

    for (char *line = buf; *line;) {
        char *nl = strchr(line, '\n');

        if (nl)
            *nl = '\0';

        char *mnt = line;

        for (int i = 0; i < 4; i++) {
            mnt = strchr(mnt, ' ');

            if (!mnt)
                break;

            *mnt++ = '\0';
        }
        char *rest = mnt ? strchr(mnt, ' ') : NULL;

        if (rest) {
            *rest++ = '\0';
            char *sep = strstr(rest, " - ");

            if (sep && !strncmp(sep + 3, "cgroup2 ", 8U)) {
                struct oqtf_path point = oqtf_unescape(mnt);

                if (strcmp(point.b, "/") &&
                    mount("tmpfs", point.b, "tmpfs", 0U, NULL) < 0)
                    oqtf_err(1, "%s: %s", point.b, strerror(errno));
            }
        }
        if (!nl)
            break;

        line = nl + 1;
    }
}

static struct oqtf_path
oqtf_cg_path(const char *rel)
{
    if (!strcmp(rel, "/"))
        return oqtf_path("%s", g.mount.b);

    return oqtf_path("%s%s", g.mount.b, rel);
}

static double
oqtf_uptime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static int
oqtf_time(const char *s, unsigned long long *out)
{
    unsigned long long total = 0;
    char *end = NULL;
    int first = 1;

    for (;;) {
        if (s[0] < '0' || s[0] > '9')
            return -1;

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
        if (v > ULLONG_MAX / mult || total > ULLONG_MAX - v * mult)
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
oqtf_pids_next(struct oqtf_pids *it, pid_t *pid)
{
    unsigned long long value = 0ULL;
    int have = 0;

    for (;;) {
        if (it->pos == it->len) {
            ssize_t n = OQTF_RETRY(read(it->fd, it->buf, sizeof(it->buf)));

            if (n < 0)
                return -1;

            if (n == 0) {
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

struct oqtf_proc {
    pid_t pid;
    int fd;
};

static int
oqtf_proc_open(struct oqtf_proc *p, pid_t pid)
{
    struct oqtf_path path = oqtf_path("/proc/%d", (int)pid);
    p->pid = pid;
    p->fd = OQTF_RETRY(open(path.b, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    return p->fd < 0 ? -1 : 0;
}

static void
oqtf_proc_close(struct oqtf_proc *p)
{
    close(p->fd);
}

static int
oqtf_proc_kill(struct oqtf_proc *p, int sig)
{
    return kill(p->pid, sig);
}

static int
oqtf_proc_member(struct oqtf_proc *p)
{
    char buf[PATH_MAX] = {0};

    if (oqtf_read_at(p->fd, "cgroup", buf, sizeof(buf) - 1U) <= 0)
        return 1;

    char *rel = oqtf_cg_line(buf);

    if (!rel)
        return 1;

    size_t len = strlen(g.rel.b);

    return !strcmp(rel, g.rel.b) ||
           (!strncmp(rel, g.rel.b, len) && rel[len] == '/');
}

static int
oqtf_proc_stat(struct oqtf_proc *p, char *state, double *start)
{
    char buf[PATH_MAX] = {0};

    if (oqtf_read_at(p->fd, "stat", buf, sizeof(buf) - 1U) <= 0)
        return -1;

    char *s = strrchr(buf, ')');

    if (!s)
        return -1;

    s += 2;

    if (state)
        *state = s[0];

    if (!start)
        return 0;

    char *t = s;

    for (int i = OQTF_STAT_STATE; i < OQTF_STAT_START; i++) {
        if (!(t = strchr(t, ' ')))
            return -1;
        t++;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long ticks = strtoull(t, &end, 10);

    if (errno || end == t)
        return -1;

    long clk = sysconf(_SC_CLK_TCK);

    if (clk <= 0)
        return -1;

    *start = (double)ticks / (double)clk;
    return 0;
}

static void
oqtf_monitor_warn(const char *what)
{
    static int warned;

    if (!warned) {
        warned = 1;
        oqtf_err(0, "unable to monitor the cgroup: %s: %s",
                    what, strerror(errno));
    }
}

static void
oqtf_cull_pid(pid_t pid, double now, double limit, int term_all, int kill_all)
{
    struct oqtf_proc p;

    if (oqtf_proc_open(&p, pid) < 0)
        return;

    if (oqtf_proc_member(&p)) {
        if (term_all)
            oqtf_proc_kill(&p, SIGTERM);

        if (kill_all)
            oqtf_proc_kill(&p, SIGKILL);

        double start = -1.0;

        if (!oqtf_proc_stat(&p, NULL, &start) && now - start >= limit)
            oqtf_proc_kill(&p, SIGKILL);
    }
    oqtf_proc_close(&p);
}

static void
oqtf_cull_dir(int dirfd, double now, double limit, int term_all, int kill_all)
{
    int fd = OQTF_RETRY(openat(dirfd, "cgroup.procs", O_RDONLY | O_CLOEXEC));

    if (fd < 0) {
        oqtf_monitor_warn("cgroup.procs");
    } else {
        struct oqtf_pids it = {.fd = fd};
        pid_t pid;
        int r;

        while ((r = oqtf_pids_next(&it, &pid)) > 0) {
            if (pid == g.self || pid == g.cmd)
                continue;

            oqtf_cull_pid(pid, now, limit, term_all, kill_all);
        }
        if (r < 0)
            oqtf_monitor_warn("cgroup.procs");

        close(fd);
    }
    int dupfd = dup(dirfd);
    DIR *d = dupfd < 0 ? NULL : fdopendir(dupfd);

    if (!d) {
        if (dupfd >= 0)
            close(dupfd);
        return;
    }
    struct dirent *e;

    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;

        struct stat st;

        if (fstatat(dirfd, e->d_name, &st, 0) < 0 || !S_ISDIR(st.st_mode))
            continue;

        int sub = OQTF_RETRY(openat(dirfd, e->d_name,
                                    O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (sub < 0)
            continue;

        oqtf_cull_dir(sub, now, limit, term_all, kill_all);
        close(sub);
    }
    closedir(d);
}

static void
oqtf_cull(void)
{
    double now = oqtf_uptime();
    double elapsed = now - g.cmd_start;
    double limit = g.timeout / 1000.0;
    int term_all = !g.shell && !g.fired && elapsed >= limit;

    if (term_all) {
        kill(g.cmd, SIGTERM);
        g.fired = 1;
    }
    if (g.fired && !g.hard &&
        elapsed >= limit + OQTF_GRACE_MS / 1000.0) {
        g.hard = 1;
        oqtf_hardkill();
    }
    int kill_all = g.hard;
    int dirfd = OQTF_RETRY(open(g.cg.b, O_RDONLY | O_DIRECTORY | O_CLOEXEC));

    if (dirfd < 0) {
        oqtf_monitor_warn(g.cg.b);
        return;
    }
    oqtf_cull_dir(dirfd, now, limit, term_all, kill_all);
    close(dirfd);
}

static int
oqtf_sweep_dir(int dirfd)
{
    int left = 0;
    int fd = OQTF_RETRY(openat(dirfd, "cgroup.procs", O_RDONLY | O_CLOEXEC));

    if (fd >= 0) {
        struct oqtf_pids it = {.fd = fd};
        pid_t pid;

        while (oqtf_pids_next(&it, &pid) > 0) {
            if (pid == g.self)
                continue;

            struct oqtf_proc p;

            if (oqtf_proc_open(&p, pid) < 0)
                continue;

            if (oqtf_proc_member(&p)) {
                char state = 0;

                oqtf_proc_stat(&p, &state, NULL);

                if (state != 'Z') {
                    left++;
                    oqtf_proc_kill(&p, SIGKILL);
                }
            }
            oqtf_proc_close(&p);
        }
        close(fd);
    }
    int dupfd = dup(dirfd);
    DIR *d = dupfd < 0 ? NULL : fdopendir(dupfd);

    if (!d) {
        if (dupfd >= 0)
            close(dupfd);
        return left;
    }
    struct dirent *e;

    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;

        struct stat st;

        if (fstatat(dirfd, e->d_name, &st, 0) < 0 || !S_ISDIR(st.st_mode))
            continue;

        int sub = OQTF_RETRY(openat(dirfd, e->d_name,
                                    O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (sub < 0)
            continue;

        left += oqtf_sweep_dir(sub);
        close(sub);
    }
    closedir(d);
    return left;
}

static int
oqtf_cg_kill(void)
{
    struct oqtf_path path = oqtf_path("%s/cgroup.kill", g.cg.b);
    return oqtf_write_file(path.b, "1", 1U);
}

static int
oqtf_move_self_out(void)
{
    struct oqtf_path path = oqtf_path("%s/cgroup.procs", g.parent.b);
    return oqtf_write_pid(path.b, g.self);
}

static void
oqtf_hardkill(void)
{
    if (oqtf_move_self_out() == 0 && oqtf_cg_kill() == 0)
        return;

    int dirfd = OQTF_RETRY(open(g.cg.b, O_RDONLY | O_DIRECTORY | O_CLOEXEC));

    if (dirfd >= 0) {
        oqtf_sweep_dir(dirfd);
        close(dirfd);
    }
}

static void
oqtf_sweep(void)
{
    int left = 0;
    oqtf_cg_kill();

    for (size_t i = 0U; i < OQTF_SWEEP_TRY; i++) {
        int dirfd = OQTF_RETRY(open(g.cg.b, O_RDONLY | O_DIRECTORY | O_CLOEXEC));

        if (dirfd < 0)
            return;

        left = oqtf_sweep_dir(dirfd);
        close(dirfd);

        if (!left)
            return;

        oqtf_msleep(100U);
    }
    oqtf_err(0, "unable to terminate %d process(es) left in %s", left, g.cg.b);
}

static void
oqtf_rm_children(int dirfd)
{
    int dupfd = dup(dirfd);
    DIR *d = dupfd < 0 ? NULL : fdopendir(dupfd);

    if (!d) {
        if (dupfd >= 0)
            close(dupfd);
        return;
    }
    struct dirent *e;

    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;

        struct stat st;

        if (fstatat(dirfd, e->d_name, &st, 0) < 0 || !S_ISDIR(st.st_mode))
            continue;

        int sub = OQTF_RETRY(openat(dirfd, e->d_name,
                                    O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (sub < 0)
            continue;

        oqtf_rm_children(sub);
        close(sub);
        unlinkat(dirfd, e->d_name, AT_REMOVEDIR);
    }
    closedir(d);
}

static int
oqtf_teardown(void)
{
    if (oqtf_move_self_out() < 0)
        oqtf_err(0, "%s: %s", g.parent.b, strerror(errno));

    oqtf_sweep();

    for (size_t i = 0U; i < OQTF_RMDIR_TRY; i++) {
        int dirfd = OQTF_RETRY(open(g.cg.b, O_RDONLY | O_DIRECTORY | O_CLOEXEC));

        if (dirfd >= 0) {
            oqtf_rm_children(dirfd);
            close(dirfd);
        }
        if (!rmdir(g.cg.b))
            return 0;

        oqtf_msleep(50U);
    }
    return -1;
}

static void
oqtf_sandbox(void)
{
    if (unshare(CLONE_NEWCGROUP) < 0) {
        if (errno != EPERM && errno != EACCES)
            oqtf_err(0, "cgroup namespace: %s, the command is not confined",
                     strerror(errno));
        return;
    }
    if (unshare(CLONE_NEWNS) < 0 ||
        mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        oqtf_err(1, "cgroup sandbox: %s", strerror(errno));

    if (!strcmp(g.mount.b, "/"))
        oqtf_err(1, "cgroup sandbox: cgroup2 is mounted at /");

    oqtf_hide_cgroup_mounts();

    if (mount("cgroup2", g.mount.b, "cgroup2", 0U, NULL) < 0)
        oqtf_err(1, "cgroup sandbox: %s: %s", g.mount.b, strerror(errno));
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
            oqtf_err(2, "unknown option %s", t);

        if (!g.timeout && !oqtf_time(t, &g.timeout)) {
            a++;
            continue;
        }
        break;
    }
    if (!g.timeout)
        oqtf_err(2, "Missing time");

    char *shell_argv[2] = {0};
    char **args = NULL;

    if (argc == a) {
        char *sh = getenv("SHELL");

        if (!sh || !sh[0])
            oqtf_err(1, "Missing env SHELL, nothing to exec!");

        shell_argv[0] = sh;
        args = shell_argv;
        g.shell = 1;
    } else {
        args = argv + a;
    }
    g.self = getpid();
    g.mount = oqtf_cgroup_mount();
    g.base = oqtf_self_cg();

    struct oqtf_path name = oqtf_path("oqtf-%d", (int)g.self);
    struct oqtf_path rel = oqtf_path("%s%s%s", g.base.b,
                                     strcmp(g.base.b, "/") ? "/" : "",
                                     name.b);
    g.cg = oqtf_cg_path(rel.b);
    g.parent = oqtf_cg_path(g.base.b);
    rmdir(g.cg.b);

    if (mkdir(g.cg.b, 0755U) < 0)
        oqtf_err(1, "%s: %s", g.cg.b, strerror(errno));

    struct oqtf_path procs = oqtf_path("%s/cgroup.procs", g.cg.b);

    if (oqtf_write_pid(procs.b, g.self) < 0) {
        int saved = errno;
        rmdir(g.cg.b);
        oqtf_err(1, "%s: %s", procs.b, strerror(saved));
    }
    g.rel = oqtf_self_cg();

    if (strcmp(g.rel.b, rel.b)) {
        oqtf_move_self_out();
        rmdir(g.cg.b);
        oqtf_err(1, "cgroup namespace and cgroup2 mount disagree: %s", g.rel.b);
    }
    struct sigaction sa_old = {0};
    struct sigaction sa_dfl = {.sa_handler = SIG_DFL};
    sigaction(SIGCHLD, NULL, &sa_old);
    sigaction(SIGCHLD, &sa_dfl, NULL);
    pid_t pid = fork();

    if (pid < 0) {
        int saved = errno;
        oqtf_move_self_out();
        rmdir(g.cg.b);
        oqtf_err(1, "fork: %s", strerror(saved));
    }
    if (!pid) {
        sigaction(SIGCHLD, &sa_old, NULL);
        oqtf_sandbox();
        execvp(args[0], args);
        oqtf_err(127, "%s: %s", args[0], strerror(errno));
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
    int wait_err = 0;
    int bail = 0;

    for (;;) {
        pid_t r = OQTF_RETRY(waitpid(g.cmd, &status, WNOHANG));

        if (r == g.cmd)
            break;

        if (r < 0) {
            wait_err = errno;
            break;
        }
        double elapsed = oqtf_uptime() - g.cmd_start;

        if (g.fired &&
            elapsed >= g.timeout / 1000.0 + 3.0 * (OQTF_GRACE_MS / 1000.0)) {
            bail = 1;
            break;
        }
        oqtf_cull();
        oqtf_msleep(scan);
    }
    int td = oqtf_teardown();

    if (wait_err)
        oqtf_err(1, "waitpid: %s", strerror(wait_err));

    if (bail)
        oqtf_err(1, "unable to terminate the command");

    if (td < 0)
        oqtf_err(1, "%s: %s", g.cg.b, strerror(errno));

    if (g.fired)
        return 124;

    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);

    return 1;
}
