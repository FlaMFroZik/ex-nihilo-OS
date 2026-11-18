#ifndef USER_H
#define USER_H

#include <stdint.h>

/* Демо-код Ring 3 (kernel/user.c) и его состояние: заполняется
 * юзерским кодом по ходу syscall'ов, читается kmain после SYS_KEXIT. */
struct user_demo_state {
    int32_t  open_rc;
    int32_t  read_rc;
    int32_t  close_rc;
    uint32_t cpl;
    int32_t  execve_attempted;  /* 1 — ушли в execve("/USER1.ELF") */
    int32_t  execve_rc;         /* сюда попадаем, только если execve не удался */
    char     text[256];
};

extern struct user_demo_state user_demo_state;
void user_main(void);

/* Демо защиты памяти: пишет в код ядра и должен умереть по #PF. */
void user_bad_main(void);

#endif
