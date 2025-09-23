/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */
void sema_init(struct semaphore *sema, unsigned value)
{
   ASSERT(sema != NULL);

   sema->value = value;
   list_init(&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. This is
   sema_down function. */
void sema_down(struct semaphore *sema)
{
   enum intr_level old_level;

   ASSERT(sema != NULL);
   ASSERT(!intr_context());

   // Interrupt를 비활성화하여 동시 접근으로 인한 문제를 방지
   old_level = intr_disable();
   // 세마포어 값이 0이면 자원이 사용 중이므로 대기
   while (sema->value == 0)
   {
      /* 현재 쓰레드(Current Thread)를 Semaphore 대기 리스트에 삽입, 우선순위가 높은 스레드가 앞에 오도록 cmp_sema_priority 사용 */
      list_insert_ordered(&sema->waiters, &thread_current()->elem, thread_cmp_priority, NULL);
      // 현재 쓰레드를 block상태로 전환하여 CPU에서 제외
      thread_block();
   }
   // 자원이 확보되었으므로, Semaphore 값을 1 감소
   sema->value--;

   // Interrupt를 이전 상태로 복원
   intr_set_level(old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool sema_try_down(struct semaphore *sema)
{
   enum intr_level old_level;
   bool success;

   ASSERT(sema != NULL);

   old_level = intr_disable();
   if (sema->value > 0)
   {
      sema->value--;
      success = true;
   }
   else
      success = false;
   intr_set_level(old_level);

   return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void sema_up(struct semaphore *sema)
{
   enum intr_level old_level;

   ASSERT(sema != NULL);

   // Interrupt를 비활성화하여 동시성 문제 방지
   old_level = intr_disable();

   // 대기 중인 Thread가 있다면
   if (!list_empty(&sema->waiters))
   {
      // waiters 리스트를 우선순위를 기준으로 정렬
      list_sort(&sema->waiters, thread_cmp_priority, NULL);
      // 가장 우선순위가 높은(front) Thread를 꺼냄
      struct thread *t = list_entry(list_pop_front(&sema->waiters), struct thread, elem);
      // 해당 Thread를 실행 가능한 상태로 unblock 전환
      thread_unblock(t);
   }
   // Semaphore 값 1 증가, 자원 반환
   sema->value++;
   // 더 높은 우선순위의 Thread가 있다면 CPU를 선점해 양보할수 있도록 확인
   preempt_priority();

   // Interrupt 상태를 복원
   intr_set_level(old_level);
}

static void sema_test_helper(void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void sema_self_test(void)
{
   struct semaphore sema[2];
   int i;

   printf("Testing semaphores...");
   sema_init(&sema[0], 0);
   sema_init(&sema[1], 0);
   thread_create("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
   for (i = 0; i < 10; i++)
   {
      sema_up(&sema[0]);
      sema_down(&sema[1]);
   }
   printf("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper(void *sema_)
{
   struct semaphore *sema = sema_;
   int i;

   for (i = 0; i < 10; i++)
   {
      sema_down(&sema[0]);
      sema_up(&sema[1]);
   }
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void lock_init(struct lock *lock)
{
   ASSERT(lock != NULL);

   lock->holder = NULL;
   list_init(&lock->waiters); // lock을 기다리는 thread들을 담을 목록 초기화
   sema_init(&lock->semaphore, 1);
}

/*
   lock 획득(도네이션 포함)
   LOCK을 획득한다. 필요하다면 LOCK이 사용 가능해질 때까지 슬립(대기) 상태로 들어간다.
   현재 스레드가 이미 이 LOCK을 가지고 있어서는 안 된다.

   이 함수는 슬립 상태에 들어갈 수 있으므로 인터럽트 핸들러 내부에서는 호출할 수 없다.
   인터럽트가 비활성화된 상태에서 호출할 수는 있지만, 슬립이 필요하다면 인터럽트는 다시 활성화된다.

   Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void lock_acquire(struct lock *lock)
{
   ASSERT(lock != NULL);
   ASSERT(!intr_context());
   ASSERT(!lock_held_by_current_thread(lock));
   if (!thread_mlfqs)
   {
      struct thread *current_thread = thread_current();
      if (lock->holder != NULL)
      {
         // 누군가 이미 lock을 보유 중이면: 현재 스레드의 waiting_lock에 대상 lock을 기록하고,
         current_thread->waiting_lock = lock;
         // lock 보유자(holder)의 donation_list에 현재 스레드의 donation_elem을 우선순위 내림차순으로 삽입한다.
         list_insert_ordered(&lock->holder->donation_list, &current_thread->donation_elem, thread_cmp_priority_donation, NULL);
         // lock 보유자(holder)에게 우선순위를 기부한다.
         donation_priority(current_thread);
      }

      // sema_down()으로 실제 lock을 획득한다(필요 시 BLOCKED 상태로 잠들 수 있음)
      sema_down(&lock->semaphore);
      // lock을 얻었으므로 더 이상 이 lock을 기다리지 않으므로 waiting_lock을 NULL로, holder를 현재 스레드로 설정
      current_thread->waiting_lock = NULL;
      lock->holder = thread_current();
   }
   else
   {
      sema_down(&lock->semaphore);
      lock->holder = thread_current();
   }
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool lock_try_acquire(struct lock *lock)
{
   bool success;

   ASSERT(lock != NULL);
   ASSERT(!lock_held_by_current_thread(lock));

   success = sema_try_down(&lock->semaphore);
   if (success)
      lock->holder = thread_current();
   return success;
}

/* Releases LOCK, which must be owned by the current thread.
   This is lock_release function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void lock_release(struct lock *lock)
{
   ASSERT(lock != NULL);
   ASSERT(lock_held_by_current_thread(lock));

   enum intr_level old_level;
   struct thread *holder_thread = lock->holder;

   old_level = intr_disable();

   if (!thread_mlfqs)
   {

      lock->holder = NULL;

      // holder의 donation_list에서 이 lock으로부터 유입된 도네이션 항목을 제거
      remove_with_lock(lock, &holder_thread->donation_list);
      // holder의 우선순위를 재계산(refresh_priority)
      refresh_priority(holder_thread);

      // sema_up()으로 가장 우선순위 높은 대기자를 READY로 만든다.
      sema_up(&lock->semaphore);
      //  현재의 priority보다 원래 가지고 있던 priority가 더 클 경우 양보
      if (holder_thread->priority < holder_thread->original_priority)
      {
         thread_yield();
      }
   }
   else
   {
      lock->holder = NULL;
      sema_up(&lock->semaphore);
   }

   intr_set_level(old_level);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool lock_held_by_current_thread(const struct lock *lock)
{
   ASSERT(lock != NULL);

   return lock->holder == thread_current();
}

/* One semaphore in a list. */
struct semaphore_elem
{
   struct list_elem elem;      /* List element. */
   struct semaphore semaphore; /* This semaphore. */
   int priority;               // 우선순위
};

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void cond_init(struct condition *cond)
{
   ASSERT(cond != NULL);

   list_init(&cond->waiters);
}

/*
   조건 변수를 이용해 현재 Thread를 대기(wait) 상태로 전환
   semaphore_elem struct를 생성해서 현재 Thread의 우선 순위를 저장하고,
   조건 변수의 대기 스트(cond->waiters)에 우선순위 기준으로 삽입
   그 다음 Lock을 잠시 해제하고(sema_down에서 block으로 대기하기 때문)
   Semaphore를 통해 신호를 받을 때까지 대기
   신호를 받아 깨어나면 다시 Lock을 획득하고 Critical Section(임계구역) 진입 시도

   Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void cond_wait(struct condition *cond, struct lock *lock)
{
   struct semaphore_elem waiter;

   ASSERT(cond != NULL);
   ASSERT(lock != NULL);
   ASSERT(!intr_context());
   ASSERT(lock_held_by_current_thread(lock));

   // 현재 Thread의 우선순위(priority)를 저장 (우선순위 기반 정렬을 위해)
   waiter.priority = thread_get_priority();

   // 해당 Semaphore를 0으로 초기화하여 sema_down() 시 block이 되도록 함
   sema_init(&waiter.semaphore, 0);

   // 조건 변수 대기 리스트(cond->waiters)에 우선순위(priority)를 기준으로 삽입
   list_insert_ordered(&cond->waiters, &waiter.elem, cmp_sema_priority, NULL);

   // Lock을 해제하여 다른 Thread가 자원을 사용할 수 있도록 함
   lock_release(lock);

   // Semaphore가 올라갈 때(sema_up)까지 대기함
   sema_down(&waiter.semaphore);

   // 깨어난 후 Lock을 다시 획득
   lock_acquire(lock);
}

/*
   조건 변수(cond)의 대기 리스트에서 가장 우선순위가 높은 Thread 하나를 깨움
   이를 위해 먼저 대기 리스트(cond->waiters)를 우선순위(priority) 기준으로 정렬한 후
   가장 앞에 있는 Semaphore를 찾아 sema_up()을 호출하여 대기중인 Thread를 unblock 상태로 만듬
   이 함수는 반드시 Lock을 소유한 상태에서 호출되어야 하며, 대기중인 Thread가 없다면 아무 작업도 하지 않음

   If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void cond_signal(struct condition *cond, struct lock *lock UNUSED)
{
   ASSERT(cond != NULL);
   ASSERT(lock != NULL);
   ASSERT(!intr_context());                   // Interrupt Context에서는 호출할 수 없다
   ASSERT(lock_held_by_current_thread(lock)); // Current Thread가 Lock을 가지고 있어야 함

   // 대기중인 Thread가 있다면
   if (!list_empty(&cond->waiters))
   {
      // 조건 변수 대기 리스트는 semaphore_elem.priority 기준으로 정렬한다.
      list_sort(&cond->waiters, cmp_sema_priority, NULL);
      // 가장 높은 우선순위 대기자의 세마포어를 올려서 깨운다.
      sema_up(&list_entry(list_pop_front(&cond->waiters), struct semaphore_elem, elem)->semaphore);
   }
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void cond_broadcast(struct condition *cond, struct lock *lock)
{
   ASSERT(cond != NULL);
   ASSERT(lock != NULL);

   while (!list_empty(&cond->waiters))
      cond_signal(cond, lock);
}

/*
 * cmp_sema_priority
 * 두 개의 리스트 요소 a, b를 받아 각각을 semaphore_elem 구조체로 변환한 뒤,
 * 그 안의 priority 값을 비교하여 a가 b보다 우선순위가 높으면 true를 반환함
 * 이 함수는 리스트를 우선순위 기준으로 내림차순(높은 우선순위 먼저) 정렬할 때 사용됨
 */
bool cmp_sema_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
   // 리스트 요소 a, b를 semaphore_elem 구조체로 캐스팅
   struct semaphore_elem *semaphore_a = list_entry(a, struct semaphore_elem, elem);
   struct semaphore_elem *semaphore_b = list_entry(b, struct semaphore_elem, elem);

   // priority 값을 비교하여 a가 더 크면 true (내림차순 정렬)
   return semaphore_a->priority > semaphore_b->priority;
}

/*
 * remove_with_lock
 * 주어진 donation_list(보통 lock 보유자의 donation_list)에서
 * 특정 lock을 기다리며 나에게 우선순위를 기부하던 thread들을 나의 donation_list에서 제거 하는 함수.
 * lock을 풀 때 donation을 정리하는 단계
 */
void remove_with_lock(struct lock *lock, struct list *donation_list)
{

   struct list_elem *cur = list_begin(donation_list);

   while (cur != list_end(donation_list))
   {

      struct thread *t = list_entry(cur, struct thread, donation_elem);
      struct list_elem *next = list_next(cur);

      if (t->waiting_lock == lock)
      {
         cur = list_remove(cur); // remove한 다음 업데이트 (list_remove는 remove한 다음 요소 반환)
      }
      else
      {
         cur = next;
      }
   }
}

/*
 * refresh_priority
 * thread의 우선순위가 변경되었을 때, donation을 고려하여 우선순위르 다시 결정하는 함수
 */
void refresh_priority(struct thread *t)
{
   // 스레드 t의 priority를 기본값으로 원상복구 한다.
   t->priority = t->original_priority;
   // donation_list가 비어있지 않다면
   if (!list_empty(&t->donation_list))
   {

      // donation_list는 각 원소가 thread.donation_elem이므로 해당 비교자를 사용한다.
      list_sort(&t->donation_list, thread_cmp_priority_donation, NULL);

      // donation_list의 최댓값(기부 받은 가장 높은 priority)과 비교하여 더 큰 값을 현재 priority로 반영
      struct thread *max_t = list_entry(list_front(&t->donation_list), struct thread, donation_elem);

      if (t->priority < max_t->priority)
      {
         t->priority = max_t->priority;
      }
   }
}
