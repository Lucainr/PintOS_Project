#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#ifdef VM
#include "vm/vm.h"
#endif

/* 스레드의 생명주기 상태 */
enum thread_status
{
	THREAD_RUNNING, /* 실행 중인 스레드 */
	THREAD_READY,	/* 실행 중은 아니지만 실행 준비된 상태 */
	THREAD_BLOCKED, /* 이벤트를 기다리며 차단된 상태 */
	THREAD_DYING	/* 곧 종료될 예정인 상태 */
};

/* 스레드 식별자 타입 */
typedef int tid_t;
#define TID_ERROR ((tid_t) - 1) /* tid_t 에러 값 */

/* 스레드 우선순위 */
#define PRI_MIN 0	   /* 최소 우선순위 */
#define PRI_DEFAULT 31 /* 기본 우선순위 */
#define PRI_MAX 63	   /* 최대 우선순위 */

/* 커널 스레드 또는 사용자 프로세스
 *
 * 각 스레드 구조체는 자신의 4KB 페이지에 저장된다.
 * 스레드 구조체는 페이지의 맨 아래(0 오프셋)에 위치하고,
 * 나머지 공간은 커널 스택을 위해 예약된다.
 * 커널 스택은 페이지 상단(4KB)에서부터 아래로 자라난다.
 *
 * 그림으로 표현하면:
 *
 *      4 kB +---------------------------------+
 *           |          커널 스택               |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |        위에서 아래로 성장        |
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
 *           |          intr_frame             |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * 요약하자면 두 가지 주의사항이 있다:
 *
 * 1. `struct thread`가 너무 커지면 안 된다.
 *    커지면 커널 스택 공간이 부족해진다. 기본 구조체 크기는 몇 바이트에 불과하며,
 *    이상적으로는 1KB 미만을 유지해야 한다.
 *
 * 2. 커널 스택이 너무 커지면 안 된다.
 *    스택 오버플로우가 발생하면 스레드 상태가 손상된다.
 *    따라서 커널 함수에서 큰 구조체나 배열을 로컬 변수로 선언하면 안 된다.
 *    대신 malloc()이나 palloc_get_page()로 동적 할당해야 한다.
 *
 * 이런 문제가 발생하면 thread_current()의 assertion 실패로 드러나며,
 * 이는 스택 오버플로우로 인해 `magic` 값이 변했기 때문이다.
 */

/* `elem` 멤버는 두 가지 용도로 사용된다.
 * - 실행 준비 큐(run queue)의 원소
 * - 세마포어 대기 리스트(wait list)의 원소
 * 이 두 경우는 동시에 발생하지 않기 때문에 같은 멤버로 공유 가능하다.
 * 즉, READY 상태일 때는 실행 큐에, BLOCKED 상태일 때는 세마포어 리스트에 들어간다.
 */
struct thread
{
	/* thread.c에서 관리 */
	tid_t tid;				   /* 스레드 식별자 */
	enum thread_status status; /* 스레드 상태 */
	char name[16];			   /* 이름 (디버깅용) */
	int priority;			   /* 우선순위 */
	int64_t wakeup_tick;
	/* thread.c와 synch.c에서 공유 */

#ifdef USERPROG
	/* userprog/process.c에서 사용 */
	uint64_t *pml4; /* Page map level 4 */
#endif
#ifdef VM
	/* 스레드가 소유한 전체 가상 메모리 보조 테이블 */
	struct supplemental_page_table spt;
#endif
	struct list_elem elem;
	/* thread.c에서 관리 */
	struct intr_frame tf; /* 문맥 교환(switching)을 위한 정보 */
	unsigned magic;		  /* 스택 오버플로우 감지용 값 */
};

/* 스케줄러 모드:
 * false (기본): 라운드 로빈 스케줄러 사용
 * true: 다단계 피드백 큐 스케줄러 사용
 * → 커널 실행 옵션 "-o mlfqs"로 제어됨
 */
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

#endif /* threads/thread.h */