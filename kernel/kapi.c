#include "kernel/kapi.h"
#include "hal/ata.h"
#include "hal/pmm.h"

/* Глобальное состояние ядра для ФС: том монтируется один раз,
 * дальше kopen()/kread()/... работают без параметра vol — как
 * open()/read() в обычном ядре, где том спрятан за fd. */
static blockdev_t     k_disk;
static fat16_volume_t k_vol;
static int            k_mounted;

#define K_MAX_FDS 16
static fat16_file_t k_fds[K_MAX_FDS];

int kmount(void)
{
    if (k_mounted)
        return FAT16_OK;
    if (ata_blockdev(&k_disk) != 0)
        return FAT16_ERR_IO;
    int rc = fat16_mount(&k_disk, EXNIHILO_FAT_LBA, &k_vol);
    if (rc == FAT16_OK)
        k_mounted = 1;
    return rc;
}

int kis_mounted(void) { return k_mounted; }

static fat16_file_t *kfd(int fd)
{
    if (!k_mounted || fd < 0 || fd >= K_MAX_FDS || !k_fds[fd].used)
        return NULL;
    return &k_fds[fd];
}

int kopen(const char *path, int flags)
{
    if (!k_mounted || path == NULL)
        return FAT16_ERR_INVAL;
    for (int fd = 0; fd < K_MAX_FDS; fd++) {
        if (k_fds[fd].used)
            continue;
        int rc = fat16_open(&k_vol, path, flags, &k_fds[fd]);
        return (rc == FAT16_OK) ? fd : rc;
    }
    return FAT16_ERR_NOMEM;
}

int kclose(int fd)
{
    fat16_file_t *f = kfd(fd);
    return f ? fat16_close(&k_vol, f) : FAT16_ERR_INVAL;
}

int kread(int fd, void *buf, uint32_t count)
{
    fat16_file_t *f = kfd(fd);
    return f ? fat16_read(&k_vol, f, buf, count) : FAT16_ERR_INVAL;
}

int kwrite(int fd, const void *buf, uint32_t count)
{
    fat16_file_t *f = kfd(fd);
    return f ? fat16_write(&k_vol, f, buf, count) : FAT16_ERR_INVAL;
}

int kseek(int fd, uint32_t offset, int whence)
{
    fat16_file_t *f = kfd(fd);
    return f ? fat16_seek(&k_vol, f, offset, whence) : FAT16_ERR_INVAL;
}

int kunlink(const char *path)
{
    return (k_mounted && path) ? fat16_unlink(&k_vol, path) : FAT16_ERR_INVAL;
}

int kreaddir(const char *path, uint32_t index, fat16_dirent_t *out)
{
    return (k_mounted && path && out) ? fat16_readdir(&k_vol, path, index, out)
                                      : FAT16_ERR_INVAL;
}

int kexists(const char *path)
{
    return (k_mounted && path) ? fat16_exists(&k_vol, path) : 0;
}

uint32_t kfree_clusters(void)  { return k_mounted ? fat16_free_clusters(&k_vol) : 0; }
uint32_t ktotal_clusters(void) { return k_mounted ? fat16_total_clusters(&k_vol) : 0; }

void *kpmm_alloc(void)           { return pmm_alloc(); }
void kpmm_free(void *addr)       { pmm_free(addr); }
uint32_t kpmm_free_pages(void)   { return pmm_free_pages(); }

static kprint_fn_t k_printer;
void kprint_register(kprint_fn_t fn) { k_printer = fn; }
void kprint(const char *str)         { if (k_printer && str) k_printer(str); }
