#include "kernel/syscall.h"

/* ================================================================
 * USER2.ELF — второй бинарник цепочки execve. Его задача:
 * доказать, что argv пережил два execve подряд (kmain -> user_main
 * -> USER1.ELF -> USER2.ELF), и выйти с кодом 42, который kmain
 * проверит после SYS_KEXIT.
 * ================================================================ */

int prog_main(int argc, char **argv)
{
    sys_kprint("USER2.ELF: argv[1]=");
    sys_kprint((argc > 1 && argv[1] != 0) ? argv[1] : "<none>");
    sys_kprint("\n");
    return 42;
}
