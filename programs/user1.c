#include "kernel/syscall.h"

/* ================================================================
 * USER1.ELF — первая настоящая пользовательская программа: это
 * отдельный ELF-файл на диске FAT16, а не код, влинкованный в ядро.
 * Сюда попадаем через sys_execve() из user_main, обратно execve
 * при успехе не возвращается. Демонстрируем именно это: сразу
 * заменяем себя вторым бинарником, USER2.ELF, с новым argv.
 * ================================================================ */

int prog_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    sys_kprint("USER1.ELF: hello from ELF binary\n");

    static char arg0[] = "USER2.ELF";
    static char arg1[] = "passed-by-user1";
    char *new_argv[3];
    new_argv[0] = arg0;
    new_argv[1] = arg1;
    new_argv[2] = 0;

    sys_kprint("USER1.ELF: execve -> /USER2.ELF\n");
    int rc = sys_execve("/USER2.ELF", new_argv, 0);

    /* Сюда — только если execve не удался. */
    sys_kprint("USER1.ELF: execve FAILED\n");
    return (rc < 0) ? 9 : rc;
}
