#include "userprog/syscall.h"
#include <debug.h>
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "threads/palloc.h"
#include "threads/flags.h"
#include "userprog/gdt.h"
#include "userprog/process.h"
#include "intrinsic.h"
#include "lib/kernel/stdio.h"
#include "include/threads/init.h"
#include "lib/kernel/console.h" // 커널 콘솔 입출력 함수 제공 (putbuf, printf 등)
#include "lib/user/syscall.h"   // 유저 프로그램이 사용하는 시스템 콜 번호 및 인터페이스 정의
#include "filesys/directory.h"  // 디렉터리 관련 자료구조 및 함수 (디렉터리 열기, 탐색 등)
#include "filesys/filesys.h"    // 파일 시스템 전반에 대한 함수 및 초기화/포맷 인터페이스
#include "filesys/file.h"       // 개별 파일 객체(file 구조체) 및 파일 입출력 함수 정의 (read, write 등)
#include "devices/input.h"

struct lock filesys_lock; // 파일 시스템 동기화용 전역 락

#ifndef STDIN_FILENO
#define STDIN_FILENO 0 // 표준 입력 파일 디스크립터 번호
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1 // 표준 출력 파일 디스크립터 번호
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2 // 표준 에러 파일 디스크립터 번호
#endif

static struct file stdin_dummy;  // STDIN을 나타내기 위한 더미 file 구조체
static struct file stdout_dummy; // STDOUT을 나타내기 위한 더미 file 구조체

void syscall_entry(void);
void syscall_handler(struct intr_frame *);
void usr_address_vali(const void *addr);
bool copy_user_string(char *dst, const char *src, size_t max_len);
bool check_page(const void *user_addr);
void vali_pointer(const void *user_addr, size_t size);
void vali_string(const char *str);

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

    /* 인터럽트 서비스 루틴(ISR)은 syscall_entry가
     * 사용자 스택을 커널 모드 스택으로 교체하기 전까지는
     * 어떤 인터럽트도 처리하면 안 된다.
     * 따라서 FLAG_FL을 마스킹했다.
     * 시스템 콜 진입 시점에서 아직 사용자 모드 스택이 커널 스택으로 바뀌지 않았으니,
     *  그 사이에 인터럽트가 발생하면 스택이 꼬여서 문제가 생길 수 있음 → 그래서 인터럽트를 잠깐 막아 둔다 */
    write_msr(MSR_SYSCALL_MASK,
              FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

    // syscall_init 맨 밑에 추가
    lock_init(&filesys_lock);
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f)
{
    /*
        SYS_HALT,                   Halt the operating system.
        SYS_EXIT,                   Terminate this process.
        SYS_FORK,                   Clone current process.
        SYS_EXEC,                   Switch current process.
        SYS_WAIT,                   Wait for a child process to die.
        SYS_CREATE,                 Create a file.
        SYS_REMOVE,                 Delete a file.
        SYS_OPEN,                   Open a file.
        SYS_FILESIZE,               Obtain a file's size.
        SYS_READ,                   Read from a file.
        SYS_WRITE,                  Write to a file.
        SYS_SEEK,                   Change position in a file.
        SYS_TELL,                   Report current position in a file.
        SYS_CLOSE                   Close a file
    */
    switch ((int)f->R.rax) // 시스템 콜 번호는 intr_frame의 R.rax에 담겨져서 옴
    {
    case SYS_HALT:
        syscall_halt(); // PintOS 종료
        break;
    case SYS_EXIT:
        int status = (int)f->R.rdi;
        syscall_exit(status);
        break;
    case SYS_FORK:
        f->R.rax = syscall_fork((const char *)f->R.rdi, f);
        break;
    case SYS_EXEC:
        f->R.rax = syscall_exec((const char *)f->R.rdi);
        break;
    case SYS_WAIT:
        f->R.rax = syscall_wait((int)f->R.rdi);
        break;
    case SYS_CREATE:
        f->R.rax = syscall_create((const char *)f->R.rdi, (unsigned)f->R.rsi); // 인자 1 : 파일, 인자 2 : 사이즈
        break;
    case SYS_REMOVE:
        f->R.rax = syscall_remove((const char *)f->R.rdi);
        break;
    case SYS_OPEN:
        f->R.rax = syscall_open((const char *)f->R.rdi);
        break;
    case SYS_FILESIZE:
        f->R.rax = syscall_filesize((int)f->R.rdi);
        break;
    case SYS_READ:
        f->R.rax = syscall_read((int)f->R.rdi, (void *)f->R.rsi, (unsigned)f->R.rdx);
        break;
    case SYS_WRITE:
        int fd = (int)f->R.rdi;
        const void *buf = (const void *)f->R.rsi;
        unsigned size = (unsigned)f->R.rdx;
        int bytes = syscall_write(fd, buf, size);
        f->R.rax = bytes;
        break;
    case SYS_SEEK:
        syscall_seek((int)f->R.rdi, (unsigned)f->R.rsi);
        break;
    case SYS_TELL:
        f->R.rax = syscall_tell((int)f->R.rdi);
        break;
    case SYS_CLOSE:
        syscall_close((int)f->R.rdi);
        break;
    case SYS_DUP2:
        f->R.rax = syscall_dup2((int)f->R.rdi, (int)f->R.rsi); // dup2(oldfd, newfd) 요청을 처리하고 반환값을 rax에 기록
        break;
    default:
        printf("system call!\n");
        thread_exit();
        break;
    }
}

struct file *syscall_get_std_file(int fd)
{
    switch (fd)
    {
    case STDIN_FILENO:
        return &stdin_dummy; // STDIN이 요청되면 STDIN 더미 객체 반환
    case STDOUT_FILENO:
        return &stdout_dummy; // STDOUT이 요청되면 STDOUT 더미 객체 반환
    default:
        return NULL; // 그 외 번호는 표준 스트림이 아니므로 NULL 반환
    }
}

int syscall_dup2(int oldfd, int newfd)
{
    struct thread *current = thread_current(); // 현재 실행 중인 스레드 포인터 확보

    if (current->FDT == NULL)
    {
        return -1; // FDT가 없다면 파일 디스크립터를 복제할 수 없음
    }

    if (oldfd == newfd)
    {
        return newfd; // 같은 번호로 복제 요청 시 그대로 반환
    }

    if (oldfd < 0 || oldfd >= MAX_FD || newfd < 0 || newfd >= MAX_FD)
    {
        return -1; // 허용 범위를 벗어나면 실패 처리
    }

    struct file *old_file = process_get_file(oldfd); // 원본 fd에 연결된 파일 객체 조회

    if (old_file == NULL)
    {
        return -1; // 원본 fd가 열려 있지 않다면 복제 불가
    }

    struct file *stdin_file = syscall_get_std_file(STDIN_FILENO);   // STDIN 더미 파일 포인터 캐싱
    struct file *stdout_file = syscall_get_std_file(STDOUT_FILENO); // STDOUT 더미 파일 포인터 캐싱

    if (old_file == stdin_file && current->stdin_count == 0)
    {
        return -1; // STDIN이 이미 모두 닫힌 상태라면 복제할 수 없음
    }

    if (old_file == stdout_file && current->stdout_count == 0)
    {
        return -1; // STDOUT이 이미 모두 닫힌 상태라면 복제할 수 없음
    }

    syscall_close(newfd); // 대상 fd가 열려 있다면 먼저 닫아서 자리 확보

    if (old_file == stdin_file)
    {
        current->FDT[newfd] = stdin_file; // 새 fd가 STDIN을 가리키도록 설정
        current->stdin_count++;           // STDIN 참조 카운트 증가
        return newfd;                     // 복제된 fd 반환
    }

    if (old_file == stdout_file)
    {
        current->FDT[newfd] = stdout_file; // 새 fd가 STDOUT을 가리키도록 설정
        current->stdout_count++;           // STDOUT 참조 카운트 증가
        return newfd;                      // 복제된 fd 반환
    }

    lock_acquire(&filesys_lock); // 일반 파일 공유 시 dup_count 갱신을 보호하기 위한 락 획득
    old_file->dup_count++;       // 동일 파일을 가리키는 추가 참조를 기록
    lock_release(&filesys_lock); // dup_count 갱신 후 락 해제

    current->FDT[newfd] = old_file; // 새 fd가 원래 파일 객체를 공유하도록 설정

    return newfd; // 복제된 fd 번호 반환
}

unsigned
syscall_tell(int fd)
{
    // 파일 디스크립터를 통해 파일 객체 가져오기
    struct file *file = process_get_file(fd);

    // 유효하지 않은 fd이면 0 반환 (unsigned 타입이므로 -1 대신 0 사용)
    if (file == NULL)
    {
        return 0;
    }

    // 현재 파일의 읽기/쓰기 위치(offset)를 반환
    return file_tell(file);
}

void syscall_seek(int fd, unsigned position)
{
    // 파일 디스크립터를 통해 파일 객체 가져오기
    struct file *file = process_get_file(fd);

    // 유효하지 않은 fd이면 아무 작업도 하지 않고 종료
    if (file == NULL)
    {
        return;
    }

    // 파일의 읽기/쓰기 위치를 지정된 위치로 변경
    file_seek(file, position);
}

void syscall_close(int fd)
{
    if (fd < 0 || fd >= MAX_FD)
    {
        return; // 허용 범위를 벗어난 fd는 무시
    }

    struct thread *current = thread_current(); // 현재 스레드 포인터 획득
    if (current->FDT == NULL)
    {
        return; // FDT가 준비되지 않았다면 닫을 항목이 없음
    }

    struct file *file = current->FDT[fd]; // FDT에서 대상 파일 객체 확인

    if (file == NULL)
    {
        return; // 이미 닫힌 fd는 추가 작업 불필요
    }

    struct file *stdin_file = syscall_get_std_file(STDIN_FILENO);   // STDIN 더미 파일 포인터 준비
    struct file *stdout_file = syscall_get_std_file(STDOUT_FILENO); // STDOUT 더미 파일 포인터 준비

    if (file == stdin_file)
    {
        if (current->stdin_count > 0)
        {
            current->stdin_count--; // STDIN 참조 카운트 감소
        }
        current->FDT[fd] = NULL; // 현재 fd 슬롯을 비워서 닫힘을 표시
        return;                  // 표준 입력은 더 이상 처리할 필요 없음
    }

    if (file == stdout_file)
    {
        if (current->stdout_count > 0)
        {
            current->stdout_count--; // STDOUT 참조 카운트 감소
        }
        current->FDT[fd] = NULL; // 현재 fd 슬롯을 비워서 닫힘을 표시
        return;                  // 표준 출력은 더 이상 처리할 필요 없음
    }

    current->FDT[fd] = NULL;     // 일반 파일의 경우 우선 FDT에서 제거
    lock_acquire(&filesys_lock); // 파일 객체 공유 보호를 위해 락 획득

    if (file->dup_count > 0)
    {
        file->dup_count--;           // 아직 다른 fd가 동일 파일을 참조하므로 참조만 감소
        lock_release(&filesys_lock); // dup_count 조정 후 락 해제
        return;                      // 실제 파일은 닫지 않음
    }

    file_close(file);            // 더 이상 참조가 없으니 파일 닫기 수행
    lock_release(&filesys_lock); // 파일 연산 후 락 해제
}

int syscall_write(int fd, const void *buffer, unsigned size)
{
    vali_pointer(buffer, size); // 사용자 버퍼가 유효한 커널 접근 범위인지 확인

    struct thread *current = thread_current();                      // 현재 스레드 포인터 확보
    struct file *file = process_get_file(fd);                       // fd에 대응되는 파일 객체 조회
    struct file *stdin_file = syscall_get_std_file(STDIN_FILENO);   // STDIN 더미 파일 포인터 준비
    struct file *stdout_file = syscall_get_std_file(STDOUT_FILENO); // STDOUT 더미 파일 포인터 준비

    if (file == stdin_file)
    {
        return -1; // STDIN으로는 출력할 수 없으므로 실패
    }

    if (fd == STDERR_FILENO)
    {
        return -1; // 아직 STDERR 출력은 지원하지 않으므로 실패 처리
    }

    if (file == stdout_file)
    {
        if (current->stdout_count == 0)
        {
            return -1; // STDOUT이 모두 닫힌 상태라면 출력 불가
        }
        putbuf(buffer, size); // 콘솔에 버퍼 내용을 그대로 출력
        return size;          // 출력한 바이트 수 반환
    }

    if (file == NULL)
    {
        return -1; // 열려 있지 않은 fd이므로 실패
    }

    lock_acquire(&filesys_lock);                      // 파일 시스템 접근 보호를 위해 락 획득
    int bytes_write = file_write(file, buffer, size); // 파일에 데이터 쓰기 수행
    lock_release(&filesys_lock);                      // 파일 연산 후 락 해제

    if (bytes_write < 0)
    {
        return -1; // 쓰기가 실패했다면 오류 반환
    }

    return bytes_write; // 실제로 기록한 바이트 수 반환
}

int syscall_read(int fd, void *buffer, unsigned size)
{
    vali_pointer(buffer, size); // 사용자 버퍼가 커널에서 접근 가능한지 확인

    struct thread *current = thread_current();                      // 현재 스레드 포인터 확보
    struct file *file = process_get_file(fd);                       // fd에 연결된 파일 객체 조회
    struct file *stdin_file = syscall_get_std_file(STDIN_FILENO);   // STDIN 더미 파일 포인터 준비
    struct file *stdout_file = syscall_get_std_file(STDOUT_FILENO); // STDOUT 더미 파일 포인터 준비

    if (file == stdin_file)
    {
        if (current->stdin_count == 0)
        {
            return -1; // STDIN이 모두 닫혔다면 읽기 불가
        }

        char *dst = (char *)buffer; // 입력 문자를 저장할 버퍼 포인터 준비

        for (unsigned i = 0; i < size; i++)
        {
            dst[i] = input_getc(); // 키보드에서 한 글자씩 읽어 버퍼에 기록
        }

        return size; // 요청한 길이만큼 읽었으므로 그대로 반환
    }

    if (file == stdout_file || fd == STDERR_FILENO)
    {
        return -1; // STDOUT/STDERR에서는 읽기를 지원하지 않음
    }

    if (file == NULL)
    {
        return -1; // 열려 있지 않은 fd라면 실패
    }

    lock_acquire(&filesys_lock);                    // 파일 시스템 접근 보호를 위해 락 획득
    int bytes_read = file_read(file, buffer, size); // 파일에서 데이터 읽어오기
    lock_release(&filesys_lock);                    // 파일 연산 후 락 해제

    return bytes_read; // 실제로 읽은 바이트 수 반환
}

int syscall_filesize(int fd)
{
    // 파일 디스크립터 번호를 이용해 파일 객체 가져오기
    struct file *file = process_get_file(fd);

    // 유효하지 않은 fd이거나 파일이 열려 있지 않은 경우 -1 반환
    if (file == NULL)
    {
        return -1;
    }

    // 해당 파일의 크기(바이트 단위)를 반환
    return file_length(file);
}

/* 파일을 오픈하는 시스템콜 핸들러 */
int syscall_open(const char *file_name)
{
    // 사용자 포인터 유효성 검사 (커널 주소/NULL/미매핑 주소 거부)
    usr_address_vali(file_name);

    // 파일 시스템 접근을 위한 락 획득
    lock_acquire(&filesys_lock);

    // 파일 시스템에서 파일 열기 시도
    struct file *file = filesys_open(file_name);

    // 파일이 없거나 열기에 실패한 경우 -1 반환
    if (file == NULL)
    {
        lock_release(&filesys_lock);
        return -1;
    }

    // 현재 프로세스의 파일 디스크립터 테이블(FDT)에 파일 등록
    int fd = process_add_file(file);

    // 파일 등록에 실패한 경우 → 열린 파일 닫기
    if (fd == -1)
    {
        file_close(file);
    }

    // 파일 시스템 락 해제
    lock_release(&filesys_lock);

    // 파일 디스크립터 번호 반환, 실패 시 -1 반환
    return fd;
}

/* 사용자 문자열 str이 \0을 만날 때까지 매 바이트 접근 가능한지 확인
   → 문자열 전체가 사용자 영역 내에 안전하게 존재해야 함
   → 접근 불가능한 주소를 만나면 프로세스를 종료함 (syscall_exit(-1))
   → 사용 예: exec("..."), open("...") 등에서 문자열 인자 검증 */
void vali_string(const char *str)
{
    for (const char *p = str;; p++)
    {
        vali_pointer(p, 1); // 현재 문자가 존재하는 1바이트 주소가 유효한지 확인
        if (*p == '\0')
        {
            break; // 문자열 끝(널 문자)에 도달하면 검사 종료
        }
    }
}

/* 사용자 포인터 user_addr로부터 size 바이트까지의 주소 범위를
   페이지 단위로 하나씩 순회하며 모두 접근 가능한지 확인
   → 접근 불가능한 주소가 포함되어 있으면 프로세스를 종료함 (syscall_exit(-1))
   → 사용 예: read(), write(), exec() 등에서 전달받은 사용자 버퍼 검증 */
void vali_pointer(const void *user_addr, size_t size)
{
    if (size == 0)
    {
        return; // 검사할 바이트 수가 0이면 return
    }

    const uint8_t *check_ptr = user_addr; // 현재 검사할 위치 (byte 단위 포인터)
    size_t byte_left = size;              // 검사해야 할 남은 바이트 수

    // 검사할 전체 영역을 페이지 단위로 나누어 한 페이지씩 접근 가능 여부를 확인
    while (byte_left > 0)
    {
        // 현재 check_ptr 포인터가 가리키는 주소가 사용자 영역에 있고
        // 실제로 물리 메모리에 매핑되어 있는지 확인
        if (!check_page(check_ptr))
        {
            syscall_exit(-1); // 잘못된 주소일 경우, 즉시 프로세스 종료
        }

        // 현재 페이지에서 끝까지 남은 바이트 수 계산
        /* pg_ofs() - 예를 들어, 페이지 크기가 4KB라면, 0~4095 바이트 범위 안에서 어느 지점에 데이터가 있는지를 나타내는 값이 offset입니다 */
        size_t page_left = PGSIZE - pg_ofs(check_ptr);

        // 남은 전체 바이트와 현재 페이지에서 가능한 바이트 중 더 작은 만큼만 이동
        size_t chunk = byte_left < page_left ? byte_left : page_left;

        check_ptr += chunk; // 검사할 포인터를 다음 영역으로 이동
        byte_left -= chunk; // 검사해야 할 남은 바이트 수 갱신
    }
}

/* 내부 헬퍼: 단일 가상 주소 user_addr이
   - NULL이 아니고
   - 사용자 영역에 속하며
   - 현재 프로세스의 페이지 테이블에 매핑되어 있는지 확인 */
bool check_page(const void *user_addr)
{
    return user_addr != NULL &&
           is_user_vaddr(user_addr) &&
           pml4_get_page(thread_current()->pml4, user_addr) != NULL;
}

/* 파일을 삭제하는 시스템콜 핸들러 */
bool syscall_remove(const char *file)
{
    // 사용자 포인터 유효성 검사 (커널 주소/NULL/미매핑 주소 거부)
    usr_address_vali(file);
    // 파일 시스템에서 해당 경로의 파일 삭제 시도, 성공 여부 반환
    return filesys_remove(file);
}

/* 파일을 생성하는 시스템콜 핸들러 */
bool syscall_create(const char *file, unsigned initial_size)
{
    // 사용자 포인터 유효성 검사 (커널 주소/NULL/미매핑 주소 거부)
    usr_address_vali(file);
    // 파일 시스템에 새 파일 생성 (초기 크기 지정), 성공 여부 반환 (boolean)
    return filesys_create(file, initial_size);
}

/* 사용자 주소 유효성 검증 유틸리티 */
void usr_address_vali(const void *addr)
{
    // 다음 중 하나라도 해당하면 프로세스를 종료(-1)
    // - 커널 주소 공간(is_kernel_vaddr)
    // - NULL 포인터
    // - 현재 프로세스의 페이지테이블(pml4)에 매핑되지 않은 주소
    if (is_kernel_vaddr(addr) || addr == NULL || pml4_get_page(thread_current()->pml4, addr) == NULL)
    {
        syscall_exit(-1); // 잘못된 사용자 포인터는 즉시 종료
    }
}

pid_t syscall_fork(const char *name, struct intr_frame *f)
{
    // 사용자 영역 포인터인지, 유효한 페이지에 매핑되어 있는지 선행 검사
    usr_address_vali(name);
    tid_t child = process_fork(name, f);
    // thread_create 실패 시 TID_ERROR가 내려오므로 PID_ERROR로 변환해 반환
    return child == TID_ERROR ? PID_ERROR : child;
}

int syscall_wait(int pid)
{
    return process_wait(pid);
}

int syscall_exec(const char *cmd_line)
{
    // 유효한 주소인지 검사
    usr_address_vali(cmd_line);

    // 사용자로부터 받은 문자열(cmd_line)을 복사할 커널 영역의 페이지를 할당
    // PAL_ZERO는 할당된 메모리를 0으로 초기화하라는 의미
    char *cmd_line_copy = palloc_get_page(PAL_ZERO);

    // 만약 메모리 할당에 실패했다면, exit 처리
    if (cmd_line_copy == NULL)
    {
        return -1;
    }

    // 사용자 영역 문자열을 안전하게 복사하면서 페이지 경계를 검사한다.
    if (!copy_user_string(cmd_line_copy, cmd_line, PGSIZE))
    {
        palloc_free_page(cmd_line_copy);
        return -1;
    }

    // 실제로 새로운 프로그램을 현재 프로세스 위에 실행
    // 실패하면 -1을 반환하므로, exit 처리
    if (process_exec(cmd_line_copy) == -1)
    {
        syscall_exit(-1);
    }

    // 여기까지 실행이 오면 안된다 - 라는 매크로
    NOT_REACHED();
}

bool copy_user_string(char *dst, const char *src, size_t max_len)
{
    for (size_t i = 0; i < max_len; i++)
    {
        /* 매 바이트 접근 전에 해당 주소가 사용자 영역인지 검사한다. */
        usr_address_vali(src + i);
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

/* 프로세스 종료 시스템콜 핸들러 */
void syscall_exit(int status)
{
    struct thread *current_thread = thread_current();
    current_thread->exit_status = status;
    printf("%s: exit(%d)\n", current_thread->name, status);
    thread_exit();
}

/* 전원 종료(Pintos/QEMU 종료) 시스템콜 핸들러 */
void syscall_halt(void)
{
    // 전원 끄기: Pintos/QEMU 종료
    power_off();
}
