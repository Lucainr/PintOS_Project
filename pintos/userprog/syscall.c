#include "userprog/syscall.h"
#include "filesys/filesys.h"
#include "intrinsic.h"
#include "threads/flags.h"
#include "threads/init.h" // power_off
#include "threads/interrupt.h"
#include "threads/loader.h"
#include "threads/synch.h" // lock
#include "threads/thread.h"
#include "userprog/gdt.h"
#include <lib/kernel/stdio.h>
#include <stdio.h>
#include <string.h>

#include <syscall-nr.h>
#include <userprog/process.h>

void syscall_entry(void);
void syscall_handler(struct intr_frame *);
bool create(const char *name, off_t initial_size);

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

        // bool create(const char *file, unsigned initial_size)
        // {
        //     return syscall2(SYS_CREATE, file, initial_size);
        // }
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

static void check_address(const void *addr)
{
    // 포인터가 아예들어오지 않았던가 커널영역으로 주소를 보냈을때 강제종료
    if (addr == NULL || !is_user_vaddr(addr) ||
        pml4_get_page(thread_current()->pml4, addr) == NULL)
    {
        exit(-1); // 프로세스 강제 종료
        // 이거 공부
    }
}