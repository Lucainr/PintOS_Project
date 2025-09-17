#include "userprog/syscall.h"
#include "filesys/filesys.h"
#include "intrinsic.h"
#include "threads/flags.h"
#include "threads/init.h" // power_off
#include "threads/interrupt.h"
#include "threads/loader.h"
#include "threads/malloc.h"
#include "threads/synch.h" // lock
#include "threads/thread.h"
#include "userprog/gdt.h"
#include <devices/input.h> // input_getc
#include <filesys/file.h>  // file_close()
#include <lib/kernel/stdio.h>
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include <userprog/process.h>

struct file_descriptor
{
    int fd;
    struct file *file;
    struct list_elem elem;
};

void syscall_entry(void);
void syscall_handler(struct intr_frame *);
bool create(const char *name, off_t initial_size);
static int open(const char *name);
static void close(int fd);
static off_t read(int fd, void *buffer, off_t size);
static int filesize(int fd);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define FILE_NAME_MAX 14
#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */
static struct lock filesyslock;
static void check_address(const void *addr);
static struct file_descriptor *find_fd_s(struct list *list, int fd);
static void copy_in(void *dst, const void *uaddr, size_t size);

void syscall_init(void)
{
    write_msr(MSR_STAR,
              ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG) << 32);
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* The interrupt service rountine should not serve any interrupts
     * until the syscall_entry swaps the userland stack to the kernel
     * mode stack. Therefore, we masked the FLAG_FL. */
    write_msr(MSR_SYSCALL_MASK,
              FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

    lock_init(&filesyslock); // lock 초기화
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f UNUSED)
{
    // TODO: Your implementation goes here.
    // SYS_WRIT
    // printf("system call!: %lld\n", f->R.rax);

    switch (f->R.rax)
    {
    case SYS_HALT:
    {
        power_off();
        break;
    }
    case SYS_CREATE:
    { // rdi file, rsi size

        f->R.rax = create(f->R.rdi, f->R.rsi);
        // 모든 반환형이 있는 시스템콜은 rax에 채워줘야한다.
        break;
    }
    case SYS_OPEN:
    {
        f->R.rax = open(f->R.rdi);
        // exit, close 등에서 fd구조체 삭제해야함(미완성)
        break;
    }
    case SYS_CLOSE:
    { // input fd, void
        close(f->R.rdi);
        break;
    }

    case SYS_READ:
    {
        if (f->R.rdi == 0) // 입력fd 처리
        {
            uint8_t *buffer = f->R.rsi;
            for (off_t i = 0; i < f->R.rdx; i++)
            {
                buffer[i] = input_getc();
            }
            f->R.rax = f->R.rdx;
        }
        if (f->R.rdi > 1) // 일반fd 처리
        {
            f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
        }
        else // 출력 fd처리
        {
            f->R.rax = -1;
        }

        break;
    }

    case SYS_FILESIZE:
    {
        f->R.rax = filesize(f->R.rdi);
        break;
    }

    case SYS_WRITE:
    {
        if (f->R.rdi == 1)
        {
            putbuf(f->R.rsi, f->R.rdx);
        }
        break;
    }
    case SYS_EXIT:
    {
        int status = (int)f->R.rdi;
        exit(status);
        break;
    }

    default:
        return -1;
    }
}

void exit(int status)
{
    struct thread *cur = thread_current();
    cur->exit_status = status;
    // 종료 메시지 출력: "<프로세스이름>: exit(<상태코드>)"
    printf("%s: exit(%d)\n", thread_current()->name, status);
    // 부모에게 알려주기
    sema_up(&cur->wait_sema);
    // 현재 스레드 종료
    thread_exit();
}

bool create(const char *name, off_t initial_size)
{
    check_address(name);
    bool result; // 결과값 저장
    if (name == NULL)
        return false;
    if (strlen(name) < 1 || strlen(name) > FILE_NAME_MAX)
        return false;

    lock_acquire(&filesyslock);
    result = filesys_create(name, initial_size);
    lock_release(&filesyslock);

    return result;
}

static int open(const char *name)
{
    check_address(name);
    if (strlen(name) < 1 || strlen(name) > FILE_NAME_MAX)
        return -1; // 최소 최대 길이

    lock_acquire(&filesyslock);
    struct file *file = filesys_open(name);
    lock_release(&filesyslock);
    if (!file)
        return -1; // 파일못찾으면

    struct file_descriptor *fd_s = malloc(sizeof(struct file_descriptor));
    struct thread *cur_th = thread_current();
    if (!fd_s)
        return -1; // 할당안되면
    int result = cur_th->next_fd++;

    fd_s->fd = result;
    fd_s->file = file;

    list_push_back(&cur_th->fd_list, &fd_s->elem);

    return result;
}

static void close(int fd)
{
    if (fd < 2)
        return;

    struct thread *cur_th = thread_current();
    if (cur_th->next_fd < fd)
        return;

    // fd 찾아서 해당 elem삭제하고 메모리해제
    struct file_descriptor *fd_s = find_fd_s(&cur_th->fd_list, fd);
    if (fd_s == NULL)
        return;
    list_remove(&fd_s->elem); // 리스트에서 해제

    lock_acquire(&filesyslock);
    file_close(fd_s->file); // file객체, inode 참조해제
    lock_release(&filesyslock);

    free(fd_s); // 파일디스크립터 객체 삭제
}

static off_t read(int fd, void *buffer, off_t size)
{
    if (size == 0)
        return 0;
    check_address(buffer);
    check_address(buffer + size - 1);

    struct thread *cur_th = thread_current();
    if (cur_th->next_fd < fd)
    {
        exit(-1);
    }
    struct file_descriptor *fd_s = find_fd_s(&cur_th->fd_list, fd);
    if (fd_s == NULL)
    {
        exit(-1);
    }

    lock_acquire(&filesyslock);
    off_t result = file_read(fd_s->file, buffer, size);
    lock_release(&filesyslock);
    return result;
}

static int filesize(int fd)
{
    struct file_descriptor *fd_s = find_fd_s(&thread_current()->fd_list, fd);
    if (fd_s == NULL)
        return -1;

    lock_acquire(&filesyslock);
    int size = file_length(fd_s->file);
    lock_release(&filesyslock);

    return size;
}

static struct file_descriptor *find_fd_s(struct list *list, int fd)
{
    struct list_elem *e;
    for (e = list_begin(list); e != list_end(list); e = list_next(e))
    {
        struct file_descriptor *fd_s =
            list_entry(e, struct file_descriptor, elem);
        if (fd_s->fd == fd)
            return fd_s;
    }
    return NULL;
}

static void check_address(const void *addr)
{
    // 포인터가 아예들어오지 않았던가 커널영역으로 주소를 보냈을때 강제종료
    // 페이지단위, 블록단위 ?? 그것도 검증
    if (addr == NULL || !is_user_vaddr(addr) || // 커널주소라면 빠꾸!
        pml4_get_page(thread_current()->pml4, addr) == NULL)
    {
        exit(-1); // 프로세스 강제 종료
    }
}

// static void copy_in(void *dst, const void *uaddr, size_t size)
// {
//     uint8_t *kd = dst;
//     const uint8_t *us = uaddr;

//     while (size > 0)
//     {
//         if (us == NULL || !is_user_vaddr(us))
//             exit(-1);

//         void *kpage = pml4_get_page(thread_current()->pml4,
//         pg_round_down(us)); if (kpage == NULL)
//             exit(-1);

//         size_t page_left = PGSIZE - pg_ofs(us);
//         size_t n = size < page_left ? size : page_left;

//         memcpy(kd, (uint8_t *)kpage + pg_ofs(us), n);

//         kd += n;
//         us += n;
//         size -= n;
//     }
// }