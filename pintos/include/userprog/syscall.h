#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#include "filesys/file.h"
#include <list.h>
void syscall_init(void);
struct file_descriptor
{
    int fd;
    struct file *file;
    struct list_elem elem;
};
#endif /* userprog/syscall.h */
