#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdbool.h>
#include <debug.h>
#include <stddef.h>

struct file;

/* Process identifier. */
typedef int pid_t;
#define PID_ERROR ((pid_t) - 1)

struct intr_frame;

void syscall_init(void);

void syscall_halt(void);
void syscall_exit(int status);
int syscall_exec(const char *cmd_line);
int syscall_wait(int pid);
pid_t syscall_fork(const char *name, struct intr_frame *f);
bool syscall_create(const char *file, unsigned initial_size);
bool syscall_remove(const char *file);
int syscall_open(const char *file_name);
int syscall_filesize(int fd);
int syscall_read(int fd, void *buffer, unsigned size);
int syscall_write(int fd, const void *buffer, unsigned size);
void syscall_seek(int fd, unsigned position);
unsigned syscall_tell(int fd);
void syscall_close(int fd);

int syscall_dup2(int oldfd, int newfd);    // dup2 시스템 콜을 처리하는 커널 측 핸들러 선언
struct file *syscall_get_std_file(int fd); // STD 스트림 더미 파일 객체를 돌려주는 헬퍼 선언

#endif /* userprog/syscall.h */
