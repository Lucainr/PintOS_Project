#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

tid_t process_create_initd(const char *file_name);
tid_t process_fork(const char *name, struct intr_frame *if_);
int process_exec(void *f_name);
int process_wait(tid_t);
void process_exit(void);
void process_activate(struct thread *next);
struct thread *find_child(struct list *list, tid_t tid);
/* fork 전용 구조체 */
struct fork_aux
{
    struct intr_frame p_if;
    struct thread *parent;
    struct semaphore done;
    bool success;
};
#endif /* userprog/process.h */
