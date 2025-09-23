#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "lib/stdio.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

#define MAX_ARGS 128

static void process_cleanup(void);
static bool load(const char *file_name, struct intr_frame *if_);
static void initd(void *f_name);
static void __do_fork(void *);
static int parse_args(char *, char *[]);
static void argument_stack(char *argv[], int argc, struct intr_frame *_if);

/* 초기 사용자 프로세스를 위한 최소한의 동기화.
   이것은 커널이 첫 번째 사용자 프로세스가 종료될 때까지
   대기하도록 보장하기 위한 임시 메커니즘으로,
   테스트에서 사용자 출력을 관찰할 수 있게 한다. */
static struct semaphore initd_sema;
// extern → 다른 파일에 정의된 전역 변수를 여기서 참조하겠다는 의미
extern bool thread_tests; /* threads/init.c 파일 안에서 정의되어 있다 */

/* General process initializer for initd and other process. */
static void
process_init(void)
{
	struct thread *current = thread_current();

	// 새 프로세스용 파일 디스크립터 테이블을 0으로 초기화된 상태로 준비
	current->FDT = palloc_get_multiple(PAL_ZERO, FDT_PAGES); // 사용자 프로세스의 FDT를 0으로 초기화된 페이지로 확보

	// 아직 실행 파일이 연결되지 않았으므로 기본값으로 비워둔다.
	current->running_file = NULL; // 현재 실행 파일 포인터 초기화

	// 표준 입출력 0~2(stdin, stdout, stderr)를 건너뛰고, 일반 파일 fd는 3부터 할당한다.
	current->next_FD = 3;											   // 다음에 배정할 파일 디스크립터 시작값 지정
	current->stdin_count = 1;										   // 기본 STDIN 하나가 열려 있음을 표시
	current->stdout_count = 1;										   // 기본 STDOUT 하나가 열려 있음을 표시
	current->FDT[STDIN_FILENO] = syscall_get_std_file(STDIN_FILENO);   // FDT[0]에 STDIN 더미 파일 객체 연결
	current->FDT[STDOUT_FILENO] = syscall_get_std_file(STDOUT_FILENO); // FDT[1]에 STDOUT 더미 파일 객체 연결
}

/* FILE_NAME에서 불러온 "initd"라는 첫 번째 사용자 프로그램을 시작한다.
 * 새 스레드는 process_create_initd()가 반환되기 전에
 * 스케줄될 수도 있고(심지어 종료될 수도 있다).
 * initd의 스레드 ID를 반환하며, 스레드를 생성할 수 없는 경우 TID_ERROR를 반환한다.
 * 참고: 이 함수는 반드시 한 번만 호출되어야 한다. */
tid_t process_create_initd(const char *file_name)
{
	char *fn_copy;
	tid_t tid;

	/* Initialize minimal wait synchronization for initd.
	   Only meaningful in userprog mode (not threads tests). */
	if (!thread_tests)
	{
		sema_init(&initd_sema, 0);
	}

	/* FILE_NAME의 복사본을 만든다.
	 * 그렇지 않으면 호출자와 load() 사이에 경쟁 상태(race)가 발생할 수 있다. */
	fn_copy = palloc_get_page(0); // 0의 의미는 Kernel 쪽에서 만들어라 라는 뜻
	// fn_copy는 file_name의 복사본. 즉, args-single onearg
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy(fn_copy, file_name, PGSIZE);

	/* FILE_NAME을 실행할 새로운 스레드를 생성한다. */
	char thread_name[16]; // thread 구조체 내부 name 크기가 char name[16]임
	strlcpy(thread_name, file_name, sizeof thread_name);
	char *save_ptr;
	char *token = strtok_r(thread_name, " ", &save_ptr); // file_name만큼만 wkfma

	if (token == NULL)
	{
		token = thread_name;
	}

	tid = thread_create(token, PRI_DEFAULT, initd, fn_copy);

	if (tid == TID_ERROR)
	{ // TID_ERROR 는 -1
		palloc_free_page(fn_copy);
	}
	return tid;
}

/* 첫 번째 사용자 프로세스를 실행하는 스레드 함수 */
static void
initd(void *f_name)
{
#ifdef VM
	supplemental_page_table_init(&thread_current()->spt);
#endif

	process_init();
	// process_exec()에서 initd(fn_copy)로 넘어왔던 fn_copy가 f_name임 args_single onearg
	if (process_exec(f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED();
}

/* 현재 프로세스를 `name`이라는 이름으로 복제한다.
 * 새 프로세스의 스레드 ID를 반환하며,
 * 스레드를 생성할 수 없으면 TID_ERROR를 반환한다. */
tid_t process_fork(const char *name, struct intr_frame *if_ UNUSED)
{
	struct thread *parent = thread_current();

	// 부모 스레드가 trap에서 복귀할 때 사용할 레지스터 상태를 백업
	memcpy(&parent->intr_frame, if_, sizeof(struct intr_frame));

	// __do_fork()를 실행할 새 커널 스레드를 생성한다. 실패하면 즉시 종료
	tid_t fork_tid = thread_create(name, PRI_DEFAULT, __do_fork, parent);
	if (fork_tid == TID_ERROR)
	{
		return TID_ERROR;
	}

	// 방금 만든 자식을 부모의 children 리스트에서 찾아옴
	struct thread *child = get_child_thread(fork_tid);
	if (child == NULL)
	{
		return TID_ERROR; // 리스트에서 찾지 못하면 실패로 간주 -1 return
	}

	// 자식이 주소 공간/파일 테이블 복제를 완료할 때까지 대기
	sema_down(&child->fork_sema);

	// 자식 초기화가 실패했다면 부모는 fork 실패로 처리
	if (child->fork_status != 0)
	{
		list_remove(&child->child_elem); // 부모 children 리스트에서 제거
		sema_up(&child->exit_sema);		 // 자식이 종료 루틴을 마칠 수 있게 깨움
		return TID_ERROR;
	}

	// 모든 준비가 끝났으므로 자식의 tid를 반환
	return fork_tid;
}

#ifndef VM
/* Project 2에서 부모의 주소 공간을 복제할 때 pml4_for_each에 전달하는 헬퍼 함수 */
static bool
duplicate_pte(uint64_t *pte, void *va, void *aux)
{
	struct thread *current = thread_current();
	struct thread *parent = (struct thread *)aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1) 커널 주소라면 굳이 복제할 필요가 없으므로 바로 성공 처리한다. */
	if (is_kernel_vaddr(va))
	{
		return true;
	}
	/* 2) 부모의 pml4에서 해당 가상주소(VA)에 매핑된 물리 페이지를 찾는다. */
	parent_page = pml4_get_page(parent->pml4, va);

	if (parent_page == NULL)
	{
		return false;
	}
	/* 3) 자식이 사용할 사용자 페이지(PAL_USER)를 새로 할당한다. */
	newpage = palloc_get_page(PAL_USER | PAL_ZERO);
	if (newpage == NULL)
	{
		return false;
	}
	/* 4) 부모 페이지 내용을 통째로 복사하고, 쓰기 가능 여부는 pte에서 확인한다. */
	memcpy(newpage, parent_page, PGSIZE);
	/* 5) 자식의 pml4에 해당 VA로 새 페이지를 매핑한다. */
	writable = is_writable(pte);
	if (!pml4_set_page(current->pml4, va, newpage, writable))
	{
		/* 6) 매핑에 실패하면 복제 실패로 처리한다. */
		return false;
	}
	return true;
}
#endif

/* 부모의 실행 컨텍스트를 복사하는 스레드 함수.
 * 힌트) parent->tf 는 프로세스의 사용자 영역 컨텍스트(userland context)를
 *       가지고 있지 않다.
 *       즉, process_fork 의 두 번째 인자를 이 함수에 전달해야 한다. */
static void
__do_fork(void *aux)
{
	struct intr_frame if_;
	struct thread *parent = (struct thread *)aux;
	struct thread *current = thread_current();
#ifdef VM
	supplemental_page_table_init(&current->spt);
#endif

	/* TODO: parent_if를 어떻게든 전달해야 한다. (즉, process_fork()의 if_ 인자를 의미한다) */
	struct intr_frame *parent_if = &parent->intr_frame;
	bool succ = true;

	/* 새 스레드가 사용할 FDT, FD 인덱스 등 기본 자료구조를 초기화한다. */
	process_init();
	struct file *stdin_file = syscall_get_std_file(STDIN_FILENO);	// 부모와 동일한 STDIN 더미 포인터 캐싱
	struct file *stdout_file = syscall_get_std_file(STDOUT_FILENO); // 부모와 동일한 STDOUT 더미 포인터 캐싱
	for (int fd = 0; fd < MAX_FD; fd++)
	{
		current->FDT[fd] = NULL; // 부모 상태를 그대로 채우기 위해 자식 FDT를 먼저 비워 둠
	}
	current->stdin_count = 0;  // 부모 복사를 통해 실제 STDIN 참조 개수를 다시 계산할 예정
	current->stdout_count = 0; // 부모 복사를 통해 실제 STDOUT 참조 개수를 다시 계산할 예정

	/* 부모가 fork 시스템 콜을 호출하던 시점의 레지스터 값을 자식 intr_frame에 그대로 복사한다. */
	memcpy(&if_, parent_if, sizeof(struct intr_frame));

	/* 사용자 주소 공간을 위한 pml4를 새로 만들고 부모의 페이지 매핑을 그대로 복제한다. */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
	{
		succ = false;
		goto done;
	}

	process_activate(current);
#ifdef VM
	/* 새로 만든 pml4를 활성화한 뒤, VM 기능 사용 시 부모의 SPT 엔트리를 순회하면서 lazy load 정보까지 복사한다. */
#else
	/* VM 기능이 없다면 부모의 pml4를 직접 순회해 사용자 페이지를 새로 할당하고 내용을 복제한다. */
#endif
#ifdef VM
	if (!supplemental_page_table_copy(&current->spt, &parent->spt))
	{
		succ = false;
		goto done;
	}
#else
	/* VM 미사용 시 부모의 PTE를 순회하며 자식 페이지를 만들어 붙인다. */
	if (!pml4_for_each(parent->pml4, duplicate_pte, parent))
	{
		succ = false;
		goto done;
	}
#endif

	/* 실행 중인 파일 객체도 복제하여 부모의 deny-write 상태까지 그대로 유지한다. */
	if (parent->running_file != NULL)
	{
		current->running_file = file_duplicate(parent->running_file);
		if (current->running_file == NULL)
		{
			succ = false;
			goto done;
		}
	}
	/* 부모가 열어 둔 모든 파일 디스크립터를 순회하며 자식 FDT에 채운다. */
	for (int fd = 0; fd < MAX_FD; fd++)
	{
		struct file *parent_file = parent->FDT[fd];
		if (parent_file == NULL)
		{
			continue; // 부모가 사용하지 않은 슬롯은 넘긴다
		}
		if (parent_file == stdin_file)
		{
			current->FDT[fd] = stdin_file; // STDIN 더미 포인터 공유
			current->stdin_count++;		   // 자식이 보유한 STDIN 참조 수 누적
			continue;
		}
		if (parent_file == stdout_file)
		{
			current->FDT[fd] = stdout_file; // STDOUT 더미 포인터 공유
			current->stdout_count++;		// 자식이 보유한 STDOUT 참조 수 누적
			continue;
		}
		current->FDT[fd] = file_duplicate(parent_file); // 일반 파일은 별도 file 객체를 만들어 부모와 파일 오프셋이 섞이지 않도록 함
		if (current->FDT[fd] == NULL)
		{
			succ = false;
			goto done;
		}
	}
	current->next_FD = parent->next_FD;
	/* 다음에 할당할 fd 인덱스도 동일하게 맞춰 부모와 동일한 순서로 파일을 열 수 있게 한다. */

	/* 자식 프로세스는 fork 반환값으로 0을 받아야 하므로 RAX를 0으로 설정한다. */
	if_.R.rax = 0;

done:
	current->fork_status = succ ? 0 : -1;
	if (!succ)
	{
		current->exit_status = -1;
	}
	/* 초기화 완료(성공/실패 모두)를 부모에게 알린다. */
	sema_up(&current->fork_sema);
	if (succ)
	{
		do_iret(&if_); // 성공했다면 사용자 모드로 복귀하여 자식 실행을 시작한다.
	}
	thread_exit();
}

/* 현재 실행 컨텍스트를 f_name으로 전환한다.
 * 실패하면 -1을 반환한다. */
int process_exec(void *f_name)
{
	// 최대 MAX_ARGS 개수 만큼의 인자들을 저장할 배열 선언
	char *argv[MAX_ARGS];
	// f_name은 "실행파일명과 인자1 인자2 ..." 형태의 문자열임
	// 이를 공백 기준으로 파싱하여 argv에 저장하고 argc에 개수를 저장
	int argc = parse_args(f_name, argv);

	bool success;

	/* thread 구조체 안에 있는 intr_frame은 사용할 수 없다.
	 * 그 이유는 현재 스레드가 리스케줄(reschedule)될 때,
	 * 실행 정보가 그 멤버에 저장되기 때문이다. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* 먼저 현재 컨텍스트를 종료한다.
	 * - 열린 파일 닫기
	 * - 페이지 테이블 해제
	 * - 유저 스택 정리
	 * 기존에 exec()가 시작되기 전에 기존 running_file을 file_close()로 닫고 포인터를 NULL로 초기화 했는데,
	 * 부모에서 물려받은 실행 파일 핸들이 그대로 남아서 inode_deny_write_cnt가 줄지 않았음
	 * 그래서 rox-multichild 처럼 자식이 모두 끝난 뒤에도 실행 파일 쓰기가 계속 막힘
	 * 현재는 exec()가 이전 실행 파일의 deny-write 상태를 해제하고 새 프로그램을 로드하기 때문에
	 * 참조 카운트가 0으로 돌아가고 부모의 마지막 write()도 성공함*/
	struct thread *current_thread = thread_current();
	if (current_thread->running_file != NULL)
	{
		/* 이전 실행 파일의 deny-write를 해제하고 핸들을 닫는다. */
		file_close(current_thread->running_file);
		current_thread->running_file = NULL;
	}
	process_cleanup();

	// 파일 이름 Parsing 결과의 첫번째 토큰은 실제 실행할 파일 이름임
	ASSERT(argv[0] != NULL);

	/* 그리고 나서 바이너리를 적재한다. - ELF load
	 * 이미 컴파일되어 있는 실행파일(binary file)을 메모리에 불러와 실행 준비를 한다. */
	// Load the executable by file name only (not the whole command line).
	success = load(argv[0], &_if);

	/* If load failed, free f_name and quit. */
	if (!success)
	{
		palloc_free_page(f_name);
		return -1;
	}

	argument_stack(argv, argc, &_if);
	// hex_dump(_if.rsp, _if.rsp, USER_STACK - _if.rsp, true);

	palloc_free_page(f_name); // kernel쪽의 f_name 페이지 해제

	/* 커널에서 유저 프로세스로 전환 - 컨텍스트 전환된 프로세스를 실행 시작한다
	 * Context switching 과정의 마지막 단계, 현재 실행중인 프로세스를 다른 프로세스로 전환(switch)하고, 전환된 새로운 프로세스를 실제로 실행(start) */
	do_iret(&_if);
	NOT_REACHED();
}

/* 스레드 TID가 종료될 때까지 기다렸다가, 그 종료 상태(exit status)를 반환한다.
 * 만약 커널에 의해 종료되었을 경우(즉, 예외 때문에 강제 종료된 경우), -1을 반환한다.
 * TID가 유효하지 않거나, 호출한 프로세스의 자식 프로세스가 아니거나,
 * 주어진 TID에 대해 process_wait()이 이미 성공적으로 호출된 경우,
 * 기다리지 않고 즉시 -1을 반환한다.
 *
 * 이 함수는 문제 2-2에서 구현될 예정이다. 현재는 아무 동작도 하지 않는다. */
int process_wait(tid_t child_tid)
{
	// 인터럽트를 비활성화하여 동기화 문제를 방지함
	enum intr_level old_level = intr_disable();

	// 현재 스레드(부모)의 자식 리스트에서 주어진 TID를 가진 자식을 탐색
	struct thread *child_thread = get_child_thread(child_tid);
	intr_set_level(old_level); // 인터럽트 다시 활성화

	// 만약 해당 자식이 존재하지 않는다면 잘못된 접근이므로 -1 반환
	if (child_thread == NULL)
		return -1;

	// 자식 프로세스가 종료될 때까지 부모 프로세스를 대기 상태로 전환 (sema_down)
	sema_down(&child_thread->wait_sema);

	// 이후 자식 종료 시 process_exit으로부터 대기를 마치고 깨어남 (sema_up)
	// 자식의 종료 상태(exit_status)를 받아옴
	int stat = child_thread->exit_status;

	// 자식 리스트에서 해당 자식 정보를 제거
	list_remove(&child_thread->child_elem);

	// 자식이 완전히 종료될 수 있도록 process_exit의 자식을 깨워줌 (sema_up)
	sema_up(&child_thread->exit_sema);

	// 자식의 종료 상태를 부모에게 반환
	return stat;
}

/* Exit the process. This function is called by thread_exit (). */
void process_exit(void)
{
	// 현재 종료 중인 프로세스(thread)를 가져옴
	struct thread *current_thread = thread_current();

	// 파일 디스크럽터 테이블(FDT)이 존재한다면 열린 파일을 모두 닫는다.
	if (current_thread->FDT != NULL)
	{
		for (int fd = 0; fd < MAX_FD; fd++)
		{
			if (current_thread->FDT[fd] != NULL)
			{
				syscall_close(fd); // dup_count와 STDIN/STDOUT 카운트를 반영하며 안전하게 닫기
			}
		}
		// 파일 디스크럽터 테이블에 할당했던 메모리 해제
		palloc_free_multiple(current_thread->FDT, FDT_PAGES);
	}

	file_close(current_thread->running_file);

	// syscall의 exit에서 exit_status 설정이 선행되어야함
	if (current_thread->parent != NULL)
	{
		sema_up(&current_thread->wait_sema);
		// 부모가 살아 있고(또는 기다릴 의사 표시가 됐다면)만 기다림
		if (current_thread->parent->status != THREAD_DYING)
		{
			sema_down(&current_thread->exit_sema);
		}
	}

	process_cleanup();
}

/* Free the current process's resources. */
static void
process_cleanup(void)
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

/* 다음 스레드가 사용자 코드를 실행할 수 있도록 CPU 상태를 설정한다.
 * 모든 컨텍스트 스위치마다 호출된다. */
void process_activate(struct thread *next)
{
	/* 스레드의 페이지 테이블을 활성화한다. */
	pml4_activate(next->pml4);

	/* 인터럽트 처리 시 사용할 커널 스택을 TSS에 반영한다. */
	tss_update(next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL 0			/* Ignore. */
#define PT_LOAD 1			/* Loadable segment. */
#define PT_DYNAMIC 2		/* Dynamic linking info. */
#define PT_INTERP 3			/* Name of dynamic loader. */
#define PT_NOTE 4			/* Auxiliary info. */
#define PT_SHLIB 5			/* Reserved. */
#define PT_PHDR 6			/* Program header table. */
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

/* FILE_NAME에서 ELF 실행 파일을 현재 스레드로 로드한다.
 * 실행 파일의 진입점(entry point)을 *RIP에 저장하고,
 * 초기 스택 포인터를 *RSP에 저장한다.
 * 성공하면 true를 반환하고, 실패하면 false를 반환한다.
 * ELF 실행 파일: 리눅스/유닉스 계열에서 쓰는 실행 파일 포맷
 * RIP (Instruction Pointer): CPU가 다음에 실행할 명령어 주소
 * RSP (Stack Pointer): 현재 스택의 최상단을 가리키는 포인터
 */
static bool
load(const char *file_name, struct intr_frame *if_)
{
	struct thread *t = thread_current();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create();
	if (t->pml4 == NULL)
		goto done;
	process_activate(thread_current());

	/* Open executable file. */
	file = filesys_open(file_name);
	if (file == NULL)
	{
		printf("load: %s: open failed\n", file_name);
		goto done;
	}

	file_deny_write(file);	// 현재 실행중인 파일 쓰기 금지
	t->running_file = file; // 스레드의 running_file을 현재 파일로 설정

	/* Read and verify executable header. */
	if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr || memcmp(ehdr.e_ident, "\177ELF\2\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 0x3E // amd64
		|| ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Phdr) || ehdr.e_phnum > 1024)
	{
		printf("load: %s: error loading executable\n", file_name);
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
					zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
				}
				else
				{
					/* Entirely zero.
					 * Don't read anything from disk. */
					read_bytes = 0;
					zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
				}
				if (!load_segment(file, file_page, (void *)mem_page,
								  read_bytes, zero_bytes, writable))
					goto done;
			}
			else
				goto done;
			break;
		}
	}

	/* Set up stack. */
	if (!setup_stack(if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	// file_close (file);
	return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment(const struct Phdr *phdr, struct file *file)
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
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
			 uint32_t read_bytes, uint32_t zero_bytes, bool writable)
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

/* USER_STACK 주소에 0으로 초기화된(page가 모두 0인) 메모리 페이지를 매핑해서 가장 기본적인 스택 공간을 마련해준다.
   Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack(struct intr_frame *if_)
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

// 문자열 target을 공백(" ") 기준으로 잘라서 각 토큰(인자)을 argv 배열에 저장하고, 인자의 개수를 반환하는 함수
// 예: target = "echo hello world" → argv = ["echo", "hello", "world", NULL]
static int parse_args(char *target, char *argv[])
{
	int argc = 0; // 인자의 개수를 세기 위한 변수
	char *token;
	char *save_ptr; // strtok_r에서 파싱 상태를 유지하기 위한 포인터 (reentrant-safe)

	// 첫 번째 토큰 추출. strtok_r는 문자열을 공백을 기준으로 분리
	for (token = strtok_r(target, " ", &save_ptr);
		 token != NULL;
		 token = strtok_r(NULL, " ", &save_ptr)) // 이후 토큰부터는 첫 인자에 NULL 전달
	{
		argv[argc++] = token; // 잘라낸 인자를 argv 배열에 저장하고 argc 증가
	}

	// argv는 마지막에 NULL 포인터로 끝나야 exec 계열 함수에서 제대로 처리됨 (C 언어 컨벤션)
	argv[argc] = NULL;

	// 최종적으로 인자의 개수를 반환
	return argc;
}

// 사용자 프로그램의 스택을 구성하여 인자들을 전달하는 함수
static void argument_stack(char *argv[], int argc, struct intr_frame *_if)
{
	uint64_t rsp_arr[argc]; // 각 인자 문자열의 시작 주소를 저장할 배열

	// 문자열을 스택에 역순으로 복사
	for (int i = argc - 1; i >= 0; i--)
	{
		size_t len = strlen(argv[i]) + 1;		// 문자열 길이 + 널 문자 포함
		_if->rsp -= len;						// 스택 아래로 공간 확보
		rsp_arr[i] = _if->rsp;					// 해당 문자열이 위치한 주소 저장
		memcpy((void *)_if->rsp, argv[i], len); // 스택에 문자열 복사
	}

	// 16바이트 정렬 맞추기 (rsp를 16의 배수로 내림 정렬)
	_if->rsp = _if->rsp & ~0xF; // 하위 4비트 0으로 마스킹 → 16의 배수

	// x86-64 SysV ABI: 함수 진입 시 rsp % 16 == 8이 되도록 맞춘다.
	// 이후에 NULL(8) + argv 포인터들(8*argc) + fake return(8)을 푸시할 예정이므로
	// argc가 짝수일 경우 패딩 8바이트를 추가해 총 푸시 바이트가 16으로 나머지 8이 되도록 한다.
	if ((argc % 2) == 0)
	{
		_if->rsp -= 8;
		memset((void *)_if->rsp, 0, 8);
	}

	// NULL sentinel push (argv[argc] = NULL)
	_if->rsp -= 8;								 // 포인터 크기만큼 스택 아래로
	memset((void *)_if->rsp, 0, sizeof(char *)); // 0으로 채움 (NULL)

	// argv[i] 포인터들을 역순으로 push
	for (int i = argc - 1; i >= 0; i--)
	{
		_if->rsp -= 8;										   // 8바이트 공간 확보
		memcpy((void *)_if->rsp, &rsp_arr[i], sizeof(char *)); // 각 문자열의 주소를 복사
	}

	// 가짜 주소 fake return address (unused, just for conventional layout)
	// 실제로 쓰이지 않는 가짜 리턴 주소인데, 스택 프레임의 모양을 함수 호출 규약에 맞게 유지하려고 형식적으로만 넣은 값
	_if->rsp -= 8;
	memset((void *)_if->rsp, 0, sizeof(void *)); // 가짜 리턴 주소 = 0

	// 사용자 프로그램 시작 시 인자 전달을 위한 레지스터 설정
	_if->R.rdi = argc;		   // 첫 번째 인자: argc
	_if->R.rsi = _if->rsp + 8; // 두 번째 인자: argv (가짜 리턴 주소 다음부터가 argv[0] 배열)
}

struct thread *get_child_thread(tid_t child_tid)
{
	struct thread *current_thread = thread_current(); // 현재 실행 중인 스레드(=부모 스레드)를 가져옴
	struct thread *result = NULL;					  // 결과를 저장할 포인터

	// 현재 스레드의 자식 리스트를 순회함
	for (struct list_elem *i = list_begin(&current_thread->children); i != list_end(&current_thread->children); i = i->next)
	{
		// 리스트 요소 i를 thread 구조체로 변환
		struct thread *t = list_entry(i, struct thread, child_elem);

		// 자식 스레드의 tid가 찾고자 하는 child_tid와 같다면
		if (t->tid == child_tid)
		{
			result = t; // 찾은 자식 스레드를 result에 저장
			break;		// 더 이상 탐색할 필요 없으므로 반복문 종료
		}
	}

	return result; // 찾았으면 해당 스레드 포인터 반환, 못 찾았으면 NULL 반환
}

int process_add_file(struct file *file)
{
	// 현재 실행 중인 스레드(=프로세스) 가져오기
	struct thread *current_thread = thread_current();

	// 파일 디스크립터(fd)는 0~2는 이미 예약된 상태(stdin, stdout, stderr)
	// 따라서 일반 파일은 3번부터 사용
	for (int fd = 3; fd < MAX_FD; fd++)
	{
		// 현재 FDT(File Descriptor Table)에서 비어있는 슬롯 찾기
		if (current_thread->FDT[fd] == NULL)
		{
			// 비어 있는 슬롯을 찾으면 해당 위치에 파일 포인터 저장
			current_thread->FDT[fd] = file;

			// 다음 검색할 fd 번호를 갱신
			current_thread->next_FD = fd + 1;

			// 성공적으로 등록한 fd 번호 반환
			return fd;
		}
	}

	// 모든 슬롯이 차서 더 이상 파일을 열 수 없다면 -1 반환
	return -1;
}

struct file *process_get_file(int fd)
{
	struct thread *current_thread = thread_current(); // 현재 실행 중인 스레드 포인터 획득
	if (current_thread->FDT == NULL)
	{
		return NULL; // FDT가 아직 준비되지 않았다면 접근할 수 있는 파일이 없음
	}
	if (fd < 0 || fd >= MAX_FD)
	{
		return NULL; // 허용 범위를 벗어난 파일 디스크립터는 무효로 간주
	}
	return current_thread->FDT[fd]; // 유효한 fd라면 FDT에서 대응되는 파일 포인터 반환
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
static bool
install_page(void *upage, void *kpage, bool writable)
{
	struct thread *t = thread_current();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page(t->pml4, upage) == NULL && pml4_set_page(t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment(struct page *page, void *aux)
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
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
			 uint32_t read_bytes, uint32_t zero_bytes, bool writable)
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
		if (!vm_alloc_page_with_initializer(VM_ANON, upage,
											writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack(struct intr_frame *if_)
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
