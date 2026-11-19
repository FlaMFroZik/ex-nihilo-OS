#include <stdint.h>
#include "kernel/syscall.h"
#include "kernel/user.h"

/* ================================================================
 * Код, который выполняется в Ring 3 (CPL=3).
 *
 * С включённой защитой памяти (kernel/paging.c) Ring 3 физически
 * может дотянуться только до страниц, помеченных User. Поэтому
 * весь этот файл живёт в отдельном регионе образа: функции — в
 * .usertext, данные — в .userdata/.userbss (см. linker.ld,
 * user_region_start..end), и paging помечает эти страницы User.
 * Строковые литералы сюда не годятся: они уехали бы в .rodata
 * ядра (Supervisor) — и первое же чтение из Ring 3 дало бы #PF.
 * Поэтому все строки — массивы в .userdata.
 * ================================================================ */

#define USER_TEXT __attribute__((section(".usertext")))
#define USER_DATA __attribute__((section(".userdata")))
#define USER_BSS  __attribute__((section(".userbss")))

USER_BSS struct user_demo_state user_demo_state;

static char USER_DATA msg_calling[] =
    "user(ring3): calling kopen(\"/HELLO.TXT\") via int 0x80...\n";
static char USER_DATA path_hello[] = "/HELLO.TXT";
static char USER_DATA path_shell[] = "/SHELL.ELF";
static char USER_DATA arg0_shell[] = "/SHELL.ELF";
static char USER_DATA msg_shell_fail[] = "user_shell_main: execve(/SHELL.ELF) failed\n";

USER_TEXT void user_main(void)
{
    /* CPL виден в младших 2 битах CS: здесь должно быть 3. */
    uint16_t cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    user_demo_state.cpl = cs & 3u;

    sys_kprint(msg_calling);

    int fd = sys_kopen(path_hello, 0 /* FAT16_O_RDONLY */);
    user_demo_state.open_rc = fd;

    int n = -1;
    if (fd >= 0) {
        n = sys_kread(fd, user_demo_state.text, sizeof(user_demo_state.text) - 1);
        user_demo_state.close_rc = sys_kclose(fd);
    }
    user_demo_state.read_rc = n;
    if (n > 0)
        user_demo_state.text[n] = '\0';
    else
        user_demo_state.text[0] = '\0';

    /* --- execve: заменяем себя настоящим ELF с диска --- */
    sys_kexit(0);
}

/* Демо защиты памяти: лезем писать прямо в код ядра (0x10000 —
 * Supervisor-страница). С пейджингом это #PF, обработчик убьёт
 * этот код и enter_usermode() вернёт USER_EXIT_PAGEFAULT.
 * Если защиты нет, запишем байт и честно выйдем с кодом 8 —
 * kmain увидит это как [BUG]. */
USER_TEXT void user_bad_main(void)
{
    *(volatile uint8_t *)0x10000 = 0x42;
    sys_kexit(8);
}

USER_TEXT void user_shell_main(void)
{
    char *argv[2];
    argv[0] = arg0_shell;
    argv[1] = 0;
    sys_execve(path_shell, argv, 0);
    sys_kprint(msg_shell_fail);
    sys_kexit(1);
}
