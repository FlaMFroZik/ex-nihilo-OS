#include "kernel/syscall.h"

int prog_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    sys_kprint("user\n");
    return 0;
}
