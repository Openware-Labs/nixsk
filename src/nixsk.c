#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <pwd.h>
#include <dirent.h>
#include <limits.h>
#include <stddef.h>

#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <ncurses.h>

#define NIXSK_VERSION  "1.1"
#define MAX_NAME       128
#define MAX_PATH       512
#define MAX_EXEC       512
#define MAX_ENTRIES    512
#define REG_LINE       1400

typedef struct {
    char name[MAX_NAME];
    char path[MAX_PATH];
    pid_t pid;
    time_t created;
    char exec_cmd[MAX_EXEC];
    int managed;
    int is_abstract;
} SocketEntry;

typedef struct {
    SocketEntry entries[MAX_ENTRIES];
    int count;
} Registry;

static char g_registry_path[MAX_PATH];
static char g_last_error[512];

static volatile sig_atomic_t g_stop = 0;
static char g_socket_path[MAX_PATH];
static char g_socket_name[MAX_NAME];

static void seterr(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
    va_end(ap);
}

static void sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;

    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

static int path_parent(const char *path, char *out, size_t outsz)
{
    if (!path || !*path || !out || outsz == 0)
        return -1;

    if (strlen(path) >= outsz)
        return -1;

    strcpy(out, path);

    char *slash = strrchr(out, '/');

    if (!slash) {
        strcpy(out, ".");
    } else if (slash == out) {
        out[1] = '\0';
    } else {
        *slash = '\0';
    }

    return 0;
}

static int valid_socket_path(const char *path)
{
    if (!path || !*path)
        return 0;

    const char *p = path;

    while (*p) {
        while (*p == '/')
            p++;

        if (!*p)
            break;

        const char *start = p;

        while (*p && *p != '/')
            p++;

        size_t len = (size_t)(p - start);

        if ((len == 1 && start[0] == '.') ||
            (len == 2 && start[0] == '.' && start[1] == '.'))
            return 0;
    }

    return 1;
}

static int valid_socket_name(const char *name)
{
    if (!name || !*name)
        return 0;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;

    if (strlen(name) >= MAX_NAME)
        return 0;

    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;

        if (c == '/' || c == '\t' || c == '\n' || c == '\r')
            return 0;
    }

    return 1;
}

static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[MAX_PATH];
    size_t len;

    if (!path || !*path)
        return -1;

    len = strlen(path);

    if (len >= sizeof(tmp))
        return -1;

    memcpy(tmp, path, len + 1);

    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;

        *p = '\0';

        if (mkdir(tmp, mode) < 0 && errno != EEXIST)
            return -1;

        struct stat st;

        if (stat(tmp, &st) < 0 || !S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            return -1;
        }

        *p = '/';
    }

    if (mkdir(tmp, mode) < 0 && errno != EEXIST)
        return -1;

    struct stat st;

    if (stat(tmp, &st) < 0 || !S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }

    return 0;
}

static int set_registry_path(const char *fmt, const char *value)
{
    int n = snprintf(g_registry_path,
                     sizeof(g_registry_path),
                     fmt, value);

    if (n < 0 || (size_t)n >= sizeof(g_registry_path)) {
        seterr("registry path is too long");
        return -1;
    }

    return 0;
}

static int registry_path_init(void)
{
    const char *home = getenv("NIXSK_HOME");

    if (home && *home)
        return set_registry_path("%s/registry", home);

    const char *xdg = getenv("XDG_CONFIG_HOME");

    if (xdg && *xdg)
        return set_registry_path("%s/nixsk/registry", xdg);

    const char *u = getenv("HOME");

    if (!u || !*u) {
        struct passwd *pw = getpwuid(getuid());

        if (pw && pw->pw_dir)
            u = pw->pw_dir;
    }

    if (u && *u)
        return set_registry_path("%s/.config/nixsk/registry", u);

    int n = snprintf(g_registry_path,
                     sizeof(g_registry_path),
                     "/tmp/nixsk-registry-%d",
                     (int)getuid());

    if (n < 0 || (size_t)n >= sizeof(g_registry_path)) {
        seterr("registry path is too long");
        return -1;
    }

    return 0;
}

static int valid_tsv_field(const char *s)
{
    if (!s)
        return 0;

    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;

        if (c == '\t' || c == '\n' || c == '\r')
            return 0;
    }

    return 1;
}

static int process_alive(pid_t pid)
{
    if (pid <= 0)
        return 0;

    if (kill(pid, 0) == 0)
        return 1;

    return errno == EPERM;
}

static int registry_lock(void)
{
    char lockpath[MAX_PATH];
    char dir[MAX_PATH];

    int n = snprintf(lockpath, sizeof(lockpath),
                     "%s.lock", g_registry_path);

    if (n < 0 || (size_t)n >= sizeof(lockpath)) {
        seterr("registry lock path is too long");
        return -1;
    }

    if (path_parent(g_registry_path, dir, sizeof(dir)) < 0) {
        seterr("invalid registry path");
        return -1;
    }

    if (mkdir_p(dir, 0700) < 0) {
        seterr("cannot create registry directory: %s",
               strerror(errno));
        return -1;
    }

    int fd = open(lockpath, O_CREAT | O_RDWR, 0600);

    if (fd < 0) {
        seterr("cannot open registry lock: %s",
               strerror(errno));
        return -1;
    }

    struct flock lock;
    memset(&lock, 0, sizeof(lock));

    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;

    while (fcntl(fd, F_SETLKW, &lock) < 0) {
        if (errno == EINTR)
            continue;

        seterr("cannot lock registry: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static void registry_unlock(int fd)
{
    if (fd < 0)
        return;

    struct flock lock;
    memset(&lock, 0, sizeof(lock));

    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;

    fcntl(fd, F_SETLK, &lock);
    close(fd);
}

static int split_tsv(char *line, char **fields, int max_fields)
{
    int count = 1;

    if (!line || !fields || max_fields <= 0)
        return -1;

    fields[0] = line;

    for (char *p = line; *p; p++) {
        if (*p != '\t')
            continue;

        if (count >= max_fields)
            return -1;

        *p = '\0';
        fields[count++] = p + 1;
    }

    return count;
}

static int parse_integer(const char *s, intmax_t *out)
{
    char *end = NULL;

    if (!s || !*s)
        return -1;

    errno = 0;

    intmax_t value = strtoimax(s, &end, 10);

    if (errno == ERANGE || !end || *end != '\0')
        return -1;

    *out = value;
    return 0;
}

static int registry_load(Registry *r)
{
    r->count = 0;

    FILE *f = fopen(g_registry_path, "r");

    if (!f) {
        if (errno == ENOENT)
            return 0;

        seterr("cannot read registry: %s", strerror(errno));
        return -1;
    }

    char line[REG_LINE];
    int result = 0;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);

        if (len == 0)
            continue;

        if (line[len - 1] != '\n' && !feof(f)) {
            seterr("registry contains an oversized line");
            result = -1;
            break;
        }

        while (len > 0 &&
               (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (len == 0)
            continue;

        char *fields[5];

        int count = split_tsv(line, fields, 5);

        if (count != 5) {
            seterr("registry contains a malformed entry");
            result = -1;
            break;
        }

        if (!fields[0][0] || !fields[1][0] ||
            strlen(fields[0]) >= MAX_NAME ||
            strlen(fields[1]) >= MAX_PATH ||
            strlen(fields[4]) >= MAX_EXEC ||
            !valid_tsv_field(fields[0]) ||
            !valid_tsv_field(fields[1]) ||
            !valid_tsv_field(fields[4]) ||
            !valid_socket_name(fields[0]) ||
            !valid_socket_path(fields[1])) {
            seterr("registry contains an invalid entry");
            result = -1;
            break;
        }

        intmax_t pid_value;
        intmax_t created_value;

        if (parse_integer(fields[2], &pid_value) < 0 ||
            parse_integer(fields[3], &created_value) < 0) {
            seterr("registry contains an invalid numeric field");
            result = -1;
            break;
        }

        pid_t pid = (pid_t)pid_value;
        time_t created = (time_t)created_value;

        if ((intmax_t)pid != pid_value ||
            (intmax_t)created != created_value ||
            pid_value <= 0) {
            seterr("registry contains an out-of-range value");
            result = -1;
            break;
        }

        if (r->count >= MAX_ENTRIES) {
            seterr("registry entry limit exceeded");
            result = -1;
            break;
        }

        SocketEntry *e = &r->entries[r->count];

        memset(e, 0, sizeof(*e));

        strcpy(e->name, fields[0]);
        strcpy(e->path, fields[1]);
        strcpy(e->exec_cmd, fields[4]);

        e->pid = pid;
        e->created = created;
        e->managed = 1;
        e->is_abstract = 0;

        r->count++;
    }

    if (ferror(f)) {
        seterr("error reading registry: %s", strerror(errno));
        result = -1;
    }

    if (fclose(f) != 0 && result == 0) {
        seterr("error closing registry: %s", strerror(errno));
        result = -1;
    }

    if (result < 0)
        r->count = 0;

    return result;
}

static int registry_save(Registry *r)
{
    char dir[MAX_PATH];
    char tmp[MAX_PATH];

    if (r->count < 0 || r->count > MAX_ENTRIES) {
        seterr("invalid registry entry count");
        return -1;
    }

    if (path_parent(g_registry_path, dir, sizeof(dir)) < 0) {
        seterr("invalid registry path");
        return -1;
    }

    if (mkdir_p(dir, 0700) < 0) {
        seterr("cannot create registry directory: %s",
               strerror(errno));
        return -1;
    }

    int n = snprintf(tmp, sizeof(tmp),
                     "%s.tmp.XXXXXX", g_registry_path);

    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        seterr("temporary registry path is too long");
        return -1;
    }

    int fd = mkstemp(tmp);

    if (fd < 0) {
        seterr("cannot create temporary registry: %s",
               strerror(errno));
        return -1;
    }

    if (fchmod(fd, 0600) < 0) {
        seterr("cannot set registry permissions: %s",
               strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }

    FILE *f = fdopen(fd, "w");

    if (!f) {
        seterr("cannot open temporary registry stream: %s",
               strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }

    int failed = 0;

    for (int i = 0; i < r->count; i++) {
        SocketEntry *e = &r->entries[i];

        if (!e->managed)
            continue;

        if (!valid_socket_name(e->name) ||
            !valid_socket_path(e->path) ||
            !valid_tsv_field(e->exec_cmd)) {
            seterr("registry entry contains invalid fields");
            failed = 1;
            break;
        }

        if (fprintf(f, "%s\t%s\t%jd\t%jd\t%s\n",
                    e->name,
                    e->path,
                    (intmax_t)e->pid,
                    (intmax_t)e->created,
                    e->exec_cmd) < 0) {
            seterr("cannot write registry: %s", strerror(errno));
            failed = 1;
            break;
        }
    }

    if (!failed && fflush(f) != 0) {
        seterr("cannot flush registry: %s", strerror(errno));
        failed = 1;
    }

    if (!failed && fsync(fd) < 0) {
        seterr("cannot synchronize registry: %s", strerror(errno));
        failed = 1;
    }

    if (fclose(f) != 0 && !failed) {
        seterr("cannot close registry: %s", strerror(errno));
        failed = 1;
    }

    if (failed) {
        unlink(tmp);
        return -1;
    }

    if (rename(tmp, g_registry_path) < 0) {
        seterr("cannot replace registry: %s", strerror(errno));
        unlink(tmp);
        return -1;
    }

    int dirfd = open(dir, O_RDONLY);

    if (dirfd >= 0) {
        (void)fsync(dirfd);
        close(dirfd);
    }

    return 0;
}

static SocketEntry *registry_find(Registry *r, const char *name)
{
    for (int i = 0; i < r->count; i++) {
        if (r->entries[i].managed &&
            strcmp(r->entries[i].name, name) == 0)
            return &r->entries[i];
    }

    return NULL;
}

static int registry_remove(Registry *r, const char *name)
{
    for (int i = 0; i < r->count; i++) {
        if (r->entries[i].managed &&
            strcmp(r->entries[i].name, name) == 0) {
            memmove(&r->entries[i],
                    &r->entries[i + 1],
                    (size_t)(r->count - i - 1) * sizeof(SocketEntry));

            r->count--;
            return 0;
        }
    }

    return -1;
}

static void add_external_entry(Registry *r,
                               const char *path,
                               int is_abstract)
{
    if (!path || !*path || strlen(path) >= MAX_PATH)
        return;

    for (int i = 0; i < r->count; i++) {
        if (strcmp(r->entries[i].path, path) == 0)
            return;
    }

    if (r->count >= MAX_ENTRIES)
        return;

    SocketEntry *e = &r->entries[r->count++];

    memset(e, 0, sizeof(*e));

    const char *base = strrchr(path, '/');

    if (base && base[1])
        snprintf(e->name, sizeof(e->name), "%s", base + 1);
    else
        snprintf(e->name, sizeof(e->name), "%s", path);

    snprintf(e->path, sizeof(e->path), "%s", path);

    e->managed = 0;
    e->is_abstract = is_abstract;
}

static void scan_dir_for_sockets(Registry *r, const char *dir)
{
    DIR *d = opendir(dir);

    if (!d)
        return;

    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        char path[MAX_PATH];

        int n = snprintf(path, sizeof(path),
                         "%s/%s", dir, ent->d_name);

        if (n < 0 || (size_t)n >= sizeof(path))
            continue;

        struct stat st;

        if (lstat(path, &st) == 0 && S_ISSOCK(st.st_mode))
            add_external_entry(r, path, 0);
    }

    closedir(d);
}

static void scan_system(Registry *r)
{
    const char *dirs[] = {
        "/tmp",
        "/var/tmp",
        "/run",
        "/var/run",
        "/tmp/.X11-unix",
        "/var/run/dbus",
        "/run/dbus",
        NULL
    };

    for (int i = 0; dirs[i]; i++)
        scan_dir_for_sockets(r, dirs[i]);

    char user_dir[MAX_PATH];

    snprintf(user_dir, sizeof(user_dir),
             "/run/user/%d", (int)getuid());

    scan_dir_for_sockets(r, user_dir);

    snprintf(user_dir, sizeof(user_dir),
             "/var/run/user/%d", (int)getuid());

    scan_dir_for_sockets(r, user_dir);

#ifdef __linux__
    FILE *f = fopen("/proc/net/unix", "r");

    if (f) {
        char line[512];

        (void)fgets(line, sizeof(line), f);

        while (fgets(line, sizeof(line), f)) {
            char *p = strchr(line, '@');

            if (!p)
                continue;

            p[strcspn(p, "\r\n")] = '\0';

            add_external_entry(r, p, 1);
        }

        fclose(f);
    }
#endif
}

static void sig_handler(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void install_daemon_sigactions(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);
}

static void close_all_fds_except2(int keep1, int keep2)
{
    long maxfd = sysconf(_SC_OPEN_MAX);

    if (maxfd < 0 || maxfd > 65536)
        maxfd = 1024;

    for (long i = 3; i < maxfd; i++) {
        if ((int)i != keep1 && (int)i != keep2)
            close((int)i);
    }
}

static void daemon_cleanup(void)
{
    if (g_socket_path[0])
        unlink(g_socket_path);

    if (!g_socket_name[0])
        return;

    int lockfd = registry_lock();

    if (lockfd < 0)
        return;

    Registry r;

    if (registry_load(&r) == 0) {
        if (registry_remove(&r, g_socket_name) == 0)
            registry_save(&r);
    }

    registry_unlock(lockfd);
}

static int accept_loop(int listen_fd, const char *exec_cmd)
{
    install_daemon_sigactions();

    while (!g_stop) {
        if (listen_fd >= FD_SETSIZE) {
            seterr("socket descriptor exceeds FD_SETSIZE");
            break;
        }

        fd_set fds;

        FD_ZERO(&fds);
        FD_SET(listen_fd, &fds);

        struct timeval tv;

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int n = select(listen_fd + 1, &fds, NULL, NULL, &tv);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            break;
        }

        if (n == 0)
            continue;

        int conn = accept(listen_fd, NULL, NULL);

        if (conn < 0) {
            if (errno == EINTR)
                continue;

            break;
        }

        if (exec_cmd && *exec_cmd) {
            pid_t p = fork();

            if (p == 0) {
                close(listen_fd);

                if (dup2(conn, STDIN_FILENO) < 0 ||
                    dup2(conn, STDOUT_FILENO) < 0 ||
                    dup2(conn, STDERR_FILENO) < 0) {
                    _exit(126);
                }

                if (conn > STDERR_FILENO)
                    close(conn);

                execl("/bin/sh", "sh", "-c",
                      exec_cmd, (char *)NULL);

                _exit(127);
            }

            close(conn);
        } else {
            char buf[4096];
            ssize_t r;

            while ((r = read(conn, buf, sizeof(buf))) > 0) {
                ssize_t off = 0;

                while (off < r) {
                    ssize_t w = write(conn, buf + off,
                                      (size_t)(r - off));

                    if (w < 0 && errno == EINTR)
                        continue;

                    if (w <= 0)
                        break;

                    off += w;
                }

                if (off < r)
                    break;
            }

            close(conn);
        }
    }

    close(listen_fd);
    daemon_cleanup();

    return 0;
}

static int build_path(char *out, size_t outsz,
                      const char *dir, const char *name)
{
    if (!dir || !*dir)
        dir = "/tmp";

    if (!name || !*name)
        return -1;

    size_t dl = strlen(dir);

    while (dl > 1 && dir[dl - 1] == '/')
        dl--;

    int n;

    if (dl == 1 && dir[0] == '/') {
        n = snprintf(out, outsz, "/%s", name);
    } else {
        n = snprintf(out, outsz, "%.*s/%s",
                     (int)dl, dir, name);
    }

    if (n < 0 || (size_t)n >= outsz)
        return -1;

    return 0;
}

static int do_create(const char *name,
                     const char *path,
                     const char *exec_cmd,
                     int foreground)
{
    g_last_error[0] = '\0';

    if (!valid_socket_name(name)) {
        seterr("invalid socket name");
        return 1;
    }

    if (!path || !*path) {
        seterr("empty socket path");
        return 1;
    }

    if (strlen(path) >= MAX_PATH) {
        seterr("socket path is too long");
        return 1;
    }

    if (!valid_socket_path(path)) {
        seterr("socket path contains an invalid component");
        return 1;
    }

    if (exec_cmd &&
        (strlen(exec_cmd) >= MAX_EXEC ||
         !valid_tsv_field(exec_cmd))) {
        seterr("exec command is too long or contains invalid characters");
        return 1;
    }

    size_t maxsp = sizeof(((struct sockaddr_un *)0)->sun_path) - 1;

    if (strlen(path) > maxsp) {
        seterr("socket path too long (%zu > %zu)",
               strlen(path), maxsp);
        return 1;
    }

    char dirpath[MAX_PATH];

    if (path_parent(path, dirpath, sizeof(dirpath)) < 0) {
        seterr("invalid socket directory");
        return 1;
    }

    if (mkdir_p(dirpath, 0755) < 0) {
        seterr("cannot create socket directory: %s",
               strerror(errno));
        return 1;
    }

    int lockfd = registry_lock();

    if (lockfd < 0)
        return 1;

    Registry r;

    if (registry_load(&r) < 0) {
        registry_unlock(lockfd);
        return 1;
    }

    if (registry_find(&r, name)) {
        seterr("socket '%s' already registered", name);
        registry_unlock(lockfd);
        return 1;
    }

    if (r.count >= MAX_ENTRIES) {
        seterr("registry is full");
        registry_unlock(lockfd);
        return 1;
    }

    struct stat existing;

    if (lstat(path, &existing) == 0) {
        seterr("%s already exists", path);
        registry_unlock(lockfd);
        return 1;
    }

    if (errno != ENOENT) {
        seterr("cannot inspect socket path: %s", strerror(errno));
        registry_unlock(lockfd);
        return 1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0) {
        seterr("socket(): %s", strerror(errno));
        registry_unlock(lockfd);
        return 1;
    }

    struct sockaddr_un addr;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    memcpy(addr.sun_path, path, strlen(path) + 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        seterr("bind(%s): %s", path, strerror(errno));
        close(fd);
        registry_unlock(lockfd);
        return 1;
    }

    if (listen(fd, 16) < 0) {
        seterr("listen(): %s", strerror(errno));
        unlink(path);
        close(fd);
        registry_unlock(lockfd);
        return 1;
    }

    pid_t server_pid = getpid();
    int ready_pipe[2] = {-1, -1};

    if (!foreground) {
        if (pipe(ready_pipe) < 0) {
            seterr("pipe(): %s", strerror(errno));
            unlink(path);
            close(fd);
            registry_unlock(lockfd);
            return 1;
        }

        server_pid = fork();

        if (server_pid < 0) {
            seterr("fork(): %s", strerror(errno));
            close(ready_pipe[0]);
            close(ready_pipe[1]);
            unlink(path);
            close(fd);
            registry_unlock(lockfd);
            return 1;
        }

        if (server_pid == 0) {
            close(ready_pipe[1]);

            if (setsid() < 0) {
                close(ready_pipe[0]);
                close(fd);
                _exit(1);
            }

            close_all_fds_except2(fd, ready_pipe[0]);

            int devnull = open("/dev/null", O_RDWR);

            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);

                if (devnull > STDERR_FILENO)
                    close(devnull);
            }

            char ready = 0;
            ssize_t got;

            do {
                got = read(ready_pipe[0], &ready, 1);
            } while (got < 0 && errno == EINTR);

            close(ready_pipe[0]);

            if (got != 1 || ready != 'Y') {
                close(fd);
                unlink(path);
                _exit(1);
            }

            snprintf(g_socket_path, sizeof(g_socket_path),
                     "%s", path);

            snprintf(g_socket_name, sizeof(g_socket_name),
                     "%s", name);

            _exit(accept_loop(fd, exec_cmd));
        }

        close(ready_pipe[0]);
    } else {
        snprintf(g_socket_path, sizeof(g_socket_path),
                 "%s", path);

        snprintf(g_socket_name, sizeof(g_socket_name),
                 "%s", name);
    }

    SocketEntry e;

    memset(&e, 0, sizeof(e));

    snprintf(e.name, sizeof(e.name), "%s", name);
    snprintf(e.path, sizeof(e.path), "%s", path);

    if (exec_cmd)
        snprintf(e.exec_cmd, sizeof(e.exec_cmd), "%s", exec_cmd);

    e.pid = server_pid;
    e.created = time(NULL);
    e.managed = 1;
    e.is_abstract = 0;

    r.entries[r.count++] = e;

    if (registry_save(&r) < 0) {
        if (!foreground) {
            char cancel = 'N';
            (void)write(ready_pipe[1], &cancel, 1);
            close(ready_pipe[1]);
        }

        unlink(path);

        if (!foreground)
            close(fd);

        registry_unlock(lockfd);
        return 1;
    }

    registry_unlock(lockfd);

    if (foreground)
        return accept_loop(fd, exec_cmd);

    char ready = 'Y';
    ssize_t written;

    do {
        written = write(ready_pipe[1], &ready, 1);
    } while (written < 0 && errno == EINTR);

    close(ready_pipe[1]);
    close(fd);

    if (written != 1) {
        seterr("cannot notify daemon process");
        kill(server_pid, SIGTERM);
        return 1;
    }

    return 0;
}

static int do_remove(const char *name)
{
    g_last_error[0] = '\0';

    if (!valid_socket_name(name)) {
        seterr("invalid socket name");
        return 1;
    }

    int lockfd = registry_lock();

    if (lockfd < 0)
        return 1;

    Registry r;

    if (registry_load(&r) < 0) {
        registry_unlock(lockfd);
        return 1;
    }

    SocketEntry *e = registry_find(&r, name);

    if (!e) {
        seterr("socket '%s' not found in registry", name);
        registry_unlock(lockfd);
        return 1;
    }

    pid_t pid = e->pid;
    char path[MAX_PATH];

    snprintf(path, sizeof(path), "%s", e->path);

    if (pid > 0 && process_alive(pid)) {
        if (kill(pid, SIGTERM) < 0 && errno != ESRCH) {
            seterr("cannot terminate process %d: %s",
                   (int)pid, strerror(errno));
            registry_unlock(lockfd);
            return 1;
        }
    }

    if (registry_remove(&r, name) < 0) {
        seterr("registry entry disappeared");
        registry_unlock(lockfd);
        return 1;
    }

    if (registry_save(&r) < 0) {
        registry_unlock(lockfd);
        return 1;
    }

    registry_unlock(lockfd);

    if (pid > 0) {
        for (int i = 0; i < 30; i++) {
            if (!process_alive(pid))
                break;

            sleep_ms(100);
        }

        if (process_alive(pid)) {
            if (kill(pid, SIGKILL) < 0 && errno != ESRCH) {
                seterr("cannot force-terminate process %d: %s",
                       (int)pid, strerror(errno));
                return 1;
            }
        }
    }

    if (unlink(path) < 0 && errno != ENOENT) {
        seterr("cannot remove socket path: %s", strerror(errno));
        return 1;
    }

    return 0;
}

static int cmd_create(int argc, char **argv)
{
    const char *sock = NULL;
    const char *dir = NULL;
    const char *exec_cmd = NULL;

    int foreground = 0;
    int verbose = 0;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];

        if (!strncmp(a, "--sock=", 7))
            sock = a + 7;
        else if (!strncmp(a, "--exec=", 7))
            exec_cmd = a + 7;
        else if (!strncmp(a, "--dir=", 6))
            dir = a + 6;
        else if (!strcmp(a, "--sock") && i + 1 < argc)
            sock = argv[++i];
        else if (!strcmp(a, "--exec") && i + 1 < argc)
            exec_cmd = argv[++i];
        else if (!strcmp(a, "--dir") && i + 1 < argc)
            dir = argv[++i];
        else if (!strcmp(a, "--foreground") || !strcmp(a, "-f"))
            foreground = 1;
        else if (!strcmp(a, "--verbose") || !strcmp(a, "-v"))
            verbose = 1;
        else if (a[0] != '-' && !dir)
            dir = a;
        else {
            seterr("unknown argument: %s", a);
            fprintf(stderr, "nixsk: %s\n", g_last_error);
            return 2;
        }
    }

    if (!sock || !*sock) {
        fprintf(stderr, "nixsk: --sock=NAME required\n");
        return 2;
    }

    char name[MAX_NAME] = {0};
    char path[MAX_PATH] = {0};

    if (strchr(sock, '/')) {
        char tmp[MAX_PATH];

        if (strlen(sock) >= sizeof(tmp)) {
            fprintf(stderr, "nixsk: socket path is too long\n");
            return 2;
        }

        snprintf(tmp, sizeof(tmp), "%s", sock);

        size_t len = strlen(tmp);

        while (len > 1 && tmp[len - 1] == '/')
            tmp[--len] = '\0';

        char *slash = strrchr(tmp, '/');

        if (!slash) {
            if (strlen(tmp) >= sizeof(name)) {
                fprintf(stderr, "nixsk: socket name is too long\n");
                return 2;
            }

            snprintf(name, sizeof(name), "%s", tmp);

            if (build_path(path, sizeof(path), "/tmp", name) < 0)
                return 2;
        } else if (slash == tmp) {
            if (strlen(slash + 1) >= sizeof(name)) {
                fprintf(stderr, "nixsk: socket name is too long\n");
                return 2;
            }

            snprintf(name, sizeof(name), "%s", slash + 1);

            if (build_path(path, sizeof(path), "/", name) < 0)
                return 2;
        } else {
            if (strlen(slash + 1) >= sizeof(name)) {
                fprintf(stderr, "nixsk: socket name is too long\n");
                return 2;
            }

            snprintf(name, sizeof(name), "%s", slash + 1);
            *slash = '\0';

            if (build_path(path, sizeof(path), tmp, name) < 0)
                return 2;
        }
    } else {
        if (strlen(sock) >= sizeof(name)) {
            fprintf(stderr, "nixsk: socket name is too long\n");
            return 2;
        }

        snprintf(name, sizeof(name), "%s", sock);

        if (build_path(path, sizeof(path), dir, name) < 0) {
            fprintf(stderr, "nixsk: resulting socket path is too long\n");
            return 2;
        }
    }

    if (!valid_socket_name(name) || !valid_socket_path(path)) {
        fprintf(stderr, "nixsk: invalid socket name or path\n");
        return 2;
    }

    if (do_create(name, path, exec_cmd, foreground) != 0) {
        fprintf(stderr, "nixsk: %s\n",
                g_last_error[0] ? g_last_error : "operation failed");
        return 1;
    }

    if (verbose && !foreground)
        printf("nixsk: created '%s' at %s\n", name, path);

    return 0;
}

static int cmd_remove(int argc, char **argv)
{
    const char *sock = NULL;
    int verbose = 0;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];

        if (!strncmp(a, "--sock=", 7))
            sock = a + 7;
        else if (!strcmp(a, "--sock") && i + 1 < argc)
            sock = argv[++i];
        else if (!strcmp(a, "--verbose") || !strcmp(a, "-v"))
            verbose = 1;
        else {
            seterr("unknown argument: %s", a);
            fprintf(stderr, "nixsk: %s\n", g_last_error);
            return 2;
        }
    }

    if (!sock || !*sock) {
        fprintf(stderr, "nixsk: --sock=NAME required\n");
        return 2;
    }

    const char *s = strrchr(sock, '/');
    const char *base = s ? s + 1 : sock;

    if (!valid_socket_name(base)) {
        fprintf(stderr, "nixsk: invalid socket name\n");
        return 2;
    }

    char name[MAX_NAME];

    snprintf(name, sizeof(name), "%s", base);

    if (do_remove(name) != 0) {
        fprintf(stderr, "nixsk: %s\n",
                g_last_error[0] ? g_last_error : "operation failed");
        return 1;
    }

    if (verbose)
        printf("nixsk: removed '%s'\n", name);

    return 0;
}

static int cmd_list(int argc, char **argv)
{
    int verbose = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--verbose") ||
            !strcmp(argv[i], "-v")) {
            verbose = 1;
        } else {
            fprintf(stderr, "nixsk: unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    Registry r;

    int lockfd = registry_lock();

    if (lockfd < 0)
        return 1;

    int load_result = registry_load(&r);

    registry_unlock(lockfd);

    if (load_result < 0) {
        fprintf(stderr, "nixsk: %s\n", g_last_error);
        return 1;
    }

    scan_system(&r);

    if (verbose) {
        printf("%-20s %-36s %-8s %-10s %-8s %-6s %-10s %s\n",
               "NAME", "PATH", "PID", "STATUS",
               "OWNER", "PERM", "TYPE", "EXEC");

        printf("----------------------------------------------------------------"
               "---------------------------------------------------------------\n");
    } else {
        printf("%-24s %-40s %-8s %-10s %s\n",
               "NAME", "PATH", "PID", "STATUS", "EXEC");

        printf("----------------------------------------------------------------"
               "-------------------------------\n");
    }

    for (int i = 0; i < r.count; i++) {
        SocketEntry *e = &r.entries[i];

        int alive = 0;
        int exists = 0;

        const char *status;

        if (e->managed) {
            alive = process_alive(e->pid);
            exists = access(e->path, F_OK) == 0;

            status = (alive && exists) ? "running" :
                     (alive && !exists) ? "orphan" :
                     (!alive && exists) ? "stale" : "dead";
        } else {
            status = "external";
        }

        char pid_str[32];

        if (e->managed && e->pid > 0)
            snprintf(pid_str, sizeof(pid_str), "%d", (int)e->pid);
        else
            strcpy(pid_str, "-");

        char exec_str[MAX_EXEC];

        snprintf(exec_str, sizeof(exec_str), "%s",
                 e->exec_cmd[0] ? e->exec_cmd : "-");

        if (verbose) {
            char owner_str[32] = "-";
            char perm_str[16] = "-";
            char type_str[16];

            if (e->is_abstract) {
                strcpy(type_str, "abstract");
            } else {
                strcpy(type_str, "file");

                struct stat stbuf;

                if (stat(e->path, &stbuf) == 0) {
                    struct passwd *pw = getpwuid(stbuf.st_uid);

                    if (pw)
                        snprintf(owner_str, sizeof(owner_str),
                                 "%s", pw->pw_name);
                    else
                        snprintf(owner_str, sizeof(owner_str),
                                 "%d", (int)stbuf.st_uid);

                    snprintf(perm_str, sizeof(perm_str),
                             "%04o", (unsigned)(stbuf.st_mode & 07777));
                }
            }

            printf("%-20.20s %-36.36s %-8s %-10s %-8.8s %-6s %-10s %s\n",
                   e->name, e->path, pid_str, status,
                   owner_str, perm_str, type_str, exec_str);
        } else {
            printf("%-24.24s %-40.40s %-8s %-10s %s\n",
                   e->name, e->path, pid_str, status, exec_str);
        }
    }

    return 0;
}

static void ui_message(const char *msg)
{
    int dw = (int)strlen(msg) + 6;

    if (dw > COLS - 4)
        dw = COLS - 4;

    if (dw < 20)
        dw = 20;

    if (COLS < 24 || LINES < 8)
        return;

    int dh = 5;
    int dy = (LINES - dh) / 2;
    int dx = (COLS - dw) / 2;

    if (dy < 0) dy = 0;
    if (dx < 0) dx = 0;

    WINDOW *w = newwin(dh, dw, dy, dx);

    if (!w)
        return;

    box(w, 0, 0);

    wattron(w, A_BOLD);
    mvwprintw(w, 0, (dw - 6) / 2, " Info ");
    wattroff(w, A_BOLD);

    mvwprintw(w, 2, 2, "%.*s", dw - 4, msg);
    mvwprintw(w, 3, (dw - 17) / 2, "Press any key...");

    wrefresh(w);
    wgetch(w);

    delwin(w);
    touchwin(stdscr);
    refresh();
}

static void ui_message_fmt(const char *fmt, ...)
{
    char buf[512];

    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    ui_message(buf);
}

static int ui_confirm(const char *msg)
{
    if (COLS < 36 || LINES < 10)
        return 0;

    int dw = (int)strlen(msg) + 6;

    if (dw > COLS - 4)
        dw = COLS - 4;

    if (dw < 32)
        dw = 32;

    int dh = 7;
    int dy = (LINES - dh) / 2;
    int dx = (COLS - dw) / 2;

    WINDOW *w = newwin(dh, dw, dy, dx);

    if (!w)
        return 0;

    keypad(w, TRUE);
    box(w, 0, 0);

    wattron(w, A_BOLD);
    mvwprintw(w, 0, (dw - 9) / 2, " Confirm ");
    wattroff(w, A_BOLD);

    mvwprintw(w, 2, 2, "%.*s", dw - 4, msg);
    mvwprintw(w, 5, (dw - 5) / 2, "[y/N]");

    wrefresh(w);

    int ret = 0;
    int ch;

    while ((ch = wgetch(w)) != ERR) {
        if (ch == 'y' || ch == 'Y') {
            ret = 1;
            break;
        }

        if (ch == 'n' || ch == 'N' ||
            ch == 27 || ch == '\n' ||
            ch == KEY_ENTER || ch == 'q' || ch == 'Q') {
            break;
        }
    }

    delwin(w);
    touchwin(stdscr);
    refresh();

    return ret;
}

static int ui_load_registry(Registry *r)
{
    int lockfd = registry_lock();

    if (lockfd < 0)
        return -1;

    int result = registry_load(r);

    registry_unlock(lockfd);

    if (result == 0)
        scan_system(r);

    return result;
}

static void ui_draw(Registry *r, int sel)
{
    erase();

    int h, w;

    getmaxyx(stdscr, h, w);

    if (h < 6 || w < 30) {
        mvprintw(0, 0, "Terminal too small. Resize or press q.");
        refresh();
        return;
    }

    attron(A_REVERSE | A_BOLD);

    mvhline(0, 0, ' ', w);

    mvprintw(0, 1, " nixsk %s - UNIX Socket Manager ",
             NIXSK_VERSION);

    attroff(A_REVERSE | A_BOLD);

    attron(A_BOLD);

    mvprintw(2, 2, "%-24s %-36s %-8s %-8s %s",
             "NAME", "PATH", "PID", "STATUS", "EXEC");

    attroff(A_BOLD);

    mvhline(3, 1, ACS_HLINE, w - 2);

    int max_rows = h - 6;

    if (max_rows < 1)
        max_rows = 1;

    if (r->count == 0) {
        mvprintw(5, 4, "No sockets found. Press 'c' to create one.");
    } else {
        int start = 0;

        if (sel >= max_rows)
            start = sel - max_rows + 1;

        for (int i = start;
             i < r->count && i < start + max_rows;
             i++) {
            SocketEntry *e = &r->entries[i];

            int color = 0;
            const char *status;

            if (e->managed) {
                int alive = process_alive(e->pid);
                int exists = access(e->path, F_OK) == 0;

                status = (alive && exists) ? "running" :
                         (alive && !exists) ? "orphan" :
                         (!alive && exists) ? "stale" : "dead";

                if (alive && exists)
                    color = 1;
                else if (alive || exists)
                    color = 2;
                else
                    color = 3;
            } else {
                status = "external";
                color = 4;
            }

            char pid_str[32];

            if (e->managed && e->pid > 0)
                snprintf(pid_str, sizeof(pid_str), "%d", (int)e->pid);
            else
                strcpy(pid_str, "-");

            if (i == sel)
                attron(A_REVERSE);

            if (color && has_colors())
                attron(COLOR_PAIR(color));

            mvprintw(4 + (i - start), 2,
                     "%-24.24s %-36.36s %-8s %-8s %.30s",
                     e->name, e->path, pid_str,
                     status, e->exec_cmd);

            if (color && has_colors())
                attroff(COLOR_PAIR(color));

            if (i == sel)
                attroff(A_REVERSE);
        }
    }

    attron(A_REVERSE);

    mvhline(h - 2, 0, ' ', w);

    mvprintw(h - 2, 1,
             " [c]reate  [d]elete  [r]efresh  [q]uit   %d socket(s)",
             r->count);

    attroff(A_REVERSE);

    refresh();
}

static void ui_create_dialog(void)
{
    if (COLS < 50 || LINES < 16) {
        ui_message("Terminal is too small for the create dialog.");
        return;
    }

    int dh = 13;
    int dw = 66;

    if (dw > COLS - 2)
        dw = COLS - 2;

    if (dh > LINES - 2)
        dh = LINES - 2;

    int dy = (LINES - dh) / 2;
    int dx = (COLS - dw) / 2;

    WINDOW *dlg = newwin(dh, dw, dy, dx);

    if (!dlg)
        return;

    keypad(dlg, TRUE);
    box(dlg, 0, 0);

    wattron(dlg, A_BOLD);
    mvwprintw(dlg, 0, (dw - 8) / 2, " Create ");
    wattroff(dlg, A_BOLD);

    char name[MAX_NAME] = {0};
    char dir[MAX_PATH] = {0};
    char exec_cmd[MAX_EXEC] = {0};

    const char *tmp = getenv("TMPDIR");

    if (!tmp || !*tmp)
        tmp = "/tmp";

    mvwprintw(dlg, 2, 3, "Name:");
    mvwprintw(dlg, 4, 3, "Directory:");
    mvwprintw(dlg, 6, 3, "Exec cmd (optional):");
    mvwprintw(dlg, 9, 3, "(empty exec = echo server)");
    mvwprintw(dlg, 10, 3, "[empty directory uses TMPDIR]");

    wrefresh(dlg);

    echo();
    curs_set(1);

    wmove(dlg, 2, 9);
    wgetnstr(dlg, name, MAX_NAME - 1);

    wmove(dlg, 4, 15);
    wgetnstr(dlg, dir, MAX_PATH - 1);

    wmove(dlg, 6, 24);
    wgetnstr(dlg, exec_cmd, MAX_EXEC - 1);

    noecho();
    curs_set(0);

    delwin(dlg);
    touchwin(stdscr);
    refresh();

    if (!name[0])
        return;

    if (!dir[0])
        snprintf(dir, sizeof(dir), "%s", tmp);

    char path[MAX_PATH];

    if (build_path(path, sizeof(path), dir, name) < 0) {
        ui_message("Socket path is too long.");
        return;
    }

    if (do_create(name, path,
                  exec_cmd[0] ? exec_cmd : NULL, 0) != 0) {
        ui_message_fmt("Create failed: %s", g_last_error);
    } else {
        ui_message_fmt("Created '%s' at %s", name, path);
    }
}

static int ui_run(void)
{
    Registry r;

    if (ui_load_registry(&r) < 0) {
        fprintf(stderr, "nixsk: %s\n", g_last_error);
        return 1;
    }

    int sel = 0;

    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);

    if (has_colors()) {
        start_color();
        use_default_colors();

        init_pair(1, COLOR_GREEN, -1);
        init_pair(2, COLOR_YELLOW, -1);
        init_pair(3, COLOR_RED, -1);
        init_pair(4, COLOR_CYAN, -1);
    }

    int running = 1;

    while (running) {
        ui_draw(&r, sel);

        int ch = getch();

        switch (ch) {
        case 'q':
        case 'Q':
        case 27:
            running = 0;
            break;

        case 'r':
        case 'R':
            if (ui_load_registry(&r) < 0) {
                ui_message_fmt("Refresh failed: %s", g_last_error);
            }

            if (sel >= r.count)
                sel = r.count > 0 ? r.count - 1 : 0;

            break;

        case KEY_UP:
        case 'k':
            if (sel > 0)
                sel--;
            break;

        case KEY_DOWN:
        case 'j':
            if (sel < r.count - 1)
                sel++;
            break;

        case 'c':
        case 'C':
            ui_create_dialog();

            if (ui_load_registry(&r) < 0)
                ui_message_fmt("Refresh failed: %s", g_last_error);

            if (sel >= r.count)
                sel = r.count > 0 ? r.count - 1 : 0;

            break;

        case 'd':
        case 'D': {
            if (r.count == 0)
                break;

            if (!r.entries[sel].managed) {
                ui_message_fmt("Cannot delete external socket '%s'",
                               r.entries[sel].name);
                break;
            }

            char prompt[256];

            snprintf(prompt, sizeof(prompt),
                     "Delete '%s' (%s)?",
                     r.entries[sel].name,
                     r.entries[sel].path);

            if (!ui_confirm(prompt))
                break;

            char name[MAX_NAME];

            snprintf(name, sizeof(name), "%s",
                     r.entries[sel].name);

            if (do_remove(name) == 0)
                ui_message_fmt("Removed '%s'", name);
            else
                ui_message_fmt("Remove failed: %s", g_last_error);

            if (ui_load_registry(&r) < 0)
                ui_message_fmt("Refresh failed: %s", g_last_error);

            if (sel >= r.count)
                sel = r.count > 0 ? r.count - 1 : 0;

            break;
        }

        case KEY_RESIZE:
            break;
        }
    }

    endwin();

    return 0;
}

static void usage(FILE *f)
{
    fprintf(f,
        "nixsk %s - UNIX socket toolkit\n"
        "\n"
        "Usage:\n"
        "  nixsk create --sock=NAME [--dir=DIR] [--exec=CMD]\n"
        "               [--foreground] [--verbose]\n"
        "  nixsk create --sock=/full/path/NAME [--exec=CMD]\n"
        "               [--foreground] [--verbose]\n"
        "  nixsk remove --sock=NAME [--verbose]\n"
        "  nixsk list [--verbose]\n"
        "  nixsk --ui\n"
        "  nixsk --version\n"
        "  nixsk --help\n"
        "\n"
        "Registry: %s\n",
        NIXSK_VERSION,
        g_registry_path);
}

int main(int argc, char **argv)
{
    if (registry_path_init() < 0) {
        fprintf(stderr, "nixsk: %s\n", g_last_error);
        return 1;
    }

    if (argc >= 2) {
        if (!strcmp(argv[1], "--ui") ||
            !strcmp(argv[1], "-u"))
            return ui_run();

        if (!strcmp(argv[1], "--version") ||
            !strcmp(argv[1], "-V")) {
            printf("nixsk %s\n", NIXSK_VERSION);
            return 0;
        }

        if (!strcmp(argv[1], "--help") ||
            !strcmp(argv[1], "-h")) {
            usage(stdout);
            return 0;
        }

        if (!strcmp(argv[1], "create"))
            return cmd_create(argc - 1, argv + 1);

        if (!strcmp(argv[1], "remove"))
            return cmd_remove(argc - 1, argv + 1);

        if (!strcmp(argv[1], "list"))
            return cmd_list(argc - 1, argv + 1);

        if (!strcmp(argv[1], "ui"))
            return ui_run();
    }

    usage(stderr);

    return 1;
}
