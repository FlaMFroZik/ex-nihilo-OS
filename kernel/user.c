#include <stdint.h>
#include "kernel/syscall.h"
#include "kernel/user.h"

/* ================================================================
 * user_main — демо-код, который выполняется в Ring 3 (CPL=3).
 *
 * Он НЕ может вызвать kopen() напрямую: kopen — функция ядра, а
 * этот код привилегий на неё не имеет. Поэтому каждый вызов идёт
 * через стаб sys_*() -> `int 0x80` -> Ring 0 -> kopen()/kread()/...
 * -> результат обратно через iret. Итоги складываются в
 * user_demo_state, чтобы kmain (Ring 0) после SYS_KEXIT напечатал,
 * что реально вернулось из syscall'ов.
 * ================================================================ */

/* Поля — см. kernel/user.h: open_rc/read_rc/close_rc — что вернули
 * SYS_KOPEN/SYS_KREAD/SYS_KCLOSE, cpl — CPL из CS, text — файл. */
struct user_demo_state user_demo_state;

void user_main(void)
{
    /* CPL виден в младших 2 битах CS: здесь должно быть 3. */
    uint16_t cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    user_demo_state.cpl = cs & 3u;

    sys_kprint("user(ring3): calling kopen(\"/HELLO.TXT\") via int 0x80...\n");

    int fd = sys_kopen("/HELLO.TXT", 0 /* FAT16_O_RDONLY */);
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
    sys_kprint("user(ring3): execve(\"/USER1.ELF\")...\n");

    static char arg0[] = "/USER1.ELF";
    static char arg1[] = "from-user-main";
    char *argv[3];
    argv[0] = arg0;
    argv[1] = arg1;
    argv[2] = 0;

    user_demo_state.execve_attempted = 1;
    int erc = sys_execve("/USER1.ELF", argv, 0);
    user_demo_state.execve_rc = erc;    /* успех сюда не возвращается */

    sys_kprint("user(ring3): execve failed, exiting via SYS_KEXIT\n");
    sys_kexit(7);   /* enter_usermode() в Ring 0 вернёт 7 */
}
