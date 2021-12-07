/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Sample/test code for running a user program.  You can use this for
 * reference when implementing the execv() system call. Remember though
 * that execv() needs to do more than this function does.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <lib.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vm.h>
#include <vfs.h>
#include <syscall.h>
#include <copyinout.h>
#include <test.h>
#include "opt-A2.h"

#if OPT_A2
int as_define_stack_args(struct addrspace *as, vaddr_t *stackptr, char **args, unsigned long nargs)
{
	KASSERT(as->as_stackpbase != 0);

	int err = 0;

	int *args_count = kmalloc((nargs) * sizeof(int));

	for (int i = 0; i < (int)nargs; i++)
	{
		args_count[i] = strlen(args[i]) + 1;
	}

	*stackptr = USERSTACK;

	vaddr_t *arr_arg = kmalloc((nargs) * sizeof(vaddr_t));

	for (int i = nargs - 1; i >= 0; i--)
	{
		*stackptr -= args_count[i];
		arr_arg[i] = *stackptr;
	}

	vaddr_t top = ROUNDUP(*stackptr - (nargs + 1) * sizeof(vaddr_t), 8) - 8;
	*stackptr = top;

	//kprintf("0x%08x\n", top);

	err = copyout((void *)arr_arg, (userptr_t)top, sizeof(vaddr_t) * nargs);
	err = copyout(NULL, (userptr_t)top + sizeof(vaddr_t) * nargs, sizeof(NULL));

	top = arr_arg[0];

	//kprintf("0x%08x\n", arr_arg[i]);

	size_t *got = kmalloc(sizeof(int));

	for (int i = 0; i < (int)nargs; i++)
	{
		//kprintf(args[i]);
		//kprintf("\n");
		err = copyoutstr(args[i], (userptr_t) top, (size_t)args_count[i], got);

		top += args_count[i];
	}

	return 0;
}
#else
#endif

/*
 * Load program "progname" and start running it in usermode.
 * Does not return except on error.
 *
 * Calls vfs_open on progname and thus may destroy it.
 */
int runprogram(char *progname, char **args, unsigned long nargs)
{
	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;

	/* Open the file. */
	result = vfs_open(progname, O_RDONLY, 0, &v);
	if (result)
	{
		return result;
	}

	/* We should be a new process. */
	KASSERT(curproc_getas() == NULL);

	/* Create a new address space. */
	as = as_create();
	if (as == NULL)
	{
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	curproc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result)
	{
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack_args(as, &stackptr, args, nargs);
	if (result)
	{
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}

		/* Warp to user mode. */
		enter_new_process(nargs /*argc*/, (userptr_t) stackptr /*userspace addr of argv*/,
						  stackptr, entrypoint);

	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}

int sys_execv(const char *program, char **args)
{
	if (program == NULL || args == NULL)
	{
		return EFAULT;
	}

	char *programname;
	programname = kmalloc(128 * sizeof(char));
	int err;
	int *programname_len = kmalloc(sizeof(int));
	err = copyinstr((const_userptr_t)program, programname, 128 * sizeof(char), (size_t *)programname_len);
	if (err)
	{
		return err;
	}

	int arg_c = 0;
	char *next;
	err = copyin((const_userptr_t)&args[arg_c], &next, sizeof(char *));

	while (next)
	{
		err = copyin((const_userptr_t)&args[arg_c], &next, sizeof(char *));
		arg_c++;
	}
	arg_c--;
	
	char **args_k = kmalloc((arg_c) * sizeof(char *));
	/*
	char *programname_copy;
	programname_copy = kmalloc(128 * sizeof(char));

	strcpy(programname_copy, programname);

	args_k[0] = programname_copy;
	*/

	int *eachargsize = kmalloc(sizeof(int));

	for (int i = 0; i < arg_c; i++)
	{
		char *arg = kmalloc(128 * sizeof(char));
		err = copyinstr((const_userptr_t) args[i], arg, 128 * sizeof(char), (size_t *)eachargsize);
		args_k[i] = arg;
	}
	/*
	for (int i = 0; i < arg_c; i++){
		kprintf(args_k[i]);
		kprintf("\n");
	}
*/

	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;

	/* Open the file. */

	result = vfs_open(programname, O_RDONLY, 0, &v);
	if (result)
	{
		return result;
	}

	/* Create a new address space. */
	as = as_create();
	if (as == NULL)
	{
		vfs_close(v);
		return ENOMEM;
	}

	struct addrspace *old_as = curproc_getas();

	as_destroy(old_as);

	/* Switch to it and activate it. */
	curproc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result)
	{
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack_args(as, &stackptr, args_k, (unsigned long) arg_c);

	if (result)
	{
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}

	kfree(programname);
	kfree(programname_len);
	for(int i=0; i < arg_c; i++){
		kfree(args_k[i]);
	}

	/* Warp to user mode. */
	enter_new_process(arg_c /*argc*/, (userptr_t) stackptr/*userspace addr of argv*/,
					  stackptr, entrypoint);

	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
};
