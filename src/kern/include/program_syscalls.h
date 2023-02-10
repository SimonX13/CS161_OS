#ifndef _PROGRAM_SYSCALLS_H_
#define _PROGRAM_SYSCALLS_H_

#include <cdefs.h>
#include <kern/seek.h>
#include <limits.h>
#include <addrspace.h>

/*
 * process syscall functions
 */
int fork(struct trapframe *tf, int *retval);
int execv(const char *program, char **args);
int waitpid(int pid, userptr_t status, int options, int *retval);
int _exit(int exitcode);
int getpid(int *retval);

// helper functions
// void kfree_buf(char **buf, int len);
// void kfree_as(struct addrspace *oldaddrspace, struct addrspace *as);


// helper functions

// void kernel_args_free(char **buf, int len);
// void set_pid_exit(pid_t pidIndex, int exit_status, bool exit_flag );
// bool get_pid_in_table(pid_t pidIndex);
// pid_t get_parent_pid(pid_t pidIndex);

#endif
