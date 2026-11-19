#include <stdint.h>
#include "kernel/syscall.h"
#include "fs/fat16.h"

/* ================================================================
 * SHELL.ELF — интерактивная оболочка (Ring 3), запускаемая ядром.
 * Сразу после запуска очищает экран и работает в цикле команд
 * с приглашением вида "> ".
 * ================================================================ */

#define LINE_MAX 128
#define PATH_MAX 128

static char cwd[PATH_MAX] = "/";

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

    /* Нормализация компонент '.' и '..' */
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

        if (clen == 1 && tmp[start] == '.') {
            continue;
        }
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

static void cmd_ls(const char *arg)
{
    char path[PATH_MAX];
    resolve_path(arg, path, PATH_MAX);

    fat16_dirent_t ent;
    uint32_t idx = 0;
    int count = 0;

    while (1) {
        int rc = sys_kreaddir(path, idx, &ent);
        if (rc == 1)
            break;
        if (rc < 0) {
            sys_kprint("ls: cannot open directory\n");
            return;
        }
        if (ent.attr & FAT16_ATTR_DIR) {
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
}

static void cmd_cd(const char *arg)
{
    if (!arg || !arg[0]) {
        str_copy(cwd, "/", PATH_MAX);
        return;
    }

    char path[PATH_MAX];
    resolve_path(arg, path, PATH_MAX);

    fat16_dirent_t ent;
    int rc = sys_kreaddir(path, 0, &ent);
    if (rc < 0) {
        sys_kprint("cd: no such directory\n");
        return;
    }
    str_copy(cwd, path, PATH_MAX);
}

static void cmd_cat(const char *arg)
{
    if (!arg || !arg[0]) {
        sys_kprint("usage: cat <file>\n");
        return;
    }

    char path[PATH_MAX];
    resolve_path(arg, path, PATH_MAX);

    int fd = sys_kopen(path, 0);
    if (fd < 0) {
        sys_kprint("cat: cannot open file\n");
        return;
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

static void cmd_help(void)
{
    sys_kprint("Commands:\n");
    sys_kprint("  help        - show this help\n");
    sys_kprint("  pwd         - print working directory\n");
    sys_kprint("  ls [dir]    - list directory contents\n");
    sys_kprint("  cd <dir>    - change directory\n");
    sys_kprint("  cat <file>  - print file contents\n");
    sys_kprint("  echo <text> - print text\n");
    sys_kprint("  clear       - clear screen\n");
    sys_kprint("  exit        - exit shell\n");
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
    sys_kprint("ex-nihilo-OS shell (type 'help' for commands)\n");

    char line[LINE_MAX];
    for (;;) {
        sys_kprint(cwd);
        sys_kprint("> ");

        read_line(line, LINE_MAX);

        /* Пропускаем ведущие пробелы */
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            continue;

        /* Отделяем команду и первый аргумент */
        char *cmd = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        char *arg = p;
        if (*p) {
            *p++ = '\0';
            while (*p == ' ' || *p == '\t')
                p++;
            arg = p;
        }

        if (str_eq(cmd, "help")) {
            cmd_help();
        } else if (str_eq(cmd, "pwd")) {
            sys_kprint(cwd);
            sys_kprint("\n");
        } else if (str_eq(cmd, "ls")) {
            cmd_ls(arg);
        } else if (str_eq(cmd, "cd")) {
            cmd_cd(arg);
        } else if (str_eq(cmd, "cat")) {
            cmd_cat(arg);
        } else if (str_eq(cmd, "clear")) {
            sys_kclear();
        } else if (str_eq(cmd, "echo")) {
            sys_kprint(arg);
            sys_kprint("\n");
        } else if (str_eq(cmd, "exit")) {
            break;
        } else {
            sys_kprint("unknown command: ");
            sys_kprint(cmd);
            sys_kprint("\n");
        }
    }

    return 0;
}
