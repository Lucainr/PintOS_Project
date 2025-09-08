#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include <list.h>
/* [8254] 문서 참고: 8254 타이머 칩 하드웨어 세부사항 */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* 슬립 대기 스레드들을 담는 리스트 */
static struct list sleep_list;

/* wakeup_tick이 더 작은 스레드가 앞으로 오도록 정렬 */
static bool wakeup_earlier(const struct list_elem *a,
						   const struct list_elem *b,
						   void *aux UNUSED)
{
	const struct thread *ta = list_entry(a, struct thread, elem);
	const struct thread *tb = list_entry(b, struct thread, elem);
	return ta->wakeup_tick < tb->wakeup_tick;
}

/* OS가 부팅된 이후 지난 타이머 틱 수 */
static int64_t ticks;

/* 한 타이머 틱 동안 반복할 루프 수.
   timer_calibrate()에서 초기화됨 */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops(unsigned loops);
static void busy_wait(int64_t loops);
static void real_time_sleep(int64_t num, int32_t denom);

/* 8254 프로그래머블 인터벌 타이머(PIT)를
   TIMER_FREQ만큼 초당 인터럽트가 발생하도록 설정하고,
   해당 인터럽트를 등록함 */
void timer_init(void)
{
	/* 8254 입력 주파수를 TIMER_FREQ로 나눈 값(반올림) */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb(0x43, 0x34); /* CW: counter 0, LSB -> MSB, 모드 2, 이진 */
	outb(0x40, count & 0xff);
	outb(0x40, count >> 8);

	list_init(&sleep_list); // 슬립 리스트 초기화
	intr_register_ext(0x20, timer_interrupt, "8254 Timer");
}

/* loops_per_tick 보정 (짧은 delay 구현용) */
void timer_calibrate(void)
{
	unsigned high_bit, test_bit;

	ASSERT(intr_get_level() == INTR_ON);
	printf("Calibrating timer...  ");

	/* 한 틱 안에서 반복 가능한 최대 2의 제곱수 구하기 */
	loops_per_tick = 1u << 10;
	while (!too_many_loops(loops_per_tick << 1))
	{
		loops_per_tick <<= 1;
		ASSERT(loops_per_tick != 0);
	}

	/* 그 다음 8비트를 더 정밀하게 보정 */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops(high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf("%'" PRIu64 " loops/s.\n", (uint64_t)loops_per_tick * TIMER_FREQ);
}

/* OS 부팅 이후 지난 타이머 틱 수 반환 */
int64_t timer_ticks(void)
{
	enum intr_level old_level = intr_disable();
	int64_t t = ticks;
	intr_set_level(old_level);
	barrier();
	return t;
}

/* then 시점 이후 지난 틱 수 반환 */
int64_t timer_elapsed(int64_t then)
{
	return timer_ticks() - then;
}

/* 현재 스레드를 ticks 동안 슬립시킴 */
void timer_sleep(int64_t ticks)
{
	if (ticks <= 0)
		return;

	ASSERT(intr_get_level() == INTR_ON);
	enum intr_level old_level = intr_disable();

	struct thread *curr = thread_current();
	curr->wakeup_tick = timer_ticks() + ticks;

	/* 슬립 리스트에 wakeup_tick 기준으로 삽입 */
	list_insert_ordered(&sleep_list, &curr->elem, wakeup_earlier, NULL);
	thread_block(); // 현재 스레드 블록 상태로 전환

	intr_set_level(old_level);
}

/* ms 밀리초 동안 슬립 */
void timer_msleep(int64_t ms)
{
	real_time_sleep(ms, 1000);
}

/* us 마이크로초 동안 슬립 */
void timer_usleep(int64_t us)
{
	real_time_sleep(us, 1000 * 1000);
}

/* ns 나노초 동안 슬립 */
void timer_nsleep(int64_t ns)
{
	real_time_sleep(ns, 1000 * 1000 * 1000);
}

/* 타이머 통계 출력 */
void timer_print_stats(void)
{
	printf("Timer: %" PRId64 " ticks\n", timer_ticks());
}

/* 타이머 인터럽트 핸들러 */
static void timer_interrupt(struct intr_frame *args UNUSED)
{
	ticks++;

	bool need_yield = false;
	/* 슬립 리스트에서 깨어날 시간 된 스레드 깨우기 */
	while (!list_empty(&sleep_list))
	{
		struct thread *t = list_entry(list_front(&sleep_list), struct thread, elem);
		if (t->wakeup_tick <= ticks)
		{
			list_pop_front(&sleep_list);
			thread_unblock(t);
			need_yield = true;
		}
		else
		{
			break;
		}
	}
	thread_tick();
	if (need_yield)
	{
		intr_yield_on_return();
	}
}

/* 루프 반복 횟수가 한 틱을 초과하는지 확인 */
static bool too_many_loops(unsigned loops)
{
	/* 새로운 틱이 될 때까지 대기 */
	int64_t start = ticks;
	while (ticks == start)
		barrier();

	/* 루프 실행 */
	start = ticks;
	busy_wait(loops);

	/* 틱이 바뀌었으면 너무 오래 걸린 것 */
	barrier();
	return start != ticks;
}

/* loops 횟수만큼 단순 반복 (짧은 delay용) */
static void NO_INLINE busy_wait(int64_t loops)
{
	while (loops-- > 0)
		barrier();
}

/* 약 num/denom 초 동안 슬립 */
static void real_time_sleep(int64_t num, int32_t denom)
{
	/* 초 → 틱 단위로 변환
	   (NUM / DENOM)초 = NUM * TIMER_FREQ / DENOM 틱 */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT(intr_get_level() == INTR_ON);
	if (ticks > 0)
	{
		/* 최소 1틱 이상이면 timer_sleep() 사용
		   → CPU를 양보하고 다른 스레드가 실행될 수 있음 */
		timer_sleep(ticks);
	}
	else
	{
		/* 1틱보다 작으면 busy-wait 사용
		   오버플로우 방지를 위해 분모를 1000으로 나눠 스케일링 */
		ASSERT(denom % 1000 == 0);
		busy_wait(loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}