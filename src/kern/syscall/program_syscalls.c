#include <types.h>
#include <kern/errno.h>
#include <kern/syscall.h>
#include <lib.h>
#include <mips/trapframe.h>
#include <thread.h>
#include <current.h>
#include <syscall.h>
#include <synch.h>
#include <program_syscalls.h>
#include <proc.h>
#include <vfs.h>
#include <filetable.h>
#include <openfile.h>
#include <limits.h>
#include <copyinout.h>
#include <uio.h>
#include <kern/iovec.h>
#include <stat.h>
#include <kern/fcntl.h>
#include <endian.h>
#include <kern/seek.h>
#include <kern/unistd.h>
#include <addrspace.h>
#include <kern/wait.h>


/*
 * this helper function helps connect this file to the syscall.c file, to the function enter_forked_process
 */
static
void enter_forked_process_api(void *ptr, unsigned long nargs)
{
    (void) nargs;
    enter_forked_process((struct trapframe *) ptr);
}


int fork(struct trapframe *tf, int *retval) {
    int result;
    // create new process w same name as current process
    struct proc *child;
    child = proc_create_runprogram("child_process");
    if (child == NULL) {
        return ENPROC;
    }

    //add this new created process to the array containing childs created by the parent process
    array_add(curproc->child_process_array, child, NULL);

    // copy stack
    result = as_copy(curproc->p_addrspace, &child->p_addrspace);
    if (result) {
        proc_destroy(child); // free the process
        return result;
    }

    child->p_filetable = filetable_create();
	if (child->p_filetable == NULL) {
		proc_destroy(child);
		return ENOMEM;
	}

    // copy open file table
    result = filetable_copy(curproc->p_filetable,&child->p_filetable);
    if (result) {
        proc_destroy(child);
        return result;
    }

    /* create a trapframe used to store registers */
    struct trapframe *child_trapframe = kmalloc(sizeof(struct trapframe));
    if (child_trapframe == NULL) {
        kfree(child_trapframe);
        proc_destroy(child);
        return ENOMEM;
    }

    /*the trap frame for the child holding parents data temporarily*/
    memcpy(child_trapframe, tf, sizeof(struct trapframe));

    /*using the thread fork function to create new thread*/
    result = thread_fork("child proc",
                        child,
                        enter_forked_process_api,
                        child_trapframe,
                        0);
    if (result) {
        proc_destroy(child);
        return -1;
    }

    // return pid of child process
    *retval = child->pid;

    return 0;
}


/*
 * helper function for freeing the kernel argument buffer
 */
static void
kernel_args_free(char **buffer, int size)
{
    for (int i = 0; i < size; i++) {
        kfree(buffer[i]);
    }
    kfree(buffer);
}

int execv(const char *program, char **args)
{
    int args_len = 0, result = 0, buffer_size = 0;
    char ** args_ptr = kmalloc(sizeof (char*));
   

    // copyin the args array to ensure it is a valid pointer
    result = copyin((userptr_t) args, args_ptr, sizeof(char**));
    if (result) {
        kfree(args_ptr);
        return result;
    }
    // loop through the args array to get the number of arguments (args_len)
    while(args_len <= ARG_MAX){
        if(args[args_len] == NULL){
            break;
        }
        args_len++;
    }

    if (args_len > ARG_MAX) {
        kfree(args_ptr);
        return E2BIG;
    }

    //
    char *str_safe_check = kmalloc(sizeof(char) * ARG_MAX);
    if (str_safe_check == NULL) {
        kfree(args_ptr);
        
        return ENOMEM;
    }

    // create buffer for copying argument strings
    char **kernel_args = kmalloc(sizeof(char*) * args_len);
    if (kernel_args == NULL) {
        kfree(args_ptr);
        kfree(str_safe_check);
        return ENOMEM;
    }

    for (int i = 0; i < args_len; i++) {
        
        //make sure the pointer is legit
        result = copyin((userptr_t) args + sizeof (char *) * i , args_ptr, sizeof (char*));
        if (result) {
            kfree(args_ptr);
            kfree(kernel_args);
            return result;
        }
        
      
        // ensure it is valid the string is valid
        result = copyinstr((userptr_t) args[i], str_safe_check,  sizeof (char) * ARG_MAX, NULL);
        if (result) {
            kfree(args_ptr);
            kfree(kernel_args);
            kfree(str_safe_check);
            return result;
        }
        
        int arglen = strlen(args[i]) + 1; // including the null in the string allocation

        // allocate on for the kernel area
        kernel_args[i] = kmalloc(sizeof (char) * arglen);
        if (kernel_args[i] == NULL) {
            kfree(args_ptr);
            kernel_args_free(kernel_args, i-1);
            kfree(str_safe_check);
            return ENOMEM;
        }
        
        // copy each argument into kernel
        result = copyinstr((userptr_t) args[i], kernel_args[i], sizeof (char) * arglen,
            NULL);
        if (result) {
            kfree(args_ptr);
            kernel_args_free(kernel_args, i);
            kfree(str_safe_check);
            return result;
        }
        
        // increased the kernel buffer and round up if necessary 
        if (arglen % 4 != 0) {
            buffer_size += ROUNDUP(arglen,4);
        }
        else{
            buffer_size += arglen;
        }
        
    }

    kfree(str_safe_check);
    kfree(args_ptr);

    // copyin the value of program name 
    char *program_name = kmalloc(PATH_MAX);
    if (program_name == NULL) {
        kernel_args_free(kernel_args, args_len);
        return ENOMEM;
    }
    // copy in the pointer of program name
    result = copyinstr((userptr_t) program, program_name, PATH_MAX, NULL);
    if (result) {
        kernel_args_free(kernel_args, args_len);
        kfree(program_name);
        return result;
    }


    /*
    run program content
    */
    // open program executable for use by load_elf
    struct vnode *v;
    vaddr_t entrypoint, stackptr;

    result = vfs_open(program_name, O_RDONLY, 0, &v);
    if (result) {
        kernel_args_free(kernel_args, args_len);
        kfree(program_name);
        return result;
    }
    kfree(program_name);

    // create new address space 
    struct addrspace *newaddress_space = as_create();
    if (newaddress_space == NULL) {
        kernel_args_free(kernel_args, args_len);
        return ENOMEM;
    }
    // set address space to the new one
    struct addrspace *oldaddress_space = proc_setas(newaddress_space);
    as_activate();    

    // load executable file
    result = load_elf(v, &entrypoint);
    if (result) {
        kernel_args_free(kernel_args, args_len);
        
        proc_setas(oldaddress_space);
        as_activate();
        as_destroy(newaddress_space);
        return result;
    }

     
    result = as_define_stack(newaddress_space, &stackptr);
    if (result) {
        kernel_args_free(kernel_args, args_len);
       
        proc_setas(oldaddress_space);
        as_activate();
        as_destroy(newaddress_space);
        return result;
    }


    /*wrap to user mode */

    // create array for storing argument locations in user stack
    char **user_args = kmalloc(sizeof(char *) * (args_len + 1));
    if (user_args == NULL) {
        kernel_args_free(kernel_args, args_len);
         
        proc_setas(oldaddress_space);
        as_activate();
        as_destroy(newaddress_space);
        return ENOMEM;
    }

    // decrement stack pointer so that we can store arguments on top of the stack
    stackptr -= buffer_size;
    for (int i = 0; i < args_len; i++) {
        // copy argument string onto user stack from kernel buffer
        int arglen = strlen(kernel_args[i]) + 1;
        result = copyoutstr(kernel_args[i], (userptr_t) stackptr, arglen, NULL);
        if (result) {
            kernel_args_free(kernel_args, args_len);
            proc_setas(oldaddress_space);
            as_activate();
            as_destroy(newaddress_space);
            kfree(user_args);
            return result;
        }
        
        // store address of current argument
        user_args[i] = (char *) stackptr;       
        if (arglen % 4 != 0) {
            
            stackptr += ROUNDUP(arglen,4);
        }
        else{
            stackptr += arglen;
        }
    }

    // string ends with null pointer
    user_args[args_len] = 0;
    kernel_args_free(kernel_args, args_len);// free the pointers that used for copy in

    // stack grows downward so that we subtract somespace for arguments to be sotred
    stackptr -= (buffer_size + (args_len + 1) * (sizeof(char *)));

    for (int i = 0; i <= args_len ; i++) {
        result = copyout(user_args + i, (userptr_t) stackptr, sizeof(char *));
        if (result) {
             
            proc_setas(oldaddress_space);
            as_activate();
            as_destroy(newaddress_space);
            kfree(user_args);
            return result;
        }
        stackptr += sizeof(char *);
    }

    // set the stack pointer to the new top of the stack
    kfree(user_args);
    stackptr -= (args_len + 1) * (sizeof(char *));
    as_destroy(oldaddress_space);
    enter_new_process(args_len, (userptr_t) stackptr, NULL, stackptr, entrypoint);
    return 0;
}


int getpid(int *retval)
{
    *retval = curproc->pid; 
    return 0;
}


int waitpid(int pid, userptr_t status, int options, int *retval)
{
    if (pid < PID_MIN || pid >= PID_MAX) {
		return ESRCH;
	}	
    if(pid_array[pid] == NULL){
        return ESRCH;
    }
    /* options must be 0 */
    if (options != 0) {
        return EINVAL;
    }
    // check that we are parent of child proc pointed to by pid
    if (pid_array[pid]->parent_pid != curproc->pid) {
        return ECHILD;
    }
    int result, user_status;
   
    if ( get_pid_exitFlag(pid) == 1) { // status is 1, return immediately, child already exit
       
        if (status != NULL) {
            user_status = get_pid_exitStatus(pid);
            result = copyout(&user_status, status, sizeof(int)); 
            if (result) {
                return EFAULT;
            }
        }
        else{
            return EFAULT;
        }
    }
    /* parent waits for child to exit */
    else {
        
        P(get_exitLock(pid));
         // have the parent keep waiting until the child exits
        if (status != NULL) {
            user_status = get_pid_exitStatus(pid);
            result = copyout(&user_status, (userptr_t) status, sizeof(int32_t));
            if (result) {
                return EFAULT;
            }
        }
        //destroy the child data stored in childProcs for this process and destroy process. 
        lock_acquire(curproc->child_process_lock);
        unsigned i ;
        for (  i =0; i <= (curproc->child_process_array->num -1);i--) {
            struct proc *childProc = array_get(curproc->child_process_array,  i);
            if (childProc->pid == pid) {
                array_remove(curproc->child_process_array, i);
                proc_destroy(childProc);
                break;
            }
        }
        lock_release(curproc->child_process_lock);
    }

    *retval = pid; //return pid
    return 0;
}




static void
set_pid_exit(pid_t index, int exit_status, bool exit_flag )
{
	pid_array[(int) index]->exit_status = exit_status;
	pid_array[(int) index]->exit_flag = exit_flag;
}



int _exit(int exitcode)
{
    // children's flag set true as parent is dead
    lock_acquire(curproc->child_process_lock);
    while (curproc->child_process_array->num > 0) {
        struct proc *childProc = array_get(curproc->child_process_array, 0);
        if (get_pid_exitFlag(childProc->pid)!=true) {
            childProc->parent_dead = true;
        } else {
            proc_destroy(childProc);
        }
        array_remove(curproc->child_process_array, 0);
    }

    lock_release(curproc->child_process_lock);
    set_pid_exit(curproc->pid,  exitcode,  true );
    
    V(get_exitLock(curproc->pid)); // stop the parent from waiting
    if (curproc->parent_dead) {
		proc_destroy(curproc);
	}
    thread_exit(); // exit not return 
    return 0;
}