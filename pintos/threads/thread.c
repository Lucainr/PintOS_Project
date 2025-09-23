#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/fixed-point.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* sleep에 들어간 Thread들을 관리하는 list */
static struct list sleep_list;

static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4		  /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

int load_avg;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule(void);
static tid_t allocate_tid(void);

/*
 * donation_priority
 * - 스레드 t가 어떤 lock을 기다리는 동안(대기/블록), 낮은 우선순위 보유자 때문에
 *   오래 대기하지 않도록 보유자에게 우선순위를 "donation"(기부)한다.
 *
 * - t->waiting_lock가 NULL이 아닐 때, 그 lock의 holder에게 t의 우선순위를 전달한다.
 * - holder가 또 다른 lock을 기다리는 중이라면, 그 holder의 holder로 기부한다
 *   (중첩 도네이션 체인).
 * - donation_list는 thread.donation_elem 기반의 리스트이므로, 전용 비교자로 정렬 유지한다.
 *
 * - 이 함수는 도네이션을 기부만 한다. 원상 복구는 lock_release() 경로에서
 *   remove_with_lock() + refresh_priority()로 처리한다.
 */
void donation_priority(struct thread *t)
{
	struct thread *lock_thread;
	/*
	 t->waiting_lock가 NULL이 아닐 때까지 loop
	 가장 아래의 t = lock_thread를 통해 또다른 lock을 대기하는 다른 스레드 목록을 탐색
	 loop를 돌아 holder가 또 다른 lock을 기다리는 중이라면 그 holder의 holder로 기부 (연쇄?연속적으로 기부)
	*/
	while (t->waiting_lock != NULL)
	{
		// 그 lock의 holder에게 t의 우선순위를 전달한다.
		lock_thread = t->waiting_lock->holder;

		// lock의 소유자가 없다면 break;
		if (lock_thread == NULL)
		{
			break;
		}
		// donation_list는 thread.donation_elem 기반 리스트이므로 전용 비교자를 사용해 정렬
		list_sort(&lock_thread->donation_list, thread_cmp_priority_donation, NULL);

		// lock을 가지고 있는 스레드(holder)의 우선순위보다 나의 우선순위가 더 높다면 우선순위를 기부
		if (lock_thread->priority < t->priority)
		{
			lock_thread->priority = t->priority;
		}
		/* lock->holder로 바꿔서 내가 기다리는 lock의 holder가
		 * 기다리는 lock이 또 있을 수 있으니 변환하여 loop 재진입 */
		t = lock_thread;
	}
}

void preempt_priority(void)
{
	// 현재 Thread가 Idel_thread여선 안됨
	if (thread_current() == idle_thread)
	{
		return;
	}

	// ready_list가 비어서도 안된다.
	if (list_empty(&ready_list))
	{
		return;
	}

	struct thread *curr = thread_current();
	struct thread *ready = list_entry(list_front(&ready_list), struct thread, elem);

	// 현재 Thread보다 Ready_list에 있는 thread의 pritority가 더 크면 양보해야함
	if (curr->priority < ready->priority)
	{
		thread_yield();
	}
}

/* Ready/Sleep 리스트 정렬을 위한 비교 함수들 */
/* ready_list: 우선순위가 높은 스레드가 앞에 오도록(내림차순) */
bool thread_cmp_priority(const struct list_elem *a,
						 const struct list_elem *b,
						 void *aux UNUSED)
{
	const struct thread *thread_a = list_entry(a, struct thread, elem);
	const struct thread *thread_b = list_entry(b, struct thread, elem);
	return thread_a->priority > thread_b->priority;
}

/* donation_list용 비교함수
   thread.donation_elem을 원소로 가지는 리스트에서 사용 */
bool thread_cmp_priority_donation(const struct list_elem *a,
								  const struct list_elem *b,
								  void *aux UNUSED)
{
	const struct thread *thread_a = list_entry(a, struct thread, donation_elem);
	const struct thread *thread_b = list_entry(b, struct thread, donation_elem);
	return thread_a->priority > thread_b->priority;
}

/* sleep_list: 더 빨리 깨어날 스레드가 앞에 오도록(오름차순) */
bool thread_cmp_wakeup(const struct list_elem *a,
					   const struct list_elem *b,
					   void *aux UNUSED)
{
	const struct thread *thread_a = list_entry(a, struct thread, elem);
	const struct thread *thread_b = list_entry(b, struct thread, elem);
	return thread_a->wakeup < thread_b->wakeup;
}

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *)(pg_round_down(rrsp())))

// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = {0, 0x00af9a000000ffff, 0x00cf92000000ffff};

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void thread_init(void)
{
	ASSERT(intr_get_level() == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof(gdt) - 1,
		.address = (uint64_t)gdt};
	lgdt(&gdt_ds);

	/* Init the globla thread context */
	lock_init(&tid_lock);
	list_init(&ready_list);
	list_init(&destruction_req);
	list_init(&sleep_list);
	list_init(&all_list);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread();
	init_thread(initial_thread, "main", PRI_DEFAULT);

	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid();
}

/* Thread를 재우고 sleep상태가 된 Thread를 sleep_list에 담아 관리 */
void thread_sleep(int64_t ticks)
{
	struct thread *cur_thread; // 현재 Thread
	enum intr_level old_level; // 이전 Interrupt 상태

	old_level = intr_disable();	   // Interrupt OFF
	cur_thread = thread_current(); // 현재 Thread를 불러와 담아줌

	ASSERT(cur_thread != idle_thread); // 현재 Thread가 idle_thread가 되어선 안됨

	cur_thread->wakeup = ticks; // 현재 Thread가 일어나야 할 시간을 설정(ticks)
	/* 일어날 시각 순(wakeup 오름차순)으로 정렬 삽입 */
	list_insert_ordered(&sleep_list, &cur_thread->elem, thread_cmp_wakeup, NULL);

	thread_block(); // Thread를 block 상태로 변경

	intr_set_level(old_level); // Interrupt ON
}

void thread_awake(int64_t ticks)
{
	/* sleep_list는 wakeup 오름차순으로 유지되므로, 앞에서부터
	   현재 tick 이하인 스레드만 깨우고 중단한다. */
	bool need_yield = false;
	while (!list_empty(&sleep_list))
	{
		struct thread *t = list_entry(list_front(&sleep_list), struct thread, elem);
		if (t->wakeup <= ticks)
		{
			list_pop_front(&sleep_list);
			thread_unblock(t);
			if (t->priority > thread_current()->priority)
			{
				need_yield = true;
			}
		}
		else
		{
			break;
		}
	}
	/* 인터럽트 컨텍스트에서는 즉시 스케줄링할 수 없으므로 반환 시 양보. */
	if (need_yield && intr_context())
	{
		intr_yield_on_return();
	}
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start(void)
{
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init(&idle_started, 0);
	thread_create("idle", PRI_MIN, idle, &idle_started);

	load_avg = LOAD_AVG_DEFAULT;

	/* Start preemptive thread scheduling. */
	intr_enable();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down(&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void thread_tick(void)
{
	struct thread *t = thread_current();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return();
}

/* Prints thread statistics. */
void thread_print_stats(void)
{
	printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
		   idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t thread_create(const char *name, int priority,
					thread_func *function, void *aux)
{
	struct thread *t;
	tid_t tid;

	ASSERT(function != NULL);

	/* Allocate thread. */
	t = palloc_get_page(PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread(t, name, priority);
	tid = t->tid = allocate_tid();

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t)kernel_thread;
	t->tf.R.rdi = (uint64_t)function;
	t->tf.R.rsi = (uint64_t)aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	thread_unblock(t);

	// Project2 - User Program
	struct thread *current_thread = thread_current();
	// 자식 스레드의 부모를 current_thread로 설정
	t->parent = current_thread;
	// 부모 스레드의 자식 리스트에 자식 스레드 추가
	list_push_back(&current_thread->children, &t->child_elem);

	// 현재 생성된 Thread보다 readylist에 있는 Thread의 우선순위(priority)가 더 높다면 양보(yield)
	preempt_priority();

	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void thread_block(void)
{
	ASSERT(!intr_context());
	ASSERT(intr_get_level() == INTR_OFF);
	thread_current()->status = THREAD_BLOCKED;
	schedule();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void thread_unblock(struct thread *t)
{
	enum intr_level old_level;

	ASSERT(is_thread(t));

	old_level = intr_disable();
	ASSERT(t->status == THREAD_BLOCKED);
	/* 준비 큐는 우선순위 내림차순으로 유지한다. */
	list_insert_ordered(&ready_list, &t->elem, thread_cmp_priority, NULL);
	t->status = THREAD_READY;
	intr_set_level(old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name(void)
{
	return thread_current()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current(void)
{
	struct thread *t = running_thread();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT(is_thread(t));
	ASSERT(t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid(void)
{
	return thread_current()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit(void)
{
	ASSERT(!intr_context());

#ifdef USERPROG
	process_exit();
#endif

	if (thread_mlfqs)
	{
		/*
		 * MLFQS 사용 시, 모든 스레드는 all_list로 관리된다.
		 * - 스레드가 종료되면(all_list에서) 자신의 노드를 제거하여
		 *   이후 일괄 재계산(mlfqs_recalc_*) 시 더 이상 순회 대상이 되지 않도록 한다.
		 */
		list_remove(&thread_current()->all_elem);
	}

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable();
	do_schedule(THREAD_DYING);
	NOT_REACHED();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void thread_yield(void)
{
	struct thread *curr_thread = thread_current();
	enum intr_level old_level;

	ASSERT(!intr_context());

	old_level = intr_disable(); // Interrupt OFF
	if (curr_thread != idle_thread)
	{
		/* 현재 스레드를 준비 큐에 되돌릴 때도 우선순위 정렬 유지 */
		list_insert_ordered(&ready_list, &curr_thread->elem, thread_cmp_priority, NULL);
	}
	do_schedule(THREAD_READY);
	intr_set_level(old_level); // Interrupt ON (OFF하기 전 이전 상태 = old_level)
}

/* Sets the current thread's priority to NEW_PRIORITY. */
/* 현재 스레드의 우선순위를 갱신하고 필요 시 양보한다.
 * mlfqs 스케줄러를 활성하면 thread_mlfqs 변수는 true로 설정된다.
 * Advanced scheduler에서는 우선순위를 임의로 변경할 수 없다.
 */
void thread_set_priority(int new_priority)
{
	enum intr_level old_level = intr_disable(); // Interrupt OFF

	if (thread_mlfqs)
	{
		intr_set_level(old_level); // Interrupt ON
		return;
	}

	// 기본 우선순위만 갱신하고, 도네이션을 반영하여 실제 우선순위를 재계산한다.
	thread_current()->original_priority = new_priority;
	refresh_priority(thread_current());

	// 더 높은 우선순위 준비 스레드가 있으면 양보
	preempt_priority();

	intr_set_level(old_level); // Interrupt ON
}

/* Returns the current thread's priority. */
int thread_get_priority(void)
{
	return thread_current()->priority;
}

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice UNUSED)
{
	/* TODO: Your implementation goes here */
	struct thread *t = thread_current();

	enum intr_level old_level = intr_disable();
	t->niceness = nice;
	mlfqs_priority(t);
	preempt_priority();
	intr_set_level(old_level);
}

/* Returns the current thread's nice value. */
int thread_get_nice(void)
{
	/* TODO: Your implementation goes here */
	// return 0;
	struct thread *t = thread_current();

	enum intr_level old_level = intr_disable();
	int nice = t->niceness;
	intr_set_level(old_level);

	return nice;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void)
{
	/* TODO: Your implementation goes here */
	// return 0;
	enum intr_level old_level = intr_disable();
	int load_avg_val = fp_to_int_round(mult_mixed(load_avg, 100));
	intr_set_level(old_level);

	return load_avg_val;
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void)
{
	/* TODO: Your implementation goes here */
	// return 0;
	struct thread *t = thread_current();

	enum intr_level old_level = intr_disable();
	int recent_cpu = fp_to_int_round(mult_mixed(t->recent_cpu, 100));
	intr_set_level(old_level);

	return recent_cpu;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle(void *idle_started_ UNUSED)
{
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current();
	sema_up(idle_started);

	for (;;)
	{
		/* Let someone else run. */
		intr_disable();
		thread_block();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread(thread_func *function, void *aux)
{
	ASSERT(function != NULL);

	intr_enable(); /* The scheduler runs with interrupts off. */
	function(aux); /* Execute the thread function. */
	thread_exit(); /* If function() returns, kill the thread. */
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread(struct thread *t, const char *name, int priority)
{
	ASSERT(t != NULL);
	ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT(name != NULL);

	memset(t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy(t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t)t + PGSIZE - sizeof(void *);

	if (thread_mlfqs)
	{
		if (t == initial_thread)
		{ // 초기 thread일 경우 기본값으로 설정
			t->niceness = NICE_DEFAULT;
			t->recent_cpu = RECENT_CPU_DEFAULT;
		}
		else
		{ // 초기 thread가 아닐 경우, 나머지 스레드만 thread_currnet()로부터 값을 상속받음
			t->niceness = thread_current()->niceness;
			t->recent_cpu = thread_current()->recent_cpu;
		}
		mlfqs_priority(t);
		list_push_back(&all_list, &t->all_elem);
	}
	else
	{
		t->priority = priority; // 현재 우선순위(priority)
	}

	/* 우선 순위 priority */
	t->original_priority = priority; // 원래 우선순위(priority)

	/* donation 상태 초기화 */
	list_init(&t->donation_list);
	t->waiting_lock = NULL; // 현재 대기중인 lock이 없음

	t->magic = THREAD_MAGIC;

	t->exit_status = 0;
	// Project 2 - User Program
	sema_init(&t->wait_sema, 0);
	sema_init(&t->exit_sema, 0);
	list_init(&t->children);
	sema_init(&t->fork_sema, 0);
	t->fork_status = -1;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run(void)
{
	if (list_empty(&ready_list))
		return idle_thread;
	else
		return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void do_iret(struct intr_frame *tf)
{
	__asm __volatile(
		"movq %0, %%rsp\n"
		"movq 0(%%rsp),%%r15\n"
		"movq 8(%%rsp),%%r14\n"
		"movq 16(%%rsp),%%r13\n"
		"movq 24(%%rsp),%%r12\n"
		"movq 32(%%rsp),%%r11\n"
		"movq 40(%%rsp),%%r10\n"
		"movq 48(%%rsp),%%r9\n"
		"movq 56(%%rsp),%%r8\n"
		"movq 64(%%rsp),%%rsi\n"
		"movq 72(%%rsp),%%rdi\n"
		"movq 80(%%rsp),%%rbp\n"
		"movq 88(%%rsp),%%rdx\n"
		"movq 96(%%rsp),%%rcx\n"
		"movq 104(%%rsp),%%rbx\n"
		"movq 112(%%rsp),%%rax\n"
		"addq $120,%%rsp\n"
		"movw 8(%%rsp),%%ds\n"
		"movw (%%rsp),%%es\n"
		"addq $32, %%rsp\n"
		"iretq"
		: : "g"((uint64_t)tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch(struct thread *th)
{
	uint64_t tf_cur = (uint64_t)&running_thread()->tf;
	uint64_t tf = (uint64_t)&th->tf;
	ASSERT(intr_get_level() == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile(
		/* Store registers that will be used. */
		"push %%rax\n"
		"push %%rbx\n"
		"push %%rcx\n"
		/* Fetch input once */
		"movq %0, %%rax\n"
		"movq %1, %%rcx\n"
		"movq %%r15, 0(%%rax)\n"
		"movq %%r14, 8(%%rax)\n"
		"movq %%r13, 16(%%rax)\n"
		"movq %%r12, 24(%%rax)\n"
		"movq %%r11, 32(%%rax)\n"
		"movq %%r10, 40(%%rax)\n"
		"movq %%r9, 48(%%rax)\n"
		"movq %%r8, 56(%%rax)\n"
		"movq %%rsi, 64(%%rax)\n"
		"movq %%rdi, 72(%%rax)\n"
		"movq %%rbp, 80(%%rax)\n"
		"movq %%rdx, 88(%%rax)\n"
		"pop %%rbx\n" // Saved rcx
		"movq %%rbx, 96(%%rax)\n"
		"pop %%rbx\n" // Saved rbx
		"movq %%rbx, 104(%%rax)\n"
		"pop %%rbx\n" // Saved rax
		"movq %%rbx, 112(%%rax)\n"
		"addq $120, %%rax\n"
		"movw %%es, (%%rax)\n"
		"movw %%ds, 8(%%rax)\n"
		"addq $32, %%rax\n"
		"call __next\n" // read the current rip.
		"__next:\n"
		"pop %%rbx\n"
		"addq $(out_iret -  __next), %%rbx\n"
		"movq %%rbx, 0(%%rax)\n" // rip
		"movw %%cs, 8(%%rax)\n"	 // cs
		"pushfq\n"
		"popq %%rbx\n"
		"mov %%rbx, 16(%%rax)\n" // eflags
		"mov %%rsp, 24(%%rax)\n" // rsp
		"movw %%ss, 32(%%rax)\n"
		"mov %%rcx, %%rdi\n"
		"call do_iret\n"
		"out_iret:\n"
		: : "g"(tf_cur), "g"(tf) : "memory");
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status)
{
	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(thread_current()->status == THREAD_RUNNING);
	while (!list_empty(&destruction_req))
	{
		struct thread *victim =
			list_entry(list_pop_front(&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current()->status = status;
	schedule();
}

static void
schedule(void)
{
	struct thread *curr = running_thread();
	struct thread *next = next_thread_to_run();

	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(curr->status != THREAD_RUNNING);
	ASSERT(is_thread(next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate(next);
#endif

	if (curr != next)
	{
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread)
		{
			ASSERT(curr != next);
			list_push_back(&destruction_req, &curr->elem);
		}

		/* 스레드를 전환하기 전에, 현재 실행 중인 스레드의 정보를 먼저 저장합니다. */
		thread_launch(next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid(void)
{
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire(&tid_lock);
	tid = next_tid++;
	lock_release(&tid_lock);

	return tid;
}

/* recent_cpu와 nice값을 이용하여 priority를 계산 */
void mlfqs_priority(struct thread *t)
{
	/*
	 * MLFQS 우선순위 계산
	 * priority = PRI_MAX - (recent_cpu / 4) - (nice * 2)
	 * - idle 스레드는 계산 대상에서 제외
	 * - 계산 후 [PRI_MIN, PRI_MAX] 범위로 clamp
	 */
	if (t == idle_thread)
	{
		return;
	}

	/* recent_cpu/4는 고정소수점으로 표현되어 있으므로 div_mixed를 사용 */
	t->priority = fp_to_int(
		add_mixed(
			div_mixed(t->recent_cpu, -4), // -(recent_cpu/4)
			PRI_MAX - t->niceness * 2	  // PRI_MAX - nice*2
			));
	/* 범위 보정 */
	if (t->priority > PRI_MAX)
		t->priority = PRI_MAX;
	if (t->priority < PRI_MIN)
		t->priority = PRI_MIN;
}

/* recent_cpu 값 계산 */
void mlfqs_recent_cpu(struct thread *t)
{
	/*
	 * MLFQS recent_cpu 재계산 (1초마다)
	 * recent_cpu = (2*load_avg)/(2*load_avg + 1) * recent_cpu + nice
	 * - idle 스레드는 계산 대상에서 제외
	 */
	if (t == idle_thread)
	{
		return;
	}

	/* coefficient = 2*load_avg / (2*load_avg + 1) */
	t->recent_cpu = add_mixed(
		mult_fp(
			div_fp(
				mult_mixed(load_avg, 2),			  // 2*load_avg
				add_mixed(mult_mixed(load_avg, 2), 1) // 2*load_avg + 1
				),
			t->recent_cpu),
		t->niceness);
}

/* load_avg 값 계산 */
void mlfqs_load_avg(void)
{
	/*
	 * 시스템 load_avg 재계산 (1초마다)
	 * load_avg = (59/60)*load_avg + (1/60)*ready_threads
	 * - ready_threads = ready_list의 스레드 수 + (running이 idle이 아니면 +1)
	 */
	int ready_threads = list_size(&ready_list);
	if (thread_current() != idle_thread)
	{
		ready_threads++;
	}

	load_avg = add_fp(
		mult_fp(div_fp(int_to_fp(59), int_to_fp(60)), load_avg),	   // (59/60) * load_avg
		mult_mixed(div_fp(int_to_fp(1), int_to_fp(60)), ready_threads) // (1/60) * ready_threads
	);
}

/* recent_cpu 값 1 증가 */
void mlfqs_increment(void)
{
	/*
	 * 매 tick: running 스레드의 recent_cpu += 1 (idle 제외)
	 * - timer_interrupt()에서 호출됨.
	 */
	if (thread_current() == idle_thread)
	{
		return;
	}

	thread_current()->recent_cpu = add_mixed(thread_current()->recent_cpu, 1);
}

/* 모든 thread의 recent_cpu와 priority값 재계산 */
void mlfqs_recalc_recent_cpu(void)
{
	/*
	 * 매 1초마다: 모든 스레드의 recent_cpu를 일괄 재계산한다.
	 * - 대상: all_list에 담긴 모든 스레드(종료된 스레드는 thread_exit에서 제거됨)
	 */
	struct list_elem *e = list_begin(&all_list);
	while (e != list_end(&all_list))
	{
		struct thread *t = list_entry(e, struct thread, all_elem);
		mlfqs_recent_cpu(t);
		e = list_next(e);
	}
}

void mlfqs_recalc_priority(void)
{
	/*
	 * 매 4 ticks마다: 모든 스레드의 priority를 일괄 재계산한다.
	 * - 계산 공식: PRI_MAX - recent_cpu/4 - nice*2 (idle 제외)
	 * - 재계산 후 ready_list를 우선순위 기준으로 재정렬하여 선점 조건이 반영되도록 한다.
	 */
	struct list_elem *e = list_begin(&all_list);
	while (e != list_end(&all_list))
	{
		struct thread *t = list_entry(e, struct thread, all_elem);
		mlfqs_priority(t);
		e = list_next(e);
	}
	// 우선순위 갱신 결과를 ready_list에 반영 (내림차순 정렬)
	list_sort(&ready_list, thread_cmp_priority, NULL);
}
