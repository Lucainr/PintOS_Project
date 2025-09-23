#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#include "threads/synch.h"
#ifdef VM
#include "vm/vm.h"
#endif

/* States in a thread's life cycle. */
enum thread_status
{
	THREAD_RUNNING, /* Running thread. */
	THREAD_READY,	/* Not running but ready to run. */
	THREAD_BLOCKED, /* Waiting for an event to trigger. */
	THREAD_DYING	/* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) - 1) /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0	   /* Lowest priority. */
#define PRI_DEFAULT 31 /* Default priority. */
#define PRI_MAX 63	   /* Highest priority. */

#define NICE_DEFAULT 0
#define RECENT_CPU_DEFAULT 0
#define LOAD_AVG_DEFAULT 0

// FDT
#define FDT_PAGES 1					  // 프로세스 FDT 초기화 시 할당할 페이지
#define MAX_FD (FDT_PAGES * (1 << 9)) // 최대 FD 개수 (전체 범위 순회 시 사용)

/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */
struct thread
{
	/* Owned by thread.c. */
	tid_t tid;				   /* Thread identifier. */
	enum thread_status status; /* Thread state. */
	char name[16];			   /* Name (for debugging purposes). */
	int priority;			   /* Priority. */

	int64_t wakeup;		   // 일어나야 하는 ticks 값
	int original_priority; // 기부 받기전 원래 가지고 있던 priority

	struct list donation_list; // 이 thread에게 우선순위(priority)를 기부한 thread들의 목록
	struct lock *waiting_lock; // 이 thread가 기다리고 있는 lock

	/* Shared between thread.c and synch.c. */
	struct list_elem elem;			/* List element. */
	struct list_elem donation_elem; /* 다른 스레드의 donations 리스트에 포함되기 위한 요소 */

	int niceness;
	int recent_cpu;
	struct list_elem all_elem;

#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4; /* Page map level 4 */
	// Project 2 - User Program
	int exit_status; // 종료 상태 값

	struct semaphore wait_sema; // 부모의 자식 종료 대기용 세마포어
	struct semaphore exit_sema; // 자식 종료 시 자식의 부모 wait 마무리 대기용 세마포어
	struct semaphore fork_sema; // 자식 프로세스 초기화 대기용 세마포어
	int fork_status;			// fork 초기화 성공 여부 (0:성공, -1:실패)

	struct list children;		  // 자식 프로세스 리스트
	struct list_elem child_elem;  // 부모의 children 리스트에 들어갈 element
	struct thread *parent;		  // 부모 프로세스 포인터
	struct intr_frame intr_frame; // 저삭 프로세스의 부모 레지스터 값 복제용 인터럽트 프레임

	struct file **FDT;		   // File Descriptor Table
	int next_FD;			   // 다음 사용 가능한 fd값
	struct file *running_file; // 현재 프로세스에서 실행 중인 파일

	int stdin_count;  // STDIN fd 개수
	int stdout_count; // STDOUT fd 개수
#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf; /* Information for switching */
	unsigned magic;		  /* Detects stack overflow. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init(void);
void thread_start(void);

void thread_tick(void);
void thread_print_stats(void);

typedef void thread_func(void *aux);
tid_t thread_create(const char *name, int priority, thread_func *, void *);

void thread_block(void);
void thread_unblock(struct thread *);

struct thread *thread_current(void);
tid_t thread_tid(void);
const char *thread_name(void);

void thread_exit(void) NO_RETURN;
void thread_yield(void);

int thread_get_priority(void);
void thread_set_priority(int);

int thread_get_nice(void);
void thread_set_nice(int);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

void do_iret(struct intr_frame *tf);

void thread_sleep(int64_t ticks);
void thread_awake(int64_t ticks);

void preempt_priority(void);
bool thread_cmp_priority(const struct list_elem *a, const struct list_elem *b, void *);
bool thread_cmp_priority_donation(const struct list_elem *a, const struct list_elem *b, void *);
bool thread_cmp_wakeup(const struct list_elem *a, const struct list_elem *b, void *);

void donation_priority(struct thread *t);
void remove_with_lock(struct lock *lock, struct list *donation_list);
void refresh_priority(struct thread *t);

void mlfqs_priority(struct thread *t);
void mlfqs_recent_cpu(struct thread *t);
void mlfqs_load_avg(void);
void mlfqs_increment(void);
void mlfqs_recalc_recent_cpu(void);
void mlfqs_recalc_priority(void);

#endif /* threads/thread.h */
