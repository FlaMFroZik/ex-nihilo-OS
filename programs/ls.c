#include <stdint.h>
#include "kernel/syscall.h"
#include "fs/fat16.h"

static void put_char(char c)
{
    char buf[2] = { c, '\0' };
    sys_kprint(buf);
}

static void print_u32(uint32_t v)
{
    char buf[12];
    int i = 0;
    if (v == 0) {
        sys_kprint("0");
        return;
    }
    while (v > 0 && i < 11) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0)
        put_char(buf[--i]);
}

int prog_main(int argc, char **argv)
{
    const char *path = (argc > 1 && argv[1][0]) ? argv[1] : "/";
    fat16_dirent_t ent;
    uint32_t idx = 0;
    int count = 0;

    while (1) {
        int rc = sys_kreaddir(path, idx, &ent);
        if (rc == 0)
            break;
        if (rc < 0) {
            sys_kprint("ls: cannot access directory\n");
            return 1;
        }
        if (ent.attr & FAT16_ATTR_DIRECTORY) {
            sys_kprint("  <DIR>  ");
            sys_kprint(ent.name);
            sys_kprint("\n");
        } else {
            sys_kprint("  ");
            print_u32(ent.size);
            sys_kprint("  ");
            sys_kprint(ent.name);
            sys_kprint("\n");
        }
        idx++;
        count++;
    }
    if (count == 0)
        sys_kprint("  (empty)\n");
    return 0;
}
