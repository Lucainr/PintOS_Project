#include "userprog/syscall.h"
#include "intrinsic.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/loader.h"
#include "threads/thread.h"
#include "userprog/gdt.h"
#include <lib/kernel/stdio.h>
#include <stdio.h>
#include <syscall-nr.h>
#include <userprog/process.h>

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

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
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f UNUSED)
{
    // TODO: Your implementation goes here.
    // SYS_WRIT
    // printf("system call!: %lld\n", f->R.rax);
    if (f->R.rax == SYS_WRITE)
    { // fd, buffer, size
        if (f->R.rdi == 1)
        {
            putbuf(f->R.rsi, f->R.rdx);
        }
    } // 사용자 프로세스가 printf를 쓸 수 있다.

    if (f->R.rax == SYS_EXIT)
    {
        int status = (int)f->R.rdi;
        exit(status);
    }
}

void exit(int status)
{
    // 종료 메시지 출력: "<프로세스이름>: exit(<상태코드>)"
    printf("%s: exit(%d)\n", thread_current()->name, status);
    // 현재 스레드 종료
    thread_exit();
}