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
static char USER_DATA msg_execve[] =
    "user(ring3): execve(\"/USER1.ELF\")...\n";
static char USER_DATA msg_execve_failed[] =
    "user(ring3): execve failed, exiting via SYS_KEXIT\n";
static char USER_DATA path_hello[] = "/HELLO.TXT";
static char USER_DATA path_user1[] = "/USER1.ELF";
static char USER_DATA arg0_user1[] = "/USER1.ELF";
static char USER_DATA arg1_user1[] = "from-user-main";
static char USER_DATA path_shell[] = "/SHELL.ELF";
static char USER_DATA arg0_shell[] = "/SHELL.ELF";

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
    sys_kprint(msg_execve);

    char *argv[3];
    argv[0] = arg0_user1;
    argv[1] = arg1_user1;
    argv[2] = 0;

    user_demo_state.execve_attempted = 1;
    int erc = sys_execve(path_user1, argv, 0);
    user_demo_state.execve_rc = erc;    /* успех сюда не возвращается */

    sys_kprint(msg_execve_failed);
    sys_kexit(7);   /* enter_usermode() в Ring 0 вернёт 7 */
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
    sys_kexit(1);
}
