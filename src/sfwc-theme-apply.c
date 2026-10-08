#define _POSIX_C_SOURCE 200809L
/*
 * sfwc-theme-apply: render the theme's templates for the companion tools.
 *
 *   sfwc-theme-apply [--theme NAME] [--config-dir DIR] [--templates DIR]... [--out DIR]
 *                    [--skip NAME]... [--no-signal] [--quiet]
 *
 * Every FILE.in found in a template directory is rendered (see src/template.h for the
 * placeholders) to <out>/FILE; the first directory that has a given template wins, so a user
 * template overrides the packaged one. Outputs are only rewritten when their content changes;
 * "updated FILE" is printed for each, and tools that can reload are told to do so.
 * SFWC_TEMPLATES=dir[:dir...] replaces the default template directories.
 * Exit status: 0 = ok, 1 = a template or the theme was bad, 2 = usage error.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "template.h"
#include "theme.h"

#define MAX_LIST 32

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    size_t n;
    while (buf && (n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            char *bigger = realloc(buf, cap * 2);
            if (!bigger) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = bigger;
            cap *= 2;
        }
    }
    fclose(f);
    if (buf) {
        buf[len] = 0;
    }
    return buf;
}

static bool write_atomic(const char *dir, const char *name, const char *text)
{
    char tmp[1024], dst[1024];
    snprintf(tmp, sizeof tmp, "%s/.%s.tmp", dir, name);
    snprintf(dst, sizeof dst, "%s/%s", dir, name);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return false;
    }
    bool ok = fputs(text, f) >= 0;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
        ok = rename(tmp, dst) == 0;
    }
    if (!ok) {
        unlink(tmp);
    }
    return ok;
}

/* Tell the tool that uses this output to reload. Best effort. */
static void notify_tool(const char *output)
{
    const char *argv[4] = {0};
    if (!strcmp(output, "waybar.css")) {
        argv[0] = "pkill", argv[1] = "-USR2", argv[2] = "-x", argv[3] = "waybar";
    } else if (!strcmp(output, "mako.conf")) {
        argv[0] = "makoctl", argv[1] = "reload";
    } else {
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            dup2(null, 1);
            dup2(null, 2);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

static bool listed(char *const *list, int n, const char *s)
{
    for (int i = 0; i < n; i++) {
        if (!strcmp(list[i], s)) {
            return true;
        }
    }
    return false;
}

static void theme_log(int level, int line, const char *text, void *data)
{
    (void)level;
    (void)data;
    fprintf(stderr, "sfwc-theme-apply: theme line %d: %s\n", line, text);
}

int main(int argc, char **argv)
{
    const char *theme_name = "default", *config_dir = NULL, *out_dir = NULL;
    char *tdirs[MAX_LIST], *skip[MAX_LIST];
    int n_tdirs = 0, n_skip = 0;
    bool signal_tools = true, quiet = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool has_arg = i + 1 < argc;
        if (!strcmp(a, "--theme") && has_arg) {
            theme_name = argv[++i];
        } else if (!strcmp(a, "--config-dir") && has_arg) {
            config_dir = argv[++i];
        } else if (!strcmp(a, "--out") && has_arg) {
            out_dir = argv[++i];
        } else if (!strcmp(a, "--templates") && has_arg && n_tdirs < MAX_LIST) {
            tdirs[n_tdirs++] = argv[++i];
        } else if (!strcmp(a, "--skip") && has_arg && n_skip < MAX_LIST) {
            skip[n_skip++] = argv[++i];
        } else if (!strcmp(a, "--no-signal")) {
            signal_tools = false;
        } else if (!strcmp(a, "--quiet")) {
            quiet = true;
        } else {
            fprintf(stderr,
                    "usage: sfwc-theme-apply [--theme NAME] [--config-dir DIR] [--templates DIR]...\n"
                    "                        [--out DIR] [--skip NAME]... [--no-signal] [--quiet]\n");
            return 2;
        }
    }

    /* the theme: built-in default, then the theme file on top */
    struct theme theme;
    theme_init_default(&theme);
    /* "default" may be missing (the built-in copy is used); any other theme must exist */
    char *theme_path = theme_find(theme_name, config_dir);
    if (!theme_path && strcmp(theme_name, "default") != 0) {
        fprintf(stderr, "sfwc-theme-apply: theme '%s' not found\n", theme_name);
        return 1;
    }
    if (theme_path && !theme_load_file(&theme, theme_path, theme_log, NULL)) {
        fprintf(stderr, "sfwc-theme-apply: cannot read %s\n", theme_path);
        return 1;
    }

    /* template directories, most specific first */
    static char dirs_buf[MAX_LIST + 8][1024];
    const char *dirs[MAX_LIST + 8];
    int n_dirs = 0;
    for (int i = 0; i < n_tdirs; i++) {
        dirs[n_dirs++] = tdirs[i];
    }
    const char *env_dirs = getenv("SFWC_TEMPLATES"); /* colon separated, for development */
    if (n_tdirs == 0 && env_dirs && *env_dirs) {
        static char env_buf[2048];
        snprintf(env_buf, sizeof env_buf, "%s", env_dirs);
        for (char *save = NULL, *d = strtok_r(env_buf, ":", &save); d && n_dirs < MAX_LIST;
             d = strtok_r(NULL, ":", &save)) {
            dirs[n_dirs++] = d;
        }
    } else if (n_tdirs == 0) {
        const char *home = getenv("HOME"), *xch = getenv("XDG_CONFIG_HOME"), *xdh = getenv("XDG_DATA_HOME");
        if (theme_path) { /* <theme dir>/<name>/templates: a theme can bring its own */
            char tmp[1024];
            snprintf(tmp, sizeof tmp, "%s", theme_path);
            char *slash = strrchr(tmp, '/');
            if (slash) {
                *slash = 0;
                snprintf(dirs_buf[n_dirs], 1024, "%s/%s/templates", tmp, theme_name);
                dirs[n_dirs] = dirs_buf[n_dirs];
                n_dirs++;
            }
        }
        if (config_dir) {
            snprintf(dirs_buf[n_dirs], 1024, "%s/templates", config_dir);
            dirs[n_dirs] = dirs_buf[n_dirs];
            n_dirs++;
        }
        if (xch && *xch) {
            snprintf(dirs_buf[n_dirs], 1024, "%s/sfwc/templates", xch);
            dirs[n_dirs] = dirs_buf[n_dirs];
            n_dirs++;
        } else if (home) {
            snprintf(dirs_buf[n_dirs], 1024, "%s/.config/sfwc/templates", home);
            dirs[n_dirs] = dirs_buf[n_dirs];
            n_dirs++;
        }
        if (xdh && *xdh) {
            snprintf(dirs_buf[n_dirs], 1024, "%s/sfwc/templates", xdh);
            dirs[n_dirs] = dirs_buf[n_dirs];
            n_dirs++;
        } else if (home) {
            snprintf(dirs_buf[n_dirs], 1024, "%s/.local/share/sfwc/templates", home);
            dirs[n_dirs] = dirs_buf[n_dirs];
            n_dirs++;
        }
        dirs[n_dirs++] = "/usr/local/share/sfwc/templates";
        dirs[n_dirs++] = "/usr/share/sfwc/templates";
    }

    /* output directory */
    char out_buf[1024];
    if (!out_dir) {
        const char *rt = getenv("XDG_RUNTIME_DIR");
        if (!rt || !*rt) {
            fprintf(stderr, "sfwc-theme-apply: XDG_RUNTIME_DIR is not set (use --out DIR)\n");
            return 1;
        }
        snprintf(out_buf, sizeof out_buf, "%s/sfwc", rt);
        out_dir = out_buf;
    }
    if (mkdir(out_dir, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "sfwc-theme-apply: cannot create %s: %s\n", out_dir, strerror(errno));
        return 1;
    }

    int status = 0;
    char *done[256];
    int n_done = 0;
    for (int d = 0; d < n_dirs; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) {
            continue;
        }
        struct dirent *e;
        while ((e = readdir(dir))) {
            size_t len = strlen(e->d_name);
            if (len < 4 || strcmp(e->d_name + len - 3, ".in") != 0 || e->d_name[0] == '.') {
                continue;
            }
            char name[256];
            snprintf(name, sizeof name, "%.*s", (int)(len - 3), e->d_name);
            if (listed(done, n_done, name) || n_done >= 256) {
                continue; /* a more specific directory already provided it */
            }
            done[n_done++] = strdup(name);
            char stem[256];
            snprintf(stem, sizeof stem, "%s", name);
            char *dot = strchr(stem, '.');
            if (dot) {
                *dot = 0;
            }
            if (listed(skip, n_skip, stem)) {
                continue;
            }
            char path[1536];
            snprintf(path, sizeof path, "%s/%s", dirs[d], e->d_name);
            char *src = read_file(path);
            if (!src) {
                fprintf(stderr, "sfwc-theme-apply: cannot read %s\n", path);
                status = 1;
                continue;
            }
            char err[200];
            char *text = template_render(&theme, src, true, err, sizeof err);
            free(src);
            if (!text) {
                fprintf(stderr, "sfwc-theme-apply: %s: %s\n", path, err);
                status = 1;
                continue;
            }
            char dst[1536];
            snprintf(dst, sizeof dst, "%s/%s", out_dir, name);
            char *old = read_file(dst);
            if (old && !strcmp(old, text)) {
                free(old);
                free(text);
                continue;
            }
            free(old);
            if (!write_atomic(out_dir, name, text)) {
                fprintf(stderr, "sfwc-theme-apply: cannot write %s: %s\n", dst, strerror(errno));
                status = 1;
            } else {
                if (!quiet) {
                    printf("updated %s\n", name);
                }
                if (signal_tools) {
                    notify_tool(name);
                }
            }
            free(text);
        }
        closedir(dir);
    }
    for (int i = 0; i < n_done; i++) {
        free(done[i]);
    }
    free(theme_path);
    theme_finish(&theme);
    return status;
}
