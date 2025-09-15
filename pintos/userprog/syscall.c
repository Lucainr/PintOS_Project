#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "lib/kernel/stdio.h"
#include "include/threads/init.h"
#include "filesys/filesys.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);
void usr_address_vali (void *addr);
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
    write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 |
                            ((uint64_t)SEL_KCSEG) << 32);
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* The interrupt service rountine should not serve any interrupts
     * until the syscall_entry swaps the userland stack to the kernel
     * mode stack. Therefore, we masked the FLAG_FL. */
    write_msr(MSR_SYSCALL_MASK,
              FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f)
{
    switch ((int)f->R.rax)
    {
    case SYS_HALT:
        halt(); // PintOS 종료
        break;
    case SYS_EXIT:
    {
        int status = (int)f->R.rdi;
        exit(status);
        break;
    }
    case SYS_CREATE:
        f->R.rax = create(f->R.rdi, f->R.rsi); // 인자 1 : 파일, 인자 2 : 사이즈
        break;
    case SYS_REMOVE:
        f->R.rax = remove(f->R.rdi);
        break;
    case SYS_OPEN:
        // filesys_open()
        break;
    case SYS_FILESIZE:
        break;
    case SYS_READ:
        break;
    case SYS_WRITE: // 임시 (printf 같이 console 출력용만 구현)
    {
        int fd = (int)f->R.rdi;
        const void *buf = (const void *)f->R.rsi;
        unsigned size = (unsigned)f->R.rdx;
        int bytes = write(fd, buf, size);
        f->R.rax = bytes;
        break;
    }
    case SYS_SEEK:
        break;
    case SYS_TELL:
        break;
    case SYS_CLOSE:
        break;
    default:
        printf("system call!\n");
        thread_exit();
        break;
    }
}

// 사용자 프로그램의 write 시스템콜 처리 (only 출력용) - 임시
int write(int fd, const void *buffer, unsigned size)
{
    // 1) 표준 출력만 허용
    if (fd != 1)
    {
        return -1;
    }

    // 2) 버퍼가 없거나 길이가 0이면 쓸 내용이 없음 → 0 반환
    if (buffer == NULL || size == 0)
    {
        return 0; // 출력할 내용이 없음
    }

    // 3) 시작 포인터가 사용자 공간에 있고 매핑되어 있는지 확인 (기본 유효성 검사)
    if (!is_user_vaddr(buffer) || pml4_get_page(thread_current()->pml4, buffer) == NULL)
    {
        return -1;
    }

    // 4) 버퍼의 마지막 바이트도 사용자 공간/매핑 확인 (페이지 경계 넘김 방지용 보조 확인)
    const uint8_t *last = (const uint8_t *)buffer + size - 1; // 마지막 바이트 주소 계산
    if (!is_user_vaddr(last) || pml4_get_page(thread_current()->pml4, last) == NULL)
    {
        return -1;
    }

    // 5) 실제 출력: 콘솔에 한번에 출력해 커널 메시지와 섞이는 것 최소화
    putbuf((const char *)buffer, size);

    // 6) 성공적으로 기록한 바이트 수 반환
    return (int)size;
}

/* 파일을 오픈하는 시스템콜 핸들러 */
int
open (const char *file)
{
    // // 사용자 포인터 유효성 검사 (커널 주소/NULL/미매핑 주소 거부)
    // usr_address_vali(file);

    // struct file open_file = filesys_open(file);
    // open_file
    
}

/* 파일을 삭제하는 시스템콜 핸들러 */
bool
remove(const char *file)
{
    // 사용자 포인터 유효성 검사 (커널 주소/NULL/미매핑 주소 거부)
    usr_address_vali(file);
    // 파일 시스템에서 해당 경로의 파일 삭제 시도, 성공 여부 반환
    return filesys_remove(file);
}

/* 파일을 생성하는 시스템콜 핸들러 */
bool
create(const char *file, unsigned initial_size) {
    // 사용자 포인터 유효성 검사 (커널 주소/NULL/미매핑 주소 거부)
    usr_address_vali(file);
    // 파일 시스템에 새 파일 생성 (초기 크기 지정), 성공 여부 반환 (boolean)
    return filesys_create(file, initial_size);
}

/* 사용자 주소 유효성 검증 유틸리티 */
void 
usr_address_vali (void *addr)
{
    // 다음 중 하나라도 해당하면 프로세스를 종료(-1)
    // - 커널 주소 공간(is_kernel_vaddr)
    // - NULL 포인터
    // - 현재 프로세스의 페이지테이블(pml4)에 매핑되지 않은 주소
    if (is_kernel_vaddr(addr) || addr == NULL || pml4_get_page(thread_current()->pml4, addr) == NULL)
        exit(-1); // 잘못된 사용자 포인터는 즉시 종료
}

/* 프로세스 종료 시스템콜 핸들러 */
void
exit(int status) {
    // 종료 메시지 출력: "<프로세스이름>: exit(<상태코드>)"
    printf("%s: exit(%d)\n", thread_current()->name, status);
    // 현재 스레드 종료
    thread_exit();
}

/* 전원 종료(Pintos/QEMU 종료) 시스템콜 핸들러 */
void
halt(void)
{
    // 전원 끄기: Pintos/QEMU 종료
    power_off();
}