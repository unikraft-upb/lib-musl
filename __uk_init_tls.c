// SPDX-License-Identifier: BSD-3-Clause
/*
 * Authors: Dragos Iulian Argint <dragosargint21@gmail.com>
 *
 * Copyright (c) 2022, University Politehnica of Bucharest. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <uk/tcb_impl.h>
#include <elf.h>
#include <limits.h>
#include <sys/mman.h>
#include <string.h>
#include <stddef.h>
#include "pthread_impl.h"
#include "libc.h"
#include "atomic.h"
#include "syscall.h"

#include <uk/alloc.h>
#include <uk/assert.h>

/*
 * This glue code source is meant to replace `__libc_start_main()`
 * and tls related stuff.
 * If the pthread API is used, musl will allocate the TLS and will
 * provide a pointer to it in the `clone()` syscall. Musl will allocate
 * in fact a contiguous region which will have the following layout (Please
 * refer to the `pthread_create.c` file):
 * map -----------------------------------------------------------------
 *                     ^
 *                     |
 *                     | 4096      GUARD PAGE
 *                     |
 *                     v
 * map + guard ---------------------------------------------------------
 *                     ^
 *                     |
 *                     |
 *                     |           STACK
 *                     |
 *                     |
 *                     |
 *                     v
 * stack ---------------------------------------------------------------
 *                     ^           ^
 *                     |           |       TLS SPACE
 *                     |           v
 *               new ->|------------------------------------------------
 *                     |           ^
 *                     |           |
 *                     |           |
 *                     |           | 280   pthread structure
 *                     |           |
 *                     v           v
 * tsd -----------------------------------------------------------------
 *                     ^
 *                     | 1024
 *                     v
 * map + size ----------------------------------------------------------
 */


void *__uk_copy_tls(unsigned char *mem)
{
	pthread_t td;
	void *tls_area;

	mem -= (uintptr_t)mem & (libc.tls_align-1);
	tls_area = mem;
	ukarch_tls_area_init(tls_area);

	td = (pthread_t) ukarch_tls_tcb_get(ukarch_tls_tlsp(tls_area));
	td->dtv = td->dtv_copy = tls_area;

	return td;
}

static int __uk_init_tp(void *p)
{
	pthread_t td = p;

	td->self = td;
	/*
	 * Set the `$fs` register for the current thread.
	 * In the original code of musl this will use an `arch_prtcl`
	 * syscall to fill the `$fs` register.
	 */
	ukplat_tlsp_set(TP_ADJ(p));
	libc.can_do_threads = 1;
	/*
	 * The original musl code will invoke here a `SYS_set_tid_address`
	 * syscall, to set the tid user space address in the Kernel.
	 * FIXME: Currently this does not return the tid assigned for the caller,
	 * it returns an error code (-95) because probably there is no tid assigned
	 * at this stage. It is not a really big problem right now.
	 */
	td->tid = uk_syscall_r_set_tid_address(&td->tid);
	td->locale = &libc.global_locale;
	td->robust_list.head = &td->robust_list.head;
	return 0;
}

static void __uk_init_tls(void *tls_area)
{
	libc.tls_size = ukarch_tls_area_size();
	libc.tls_align = ukarch_tls_area_align();

	/* Failure to initialize thread pointer is always fatal. */
	if (__uk_init_tp(tls_area) < 0)
		UK_CRASH("Failed to initialize the main thread\n");
}

static void __uk_init_libc(void)
{
	libc.auxv = 0;
	__hwcap = 0;
	__sysinfo = 0;
	libc.page_size = __PAGE_SIZE;
}

/*
 * This callback will only be called for threads that are NOT
 * created with the pthread API
 */
static volatile size_t __uk_tsd_size = sizeof(void *) * PTHREAD_KEYS_MAX;

int uk_thread_uktcb_init(struct uk_thread *thread, void *tcb)
{
	struct pthread *td = (struct pthread *) tcb;

	uk_pr_debug("uk_thread_uktcb_init uk_thread %p, tcb %p\n", thread, tcb);
	/*
	 * The first thread will fill the `libc` global variable.
	 * This includes `tls_size`, `tls_align`
	 */
	if (!libc.can_do_threads) {
		__uk_init_tls(tcb);
		return 0;
	}

	td->stack = thread->_mem.stack;
	td->stack_size = __STACK_SIZE;
	td->self = td;
	td->tsd = (void *)uk_memalign(
		uk_alloc_get_default(),
		__PAGE_SIZE,
		__uk_tsd_size);
	td->locale = &libc.global_locale;

	return 0;
}

void uk_thread_tcb_fini(struct uk_thread *thread, void *tcb)
{
	struct pthread *td = (struct pthread *) tcb;

	uk_pr_debug("uk_thread_tcb_fini uk_thread %p, tcb %p\n", thread, tcb);
	uk_free(uk_alloc_get_default(), td->tsd);
}

/*
 * This callback is called for every thread, but we only use it
 * to initialize some things in libc.
 */
void ukarch_tls_tcb_init(void *tls_area)
{
	uk_pr_debug("ukarch_tls_tcb_init tls_area %p\n", tls_area);
	if (!libc.can_do_threads)
		__uk_init_libc();
}
