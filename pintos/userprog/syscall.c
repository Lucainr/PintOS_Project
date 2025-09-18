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
#include "lib/kernel/console.h"     // 커널 콘솔 입출력 함수 제공 (putbuf, printf 등)
#include "lib/user/syscall.h"       // 유저 프로그램이 사용하는 시스템 콜 번호 및 인터페이스 정의
#include "filesys/directory.h"      // 디렉터리 관련 자료구조 및 함수 (디렉터리 열기, 탐색 등)
#include "filesys/filesys.h"        // 파일 시스템 전반에 대한 함수 및 초기화/포맷 인터페이스
#include "filesys/file.h"           // 개별 파일 객체(file 구조체) 및 파일 입출력 함수 정의 (read, write 등)
#include "devices/input.h"

struct lock filesys_lock;	// 파일 시스템 동기화용 전역 락

void syscall_entry(void);
void syscall_handler(struct intr_frame *);
void usr_address_vali (const void *addr);
bool copy_user_string(char *dst, const char *src, size_t max_len);
bool check_page (const void *user_addr);
void vali_pointer (const void *user_addr, size_t size);
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
        f->R.rax = syscall_fork((const char *) f->R.rdi, f);
        break;
    case SYS_EXEC:
        f->R.rax = syscall_exec((const char *) f->R.rdi);
        break;
    case SYS_WAIT:
        f->R.rax = syscall_wait((int) f->R.rdi);
        break;
    case SYS_CREATE:
        f->R.rax = syscall_create((const char *) f->R.rdi, (unsigned) f->R.rsi); // 인자 1 : 파일, 인자 2 : 사이즈
        break;
    case SYS_REMOVE:
        f->R.rax = syscall_remove((const char *) f->R.rdi);
        break;
    case SYS_OPEN:
        f->R.rax = syscall_open((const char *) f->R.rdi);
        break;
    case SYS_FILESIZE:
        f->R.rax = syscall_filesize((int) f->R.rdi);
        break;
    case SYS_READ:
        f->R.rax = syscall_read((int) f->R.rdi, (void *) f->R.rsi, (unsigned) f->R.rdx);
        break;
    case SYS_WRITE:
        int fd = (int)f->R.rdi;
        const void *buf = (const void *)f->R.rsi;
        unsigned size = (unsigned)f->R.rdx;
        int bytes = syscall_write(fd, buf, size);
        f->R.rax = bytes;
        break;
    case SYS_SEEK:
        syscall_seek((int) f->R.rdi, (unsigned) f->R.rsi);
        break;
    case SYS_TELL:
        f->R.rax = syscall_tell((int) f->R.rdi);
        break;
    case SYS_CLOSE:
        syscall_close((int) f->R.rdi);
        break;
    default:
        printf("system call!\n");
        thread_exit();
        break;
    }
}

unsigned
syscall_tell(int fd) {
	// 파일 디스크립터를 통해 파일 객체 가져오기
	struct file *file = process_get_file(fd);

	// 유효하지 않은 fd이면 0 반환 (unsigned 타입이므로 -1 대신 0 사용)
	if (file == NULL) {
		return 0;
	}

	// 현재 파일의 읽기/쓰기 위치(offset)를 반환
	return file_tell(file);
}

void
syscall_seek(int fd, unsigned position) {
	// 파일 디스크립터를 통해 파일 객체 가져오기
	struct file *file = process_get_file(fd);

	// 유효하지 않은 fd이면 아무 작업도 하지 않고 종료
	if (file == NULL) {
		return;
	}

	// 파일의 읽기/쓰기 위치를 지정된 위치로 변경
	file_seek(file, position);
}

void
syscall_close(int fd) {
	struct thread *current_thread = thread_current();

	lock_acquire(&filesys_lock);  // 파일 시스템 동기화

	// 파일 디스크립터 테이블에서 파일 가져오기
	struct file *file = process_get_file(fd);
	if (file != NULL) {
		file_close(file);          // 파일 닫기
		current_thread->FDT[fd] = NULL;      // FDT에서 제거
	}

	lock_release(&filesys_lock);
}

int
syscall_write(int fd, const void *buffer, unsigned size) {
	// 사용자 버퍼 포인터가 유효한지 확인
	vali_pointer(buffer, size);

	// stdin(0), stderr(2)은 출력 대상이 아니므로 에러 처리
	if (fd == 0 || fd == 2) {
		return -1;
	}

	// stdout(1)인 경우 → 콘솔에 출력
	if (fd == 1) {
		putbuf(buffer, size);  // 버퍼 내용을 콘솔에 출력
		return size;           // 출력한 바이트 수 반환
	}
	
	// 일반 파일인 경우 → 해당 fd로 열린 파일 객체 조회
	struct file *file = process_get_file(fd);
	if (file == NULL) {
		return -1;
	}

	// 파일 시스템 접근을 위한 락 획득
	lock_acquire(&filesys_lock);

	// 파일에 버퍼 내용 쓰기
	int bytes_write = file_write(file, buffer, size);

	// 락 해제
	lock_release(&filesys_lock);

	// 쓰기 실패 시 -1 반환
	if (bytes_write < 0) {
		return -1;
	}

	// 성공한 경우 실제로 쓴 바이트 수 반환
	return bytes_write;
}

int
syscall_read(int fd, void *buffer, unsigned size) {
	// 사용자 버퍼 포인터가 유효한지 확인
	vali_pointer(buffer, size);

	// 버퍼를 문자 단위로 접근하기 위해 char 포인터로 변환
	char *ptr = (char *)buffer;
	int bytes_read = 0;

	// 파일 시스템 동시 접근 방지를 위한 락 획득
	lock_acquire(&filesys_lock);

	if (fd == STDIN_FILENO) {  // 표준 입력일 경우
		// 키보드 입력을 한 글자씩 읽어서 버퍼에 저장
		for (unsigned i = 0; i < size; i++) {
			*ptr++ = input_getc();
			bytes_read++;
		}
		lock_release(&filesys_lock);
	} else {
		// stdout(1), stderr(2), 음수 등 읽을 수 없는 fd는 실패 처리
		if (fd < 3) {
			lock_release(&filesys_lock);
			return -1;
		}

		// 파일 디스크립터 테이블에서 파일 객체 가져오기
		struct file *file = process_get_file(fd);
		if (file == NULL) {
			lock_release(&filesys_lock);
			return -1;
		}

		// 파일에서 size만큼 읽어 버퍼에 저장
		bytes_read = file_read(file, buffer, size);

		lock_release(&filesys_lock);
	}

	// 읽은 바이트 수 반환 (0 이상)
	return bytes_read;
}

int
syscall_filesize(int fd) {
	// 파일 디스크립터 번호를 이용해 파일 객체 가져오기
	struct file *file = process_get_file(fd);

	// 유효하지 않은 fd이거나 파일이 열려 있지 않은 경우 -1 반환
	if (file == NULL) {
		return -1;
	}

	// 해당 파일의 크기(바이트 단위)를 반환
	return file_length(file);
}

/* 파일을 오픈하는 시스템콜 핸들러 */
int
syscall_open (const char *file_name) {
    // 사용자 포인터 유효성 검사 (커널 주소/NULL/미매핑 주소 거부)
    usr_address_vali(file_name);

	// 파일 시스템 접근을 위한 락 획득
	lock_acquire(&filesys_lock);

	// 파일 시스템에서 파일 열기 시도
	struct file *file = filesys_open(file_name);

	// 파일이 없거나 열기에 실패한 경우 -1 반환
	if (file == NULL) {
		lock_release(&filesys_lock);
		return -1;
	}

	// 현재 프로세스의 파일 디스크립터 테이블(FDT)에 파일 등록
	int fd = process_add_file(file);

	// 파일 등록에 실패한 경우 → 열린 파일 닫기
	if (fd == -1) {
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
void
vali_string(const char *str) {
	for (const char *p = str;; p++) {
		vali_pointer(p, 1);  // 현재 문자가 존재하는 1바이트 주소가 유효한지 확인
		if (*p == '\0') {	
			break;  // 문자열 끝(널 문자)에 도달하면 검사 종료
		}
	}
}

/* 사용자 포인터 user_addr로부터 size 바이트까지의 주소 범위를
   페이지 단위로 하나씩 순회하며 모두 접근 가능한지 확인
   → 접근 불가능한 주소가 포함되어 있으면 프로세스를 종료함 (syscall_exit(-1))
   → 사용 예: read(), write(), exec() 등에서 전달받은 사용자 버퍼 검증 */
void
vali_pointer (const void *user_addr, size_t size) {
    if (size == 0) {
		return;  // 검사할 바이트 수가 0이면 return
	}

    const uint8_t *check_ptr = user_addr; // 현재 검사할 위치 (byte 단위 포인터)
    size_t byte_left = size;         // 검사해야 할 남은 바이트 수

    // 검사할 전체 영역을 페이지 단위로 나누어 한 페이지씩 접근 가능 여부를 확인
    while (byte_left > 0) {
        // 현재 check_ptr 포인터가 가리키는 주소가 사용자 영역에 있고
        // 실제로 물리 메모리에 매핑되어 있는지 확인
        if (!check_page (check_ptr)) {
            syscall_exit (-1);  // 잘못된 주소일 경우, 즉시 프로세스 종료
		}

        // 현재 페이지에서 끝까지 남은 바이트 수 계산
		/* pg_ofs() - 예를 들어, 페이지 크기가 4KB라면, 0~4095 바이트 범위 안에서 어느 지점에 데이터가 있는지를 나타내는 값이 offset입니다 */
        size_t page_left = PGSIZE - pg_ofs(check_ptr);

        // 남은 전체 바이트와 현재 페이지에서 가능한 바이트 중 더 작은 만큼만 이동
        size_t chunk = byte_left < page_left ? byte_left : page_left;

        check_ptr += chunk;  // 검사할 포인터를 다음 영역으로 이동
        byte_left -= chunk;  // 검사해야 할 남은 바이트 수 갱신
    }
}

/* 내부 헬퍼: 단일 가상 주소 user_addr이 
   - NULL이 아니고
   - 사용자 영역에 속하며
   - 현재 프로세스의 페이지 테이블에 매핑되어 있는지 확인 */
bool
check_page (const void *user_addr) {
    return user_addr != NULL &&
           is_user_vaddr(user_addr) &&
           pml4_get_page (thread_current ()->pml4, user_addr) != NULL;
}

/* 파일을 삭제하는 시스템콜 핸들러 */
bool
syscall_remove(const char *file)
{
    // 사용자 포인터 유효성 검사 (커널 주소/NULL/미매핑 주소 거부)
    usr_address_vali(file);
    // 파일 시스템에서 해당 경로의 파일 삭제 시도, 성공 여부 반환
    return filesys_remove(file);
}

/* 파일을 생성하는 시스템콜 핸들러 */
bool
syscall_create(const char *file, unsigned initial_size) {
    // 사용자 포인터 유효성 검사 (커널 주소/NULL/미매핑 주소 거부)
    usr_address_vali(file);
    // 파일 시스템에 새 파일 생성 (초기 크기 지정), 성공 여부 반환 (boolean)
    return filesys_create(file, initial_size);
}

/* 사용자 주소 유효성 검증 유틸리티 */
void 
usr_address_vali (const void *addr)
{
    // 다음 중 하나라도 해당하면 프로세스를 종료(-1)
    // - 커널 주소 공간(is_kernel_vaddr)
    // - NULL 포인터
    // - 현재 프로세스의 페이지테이블(pml4)에 매핑되지 않은 주소
    if (is_kernel_vaddr(addr) || addr == NULL || pml4_get_page(thread_current()->pml4, addr) == NULL) {
    	syscall_exit(-1); // 잘못된 사용자 포인터는 즉시 종료
    }
}

pid_t
syscall_fork(const char *name, struct intr_frame *f) {
	// 사용자 영역 포인터인지, 유효한 페이지에 매핑되어 있는지 선행 검사
    usr_address_vali(name);
    tid_t child = process_fork(name, f);
	// thread_create 실패 시 TID_ERROR가 내려오므로 PID_ERROR로 변환해 반환
    return child == TID_ERROR ? PID_ERROR : child;
}

int
syscall_wait(int pid) {
    return process_wait(pid);
}

int
syscall_exec(const char *cmd_line) {
	// 유효한 주소인지 검사
	usr_address_vali(cmd_line);

	// 사용자로부터 받은 문자열(cmd_line)을 복사할 커널 영역의 페이지를 할당
	// PAL_ZERO는 할당된 메모리를 0으로 초기화하라는 의미
	char *cmd_line_copy = palloc_get_page(PAL_ZERO);
	
	// 만약 메모리 할당에 실패했다면, exit 처리
	if (cmd_line_copy == NULL) {
		return -1;
	}

	// 사용자 영역 문자열을 안전하게 복사하면서 페이지 경계를 검사한다.
	if (!copy_user_string(cmd_line_copy, cmd_line, PGSIZE)) {
		palloc_free_page(cmd_line_copy);
		return -1;
	}

	// 실제로 새로운 프로그램을 현재 프로세스 위에 실행
	// 실패하면 -1을 반환하므로, exit 처리
	if (process_exec(cmd_line_copy) == -1) {
		syscall_exit(-1);
	}

    // 여기까지 실행이 오면 안된다 - 라는 매크로
	NOT_REACHED();
}

bool
copy_user_string(char *dst, const char *src, size_t max_len) {
	for (size_t i = 0; i < max_len; i++) {
		/* 매 바이트 접근 전에 해당 주소가 사용자 영역인지 검사한다. */
		usr_address_vali(src + i);
		char c = src[i];
		dst[i] = c;
		/* NULL 문자를 만났다면 복사가 완료된 것. */
		if (c == '\0') {
			return true;
		}
	}
	/* 문자열이 최대 허용 길이 안에서 끝나지 않았음. */
	return false;
}

/* 프로세스 종료 시스템콜 핸들러 */
void
syscall_exit(int status) {
    struct thread *current_thread = thread_current();
    current_thread->exit_status = status;
    printf("%s: exit(%d)\n", current_thread->name, status);
    thread_exit();
}

/* 전원 종료(Pintos/QEMU 종료) 시스템콜 핸들러 */
void
syscall_halt(void) {
    // 전원 끄기: Pintos/QEMU 종료
    power_off();
}
