#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
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
   of thread.h for details.
	struct thread의 magic 멤버에 저장되는 임의의 값.
스택 오버플로우를 감지하는 용도로 사용된다. 자세한 내용은 thread.h 맨 위의 큰 주석을 참고.*/
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value.
   기본 스레드를 위한 임의의 값.
이 값은 수정하면 안 된다. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running.
	 THREAD_READY 상태에 있는 프로세스들의 리스트.
즉, 실행할 준비는 되었지만 실제로는 실행 중이지 않은 프로세스들의 목록.
   */
static struct list ready_list;

/* Idle thread.
idle(유휴) 스레드.*/
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main().
초기 스레드, 즉 init.c:main()을 실행하는 스레드.*/
static struct thread *initial_thread;

/* Lock used by allocate_tid().
allocate_tid()에서 사용되는 락.*/
static struct lock tid_lock;

/* Thread destruction requests
스레드 제거 요청 리스트. */
static struct list destruction_req;

/* Statistics. 통계용 변수들. */
static long long idle_ticks;   /* # of timer ticks spent idle. idle_ticks: 유휴 상태에서 보낸 타이머 틱 수.*/
static long long kernel_ticks; /* # of timer ticks in kernel threads.  kernel_ticks: 커널 스레드에서 보낸 타이머 틱 수.*/
static long long user_ticks;   /* # of timer ticks in user programs.  user_ticks: 사용자 프로그램에서 보낸 타이머 틱 수.*/

/* Scheduling.  스케줄링 관련 변수.*/
#define TIME_SLICE 4		  /* # of timer ticks to give each thread.   TIME_SLICE: 각 스레드에 주어지는 타이머 틱 수 (라운드 로빈).*/
static unsigned thread_ticks; /* # of timer ticks since last yield.  thread_ticks: 마지막 yield 이후 지난 타이머 틱 수.*/

/* If false (default), use round-robin scheduler. false이면 (기본값) 라운드 로빈 스케줄러 사용.
   If true, use multi-level feedback queue scheduler. true이면 다단계 피드백 큐 스케줄러 사용.
   Controlled by kernel command-line option "-o mlfqs".  커널 실행 옵션 -o mlfqs로 제어됨.*/
bool thread_mlfqs;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule(void);
static tid_t allocate_tid(void);

/* Returns true if T appears to point to a valid thread. T가 유효한 스레드를 가리키는 것처럼 보이면 true를 반환한다. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread.
 * 현재 실행 중인 스레드를 반환한다.
CPU의 스택 포인터 rsp를 읽은 뒤 페이지 시작 주소로 내림 처리한다.
struct thread는 항상 페이지의 맨 앞에 있고, 스택 포인터는 그 페이지의 중간쯤을 가리키므로 이렇게 하면 현재 스레드를 찾을 수 있다.*/
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
   finishes.
   현재 실행 중인 코드를 스레드로 변환하여 스레드 시스템을 초기화한다.
일반적으로는 불가능하지만, 여기서는 loader.S가 스택의 맨 아래를 페이지 경계에 두었기 때문에 가능하다.

또한 실행 큐(run queue)와 tid_lock도 초기화한다.

이 함수를 호출한 뒤에는 반드시 페이지 할당자를 초기화한 다음 thread_create()로 새로운 스레드를 생성해야 한다.

이 함수가 끝나기 전까지는 thread_current()를 호출하는 것은 안전하지 않다.*/
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

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread();
	init_thread(initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread.
	인터럽트를 활성화하여 선점형 스레드 스케줄링을 시작한다.
	또한 idle 스레드를 생성한다.*/
void thread_start(void)
{
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init(&idle_started, 0);
	thread_create("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down(&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context.
   타이머 인터럽트 핸들러에 의해 매 타이머 틱마다 호출된다.
	따라서 이 함수는 외부 인터럽트 컨텍스트에서 실행된다.*/
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

/* Prints thread statistics.
	스레드 통계를 출력한다. */
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
   Priority scheduling is the goal of Problem 1-3.

   NAME이라는 이름과 초기 우선순위 PRIORITY를 가진 새로운 커널 스레드를 생성한다.
	이 스레드는 FUNCTION을 실행하며, 인자로 AUX를 전달받는다. 생성된 스레드는 준비 큐(ready queue)에 추가된다.

	생성이 성공하면 새 스레드의 tid를 반환하고, 실패하면 TID_ERROR를 반환한다.

	thread_start()가 호출된 이후라면, thread_create()가 리턴되기 전에 새 스레드가 스케줄링될 수 있다. 심지어 새 스레드가 종료될 수도 있다.
	반대로 원래 스레드가 새 스레드가 스케줄링되기 전에 오랫동안 실행될 수도 있다. 따라서 실행 순서를 보장하려면 세마포어나 다른 동기화 기법을 사용해야 한다.

	코드 상에서는 스레드의 priority 멤버를 설정해주지만, 실제 우선순위 스케줄링은 구현되어 있지 않다. 우선순위 스케줄링은 과제 1-3의 목표이다. */
tid_t thread_create(const char *name, int priority,
					thread_func *function, void *aux)
{
	struct thread *t;
	tid_t tid;

	ASSERT(function != NULL);

	/* Allocate thread.  스레드 구조체를 위한 메모리를 할당한다.*/
	t = palloc_get_page(PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. 스레드 구조체를 초기화한다. */
	init_thread(t, name, priority);
	tid = t->tid = allocate_tid();

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument.
		스케줄링되면 kernel_thread를 호출하도록 설정한다.
	참고: rdi 레지스터는 첫 번째 인자, rsi 레지스터는 두 번째 인자이다.*/
	t->tf.rip = (uintptr_t)kernel_thread;
	t->tf.R.rdi = (uint64_t)function;
	t->tf.R.rsi = (uint64_t)aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. 실행 대기 큐(run queue)에 추가한다. */
	thread_unblock(t);

	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h.
   현재 실행 중인 스레드를 수면 상태로 만든다.
	이 스레드는 thread_unblock()에 의해 깨워지기 전까지 다시 스케줄링되지 않는다.

	이 함수는 반드시 인터럽트가 꺼진 상태에서 호출되어야 한다.
	보통은 synch.h에 정의된 동기화 원시(primitives)를 사용하는 것이 더 좋다.*/
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
   update other data.

	차단(블록)된 스레드 T를 **준비 완료 상태(READY)**로 전환한다.
	만약 T가 차단 상태가 아니라면 잘못된 호출이다. (실행 중인 스레드를 준비 상태로 만들고 싶다면 thread_yield()를 사용해야 한다.)

	이 함수는 현재 실행 중인 스레드를 선점(preempt)하지 않는다.
	이는 중요할 수 있는데, 호출자가 직접 인터럽트를 꺼놨다면 “스레드를 깨우는 것”과 “다른 데이터를 수정하는 것”을 원자적으로(atomic하게) 처리할 수 있어야 하기 때문이다.  */
void thread_unblock(struct thread *t)
{
	enum intr_level old_level;

	ASSERT(is_thread(t));

	old_level = intr_disable();
	ASSERT(t->status == THREAD_BLOCKED);
	list_push_back(&ready_list, &t->elem);
	t->status = THREAD_READY;
	intr_set_level(old_level);
}

/* Returns the name of the running thread.  현재 실행 중인 스레드의 이름을 반환한다.*/
const char *
thread_name(void)
{
	return thread_current()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details.
	현재 실행 중인 스레드를 반환한다.
	내부적으로 running_thread()를 호출하고, 몇 가지 안정성(sanity) 검사를 추가한다.
	자세한 내용은 thread.h 맨 위의 주석을 참고할 것.*/
struct thread *
thread_current(void)
{
	struct thread *t = running_thread();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow.

	 T가 실제로 스레드인지 확인한다.
	만약 이 ASSERT가 실패한다면, 해당 스레드는 아마 스택 오버플로우가 발생했을 가능성이 크다.
	각 스레드는 4KB 미만의 스택만 갖고 있기 때문에, 큰 자동 배열을 선언하거나 재귀 호출이 조금만 깊어져도 스택 오버플로우가 발생할 수 있다.  */
	ASSERT(is_thread(t));
	ASSERT(t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid.  현재 실행 중인 스레드의 tid(스레드 식별자)를 반환한다.*/
tid_t thread_tid(void)
{
	return thread_current()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller.
	현재 실행 중인 스레드를 스케줄링에서 제거하고 파괴한다.
	이 함수는 호출자에게 절대 반환되지 않는다.*/
void thread_exit(void)
{
	ASSERT(!intr_context());

#ifdef USERPROG
	process_exit();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail().
	단순히 현재 스레드의 상태를 DYING으로 표시하고 다른 프로세스를 스케줄링한다.
	실제 스레드 구조체의 파괴는 schedule_tail() 호출 도중에 수행된다.*/
	intr_disable();
	do_schedule(THREAD_DYING);
	NOT_REACHED();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim.
	CPU를 양보한다.
	현재 스레드는 수면 상태로 들어가지 않으며, 스케줄러가 결정하기에 따라 즉시 다시 실행될 수도 있다.*/
void thread_yield(void)
{
	struct thread *curr = thread_current();
	enum intr_level old_level;

	ASSERT(!intr_context());

	old_level = intr_disable();
	if (curr != idle_thread)
		list_push_back(&ready_list, &curr->elem);
	do_schedule(THREAD_READY);
	intr_set_level(old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. 현재 실행 중인 스레드의 우선순위를 NEW_PRIORITY로 설정한다.*/
void thread_set_priority(int new_priority)
{
	thread_current()->priority = new_priority;
}

/* Returns the current thread's priority.  현재 실행 중인 스레드의 우선순위를 반환한다. */
int thread_get_priority(void)
{
	return thread_current()->priority;
}

/* Sets the current thread's nice value to NICE. 현재 실행 중인 스레드의 nice 값을 NICE로 설정한다.*/
void thread_set_nice(int nice UNUSED)
{
	/* TODO: Your implementation goes here  */
}

/* Returns the current thread's nice value. 현재 실행 중인 스레드의 nice 값을 반환한다.*/
int thread_get_nice(void)
{
	/* TODO: Your implementation goes here  */
	return 0;
}

/* Returns 100 times the system load average. 시스템 load average(부하 평균값)에 100을 곱한 값을 반환한다.*/
int thread_get_load_avg(void)
{
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. 현재 스레드의 recent_cpu 값에 100을 곱한 값을 반환한다 */
int thread_get_recent_cpu(void)
{
	/* TODO: Your implementation goes here */
	return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty.
	Idle 스레드. 다른 스레드가 실행 준비가 안 되어 있을 때 실행된다.

	Idle 스레드는 처음에 thread_start()에 의해 준비 큐(ready list)에 들어간다.
	한 번 스케줄링된 뒤에는 idle_thread를 초기화하고, 전달받은 세마포어를 up 하여 thread_start()가 계속 진행될 수 있게 해주며, 곧바로 차단(block)된다.
	그 이후로는 Idle 스레드는 준비 큐에 다시 나타나지 않는다.
	준비 큐가 비어 있는 경우 next_thread_to_run()에서 특별 케이스로 반환된다.*/
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

/* Function used as the basis for a kernel thread.  커널 스레드의 기본 동작으로 사용되는 함수. */
static void
kernel_thread(thread_func *function, void *aux)
{
	ASSERT(function != NULL);

	intr_enable(); /* The scheduler runs with interrupts off. */
	function(aux); /* Execute the thread function. */
	thread_exit(); /* If function() returns, kill the thread. */
}

/* Does basic initialization of T as a blocked thread named
   NAME. 스레드 T를 이름 NAME을 가진 차단 상태의 스레드로 기본 초기화한다. */
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
	t->priority = priority;
	t->wakeup_tick = 0;
	t->magic = THREAD_MAGIC;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread.
   다음에 스케줄링할 스레드를 선택해서 반환한다.
	실행 큐(run queue)가 비어 있지 않다면 그 안에서 스레드를 반환한다.
	(실행 중인 스레드가 계속 실행 가능한 경우라면 실행 큐에 들어 있다.)
	만약 실행 큐가 비어 있다면 idle_thread를 반환한다.*/
static struct thread *
next_thread_to_run(void)
{
	if (list_empty(&ready_list))
		return idle_thread;
	else
		return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread  iretq 명령어를 사용하여 스레드를 실행한다.*/
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
   added at the end of the function.
   새 스레드의 페이지 테이블을 활성화하여 스레드를 전환하고, 이전 스레드가 종료 중이라면 그것을 파괴한다.

	이 함수가 호출될 때는 이미 이전 스레드(PREV)에서 새로운 스레드로 전환이 끝난 상태이며, 인터럽트는 여전히 꺼져 있다.

	스레드 전환이 완전히 끝날 때까지는 printf()를 호출하는 것이 안전하지 않다.
	실제로는 printf()는 함수 끝부분에만 넣는 것이 좋다. */
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
	 * until switching is done.
	 * 메인 스레드 전환 로직.
	먼저 실행 컨텍스트 전체를 intr_frame에 저장한 후, do_iret을 호출하여 다음 스레드로 전환한다.
	주의: 여기서부터 전환이 끝날 때까지는 스택을 절대 사용해서는 안 된다.
	 */
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
 * It's not safe to call printf() in the schedule().
 * 새로운 프로세스를 스케줄링한다. 함수에 들어올 때는 인터럽트가 꺼져 있어야 한다.
	현재 스레드의 상태를 status로 바꾼 다음, 다른 스레드를 찾아 실행을 전환한다.
	schedule() 안에서는 printf()를 호출하는 것이 안전하지 않다.*/
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
		   schedule().
		   우리가 방금 전환한 스레드가 종료 중(DYING)이라면, 그 스레드의 구조체를 파괴한다.
		이 작업은 반드시 나중에 일어나야 하며, 그래야 thread_exit() 실행 도중 스레드 자체가 사라져버리는 문제를 피할 수 있다.

		현재 그 페이지는 스택에서 사용 중이므로, 여기서는 단순히 “해제 요청”만 큐에 넣는다.
		실제 메모리 해제 로직은 다음 번 schedule()의 시작 부분에서 실행된다.*/
		if (curr && curr->status == THREAD_DYING && curr != initial_thread)
		{
			ASSERT(curr != next);
			list_push_back(&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running.
			스레드를 전환하기 전에, 현재 실행 중인 스레드의 정보를 먼저 저장한다. */
		thread_launch(next);
	}
}

/* Returns a tid to use for a new thread.
	새 스레드에 사용할 tid를 반환한다.*/
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
