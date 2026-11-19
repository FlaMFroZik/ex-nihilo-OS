#include <stdint.h>
#include "kernel/syscall.h"
#include "fs/fat16.h"

/* ================================================================
 * SHELL.ELF — оболочка с bash-подобным синтаксисом (Ring 3).
 * Поддерживает:
 *   - приглашение user@ex-nihilo:<cwd>$
 *   - кавычки '...' и "...", комментарии #
 *   - разделители команд ; и &&
 *   - переменные $? и $PWD
 *   - команды: pwd, ls, cd, cat, echo [-n], clear, help, exit,
 *              uname, whoami, true, false
 * ================================================================ */

#define LINE_MAX 256
#define PATH_MAX 128
#define MAX_ARGS 16

static char cwd[PATH_MAX] = "/";
static int last_status = 0;

static int str_len(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void put_char(char c)
{
    char buf[2];
    buf[0] = c;
    buf[1] = '\0';
    sys_kprint(buf);
}

static void print_u32(uint32_t v)
{
    char buf[12];
    int i = 0;
    if (v == 0) {
        sys_kprint("0");
        return;
    }
    while (v > 0 && i < 11) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0)
        put_char(buf[--i]);
}

static void print_int(int v)
{
    if (v < 0) {
        put_char('-');
        v = -v;
    }
    print_u32((uint32_t)v);
}

static void resolve_path(const char *arg, char *out, int max)
{
    char tmp[PATH_MAX];
    int len = 0;

    if (!arg || !arg[0]) {
        str_copy(out, cwd, max);
        return;
    }

    if (arg[0] == '/') {
        str_copy(tmp, arg, PATH_MAX);
    } else {
        str_copy(tmp, cwd, PATH_MAX);
        len = str_len(tmp);
        if (len > 1 && tmp[len - 1] != '/' && len < PATH_MAX - 1) {
            tmp[len++] = '/';
            tmp[len] = '\0';
        }
        str_copy(tmp + len, arg, PATH_MAX - len);
    }

    char norm[PATH_MAX];
    int nlen = 1;
    norm[0] = '/';
    norm[1] = '\0';

    int i = 0;
    while (tmp[i]) {
        while (tmp[i] == '/')
            i++;
        if (!tmp[i])
            break;
        int start = i;
        while (tmp[i] && tmp[i] != '/')
            i++;
        int clen = i - start;

        if (clen == 1 && tmp[start] == '.')
            continue;
        if (clen == 2 && tmp[start] == '.' && tmp[start + 1] == '.') {
            if (nlen > 1) {
                nlen--;
                while (nlen > 1 && norm[nlen - 1] != '/')
                    nlen--;
                if (nlen > 1)
                    nlen--;
                norm[nlen] = '\0';
            }
            continue;
        }

        if (nlen > 1 && nlen < PATH_MAX - 1)
            norm[nlen++] = '/';
        for (int k = 0; k < clen && nlen < PATH_MAX - 1; k++)
            norm[nlen++] = tmp[start + k];
        norm[nlen] = '\0';
    }

    str_copy(out, norm, max);
}

static int cmd_ls(int argc, char **argv)
{
    const char *target = (argc > 1) ? argv[1] : "";
    char path[PATH_MAX];
    resolve_path(target, path, PATH_MAX);

    fat16_dirent_t ent;
    uint32_t idx = 0;
    int count = 0;

    while (1) {
        int rc = sys_kreaddir(path, idx, &ent);
        if (rc == 0)
            break;
        if (rc < 0) {
            sys_kprint("ls: cannot access directory\n");
            return 1;
        }
        if (ent.attr & FAT16_ATTR_DIRECTORY) {
            sys_kprint("  <DIR>  ");
            sys_kprint(ent.name);
            sys_kprint("\n");
        } else {
            sys_kprint("  ");
            print_u32(ent.size);
            sys_kprint("  ");
            sys_kprint(ent.name);
            sys_kprint("\n");
        }
        idx++;
        count++;
    }
    if (count == 0)
        sys_kprint("  (empty)\n");
    return 0;
}

static int cmd_cd(int argc, char **argv)
{
    if (argc < 2 || !argv[1][0]) {
        str_copy(cwd, "/", PATH_MAX);
        return 0;
    }

    char path[PATH_MAX];
    resolve_path(argv[1], path, PATH_MAX);

    fat16_dirent_t ent;
    int rc = sys_kreaddir(path, 0, &ent);
    if (rc < 0) {
        sys_kprint("bash: cd: no such directory\n");
        return 1;
    }
    str_copy(cwd, path, PATH_MAX);
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        sys_kprint("usage: cat <file>...\n");
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        char path[PATH_MAX];
        resolve_path(argv[i], path, PATH_MAX);

        int fd = sys_kopen(path, 0);
        if (fd < 0) {
            sys_kprint("cat: ");
            sys_kprint(argv[i]);
            sys_kprint(": No such file\n");
            status = 1;
            continue;
        }

        char buf[129];
        int total = 0;
        while (1) {
            int n = sys_kread(fd, buf, 128);
            if (n <= 0)
                break;
            buf[n] = '\0';
            sys_kprint(buf);
            total += n;
        }
        sys_kclose(fd);
        if (total > 0)
            sys_kprint("\n");
    }
    return status;
}

static int cmd_echo(int argc, char **argv)
{
    int i = 1;
    int newline = 1;
    if (i < argc && str_eq(argv[i], "-n")) {
        newline = 0;
        i++;
    }
    for (; i < argc; i++) {
        if (str_eq(argv[i], "$?")) {
            print_int(last_status);
        } else if (str_eq(argv[i], "$PWD")) {
            sys_kprint(cwd);
        } else {
            sys_kprint(argv[i]);
        }
        if (i + 1 < argc)
            put_char(' ');
    }
    if (newline)
        put_char('\n');
    return 0;
}

static int cmd_help(void)
{
    sys_kprint("GNU bash-like shell for ex-nihilo-OS\n");
    sys_kprint("Commands:\n");
    sys_kprint("  pwd             print working directory\n");
    sys_kprint("  ls [dir]        list directory contents\n");
    sys_kprint("  cd [dir]        change directory\n");
    sys_kprint("  cat <file>...   print file contents\n");
    sys_kprint("  echo [-n] ...   display a line of text ($?, $PWD)\n");
    sys_kprint("  clear           clear the terminal screen\n");
    sys_kprint("  uname           print system name\n");
    sys_kprint("  whoami          print effective user\n");
    sys_kprint("  true / false    return 0 / 1\n");
    sys_kprint("  help            show this help\n");
    sys_kprint("  exit            exit the shell\n");
    sys_kprint("Syntax: quotes '...' \"...\", comments #, operators ; and &&\n");
    return 0;
}

static int run_command(int argc, char **argv, int *should_exit)
{
    if (argc == 0)
        return last_status;

    const char *cmd = argv[0];
    if (str_eq(cmd, "help"))
        return cmd_help();
    if (str_eq(cmd, "pwd")) {
        sys_kprint(cwd);
        put_char('\n');
        return 0;
    }
    if (str_eq(cmd, "ls"))
        return cmd_ls(argc, argv);
    if (str_eq(cmd, "cd"))
        return cmd_cd(argc, argv);
    if (str_eq(cmd, "cat"))
        return cmd_cat(argc, argv);
    if (str_eq(cmd, "echo"))
        return cmd_echo(argc, argv);
    if (str_eq(cmd, "clear")) {
        sys_kclear();
        return 0;
    }
    if (str_eq(cmd, "uname")) {
        sys_kprint("ex-nihilo-OS\n");
        return 0;
    }
    if (str_eq(cmd, "whoami")) {
        sys_kprint("user\n");
        return 0;
    }
    if (str_eq(cmd, "true"))
        return 0;
    if (str_eq(cmd, "false"))
        return 1;
    if (str_eq(cmd, "exit")) {
        *should_exit = 1;
        return 0;
    }

    sys_kprint("bash: ");
    sys_kprint(cmd);
    sys_kprint(": command not found\n");
    return 127;
}

static void eval_line(char *line, int *should_exit)
{
    char *p = line;
    int run_next = 1;

    while (*p && !*should_exit) {
        char *argv[MAX_ARGS];
        int argc = 0;
        int sep = 0; /* 0 = end, 1 = ';', 2 = '&&' */

        while (*p) {
            while (*p == ' ' || *p == '\t')
                p++;
            if (!*p || *p == '#') {
                *p = '\0';
                break;
            }
            if (*p == ';') {
                p++;
                sep = 1;
                break;
            }
            if (p[0] == '&' && p[1] == '&') {
                p += 2;
                sep = 2;
                break;
            }

            if (argc >= MAX_ARGS - 1)
                break;

            char *arg = p;
            char *dst = p;
            while (*p) {
                if (*p == '\'' || *p == '"') {
                    char q = *p++;
                    while (*p && *p != q)
                        *dst++ = *p++;
                    if (*p == q)
                        p++;
                } else if (*p == ' ' || *p == '\t' || *p == ';' ||
                           *p == '#' || (p[0] == '&' && p[1] == '&')) {
                    break;
                } else {
                    *dst++ = *p++;
                }
            }
            if (*p == ';') {
                *dst = '\0';
                p++;
                sep = 1;
                argv[argc++] = arg;
                break;
            }
            if (p[0] == '&' && p[1] == '&') {
                *dst = '\0';
                p += 2;
                sep = 2;
                argv[argc++] = arg;
                break;
            }
            if (*p == '#') {
                *dst = '\0';
                *p = '\0';
                argv[argc++] = arg;
                break;
            }
            if (*p)
                p++;
            *dst = '\0';
            argv[argc++] = arg;
        }

        argv[argc] = 0;
        if (run_next && argc > 0)
            last_status = run_command(argc, argv, should_exit);

        if (sep == 2)
            run_next = (last_status == 0);
        else
            run_next = 1;
    }
}

static int read_line(char *buf, int max)
{
    int len = 0;
    for (;;) {
        int c = sys_kgetc();
        if (c == '\n') {
            put_char('\n');
            buf[len] = '\0';
            return len;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                sys_kprint("\b \b");
            }
            continue;
        }
        if (c >= 32 && c < 127 && len < max - 1) {
            buf[len++] = (char)c;
            put_char((char)c);
        }
    }
}

int prog_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    sys_kclear();
    sys_kprint("ex-nihilo-OS bash-like shell (type 'help' for commands)\n");

    char line[LINE_MAX];
    int should_exit = 0;
    while (!should_exit) {
        sys_kprint("user@ex-nihilo:");
        sys_kprint(cwd);
        sys_kprint("$ ");

        read_line(line, LINE_MAX);
        eval_line(line, &should_exit);
    }

    return 0;
}
