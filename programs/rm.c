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
    if (argc < 2) {
        sys_kprint("usage: rm <file|dir>...\n");
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (str_eq(argv[i], "-r") || str_eq(argv[i], "-rf") || str_eq(argv[i], "-f"))
            continue;
        if (sys_kunlink(argv[i]) < 0) {
            sys_kprint("rm: cannot remove '");
            sys_kprint(argv[i]);
            sys_kprint("'\n");
            status = 1;
        }
    }
    return status;
}
