/*
 *  i386 emulator main execution loop
 *
 *  Copyright (c) 2003-2005 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "cpu.h"
#include "tcg.h"

#ifdef TARGET_SPARC
#include "arch/sparc/arch_callbacks.h"
#endif

target_ulong virt_to_phys(target_ulong virt) {
        #define MASK2 (0xFFFFFFFF - MASK1)
        #define MASK1 (0xFFFFFFFF >> (32-TARGET_PAGE_BITS))
        target_ulong phys_addr = 0xFFFFFFFF;
        int index = (virt >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
        int i;
        for (i = 0; i < NB_MMU_MODES; i++) {
          target_ulong addr = (cpu->tlb_table[i][index].addr_code & MASK2) | (virt & (MASK1));
          if ((virt == addr)) {
            void *p = (void *)(unsigned long)((cpu->tlb_table[i][index].addr_code & TARGET_PAGE_MASK) + cpu->tlb_table[i][index].addend);
            phys_addr = tlib_host_ptr_to_guest_offset(p);
            if (phys_addr != 0xFFFFFFFF)
              phys_addr += (virt & MASK1);
            break;
          }
        }
        return phys_addr;
}

int tb_invalidated_flag;

void cpu_loop_exit(CPUState *env)
{
    env->current_tb = NULL;
    longjmp(env->jmp_env, 1);
}

static TranslationBlock *tb_find_slow(CPUState *env,
                                      target_ulong pc,
                                      target_ulong cs_base,
                                      uint64_t flags)
{
    tlib_on_translation_block_find_slow(pc);
    TranslationBlock *tb, **ptb1;
    unsigned int h;
    tb_page_addr_t phys_pc, phys_page1;
    target_ulong virt_page2;

    tb_invalidated_flag = 0;

    /* find translated block using physical mappings */
    phys_pc = get_page_addr_code(env, pc);
    phys_page1 = phys_pc & TARGET_PAGE_MASK;
    h = tb_phys_hash_func(phys_pc);
    ptb1 = &tb_phys_hash[h];
    for(;;) {
        tb = *ptb1;
        if (!tb)
            goto not_found;
        if (tb->pc == pc &&
            tb->page_addr[0] == phys_page1 &&
            tb->cs_base == cs_base &&
            tb->flags == flags) {
            /* check next page if needed */
            if (tb->page_addr[1] != -1) {
                tb_page_addr_t phys_page2;

		virt_page2 = (pc & TARGET_PAGE_MASK) +
                    TARGET_PAGE_SIZE;
		phys_page2 = get_page_addr_code(env, virt_page2);
                if (tb->page_addr[1] == phys_page2)
                    goto found;
            } else {
                goto found;
            }
        }
        ptb1 = &tb->phys_hash_next;
    }
 not_found:
   /* if no translated code available, then translate it now */
    tb = tb_gen_code(env, pc, cs_base, flags, 0);

 found:
    /* Move the last found TB to the head of the list */
    if (likely(*ptb1)) {
        *ptb1 = tb->phys_hash_next;
        tb->phys_hash_next = tb_phys_hash[h];
        tb_phys_hash[h] = tb;
    }
    /* we add the TB in the virtual pc hash table */
    env->tb_jmp_cache[tb_jmp_cache_hash_func(pc)] = tb;


    return tb;
}

static inline TranslationBlock *tb_find_fast(CPUState *env)
{
    TranslationBlock *tb;
    target_ulong cs_base, pc;
    int flags;

    /* we record a subset of the CPU state. It will
       always be the same before a given translated block
       is executed. */
    cpu_get_tb_cpu_state(env, &pc, &cs_base, &flags);
    tb = env->tb_jmp_cache[tb_jmp_cache_hash_func(pc)];
    if (unlikely(!tb || tb->pc != pc || tb->cs_base != cs_base ||
                 tb->flags != flags)) {
        tb = tb_find_slow(env, pc, cs_base, flags);
    }
    return tb;
}

static CPUDebugExcpHandler *debug_excp_handler;

CPUDebugExcpHandler *cpu_set_debug_excp_handler(CPUDebugExcpHandler *handler)
{
    CPUDebugExcpHandler *old_handler = debug_excp_handler;

    debug_excp_handler = handler;
    return old_handler;
}

/* main execution loop */

int __attribute__((weak)) process_interrupt(int interrupt_request, CPUState *env)
{
#if defined(TARGET_I386)
    if (interrupt_request & CPU_INTERRUPT_INIT) {
        svm_check_intercept(env, SVM_EXIT_INIT);
        do_cpu_init(env);
        env->exception_index = EXCP_HALTED;
        cpu_loop_exit(env);
    } else if (interrupt_request & CPU_INTERRUPT_SIPI) {
        do_cpu_sipi(env);
    } else if (env->hflags2 & HF2_GIF_MASK) {
        if ((interrupt_request & CPU_INTERRUPT_SMI) &&
                !(env->hflags & HF_SMM_MASK)) {
            svm_check_intercept(env, SVM_EXIT_SMI);
            env->interrupt_request &= ~CPU_INTERRUPT_SMI;
            do_smm_enter(env);
            return 1;
        } else if ((interrupt_request & CPU_INTERRUPT_NMI) &&
                !(env->hflags2 & HF2_NMI_MASK)) {
            env->interrupt_request &= ~CPU_INTERRUPT_NMI;
            env->hflags2 |= HF2_NMI_MASK;
            do_interrupt_x86_hardirq(env, EXCP02_NMI, 1);
            return 1;
        } else if (interrupt_request & CPU_INTERRUPT_MCE) {
            env->interrupt_request &= ~CPU_INTERRUPT_MCE;
            do_interrupt_x86_hardirq(env, EXCP12_MCHK, 0);
            return 1;
        } else if ((interrupt_request & CPU_INTERRUPT_HARD) &&
                (((env->hflags2 & HF2_VINTR_MASK) &&
                  (env->hflags2 & HF2_HIF_MASK)) ||
                 (!(env->hflags2 & HF2_VINTR_MASK) &&
                  (env->eflags & IF_MASK &&
                   !(env->hflags & HF_INHIBIT_IRQ_MASK))))) {
            int intno;
            svm_check_intercept(env, SVM_EXIT_INTR);
            env->interrupt_request &= ~(CPU_INTERRUPT_HARD | CPU_INTERRUPT_VIRQ);
            intno = cpu_get_pic_interrupt(env);
            do_interrupt_x86_hardirq(env, intno, 1);
            /* ensure that no TB jump will be modified as
               the program flow was changed */
            return 1;
        } else if ((interrupt_request & CPU_INTERRUPT_VIRQ) &&
                (env->eflags & IF_MASK) &&
                !(env->hflags & HF_INHIBIT_IRQ_MASK)) {
            int intno;
            /* FIXME: this should respect TPR */
            svm_check_intercept(env, SVM_EXIT_VINTR);
            intno = ldl_phys(env->vm_vmcb + offsetof(struct vmcb, control.int_vector));
            do_interrupt_x86_hardirq(env, intno, 1);
            env->interrupt_request &= ~CPU_INTERRUPT_VIRQ;
            return 1;
        }
    }
#elif defined(TARGET_PPC)
    if (interrupt_request & CPU_INTERRUPT_HARD) {
        ppc_hw_interrupt(env);
        if (env->pending_interrupts == 0)
            env->interrupt_request &= ~CPU_INTERRUPT_HARD;
        return 1;
    }
#elif defined(TARGET_SPARC)
    if (interrupt_request & CPU_INTERRUPT_HARD) {
        if ( cpu_interrupts_enabled(env) )
        {
            env->interrupt_index = tlib_find_best_interrupt();
            if(env->interrupt_index > 0) {
                int pil = env->interrupt_index & 0xf;
                int type = env->interrupt_index & 0xf0;

                if (((type == TT_EXTINT) &&
                            cpu_pil_allowed(env, pil)) ||
                        type != TT_EXTINT) {
                    env->exception_index = env->interrupt_index;
                    do_interrupt(env);
                    return 1;
                }
            }
        }
    } else if ((interrupt_request & CPU_INTERRUPT_RESET)) {
        cpu_reset(env);
    } else if ((interrupt_request & CPU_INTERRUPT_RUN)) {
        /* SMP systems only, start after reset */
        cpu_reset(env);
    }
#endif
    return 0;
}

void __attribute__((weak)) cpu_exec_prologue(CPUState *env) { }
void __attribute__((weak)) cpu_exec_epilogue(CPUState *env) { }

// it looks like cpu_exec is aware of possible problems and restores `env`, so the warning is not necessary
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclobbered"
int cpu_exec(CPUState *env)
{
    int ret, interrupt_request;
    TranslationBlock *tb;
    uint8_t *tc_ptr;
    unsigned long next_tb;

    if (env->wfi) {
        if (!cpu_has_work(env)) {
            return EXCP_HALTED;
        }

        env->wfi = 0;
    }

    cpu_exec_prologue(env);
    env->exception_index = -1;

    /* prepare setjmp context for exception handling */
    for(;;) {
        if (setjmp(env->jmp_env) == 0) {
            /* if an exception is pending, we execute it here */
            if (env->exception_index >= 0) {
                if (env->exception_index >= EXCP_INTERRUPT) {
                    /* exit request from the cpu execution loop */
                    ret = env->exception_index;
                    if ((ret == EXCP_DEBUG) && debug_excp_handler) {
                        debug_excp_handler(env);
                    }
                    break;
                } else {
                    do_interrupt(env);
                    if(env->exception_index != -1) {
                        if (env->exception_index == EXCP_WFI) {
                            env->exception_index = -1;
                            ret = 0;
                            break;
                        }
                        env->exception_index = -1;
                    }
                }
            }

            next_tb = 0; /* force lookup of first TB */
            for(;;) {
                interrupt_request = env->interrupt_request;
                if (unlikely(interrupt_request)) {
                    if (unlikely(env->singlestep_enabled & SSTEP_NOIRQ)) {
                        /* Mask out external interrupts for this step. */
                        interrupt_request &= ~CPU_INTERRUPT_SSTEP_MASK;
                    }
                    if (interrupt_request & CPU_INTERRUPT_DEBUG) {
                        env->interrupt_request &= ~CPU_INTERRUPT_DEBUG;
                        env->exception_index = EXCP_DEBUG;
                        cpu_loop_exit(env);
                    }
                    if (process_interrupt(interrupt_request, env)) {
                        next_tb = 0;
                    }
                    /* Don't use the cached interrupt_request value,
                       do_interrupt may have updated the EXITTB flag. */
                    if (env->interrupt_request & CPU_INTERRUPT_EXITTB) {
                        env->interrupt_request &= ~CPU_INTERRUPT_EXITTB;
                        /* ensure that no TB jump will be modified as
                           the program flow was changed */
                        next_tb = 0;
                    }
                }
                if (unlikely(env->exit_request)) {
                    env->exit_request = 0;
                    env->exception_index = EXCP_INTERRUPT;
                    cpu_loop_exit(env);
                }
                tb = tb_find_fast(env);
                /* Note: we do it here to avoid a gcc bug on Mac OS X when
                   doing it in tb_find_slow */
                if (tb_invalidated_flag) {
                    /* as some TB could have been invalidated because
                       of memory exceptions while generating the code, we
                       must recompute the hash index here */
                    next_tb = 0;
                    tb_invalidated_flag = 0;
                }
                /* see if we can patch the calling TB. When the TB
                   spans two pages, we cannot safely do a direct
                   jump. */
                if (next_tb != 0 && tb->page_addr[1] == -1) {
                    tb_add_jump((TranslationBlock *)(next_tb & ~3), next_tb & 3, tb);
                }

                /* cpu_interrupt might be called while translating the
                   TB, but before it is linked into a potentially
                   infinite loop and becomes env->current_tb. Avoid
                   starting execution if there is a pending interrupt. */
                env->current_tb = tb;
                asm volatile("" ::: "memory");
                if (likely(!env->exit_request)) {
                    tc_ptr = tb->tc_ptr;
                    /* execute the generated code */
                    next_tb = tcg_qemu_tb_exec(env, tc_ptr);
                    if ((next_tb & 3) == 2) {
                        tb = (TranslationBlock *)(long)(next_tb & ~3);
                        /* Restore PC.  */
                        cpu_pc_from_tb(env, tb);
                        env->exception_index = EXCP_INTERRUPT;
                        next_tb = 0;
                        cpu_loop_exit(env);
                    }
                }
                env->current_tb = NULL;
                /* reset soft MMU for next block (it can currently
                   only be set by a memory fault) */
            } /* for(;;) */
        } else {
            /* Reload env after longjmp - the compiler may have smashed all
             * local variables as longjmp is marked 'noreturn'. */
            env = cpu;
        }
    } /* for(;;) */

    cpu_exec_epilogue(env);

    return ret;
}
#pragma GCC diagnostic pop

