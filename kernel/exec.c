#include <stdint.h>
#include <stddef.h>
#include "kernel/exec.h"
#include "kernel/kapi.h"
#include "kernel/paging.h"
#include "hal/pmm.h"

/* --- ELF32 (только то, что нужно загрузчику) --- */
typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf32_ehdr_t;

typedef struct {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} elf32_phdr_t;

#define PT_LOAD     1
#define ET_EXEC     2
#define EM_386      3
#define ELF_ERR_EXEC (-8)       /* -ENOEXEC: не наш формат */
#define ELF_ERR_NOMEM (-12)
#define ELF_ERR_FBIG  (-27)

#define EXEC_MAX_BYTES  (64 * 1024)
#define EXEC_MAX_PAGES  (EXEC_MAX_BYTES / PAGE_SIZE)
#define EXEC_MAX_ARGS   16
#define EXEC_MAX_ARG_LEN 256
#define EXEC_LOAD_MIN   0x100000u   /* ниже — BIOS, ядро и его стек */

static void *file_pages[EXEC_MAX_PAGES];
static void *stack_page;            /* стек текущего exec-образа */

/* Файл лежит по страницам, отданным pmm по одной: доступ по смещению. */
static uint8_t *file_ptr(uint32_t off)
{
    return (uint8_t *)file_pages[off / PAGE_SIZE] + (off % PAGE_SIZE);
}

static void file_read(void *dst, uint32_t off, uint32_t len)
{
    uint8_t *d = dst;
    while (len) {
        uint32_t chunk = PAGE_SIZE - (off % PAGE_SIZE);
        if (chunk > len) chunk = len;
        const uint8_t *s = file_ptr(off);
        for (uint32_t i = 0; i < chunk; i++)
            d[i] = s[i];
        d += chunk; off += chunk; len -= chunk;
    }
}

static void free_file_pages(int n)
{
    for (int i = 0; i < n; i++) {
        kpmm_free(file_pages[i]);
        file_pages[i] = NULL;
    }
}

void kexec_cleanup(void)
{
    if (stack_page) {
        kpmm_free(stack_page);
        stack_page = NULL;
    }
}

int kexecve(const char *path, char *const argv[], char *const envp[])
{
    if (path == NULL)
        return FAT16_ERR_INVAL;

    int fd = kopen(path, FAT16_O_RDONLY);
    if (fd < 0)
        return fd;

    /* --- читаем файл целиком, по странице за раз --- */
    int npages = 0;
    uint32_t total = 0;
    int rc = FAT16_OK;
    for (;;) {
        if (total == EXEC_MAX_BYTES) {          /* есть ли ещё байт? */
            char extra;
            int n = kread(fd, &extra, 1);
            rc = (n > 0) ? ELF_ERR_FBIG : FAT16_OK;
            break;
        }
        if ((total % PAGE_SIZE) == 0) {
            file_pages[npages] = kpmm_alloc();
            if (file_pages[npages] == NULL) { rc = ELF_ERR_NOMEM; break; }
            npages++;
        }
        uint32_t want = PAGE_SIZE - (total % PAGE_SIZE);
        int n = kread(fd, file_ptr(total), want);
        if (n < 0) { rc = n; break; }
        if (n == 0) break;                      /* EOF */
        total += (uint32_t)n;
    }
    kclose(fd);
    if (rc != FAT16_OK) { free_file_pages(npages); return rc; }

    /* --- заголовок ELF --- */
    if (total < sizeof(elf32_ehdr_t)) { free_file_pages(npages); return ELF_ERR_EXEC; }
    elf32_ehdr_t eh;
    file_read(&eh, 0, sizeof(eh));

    if (eh.ident[0] != 0x7F || eh.ident[1] != 'E' || eh.ident[2] != 'L' ||
        eh.ident[3] != 'F' || eh.ident[4] != 1 /* 32 бита */ ||
        eh.ident[5] != 1 /* little-endian */ ||
        eh.type != ET_EXEC || eh.machine != EM_386 ||
        eh.phentsize != sizeof(elf32_phdr_t) ||
        eh.phnum == 0 || eh.phnum > 16 ||
        eh.phoff + (uint32_t)eh.phnum * sizeof(elf32_phdr_t) > total) {
        free_file_pages(npages);
        return ELF_ERR_EXEC;
    }

    /* --- сегменты PT_LOAD: проход 1 — только проверка --- */
    uint64_t mem_top = (uint64_t)pmm_total_pages() * PAGE_SIZE;
    if (mem_top > 0xFFFFFFFFull) mem_top = 0xFFFFFFFFull;
    int loads = 0, entry_ok = 0;

    for (uint32_t i = 0; i < eh.phnum; i++) {
        elf32_phdr_t ph;
        file_read(&ph, eh.phoff + i * sizeof(ph), sizeof(ph));
        if (ph.type != PT_LOAD)
            continue;
        if (ph.filesz > ph.memsz ||
            ph.offset + ph.filesz > total ||
            ph.vaddr < EXEC_LOAD_MIN ||
            (uint64_t)ph.vaddr + ph.memsz > mem_top ||
            ph.vaddr + ph.memsz < ph.vaddr) {
            free_file_pages(npages);
            return ELF_ERR_EXEC;
        }
        if (eh.entry >= ph.vaddr && eh.entry < ph.vaddr + ph.memsz)
            entry_ok = 1;
        loads++;
    }
    if (loads == 0 || !entry_ok) { free_file_pages(npages); return ELF_ERR_EXEC; }

    /* Проход 2 — пометить страницы сегмента User+RW (иначе Ring 3
     * в них не попадёт, а ядро с CR0.WP не сможет писать в RO),
     * скопировать файл и обнулить bss. Проход 3 — ужать права до
     * заявленных в PF_W: текст снова станет read-only. */
    for (uint32_t pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < eh.phnum; i++) {
            elf32_phdr_t ph;
            file_read(&ph, eh.phoff + i * sizeof(ph), sizeof(ph));
            if (ph.type != PT_LOAD)
                continue;
            if (pass == 0) {
                paging_map(ph.vaddr, ph.memsz, 1, 1);
                uint8_t *dst = (uint8_t *)(uintptr_t)ph.vaddr;
                uint32_t done = 0;
                while (done < ph.filesz) {      /* копия сквозь страницы */
                    uint32_t chunk = ph.filesz - done;
                    uint32_t off = ph.offset + done;
                    if (chunk > PAGE_SIZE - (off % PAGE_SIZE))
                        chunk = PAGE_SIZE - (off % PAGE_SIZE);
                    const uint8_t *s = file_ptr(off);
                    for (uint32_t j = 0; j < chunk; j++)
                        dst[done + j] = s[j];
                    done += chunk;
                }
                for (uint32_t j = ph.filesz; j < ph.memsz; j++)
                    dst[j] = 0;                 /* bss */
            } else {
                paging_map(ph.vaddr, ph.memsz, 1, (ph.flags & 0x2) != 0);
            }
        }
    }

    /* --- стек новой программы: строки, затем argc/argv/envp --- */
    void *new_stack = kpmm_alloc();
    if (new_stack == NULL) { free_file_pages(npages); return ELF_ERR_NOMEM; }

    char *sp = (char *)new_stack + PAGE_SIZE;
    char *arg_ptrs[EXEC_MAX_ARGS + 1];
    char *env_ptrs[EXEC_MAX_ARGS + 1];
    int argc = 0, envc = 0, bad = 0;

    if (argv != NULL)
        for (; argv[argc] != NULL; argc++) {
            if (argc >= EXEC_MAX_ARGS) { bad = 1; break; }
            uint32_t len = 0;
            while (argv[argc][len] != '\0' && len < EXEC_MAX_ARG_LEN) len++;
            if (len >= EXEC_MAX_ARG_LEN) { bad = 1; break; }
            sp -= len + 1;
            for (uint32_t j = 0; j <= len; j++)
                sp[j] = argv[argc][j];
            arg_ptrs[argc] = sp;
        }
    if (!bad && envp != NULL)
        for (; envp[envc] != NULL; envc++) {
            if (envc >= EXEC_MAX_ARGS) { bad = 1; break; }
            uint32_t len = 0;
            while (envp[envc][len] != '\0' && len < EXEC_MAX_ARG_LEN) len++;
            if (len >= EXEC_MAX_ARG_LEN) { bad = 1; break; }
            sp -= len + 1;
            for (uint32_t j = 0; j <= len; j++)
                sp[j] = envp[envc][j];
            env_ptrs[envc] = sp;
        }
    if (bad) {
        kpmm_free(new_stack);
        free_file_pages(npages);
        return FAT16_ERR_INVAL;
    }

    sp = (char *)((uintptr_t)sp & ~15u);
    uint32_t words = (uint32_t)(1 + argc + 1 + envc + 1);
    sp -= words * 4;
    uint32_t *frame = (uint32_t *)(uintptr_t)sp;
    uint32_t w = 0;
    frame[w++] = (uint32_t)argc;
    for (int i = 0; i < argc; i++) frame[w++] = (uint32_t)(uintptr_t)arg_ptrs[i];
    frame[w++] = 0;
    for (int i = 0; i < envc; i++) frame[w++] = (uint32_t)(uintptr_t)env_ptrs[i];
    frame[w++] = 0;

    paging_map((uint32_t)(uintptr_t)new_stack, PAGE_SIZE, 1, 1);

    /* Образ загружен: буфер файла и старый стек больше не нужны
     * (исполняемся на стеке Ring 0, старый Ring 3 брошен). */
    free_file_pages(npages);
    if (stack_page != NULL)
        kpmm_free(stack_page);
    stack_page = new_stack;

    exec_jump(eh.entry, (uint32_t)(uintptr_t)sp);
}
