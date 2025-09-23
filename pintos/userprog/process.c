#include "userprog/process.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "intrinsic.h"
#include "lib/stdio.h" // hex_dump
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h" // lock
#include "threads/vaddr.h"
#include "userprog/gdt.h"
#include "userprog/syscall.h" // 파일디스크립터 구조체
#include "userprog/tss.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup(void);
static bool load(const char *file_name, struct intr_frame *if_);
static void initd(void *f_name);
static void __do_fork(void *);
static int parse_args(char *cmdline, char **argv);
static void setup_stack_args(struct intr_frame *if_, char **argv, int argc);
static void print_dump(struct intr_frame *if_, size_t view_byte);
static bool cpy_fd_table(struct thread *p_th, struct thread *c_th);
static struct lock filesyslock;
static int exit_status = -1;

extern bool thread_tests;

/* General process initializer for initd and other process. */
static void process_init(void)
{
    struct thread *current = thread_current();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t process_create_initd(const char *file_name)
{
    char *fn_copy;
    tid_t tid;

    /* Make a copy of FILE_NAME.
     * Otherwise there's a race between the caller and load(). */
    fn_copy = palloc_get_page(0);
    if (fn_copy == NULL)
        return TID_ERROR;
    strlcpy(fn_copy, file_name, PGSIZE);

    char cmd_copy[64];
    strlcpy(cmd_copy, fn_copy, sizeof cmd_copy);

    char *saveptr;
    strtok_r(cmd_copy, " ", &saveptr);

    strlcpy(thread_current()->name, cmd_copy, sizeof cmd_copy);
    /* Create a new thread to execute FILE_NAME. */
    tid = thread_create(cmd_copy, PRI_DEFAULT, initd, fn_copy);
    if (tid == TID_ERROR)
        palloc_free_page(fn_copy);
    return tid;
}

/* A thread function that launches first user process. */
static void initd(void *cmdline)
{
#ifdef VM
    supplemental_page_table_init(&thread_current()->spt);
#endif

    process_init(); // 현재스레드를 프로세스로서 준비하는 함수.
    // Pintos에서는 프로세스 = 스레드 1개 + 별도의 주소 공간
    // 아직 ELF로딩 안함
    if (process_exec(cmdline) < 0)
        PANIC("Fail to launch initd\n");
    NOT_REACHED();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t process_fork(const char *name, struct intr_frame *if_)
{
    struct fork_aux *aux = malloc(sizeof(struct fork_aux));
    aux->p_if = *if_;
    aux->parent = thread_current();
    sema_init(&aux->done, 0);
    aux->success = false;
    /* Clone current thread to new thread.*/
    tid_t tid = thread_create(name, PRI_DEFAULT, __do_fork, aux);
    if (!tid)
    {
        free(aux);
        return TID_ERROR;
    }

    sema_down(&aux->done);
    tid_t result = aux->success ? tid : TID_ERROR;
    free(aux);
    return result;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool duplicate_pte(uint64_t *pte, void *va, void *aux)
{
    struct thread *current = thread_current();
    struct thread *parent = (struct thread *)aux;
    void *parent_page;
    void *newpage;
    bool writable;

    // 1. 커널이면 복사할 필요가 없다. (공유하기 때문에)
    if (is_kern_pte(pte))
        return true;

    /* 2. va를 통해 물리메모리주소를 얻어오는 과정 */
    parent_page = pml4_get_page(parent->pml4, va);
    if (!parent_page)
        return false;

    /* 3. 새로운 페이지를 할당받고, 그 주소를 복사 */
    newpage = palloc_get_page(PAL_USER | PAL_ZERO);
    if (!newpage)
        return false;
    memcpy(newpage, parent_page, PGSIZE);

    /* 4. 해당페이지의 권한 */
    writable = is_writable(pte);

    /* 5. 자식의 가상주소(upage = va) 를 새로 할당한 커널 주소
     * (kpage = newpage) 가 가리키는 물리 프레임에 매핑해라 */
    if (!pml4_set_page(current->pml4, va, newpage, writable))
    {
        /* 6. TODO: if fail to insert page, do error handling. */
        palloc_free_page(newpage);
        return false;
    }
    return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void __do_fork(void *aux)
{
    struct fork_aux *arg = aux;
    struct intr_frame if_;
    struct thread *parent = arg->parent;
    struct thread *current = thread_current();
    /* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
    // struct intr_frame *parent_if = arg->p_if;
    bool succ = true;

    /* 1. Read the cpu context to local stack. */
    memcpy(&if_, &arg->p_if, sizeof(struct intr_frame));

    /* 2. Duplicate PT */
    current->pml4 = pml4_create();
    if (current->pml4 == NULL)
    {
        succ = false;
        goto error;
    }
    process_activate(current);
#ifdef VM
    supplemental_page_table_init(&current->spt);
    if (!supplemental_page_table_copy(&current->spt, &parent->spt))
        goto error;
#else
    if (!pml4_for_each(parent->pml4, duplicate_pte, parent))
    {
        succ = false;
        goto error;
    }
#endif

    /* TODO: Your code goes here.
     * TODO: Hint) To duplicate the file object, use `uplicate`
     * TODO:       in include/filesys/file.h. Note that parent should not return
     * TODO:       from the fork() until this function successfully duplicates
     * TODO:       the resources of parent.*/

    // lock_acquire(&filesyslock);
    if (!cpy_fd_table(parent, current))
    {
        // lock_release(&filesyslock);
        succ = false;
        goto error;
    }
    // lock_release(&filesyslock);
    // process_init(); // 이건 왜있지?
    if_.R.rax = 0;

    /* 4) 부모에게 결과 통지 */
    arg->success = succ;
    sema_up(&arg->done);
    /* Finally, switch to the newly created process. */
    do_iret(&if_);
error:
    /* 5. 만약 실패하더라도 결과 통지 */
    arg->success = succ;
    sema_up(&arg->done);

    thread_exit();
}
static bool cpy_fd_table(struct thread *p_th, struct thread *c_th)
{
    struct list_elem *e;
    /* 1. 부모의 fd_list를 순회 */
    for (e = list_begin(&p_th->fd_list); e != list_end(&p_th->fd_list);
         e = list_next(e))
    {
        /* 2. 부모의 fd데이터 새로운 자식의 fd */
        struct file_descriptor *fd_s =
            list_entry(e, struct file_descriptor, elem);
        struct file_descriptor *nfd_s = malloc(sizeof(struct file_descriptor));
        if (nfd_s == NULL)
            return false;

        /* 3. 자식 fd구조체에 할당 */
        nfd_s->fd = fd_s->fd;
        nfd_s->file = file_duplicate(fd_s->file);
        if (nfd_s->file == NULL)
        {
            free(nfd_s);
            return false;
        }
        /* 4. 자식 스레드에 삽입 */
        list_push_back(&c_th->fd_list, &nfd_s->elem);
    }
    c_th->next_fd = p_th->next_fd;
    return true;
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int process_exec(void *f_name)
{
    char *file_name = f_name;
    bool success;

    /* 1. 인터럽트 프레임 생성 */
    struct intr_frame _if;
    _if.ds = _if.es = _if.ss = SEL_UDSEG;
    _if.cs = SEL_UCSEG;
    _if.eflags = FLAG_IF | FLAG_MBS;

    /* 2. 안에있는 옛 데이터 초기화 */
    process_cleanup();

    /* 3. 데이터 로드 */
    success = load(file_name, &_if);
    palloc_free_page(file_name);
    if (!success)
        syscall_exit(-1);

    /* 4. 유저모드로 실행 */
    do_iret(&_if);
    NOT_REACHED();
}

int process_wait(tid_t child_tid)
{
    if (thread_tests)
        return -1;
    /* 현재 스레드 */
    struct thread *cur = thread_current();

    enum intr_level old_leve = intr_disable();
    /* 2. 부모의 list에서 child 찾기 */
    struct thread *child = find_child(&cur->child_list, child_tid);
    intr_set_level(old_leve);

    if (!child)
        return -1; // 내 자식이 아님

    /* 3. wait lock 을 걸어 child가 신호를 줄때까지 대기 */
    sema_down(&child->wait_sema);
    /* 4. child의 exit_status를 저장하고 삭제 */
    int status = child->exit_status;
    list_remove(&child->family_elem);

    /* 5. child가 완전삭제될수 있도록 child 다시 실행(미리 멈춰놓음) */
    sema_up(&child->exit_sema);

    return status;
}

struct thread *find_child(struct list *list, tid_t tid)
{
    struct list_elem *e;
    for (e = list_begin(list); e != list_end(list); e = list_next(e))
    {
        struct thread *t = list_entry(e, struct thread, family_elem);
        if (t->tid == tid)
            return t;
    }
    return NULL;
}

/* Exit the process. This function is called by thread_exit (). */
void process_exit(void)
{
    struct thread *curr = thread_current();
    if (curr->exec_file != NULL)
    {
        file_close(curr->exec_file);
        curr->exec_file = NULL;
    }
    if (curr->p_tid != NULL)
    {
        sema_up(&curr->wait_sema);   // 부모다시시작해라
        sema_down(&curr->exit_sema); // 부모가 프린트찍을때까진 있어라
    }
    process_cleanup();
}

/* Free the current process's resources. */
static void process_cleanup(void)
{
    struct thread *curr = thread_current();

#ifdef VM
    supplemental_page_table_kill(&curr->spt);
#endif

    uint64_t *pml4;
    /* Destroy the current process's page directory and switch back
     * to the kernel-only page directory. */
    pml4 = curr->pml4;
    if (pml4 != NULL)
    {
        /* Correct ordering here is crucial.  We must set
         * cur->pagedir to NULL before switching page directories,
         * so that a timer interrupt can't switch back to the
         * process page directory.  We must activate the base page
         * directory before destroying the process's page
         * directory, or our active page directory will be one
         * that's been freed (and cleared). */
        curr->pml4 = NULL;
        pml4_activate(NULL);
        pml4_destroy(pml4);
    }
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void process_activate(struct thread *next)
{
    /* Activate thread's page tables. */
    pml4_activate(next->pml4);

    /* Set thread's kernel stack for use in processing interrupts. */
    tss_update(next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr
{
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct ELF64_PHDR
{
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack(struct intr_frame *if_);
static bool validate_segment(const struct Phdr *, struct file *);
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool load(const char *file_name, struct intr_frame *if_) // echo 1 2
{
    // intr_frame은 유저프로그램을 시작하기전에 cpu 레지스터를 세팅
    struct thread *t = thread_current();
    struct ELF ehdr;
    struct file *file = NULL;
    off_t file_ofs;
    bool success = false;
    int i;
    uint64_t *addr[32]; // 인자를 저장할 문자열집합

    /* Allocate and activate page directory. */
    t->pml4 = pml4_create();
    if (t->pml4 == NULL)
        goto done;
    process_activate(thread_current());

    /* 1. 인자파싱 */
    int argc = parse_args(file_name, addr);

    /* 2. file_open */
    file = filesys_open(addr[0]);
    if (file == NULL)
    {
        printf("load: %s: open failed\n", addr[0]);
        goto done;
    }

    /* Read and verify executable header. */
    if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
        memcmp(ehdr.e_ident, "\177ELF\2\1\1", 7) || ehdr.e_type != 2 ||
        ehdr.e_machine != 0x3E // amd64
        || ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Phdr) ||
        ehdr.e_phnum > 1024)
    {
        printf("load: %s: error loading executable\n", addr[0]);
        goto done;
    }

    /* Read program headers. */
    file_ofs = ehdr.e_phoff;
    for (i = 0; i < ehdr.e_phnum; i++)
    {
        struct Phdr phdr;

        if (file_ofs < 0 || file_ofs > file_length(file))
            goto done;
        file_seek(file, file_ofs);

        if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
            goto done;
        file_ofs += sizeof phdr;
        switch (phdr.p_type)
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
            /* Ignore this segment. */
            break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
            goto done;
        case PT_LOAD:
            if (validate_segment(&phdr, file))
            {
                bool writable = (phdr.p_flags & PF_W) != 0;
                uint64_t file_page = phdr.p_offset & ~PGMASK;
                uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
                uint64_t page_offset = phdr.p_vaddr & PGMASK;
                uint32_t read_bytes, zero_bytes;
                if (phdr.p_filesz > 0)
                {
                    /* Normal segment.
                     * Read initial part from disk and zero the rest. */
                    read_bytes = page_offset + phdr.p_filesz;
                    zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) -
                                  read_bytes);
                }
                else
                {
                    /* Entirely zero.
                     * Don't read anything from disk. */
                    read_bytes = 0;
                    zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
                }
                if (!load_segment(file, file_page, (void *)mem_page, read_bytes,
                                  zero_bytes, writable))
                    goto done;
            }
            else
                goto done;
            break;
        }
    }

    file_deny_write(file); // 파일쓰기거부

    if (!setup_stack(if_))
        goto done;

    /* Start address. */
    if_->rip = ehdr.e_entry;

    setup_stack_args(if_, addr, argc);

    success = true;
    t->exec_file = file;
    // print_dump(if_, 128);
done:
    /* We arrive here whether the load is successful or not. */
    // file_close(file);
    return success;
}

static void print_dump(struct intr_frame *if_, size_t view_byte)
{
    size_t avail = (size_t)((uintptr_t)USER_STACK - if_->rsp);
    size_t n = view_byte; // 보고 싶은 바이트 수 (원래 네가 쓰던 값)
    if (n > avail)
        n = avail; // 경계 넘지 않게 캡

    hex_dump((uintptr_t)if_->rsp, (void *)if_->rsp, n, true);
}

// "argument-test 1 2 3 4" → ["argument-test", "1", "2", "3", "4"]
static int parse_args(char *cmdline, char **argv)
{
    int argc = 0;
    char *token, *save_ptr;
    for (token = strtok_r(cmdline, " ", &save_ptr); token != NULL;
         token = strtok_r(NULL, " ", &save_ptr))
    {
        argv[argc++] = token;
    }
    return argc;
}

static void setup_stack_args(struct intr_frame *if_, char **argv, int argc)
{
    uint8_t *ptr = (uint8_t *)if_->rsp; // 1바이트 단위 포인터
    uint64_t *addr[32];                 // 일단 32개만잡자인자

    // 1. 문자열들 복사
    for (int i = argc - 1; i >= 0; i--)
    {
        int len = strlen(argv[i]);
        ptr -= (len + 1);
        memcpy(ptr, argv[i], len + 1);
        addr[i] = (uint64_t *)ptr;
    }

    ptr = (uint8_t *)((uintptr_t)ptr & ~0xF); // align

    ptr -= 8;
    *(uint64_t *)ptr = 0; // 마지막인자 0

    for (int i = argc - 1; i >= 0; i--)
    {
        ptr -= 8;
        *(uint64_t *)ptr = (uint64_t *)addr[i];
    }

    ptr -= 8;
    *(uint64_t *)ptr = 0; // fake address
    if_->R.rsi = (uint64_t)(ptr + 8);
    if_->R.rdi = argc;
    if_->rsp = (uint64_t)ptr;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Phdr *phdr, struct file *file)
{
    /* p_offset and p_vaddr must have the same page offset. */
    if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
        return false;

    /* p_offset must point within FILE. */
    if (phdr->p_offset > (uint64_t)file_length(file))
        return false;

    /* p_memsz must be at least as big as p_filesz. */
    if (phdr->p_memsz < phdr->p_filesz)
        return false;

    /* The segment must not be empty. */
    if (phdr->p_memsz == 0)
        return false;

    /* The virtual memory region must both start and end within the
       user address space range. */
    if (!is_user_vaddr((void *)phdr->p_vaddr))
        return false;
    if (!is_user_vaddr((void *)(phdr->p_vaddr + phdr->p_memsz)))
        return false;

    /* The region cannot "wrap around" across the kernel virtual
       address space. */
    if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
        return false;

    /* Disallow mapping page 0.
       Not only is it a bad idea to map page 0, but if we allowed
       it then user code that passed a null pointer to system calls
       could quite likely panic the kernel by way of null pointer
       assertions in memcpy(), etc. */
    if (phdr->p_vaddr < PGSIZE)
        return false;

    /* It's okay. */
    return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page(void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable)
{
    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(ofs % PGSIZE == 0);

    file_seek(file, ofs);
    while (read_bytes > 0 || zero_bytes > 0)
    {
        /* Do calculate how to fill this page.
         * We will read PAGE_READ_BYTES bytes from FILE
         * and zero the final PAGE_ZERO_BYTES bytes. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        /* Get a page of memory. */
        uint8_t *kpage = palloc_get_page(PAL_USER);
        if (kpage == NULL)
            return false;

        /* Load this page. */
        if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes)
        {
            palloc_free_page(kpage);
            return false;
        }
        memset(kpage + page_read_bytes, 0, page_zero_bytes);

        /* Add the page to the process's address space. */
        if (!install_page(upage, kpage, writable))
        {
            printf("fail\n");
            palloc_free_page(kpage);
            return false;
        }

        /* Advance. */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
    }
    return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool setup_stack(struct intr_frame *if_)
{
    uint8_t *kpage;
    bool success = false;

    kpage = palloc_get_page(PAL_USER | PAL_ZERO);
    if (kpage != NULL)
    {
        success = install_page(((uint8_t *)USER_STACK) - PGSIZE, kpage, true);
        if (success)
            if_->rsp = USER_STACK;
        else
            palloc_free_page(kpage);
    }
    return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool install_page(void *upage, void *kpage, bool writable)
{
    struct thread *t = thread_current();

    /* Verify that there's not already a page at that virtual
     * address, then map our page there. */
    return (pml4_get_page(t->pml4, upage) == NULL &&
            pml4_set_page(t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool lazy_load_segment(struct page *page, void *aux)
{
    /* TODO: Load the segment from the file */
    /* TODO: This called when the first page fault occurs on address VA. */
    /* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable)
{
    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(ofs % PGSIZE == 0);

    while (read_bytes > 0 || zero_bytes > 0)
    {
        /* Do calculate how to fill this page.
         * We will read PAGE_READ_BYTES bytes from FILE
         * and zero the final PAGE_ZERO_BYTES bytes. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        /* TODO: Set up aux to pass information to the lazy_load_segment. */
        void *aux = NULL;
        if (!vm_alloc_page_with_initializer(VM_ANON, upage, writable,
                                            lazy_load_segment, aux))
            return false;

        /* Advance. */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
    }
    return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool setup_stack(struct intr_frame *if_)
{
    bool success = false;
    void *stack_bottom = (void *)(((uint8_t *)USER_STACK) - PGSIZE);

    /* TODO: Map the stack on stack_bottom and claim the page immediately.
     * TODO: If success, set the rsp accordingly.
     * TODO: You should mark the page is stack. */
    /* TODO: Your code goes here */

    return success;
}
#endif /* VM */
