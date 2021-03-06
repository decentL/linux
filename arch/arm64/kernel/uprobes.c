/*
 * Copyright (C) 2014-2015 Pratyush Anand <panand@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/highmem.h>
#include <linux/ptrace.h>
#include <linux/uprobes.h>

#include "kprobes-arm64.h"

#define UPROBE_INV_FAULT_CODE	UINT_MAX

void arch_uprobe_copy_ixol(struct page *page, unsigned long vaddr,
		void *src, unsigned long len)
{
	void *xol_page_kaddr = kmap_atomic(page);
	void *dst = xol_page_kaddr + (vaddr & ~PAGE_MASK);

	preempt_disable();

	/* Initialize the slot */
	memcpy(dst, src, len);

	/* flush caches (dcache/icache) */
	flush_uprobe_xol_access(page, vaddr, dst, len);

	preempt_enable();

	kunmap_atomic(xol_page_kaddr);
}

unsigned long uprobe_get_swbp_addr(struct pt_regs *regs)
{
	return instruction_pointer(regs);
}

int arch_uprobe_analyze_insn(struct arch_uprobe *auprobe, struct mm_struct *mm,
		unsigned long addr)
{
	kprobe_opcode_t insn;

	/* TODO: Currently we do not support AARCH32 instruction probing */

	if (!IS_ALIGNED(addr, AARCH64_INSN_SIZE))
		return -EINVAL;

	insn = *(kprobe_opcode_t *)(&auprobe->insn[0]);

	switch (arm_kprobe_decode_insn(insn, &auprobe->ainsn)) {
	case INSN_REJECTED:
		return -EINVAL;

	case INSN_GOOD_NO_SLOT:
		auprobe->simulate = true;
		if (auprobe->ainsn.prepare)
			auprobe->ainsn.prepare(insn, &auprobe->ainsn);
		break;

	case INSN_GOOD:
	default:
		break;
	}

	return 0;
}

int arch_uprobe_pre_xol(struct arch_uprobe *auprobe, struct pt_regs *regs)
{
	struct uprobe_task *utask = current->utask;

	/* saved fault code is restored in post_xol */
	utask->autask.saved_fault_code = current->thread.fault_code;

	/* An invalid fault code between pre/post xol event */
	current->thread.fault_code = UPROBE_INV_FAULT_CODE;

	/* Instruction point to execute ol */
	instruction_pointer_set(regs, utask->xol_vaddr);

	user_enable_single_step(current);

	return 0;
}

int arch_uprobe_post_xol(struct arch_uprobe *auprobe, struct pt_regs *regs)
{
	struct uprobe_task *utask = current->utask;

	WARN_ON_ONCE(current->thread.fault_code != UPROBE_INV_FAULT_CODE);

	/* restore fault code */
	current->thread.fault_code = utask->autask.saved_fault_code;

	/* Instruction point to execute next to breakpoint address */
	instruction_pointer_set(regs, utask->vaddr + 4);

	user_disable_single_step(current);

	return 0;
}
bool arch_uprobe_xol_was_trapped(struct task_struct *t)
{
	/*
	 * Between arch_uprobe_pre_xol and arch_uprobe_post_xol, if an xol
	 * insn itself is trapped, then detect the case with the help of
	 * invalid fault code which is being set in arch_uprobe_pre_xol and
	 * restored in arch_uprobe_post_xol.
	 */
	if (t->thread.fault_code != UPROBE_INV_FAULT_CODE)
		return true;

	return false;
}

bool arch_uprobe_skip_sstep(struct arch_uprobe *auprobe, struct pt_regs *regs)
{
	kprobe_opcode_t insn;
	unsigned long addr;
	struct arch_specific_insn *ainsn;

	if (!auprobe->simulate)
		return false;

	insn = *(kprobe_opcode_t *)(&auprobe->insn[0]);
	addr = instruction_pointer(regs);
	ainsn = &auprobe->ainsn;

	if (ainsn->handler) {
		if (!ainsn->check_condn || ainsn->check_condn(insn, ainsn, regs))
			ainsn->handler(insn, addr, regs);
		else
			instruction_pointer_set(regs, instruction_pointer(regs) + 4);
	}


	return true;
}

void arch_uprobe_abort_xol(struct arch_uprobe *auprobe, struct pt_regs *regs)
{
	struct uprobe_task *utask = current->utask;

	current->thread.fault_code = utask->autask.saved_fault_code;
	/*
	 * Task has received a fatal signal, so reset back to probbed
	 * address.
	 */
	instruction_pointer_set(regs, utask->vaddr);

	user_disable_single_step(current);
}

unsigned long
arch_uretprobe_hijack_return_addr(unsigned long trampoline_vaddr,
				  struct pt_regs *regs)
{
	unsigned long orig_ret_vaddr;

	orig_ret_vaddr = procedure_link_pointer(regs);
	/* Replace the return addr with trampoline addr */
	procedure_link_pointer_set(regs, trampoline_vaddr);

	return orig_ret_vaddr;
}

int arch_uprobe_exception_notify(struct notifier_block *self,
				 unsigned long val, void *data)
{
	return NOTIFY_DONE;
}

static int __kprobes uprobe_breakpoint_handler(struct pt_regs *regs,
		unsigned int esr)
{
	if (user_mode(regs) && uprobe_pre_sstep_notifier(regs))
		return DBG_HOOK_HANDLED;

	return DBG_HOOK_ERROR;
}

static int __kprobes uprobe_single_step_handler(struct pt_regs *regs,
		unsigned int esr)
{
	struct uprobe_task *utask = current->utask;

	if (user_mode(regs)) {
		WARN_ON(utask &&
			(instruction_pointer(regs) != utask->xol_vaddr + 4));

		if (uprobe_post_sstep_notifier(regs))
			return DBG_HOOK_HANDLED;
	}

	return DBG_HOOK_ERROR;
}

/* uprobe breakpoint handler hook */
static struct break_hook uprobes_break_hook = {
	.esr_mask = BRK64_ESR_MASK,
	.esr_val = BRK64_ESR_UPROBES,
	.fn = uprobe_breakpoint_handler,
};

/* uprobe single step handler hook */
static struct step_hook uprobes_step_hook = {
	.fn = uprobe_single_step_handler,
};

static int __init arch_init_uprobes(void)
{
	register_break_hook(&uprobes_break_hook);
	register_step_hook(&uprobes_step_hook);

	return 0;
}

device_initcall(arch_init_uprobes);
