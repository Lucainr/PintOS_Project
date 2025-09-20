#include "userprog/syscall.h"
#include "filesys/file.h" // file
#include "filesys/filesys.h"
#include "filesys/inode.h" // inode
#include "intrinsic.h"
#include "threads/flags.h"
#include "threads/init.h" // power_off
#include "threads/interrupt.h"
#include "threads/loader.h"
#include "threads/malloc.h"
#include "threads/palloc.h" // palloc
#include "threads/synch.h"  // lock
#include "threads/thread.h"
#include "userprog/gdt.h"
#include <devices/input.h> // input_getc
#include <filesys/file.h>  // file_close()
#include <lib/kernel/stdio.h>
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include <userprog/process.h>

// struct file_descriptor
// {
//     int fd;
//     struct file *file;
//     struct list_elem elem;
// };
bool copy_user_string(char *dst, const char *src, size_t max_len);
void syscall_entry(void);
void syscall_handler(struct intr_frame *);
bool create(const char *name, off_t initial_size);
static int open(const char *name);
static void close(int fd);
static off_t read(int fd, void *buffer, off_t size);
static int filesize(int fd);
static off_t write(int fd, const void *buffer, off_t size);
static int wait(int tid);
void exit(int status);
static int syscall_exec(char *filename);

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
typedef int pid_t;
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
        switch (f->R.rdi)
        {
        case 0: // stdin
        {
            uint8_t *buffer = f->R.rsi;
            for (off_t i = 0; i < f->R.rdx; i++)
            {
                buffer[i] = input_getc(); // 내부에서 락 처리
            }
            f->R.rax = f->R.rdx;
            break;
        }
        case 1: // stdout
        {
            f->R.rax = -1;
            break;
        }
        default: // 일반fd
            f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
            break;
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
        switch (f->R.rdi)
        {
        case 0: // stdin
        {
            f->R.rax = -1;
            break;
        }
        case 1: // stdout
        {
            putbuf(f->R.rsi, f->R.rdx);
            f->R.rax = f->R.rdx;
            break;
        }
        default: // 일반fd
            f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
            break;
        }
        break;
    }
    case SYS_FORK:
    {
        f->R.rax = fork(f->R.rdi, f);
        break;
    }
    case SYS_WAIT:
    {
        f->R.rax = wait(f->R.rdi);
        break;
    }
    case SYS_EXIT:
    {
        int status = (int)f->R.rdi;
        exit(status);
        break;
    }

    case SYS_EXEC:
    {
        f->R.rax = syscall_exec(f->R.rdi);
        break;
    }

    case SYS_SEEK:
    {
        f->R.rax = syscall_seek(f->R.rdi, f->R.rsi);
        break;
    }

    case SYS_TELL:
    {
        f->R.rax = syscall_tell(f->R.rdi);

        break;
    }

    default:
        return -1;
    }
}
static unsigned syscall_tell(int fd)
{
    struct thread *curr_th = thread_current();
    struct file_descriptor *fd_s = find_fd_s(&curr_th->fd_list, fd);
    if (fd_s == NULL)
    {
        exit(-1);
    }
    return file_tell(fd_s->file);
}

static bool syscall_seek(int fd, off_t pos)
{
    struct thread *curr_th = thread_current();
    struct file_descriptor *fd_s = find_fd_s(&curr_th->fd_list, fd);
    if (fd_s == NULL)
    {
        return false;
    }
    file_seek(fd_s->file, pos);

    return true;
}

static int syscall_exec(char *filename)
{

    check_address(filename);

    char *fn_copy = palloc_get_page(0); // copy해야하는 이유 제대로알기
    if (fn_copy == NULL)
        exit(-1);

    if (!copy_user_string(fn_copy, filename, PGSIZE))
    {
        palloc_free_page(fn_copy);
        exit(-1);
    }
    if (process_exec(fn_copy) == -1)
    {
        exit(-1);
    }
}

static int wait(int tid)
{
    return process_wait(tid);
}

void exit(int status)
{
    struct thread *cur = thread_current();
    cur->exit_status = status;
    // 종료 메시지 출력: "<프로세스이름>: exit(<상태코드>)"
    printf("%s: exit(%d)\n", thread_current()->name, status);
    // 부모에게 알려주기
    // sema_up(&cur->wait_sema);
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

static pid_t fork(char *thread_name, struct intr_frame *if_)
{
    return process_fork(thread_name, if_);
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

static off_t write(int fd, const void *buffer, off_t size)
{ // buffer에 있는것을 fd에 쓴다.
    if (size == 0)
        return 0;

    check_address(buffer);            // 버퍼의 맨앞, 여기처리 꼭해야하는지?
    check_address(buffer + size - 1); // 버퍼의 맨뒤

    struct thread *cur_th = thread_current();
    if (cur_th->next_fd < fd)
    {
        exit(-1);
    }
    struct file_descriptor *fd_s = find_fd_s(&cur_th->fd_list, fd);
    struct file *file = fd_s->file;
    struct inode *inode = file_get_inode(file);
    int deny_cnt = inode_get_deny_cnt(inode);
    // printf("inode addr = %p deny_cnt = %d\n", inode,
    // inode_get_deny_cnt(inode));
    if (deny_cnt > 0) // true 무시
    {
        return 0;
    }

    if (fd_s == NULL)
    {
        exit(-1);
    }

    lock_acquire(&filesyslock);
    off_t result = file_write(fd_s->file, buffer, size);
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

static void copy_in(void *dst, const void *uaddr, size_t size)
{
    uint8_t *kd = dst;
    const uint8_t *us = uaddr;

    while (size > 0)
    {
        if (us == NULL || !is_user_vaddr(us))
            exit(-1);

        void *kpage = pml4_get_page(thread_current()->pml4, pg_round_down(us));
        if (kpage == NULL)
            exit(-1);

        size_t page_left = PGSIZE - pg_ofs(us);
        size_t n = size < page_left ? size : page_left;

        memcpy(kd, (uint8_t *)kpage + pg_ofs(us), n);

        kd += n;
        us += n;
        size -= n;
    }
}

bool copy_user_string(char *dst, const char *src, size_t max_len)
{
    for (size_t i = 0; i < max_len; i++)
    {
        /* 매 바이트 접근 전에 해당 주소가 사용자 영역인지 검사한다. */
        check_address(src + i);
        char c = src[i];
        dst[i] = c;
        /* NULL 문자를 만났다면 복사가 완료된 것. */
        if (c == '\0')
        {
            return true;
        }
    }
    /* 문자열이 최대 허용 길이 안에서 끝나지 않았음. */
    return false;
}