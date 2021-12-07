#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include <mips/trapframe.h>

#include "opt-A2.h"

/* this implementation of sys__exit does not do anything with the exit code */
/* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode)
{

  struct addrspace *as;
  struct proc *p = curproc;

  DEBUG(DB_SYSCALL, "Syscall: _exit(%d)\n", exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

  spinlock_acquire(&p->p_lock);

  int num_children = array_num(p->children);

  for (int i = num_children - 1; i >= 0; i--)
  {
    struct proc *child = array_get(p->children, i);

    if (child->state == RUNNING)
    {
      child->state = ORPHAN;
    }
    else if (child->state == ZOMBIE)
    {
      proc_destroy(child);
    }
    array_remove(p->children, i);
  }

  spinlock_release(&p->p_lock);

  if (p->state == RUNNING)
  {
    p->state = ZOMBIE;
    p->exitcode = exitcode;
    cv_signal(p->p_cv, p->p_wait_lock);
    if (p->pid == 0)
    {
      proc_destroy(p);
    }
  }
  else if (p->state == ORPHAN)
  {
    remove_pid(curproc);

    /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
    proc_destroy(p);
  }

  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}

/* stub handler for getpid() system call                */
int sys_getpid(pid_t *retval)
{
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
  *retval = curproc->pid;
  return (0);
}

/* stub handler for waitpid() system call                */

int sys_waitpid(pid_t pid,
                userptr_t status,
                int options,
                pid_t *retval)
{
  int exitstatus;
  int result;

  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0)
  {
    return (EINVAL);
  }

  bool ischild = false;
  struct proc *child;
  for (int i = 0; i < (int)array_num(curproc->children); i++)
  {
    if (((struct proc *)array_get(curproc->children, i))->pid == pid)
    {
      ischild = true;
      child = pid_table->arr[pid];
      break;
    }
  }

  if (!ischild)
  {
    return (ECHILD);
  }

  lock_acquire(child->p_wait_lock);
  while (child->state != ZOMBIE)
  {
    cv_wait(child->p_cv, child->p_wait_lock);
  }
  exitstatus = child->exitcode;
  lock_release(child->p_wait_lock);

  exitstatus = _MKWAIT_EXIT(exitstatus);

  result = copyout((void *)&exitstatus, status, sizeof(int));
  if (result)
  {
    return (result);
  }

  *retval = pid;
  return 0;
}

int sys_fork(struct trapframe *tf, pid_t *retval)
{
  struct proc *child = proc_create_runprogram("childfromfork");
  if (child == NULL)
  {
    return (ENOMEM);
  }

  struct addrspace *adds = as_create();
  int err = as_copy(curproc->p_addrspace, &adds);
  if (err)
  {
    return (err);
  }
  spinlock_acquire(&child->p_lock);
  child->p_addrspace = adds;
  spinlock_release(&child->p_lock);

  pid_t pid = get_pid(child);

  if (pid == 0)
  {
    return (EMPROC);
  }

  spinlock_acquire(&child->p_lock);
  child->pid = pid;
  child->parent_pid = curproc->pid;
  spinlock_release(&child->p_lock);

  spinlock_acquire(&curproc->p_lock);
  array_add(curproc->children, child, NULL);
  spinlock_release(&curproc->p_lock);

  struct trapframe *tf_p = kmalloc(sizeof(struct trapframe));
  memcpy(tf_p, tf, sizeof(struct trapframe));

  *retval = pid;

  int thread_err = thread_fork("thread_fork", child, enter_forked_process, tf_p, 0);

  if (thread_err)
  {
    return thread_err;
  }

  return 0;
}
