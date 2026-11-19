#include "kernel/syscall.h"

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

int prog_main(int argc, char **argv)
{
    int i = 1;
    int newline = 1;
    if (i < argc && str_eq(argv[i], "-n")) {
        newline = 0;
        i++;
    }
    for (; i < argc; i++) {
        sys_kprint(argv[i]);
        if (i + 1 < argc)
            sys_kprint(" ");
    }
    if (newline)
        sys_kprint("\n");
    return 0;
}
