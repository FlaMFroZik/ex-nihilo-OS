#include "kernel/syscall.h"

/* ================================================================
 * SHELL.ELF — пользовательская оболочка (Ring 3), загружаемая
 * ядром через kexecve("/SHELL.ELF", ...).
 * Сразу после запуска очищает весь экран и выводит приглашение.
 * ================================================================ */

int prog_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    sys_kclear();
    sys_kprint("ex-nihilo-OS shell\n$ ");
    return 0;
}
