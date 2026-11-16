#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <pwd.h>
#include <dirent.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <ncurses.h>

#define NIXSK_VERSION  "1.0"
#define MAX_NAME       128
#define MAX_PATH       512
#define MAX_EXEC       512
#define MAX_ENTRIES    512
#define REG_LINE       1400

typedef struct {
    char   name[MAX_NAME];
    char   path[MAX_PATH];
    pid_t  pid;
    time_t created;
    char   exec_cmd[MAX_EXEC];
    int    managed;
    int    is_abstract;
} SocketEntry;

typedef struct {
    SocketEntry entries[MAX_ENTRIES];
    int         count;
} Registry;

static char g_registry_path[MAX_PATH];
static char g_last_error[512];

static void seterr(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
    va_end(ap);
}

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int mkdir_p(const char *path, mode_t mode) {
    char tmp[MAX_PATH];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    strcpy(tmp, path);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
    return 0;
}

static void registry_path_init(void) {
    const char *home = getenv("NIXSK_HOME");
    if (home && *home) {
        snprintf(g_registry_path, sizeof(g_registry_path),
                 "%s/registry", home);
        return;
    }
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(g_registry_path, sizeof(g_registry_path),
                 "%s/nixsk/registry", xdg);
        return;
    }
    const char *u = getenv("HOME");
    if (!u || !*u) {
        struct passwd *pw = getpwuid(getuid());
        if (pw && pw->pw_dir) u = pw->pw_dir;
    }
    if (u && *u) {
        snprintf(g_registry_path, sizeof(g_registry_path),
                 "%s/.config/nixsk/registry", u);
        return;
    }
    snprintf(g_registry_path, sizeof(g_registry_path),
             "/tmp/nixsk-registry-%d", (int)getuid());
}

static int split_tsv(char *line, char **fields, int maxf) {
    int n = 0;
    char *p = line;
    if (!maxf) return 0;
    fields[n++] = p;
    while (*p && n < maxf) {
        if (*p == '\t') { *p = '\0'; fields[n++] = p + 1; }
        p++;
    }
    char *last = fields[n - 1];
    size_t l = strlen(last);
    while (l > 0 && (last[l - 1] == '\n' || last[l - 1] == '\r'))
        last[--l] = '\0';
    return n;
}

static int registry_load(Registry *r) {
    r->count = 0;
    FILE *f = fopen(g_registry_path, "r");
    if (!f) return 0;
    char line[REG_LINE];
    while (fgets(line, sizeof(line), f) && r->count < MAX_ENTRIES) {
        char *fields[6];
        int n = split_tsv(line, fields, 6);
        if (n < 4) continue;
        SocketEntry *e = &r->entries[r->count];
        memset(e, 0, sizeof(*e));
        strncpy(e->name, fields[0], sizeof(e->name) - 1);
        strncpy(e->path, fields[1], sizeof(e->path) - 1);
        e->pid     = (pid_t)strtol(fields[2], NULL, 10);
        e->created = (time_t)strtoll(fields[3], NULL, 10);
        if (n >= 5)
            strncpy(e->exec_cmd, fields[4], sizeof(e->exec_cmd) - 1);
        e->managed = 1;
        e->is_abstract = 0;
        r->count++;
    }
    fclose(f);
    return 0;
}

static int registry_save(Registry *r) {
    char dir[MAX_PATH];
    strncpy(dir, g_registry_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdir_p(dir, 0700); }

    FILE *f = fopen(g_registry_path, "w");
    if (!f) {
        seterr("cannot write registry %s: %s",
               g_registry_path, strerror(errno));
        return -1;
    }
    for (int i = 0; i < r->count; i++) {
        SocketEntry *e = &r->entries[i];
        if (!e->managed) continue;
        fprintf(f, "%s\t%s\t%d\t%jd\t%s\n",
                e->name, e->path, (int)e->pid,
                (intmax_t)e->created, e->exec_cmd);
    }
    fclose(f);
    return 0;
}

static SocketEntry *registry_find(Registry *r, const char *name) {
    for (int i = 0; i < r->count; i++)
        if (r->entries[i].managed && strcmp(r->entries[i].name, name) == 0)
            return &r->entries[i];
    return NULL;
}

static int registry_remove(Registry *r, const char *name) {
    for (int i = 0; i < r->count; i++) {
        if (r->entries[i].managed && strcmp(r->entries[i].name, name) == 0) {
            memmove(&r->entries[i], &r->entries[i + 1],
                    (size_t)(r->count - i - 1) * sizeof(SocketEntry));
            r->count--;
            return 0;
        }
    }
    return -1;
}

static void add_external_entry(Registry *r, const char *path, int is_abstract) {
    for (int i = 0; i < r->count; i++) {
        if (strcmp(r->entries[i].path, path) == 0) return;
    }
    if (r->count >= MAX_ENTRIES) return;
    SocketEntry *e = &r->entries[r->count++];
    memset(e, 0, sizeof(*e));
    
    const char *base = strrchr(path, '/');
    if (base && base[1]) {
        strncpy(e->name, base + 1, sizeof(e->name) - 1);
    } else {
        strncpy(e->name, path, sizeof(e->name) - 1);
    }
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->managed = 0;
    e->is_abstract = is_abstract;
    e->pid = 0;
    e->created = 0;
}

static void scan_dir_for_sockets(Registry *r, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode)) {
            add_external_entry(r, path, 0);
        }
    }
    closedir(d);
}

static void scan_system(Registry *r) {
    const char *dirs[] = {
        "/tmp", "/var/tmp", "/run", "/var/run",
        "/tmp/.X11-unix", "/var/run/dbus", "/run/dbus", NULL
    };

    for (int i = 0; dirs[i]; i++) scan_dir_for_sockets(r, dirs[i]);

    char user_dir[MAX_PATH];
    snprintf(user_dir, sizeof(user_dir), "/run/user/%d", (int)getuid());
    scan_dir_for_sockets(r, user_dir);
    snprintf(user_dir, sizeof(user_dir), "/var/run/user/%d", (int)getuid());
    scan_dir_for_sockets(r, user_dir);

#ifdef __linux__
    FILE *f = fopen("/proc/net/unix", "r");
    if (f) {
        char line[512];
        if (fgets(line, sizeof(line), f)) { }
        while (fgets(line, sizeof(line), f)) {
            char *p = strchr(line, '@');
            if (p) {
                p[strcspn(p, "\r\n")] = '\0';
                add_external_entry(r, p, 1);
            }
        }
        fclose(f);
    }
#endif
}

static volatile sig_atomic_t g_stop = 0;
static char g_socket_path[MAX_PATH];
static char g_socket_name[MAX_NAME];

static void sig_handler(int sig) { (void)sig; g_stop = 1; }

static void install_daemon_sigactions(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGHUP,  &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);
}

static void close_all_fds_except(int keep) {
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 65536) maxfd = 1024;
    for (long i = 3; i < maxfd; i++)
        if ((int)i != keep) close((int)i);
}

static int accept_loop(int listen_fd, const char *exec_cmd) {
    install_daemon_sigactions();

    while (!g_stop) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen_fd, &fds);
        struct timeval tv;
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        int n = select(listen_fd + 1, &fds, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) continue;

        int conn = accept(listen_fd, NULL, NULL);
        if (conn < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (exec_cmd && *exec_cmd) {
            pid_t p = fork();
            if (p == 0) {
                close(listen_fd);
                dup2(conn, 0);
                dup2(conn, 1);
                dup2(conn, 2);
                if (conn > 2) close(conn);
                execl("/bin/sh", "sh", "-c", exec_cmd, (char *)NULL);
                _exit(127);
            }
            close(conn);
        } else {
            char buf[4096];
            ssize_t r;
            while ((r = read(conn, buf, sizeof(buf))) > 0) {
                ssize_t off = 0;
                while (off < r) {
                    ssize_t w = write(conn, buf + off, r - off);
                    if (w <= 0) break;
                    off += w;
                }
                if (off < r) break;
            }
            close(conn);
        }
    }

    close(listen_fd);
    if (g_socket_path[0]) unlink(g_socket_path);
    if (g_socket_name[0]) {
        Registry r;
        registry_load(&r);
        if (registry_remove(&r, g_socket_name) == 0)
            registry_save(&r);
    }
    return 0;
}

static int do_create(const char *name, const char *path,
                     const char *exec_cmd, int foreground) {
    g_last_error[0] = '\0';

    if (!name || !*name) { seterr("empty socket name"); return 1; }
    if (strchr(name, '/')) { seterr("name must not contain '/'"); return 1; }

    size_t maxsp = sizeof(((struct sockaddr_un *)0)->sun_path) - 1;
    if (strlen(path) > maxsp) {
        seterr("socket path too long (%zu > %zu)", strlen(path), maxsp);
        return 1;
    }

    char dirpath[MAX_PATH];
    strncpy(dirpath, path, sizeof(dirpath) - 1);
    dirpath[sizeof(dirpath) - 1] = '\0';
    char *slash = strrchr(dirpath, '/');
    if (slash && slash != dirpath) { *slash = '\0'; mkdir_p(dirpath, 0755); }

    Registry r;
    registry_load(&r);
    if (registry_find(&r, name)) {
        seterr("socket '%s' already registered", name);
        return 1;
    }
    if (access(path, F_OK) == 0) {
        seterr("%s already exists", path);
        return 1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { seterr("socket(): %s", strerror(errno)); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        seterr("bind(%s): %s", path, strerror(errno));
        close(fd);
        return 1;
    }
    if (listen(fd, 16) < 0) {
        seterr("listen(): %s", strerror(errno));
        unlink(path);
        close(fd);
        return 1;
    }

    pid_t server_pid;
    if (foreground) {
        server_pid = getpid();
        strncpy(g_socket_path, path, sizeof(g_socket_path) - 1);
        strncpy(g_socket_name, name, sizeof(g_socket_name) - 1);
    } else {
        server_pid = fork();
        if (server_pid < 0) {
            seterr("fork(): %s", strerror(errno));
            unlink(path);
            close(fd);
            return 1;
        }
        if (server_pid == 0) {
            setsid();
            close_all_fds_except(fd);
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, 0);
                dup2(devnull, 1);
                dup2(devnull, 2);
                if (devnull > 2) close(devnull);
            }
            strncpy(g_socket_path, path, sizeof(g_socket_path) - 1);
            strncpy(g_socket_name, name, sizeof(g_socket_name) - 1);
            _exit(accept_loop(fd, exec_cmd));
        }
    }

    SocketEntry e;
    memset(&e, 0, sizeof(e));
    strncpy(e.name, name, sizeof(e.name) - 1);
    strncpy(e.path, path, sizeof(e.path) - 1);
    e.pid = server_pid;
    e.created = time(NULL);
    if (exec_cmd) strncpy(e.exec_cmd, exec_cmd, sizeof(e.exec_cmd) - 1);
    e.managed = 1;
    e.is_abstract = 0;
    r.entries[r.count++] = e;
    if (registry_save(&r) < 0) {
        if (!foreground) kill(server_pid, SIGTERM);
        unlink(path);
        if (!foreground) close(fd);
        return 1;
    }

    if (foreground) return accept_loop(fd, exec_cmd);
    close(fd);
    return 0;
}

static int do_remove(const char *name) {
    g_last_error[0] = '\0';
    Registry r;
    registry_load(&r);
    SocketEntry *e = registry_find(&r, name);
    if (!e) {
        seterr("socket '%s' not found in registry", name);
        return 1;
    }

    pid_t pid = e->pid;
    char path[MAX_PATH];
    strncpy(path, e->path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    if (pid > 0 && kill(pid, 0) == 0) {
        kill(pid, SIGTERM);
        for (int i = 0; i < 30; i++) {
            sleep_ms(100);
            if (kill(pid, 0) != 0) break;
        }
        if (kill(pid, 0) == 0) kill(pid, SIGKILL);
    }
    unlink(path);
    registry_remove(&r, name);
    registry_save(&r);
    return 0;
}

static void build_path(char *out, size_t outsz,
                       const char *dir, const char *name) {
    if (!dir || !*dir) { dir = "/tmp"; }
    size_t dl = strlen(dir);
    while (dl > 1 && dir[dl - 1] == '/') dl--;
    if (dl == 1 && dir[0] == '/') {
        snprintf(out, outsz, "/%s", name);
    } else if (dl == 0) {
        snprintf(out, outsz, "/tmp/%s", name);
    } else {
        snprintf(out, outsz, "%.*s/%s", (int)dl, dir, name);
    }
}

static int cmd_create(int argc, char **argv) {
    const char *sock = NULL;
    const char *dir  = NULL;
    const char *exec_cmd = NULL;
    int foreground = 0;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if      (!strncmp(a, "--sock=", 7))  sock = a + 7;
        else if (!strncmp(a, "--exec=", 7))  exec_cmd = a + 7;
        else if (!strncmp(a, "--dir=", 6))   dir = a + 6;
        else if (!strcmp(a, "--sock") && i + 1 < argc) sock = argv[++i];
        else if (!strcmp(a, "--exec") && i + 1 < argc) exec_cmd = argv[++i];
        else if (!strcmp(a, "--dir")  && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(a, "--foreground") || !strcmp(a, "-f")) foreground = 1;
        else if (a[0] != '-' && !dir) dir = a;
        else { seterr("unknown argument: %s", a); return 2; }
    }
    if (!sock) { seterr("--sock=NAME required"); return 2; }

    char name[MAX_NAME];
    char path[MAX_PATH];
    name[0] = '\0';

    if (strchr(sock, '/')) {
        char tmp[MAX_PATH];
        strncpy(tmp, sock, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        size_t tl = strlen(tmp);
        while (tl > 1 && tmp[tl - 1] == '/') tmp[--tl] = '\0';

        char *s = strrchr(tmp, '/');
        if (!s) {
            strncpy(name, tmp, sizeof(name) - 1);
            build_path(path, sizeof(path), "/tmp", name);
        } else if (s == tmp) {
            strncpy(name, s + 1, sizeof(name) - 1);
            strcpy(tmp, "/");
            build_path(path, sizeof(path), tmp, name);
        } else {
            strncpy(name, s + 1, sizeof(name) - 1);
            *s = '\0';
            build_path(path, sizeof(path), tmp, name);
        }
    } else {
        strncpy(name, sock, sizeof(name) - 1);
        build_path(path, sizeof(path), dir, name);
    }
    name[sizeof(name) - 1] = '\0';

    if (!name[0]) { seterr("invalid socket name"); return 2; }

    if (do_create(name, path, exec_cmd, foreground) != 0) {
        fprintf(stderr, "nixsk: %s\n", g_last_error);
        return 1;
    }
    if (!foreground)
        printf("nixsk: created '%s' at %s\n", name, path);
    return 0;
}

static int cmd_remove(int argc, char **argv) {
    const char *sock = NULL;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if      (!strncmp(a, "--sock=", 7))  sock = a + 7;
        else if (!strcmp(a, "--sock") && i + 1 < argc) sock = argv[++i];
        else { seterr("unknown argument: %s", a); return 2; }
    }
    if (!sock) { seterr("--sock=NAME required"); return 2; }

    char name[MAX_NAME];
    const char *s = strrchr(sock, '/');
    strncpy(name, s ? s + 1 : sock, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    if (!name[0]) { seterr("invalid socket name"); return 2; }

    if (do_remove(name) != 0) {
        fprintf(stderr, "nixsk: %s\n", g_last_error);
        return 1;
    }
    printf("nixsk: removed '%s'\n", name);
    return 0;
}

static int cmd_list(int argc, char **argv) {
    int verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v"))
            verbose = 1;
    }

    Registry r;
    registry_load(&r);
    scan_system(&r);

    if (verbose) {
        printf("%-20s %-36s %-8s %-10s %-8s %-6s %-10s %s\n",
               "NAME", "PATH", "PID", "STATUS", "OWNER", "PERM", "TYPE", "EXEC");
        printf("--------------------------------------------------------------"
               "--------------------------------------------------------------\n");
    } else {
        printf("%-24s %-40s %-8s %-10s %s\n",
               "NAME", "PATH", "PID", "STATUS", "EXEC");
        printf("--------------------------------------------------------------"
               "--------------------------------------------------------------\n");
    }

    for (int i = 0; i < r.count; i++) {
        SocketEntry *e = &r.entries[i];
        int alive = 0;
        int exists = 0;
        const char *st;

        if (e->managed) {
            alive = e->pid > 0 && kill(e->pid, 0) == 0;
            exists = access(e->path, F_OK) == 0;
            st = (alive && exists)  ? "running" :
                 (alive && !exists) ? "orphan"  :
                 (!alive && exists) ? "stale"   : "dead";
        } else {
            st = "external";
        }

        char pid_str[16];
        if (e->managed && e->pid > 0)
            snprintf(pid_str, sizeof(pid_str), "%d", (int)e->pid);
        else
            strcpy(pid_str, "-");

        char exec_str[MAX_EXEC] = "-";
        if (e->exec_cmd[0])
            strncpy(exec_str, e->exec_cmd, sizeof(exec_str) - 1);

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
                    if (pw) strncpy(owner_str, pw->pw_name, sizeof(owner_str) - 1);
                    else snprintf(owner_str, sizeof(owner_str), "%d", (int)stbuf.st_uid);
                    snprintf(perm_str, sizeof(perm_str), "%04o", stbuf.st_mode & 07777);
                }
            }
            printf("%-20s %-36s %-8s %-10s %-8s %-6s %-10s %s\n",
                   e->name, e->path, pid_str, st, owner_str, perm_str, type_str, exec_str);
        } else {
            printf("%-24s %-40s %-8s %-10s %s\n",
                   e->name, e->path, pid_str, st, exec_str);
        }
    }
    return 0;
}

static void usage(FILE *f) {
    fprintf(f,
        "nixsk %s - UNIX socket toolkit\n"
        "Usage:\n"
        "  nixsk create --sock=NAME [--dir=DIR] [--exec=CMD] [--foreground]\n"
        "  nixsk create --sock=/full/path/NAME [--exec=CMD] [--foreground]\n"
        "  nixsk remove --sock=NAME\n"
        "  nixsk list [--verbose]\n"
        "  nixsk --ui\n"
        "Registry: %s\n",
        NIXSK_VERSION, g_registry_path);
}

static void ui_message(const char *msg) {
    int dw = (int)strlen(msg) + 6;
    if (dw > COLS - 4) dw = COLS - 4;
    if (dw < 20) dw = 20;
    int dh = 5;
    int dy = (LINES - dh) / 2;
    int dx = (COLS - dw) / 2;
    if (dy < 0) dy = 0;
    if (dx < 0) dx = 0;
    WINDOW *w = newwin(dh, dw, dy, dx);
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

static void ui_message_fmt(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ui_message(buf);
}

static int ui_confirm(const char *msg) {
    int dw = (int)strlen(msg) + 6;
    if (dw > COLS - 4) dw = COLS - 4;
    if (dw < 32) dw = 32;
    int dh = 7;
    int dy = (LINES - dh) / 2;
    int dx = (COLS - dw) / 2;
    if (dy < 0) dy = 0;
    if (dx < 0) dx = 0;
    WINDOW *w = newwin(dh, dw, dy, dx);
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
        if (ch == 'y' || ch == 'Y') { ret = 1; break; }
        if (ch == 'n' || ch == 'N' || ch == 27 || ch == '\n' ||
            ch == KEY_ENTER || ch == 'q' || ch == 'Q') { ret = 0; break; }
    }
    delwin(w);
    touchwin(stdscr);
    refresh();
    return ret;
}

static void ui_draw(Registry *r, int sel) {
    erase();
    int h, w;
    getmaxyx(stdscr, h, w);

    attron(A_REVERSE | A_BOLD);
    mvhline(0, 0, ' ', w);
    mvprintw(0, 1, " nixsk %s - UNIX Socket Manager ", NIXSK_VERSION);
    attroff(A_REVERSE | A_BOLD);

    attron(A_BOLD);
    mvprintw(2, 2, "%-24s %-36s %-8s %-8s %s",
             "NAME", "PATH", "PID", "STATUS", "EXEC");
    attroff(A_BOLD);
    mvhline(3, 1, ACS_HLINE, w - 2);

    int max_rows = h - 6;
    if (max_rows < 1) max_rows = 1;

    if (r->count == 0) {
        mvprintw(5, 4, "No sockets found. Press 'c' to create one.");
    } else {
        int start = 0;
        if (sel >= max_rows) start = sel - max_rows + 1;
        for (int i = 0; i < r->count && i < start + max_rows; i++) {
            if (i < start) continue;
            SocketEntry *e = &r->entries[i];
            int color = 0;
            const char *st;

            if (e->managed) {
                int alive  = e->pid > 0 && kill(e->pid, 0) == 0;
                int exists = access(e->path, F_OK) == 0;
                st = (alive && exists)  ? "running" :
                     (alive && !exists) ? "orphan"  :
                     (!alive && exists) ? "stale"   : "dead";
                if      (alive &&  exists) color = 1;
                else if (alive ||  exists) color = 2;
                else                       color = 3;
            } else {
                st = "external";
                color = 4;
            }

            char pid_str[16];
            if (e->managed && e->pid > 0)
                snprintf(pid_str, sizeof(pid_str), "%d", (int)e->pid);
            else
                strcpy(pid_str, "-");

            if (i == sel) attron(A_REVERSE);
            if (color && has_colors()) attron(COLOR_PAIR(color));
            mvprintw(4 + (i - start), 2,
                     "%-24.24s %-36.36s %-8s %-8s %.30s",
                     e->name, e->path, pid_str, st, e->exec_cmd);
            if (color && has_colors()) attroff(COLOR_PAIR(color));
            if (i == sel) attroff(A_REVERSE);
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

static void ui_create_dialog(void) {
    int dh = 13;
    int dw = 66;
    if (dw > COLS - 2) dw = COLS - 2;
    if (dh > LINES - 2) dh = LINES - 2;
    int dy = (LINES - dh) / 2;
    int dx = (COLS - dw) / 2;
    if (dy < 0) dy = 0;
    if (dx < 0) dx = 0;

    WINDOW *dlg = newwin(dh, dw, dy, dx);
    keypad(dlg, TRUE);
    box(dlg, 0, 0);
    wattron(dlg, A_BOLD);
    mvwprintw(dlg, 0, (dw - 8) / 2, " Create ");
    wattroff(dlg, A_BOLD);

    char name[MAX_NAME] = {0};
    char dir[MAX_PATH]  = {0};
    char exec_cmd[MAX_EXEC] = {0};

    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";

    mvwprintw(dlg, 2, 3, "Name:");
    mvwprintw(dlg, 4, 3, "Directory [%s]:", tmp);
    mvwprintw(dlg, 6, 3, "Exec cmd (optional):");
    mvwprintw(dlg, 9, 3, "(empty exec = echo server)");
    mvwprintw(dlg, 10, 3, "[empty Enter uses default]");
    wrefresh(dlg);

    echo();
    curs_set(1);

    wmove(dlg, 2, 3 + 6);
    wgetnstr(dlg, name, MAX_NAME - 1);

    wmove(dlg, 4, 3 + 12 + (int)strlen(tmp) + 2);
    wgetnstr(dlg, dir, MAX_PATH - 1);

    wmove(dlg, 6, 3 + 21);
    wgetnstr(dlg, exec_cmd, MAX_EXEC - 1);

    noecho();
    curs_set(0);
    delwin(dlg);
    touchwin(stdscr);
    refresh();

    if (!name[0]) return;
    if (!dir[0]) strncpy(dir, tmp, sizeof(dir) - 1);

    char path[MAX_PATH];
    build_path(path, sizeof(path), dir, name);

    if (do_create(name, path, exec_cmd[0] ? exec_cmd : NULL, 0) != 0)
        ui_message_fmt("Create failed: %s", g_last_error);
    else
        ui_message_fmt("Created '%s' at %s", name, path);
}

static int ui_run(void) {
    Registry r;
    registry_load(&r);
    scan_system(&r);
    int sel = 0;

    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_GREEN,  -1);
        init_pair(2, COLOR_YELLOW, -1);
        init_pair(3, COLOR_RED,    -1);
        init_pair(4, COLOR_CYAN,   -1);
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
            registry_load(&r);
            scan_system(&r);
            if (sel >= r.count) sel = r.count > 0 ? r.count - 1 : 0;
            break;
        case KEY_UP:
        case 'k':
            if (sel > 0) sel--;
            break;
        case KEY_DOWN:
        case 'j':
            if (sel < r.count - 1) sel++;
            break;
        case 'c':
        case 'C': {
            ui_create_dialog();
            registry_load(&r);
            scan_system(&r);
            if (sel >= r.count) sel = r.count > 0 ? r.count - 1 : 0;
            break;
        }
        case 'd':
        case 'D': {
            if (r.count == 0) break;
            if (!r.entries[sel].managed) {
                ui_message_fmt("Cannot delete external socket '%s'",
                               r.entries[sel].name);
                break;
            }
            char prompt[256];
            snprintf(prompt, sizeof(prompt),
                     "Delete '%s' (%s)?",
                     r.entries[sel].name, r.entries[sel].path);
            if (!ui_confirm(prompt)) break;
            char nm[MAX_NAME];
            strncpy(nm, r.entries[sel].name, sizeof(nm) - 1);
            nm[sizeof(nm) - 1] = '\0';
            if (do_remove(nm) == 0)
                ui_message_fmt("Removed '%s'", nm);
            else
                ui_message_fmt("Remove failed: %s", g_last_error);
            registry_load(&r);
            scan_system(&r);
            if (sel >= r.count) sel = r.count > 0 ? r.count - 1 : 0;
            break;
        }
        case KEY_RESIZE:
            break;
        }
    }
    endwin();
    return 0;
}

int main(int argc, char **argv) {
    registry_path_init();

    if (argc >= 2) {
        if (!strcmp(argv[1], "--ui") || !strcmp(argv[1], "-u"))
            return ui_run();
        if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V")) {
            printf("nixsk %s\n", NIXSK_VERSION);
            return 0;
        }
        if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
            usage(stdout);
            return 0;
        }
        if (!strcmp(argv[1], "create")) return cmd_create(argc - 1, argv + 1);
        if (!strcmp(argv[1], "remove")) return cmd_remove(argc - 1, argv + 1);
        if (!strcmp(argv[1], "list"))   return cmd_list(argc - 1, argv + 1);
        if (!strcmp(argv[1], "ui"))     return ui_run();
    }

    usage(stderr);
    return 1;
}