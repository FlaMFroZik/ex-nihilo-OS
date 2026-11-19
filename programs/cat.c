#include "kernel/syscall.h"

int prog_main(int argc, char **argv)
{
    if (argc < 2) {
        sys_kprint("usage: cat <file>...\n");
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = sys_kopen(argv[i], 0);
        if (fd < 0) {
            sys_kprint("cat: ");
            sys_kprint(argv[i]);
            sys_kprint(": No such file\n");
            status = 1;
            continue;
        }

        char buf[129];
        int total = 0;
        while (1) {
            int n = sys_kread(fd, buf, 128);
            if (n <= 0)
                break;
            buf[n] = '\0';
            sys_kprint(buf);
            total += n;
        }
        sys_kclose(fd);
        if (total > 0)
            sys_kprint("\n");
    }
    return status;
}
