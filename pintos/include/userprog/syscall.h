#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#include "filesys/file.h"
#include <list.h>
void syscall_init(void);
bool syscall_create(const char *name, off_t initial_size);
int open(const char *name);
void close(int fd);
off_t read(int fd, void *buffer, off_t size);
int filesize(int fd);
off_t write(int fd, const void *buffer, off_t size);
int syscall_wait(int tid);
bool syscall_remove(char *filename);
void syscall_exit(int status);
int syscall_exec(char *filename);
unsigned syscall_tell(int fd);
bool syscall_seek(int fd, off_t pos);

struct file_descriptor
{
    int fd;
    struct file *file;
    struct list_elem elem;
};
#endif /* userprog/syscall.h */
