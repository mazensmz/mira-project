#include "Debugger2.hpp"
#include <Utils/Kdlsym.hpp>
#include <Mira.hpp>

extern "C"
{
	#include <sys/uio.h>
	#include <sys/proc.h>
	#include <sys/ioccom.h>
	#include <sys/fcntl.h>

	#include <vm/vm.h>
	#include <vm/pmap.h>
	
	#include <machine/psl.h>
	#include <machine/pmap.h>
	#include <machine/segments.h>
    #include <machine/trap.h>
};

struct amd64_frame 
{
	struct amd64_frame      *f_frame;
	long                    f_retaddr;
	long                    f_arg0;
};

using namespace Mira::Plugins;

void sdtossd(struct user_segment_descriptor *sd, struct soft_segment_descriptor *ssd)
{
	ssd->ssd_base  = (sd->sd_hibase << 24) | sd->sd_lobase;
	ssd->ssd_limit = (sd->sd_hilimit << 16) | sd->sd_lolimit;
	ssd->ssd_type  = sd->sd_type;
	ssd->ssd_dpl   = sd->sd_dpl;
	ssd->ssd_p     = sd->sd_p;
	ssd->ssd_long  = sd->sd_long;
	ssd->ssd_def32 = sd->sd_def32;
	ssd->ssd_gran  = sd->sd_gran;
}

bool Debugger2::IsStackSpace(void* p_Address)
{
    return ((reinterpret_cast<uint64_t>(p_Address) & 0xFFFFFFFF00000000) == 0xFFFFFF8000000000);
}

void Debugger2::OnTrapFatal(struct trapframe* frame, vm_offset_t eva)
{
	auto printf = (void(*)(const char *format, ...))kdlsym(printf);
	auto gdt = (struct user_segment_descriptor*)kdlsym(gdt);/* global descriptor tables */
	auto vm_fault_disable_pagefaults = (int(*)(void))kdlsym(vm_fault_disable_pagefaults);
	auto vm_fault_enable_pagefaults = (void(*)(int))kdlsym(vm_fault_enable_pagefaults);
	auto kthread_exit = (void(*)(void))kdlsym(kthread_exit);
#define MAX_TRAP_MSG		33

	
	WriteKernelFileLog("kernel base: %p\n", gKernelBase);

	// Print extra information in case that Oni/Mira itself crashes
	auto s_Framework = Mira::Framework::GetFramework();
	if (s_Framework)
	{
		auto s_InitParams = s_Framework->GetInitParams();
		if (s_InitParams)
		{
			WriteKernelFileLog("mira base: %p size: %p\n", s_InitParams->payloadBase, s_InitParams->payloadSize);
			WriteKernelFileLog("mira proc: %p entrypoint: %p\n", s_InitParams->process, s_InitParams->entrypoint);
			WriteKernelFileLog("mira mira_entry: %p\n", mira_entry);
		}

		WriteKernelFileLog("mira messageManager: %p pluginManager: %p rpcServer: %p\n", s_Framework->GetMessageManager(), s_Framework->GetPluginManager(), s_Framework->GetRpcServer());
    }

	if (frame)
	{
		WriteKernelFileLog("LastBranchFromOffsetFromKernelBase: %p\n", frame->tf_last_branch_from - (uint64_t)gKernelBase);
		WriteKernelFileLog("RipOffsetFromKernelBase: %p", frame->tf_rip - (uint64_t)gKernelBase);
		WriteKernelFileLog("OffsetFromMiraEntry: [tf_last_branch_from-mira_entry]:%p [mira_entry-tf_last_branch_from]:%p\n", frame->tf_last_branch_from - reinterpret_cast<uint64_t>(mira_entry), reinterpret_cast<uint64_t>(mira_entry) - frame->tf_last_branch_from);
		WriteKernelFileLog("OffsetFromMiraEntryRIP: (%p)", (uint64_t)frame->tf_rip - (uint64_t)mira_entry);
	}

	WriteKernelFileLog("call stack:\n");
	auto s_Saved = vm_fault_disable_pagefaults();
    auto amdFrame = reinterpret_cast<struct amd64_frame*>(frame->tf_rbp);
	if (amdFrame)
	{
		auto amdFrameCount = 0;
		while (Debugger2::IsStackSpace(amdFrame))
		{
			WriteKernelFileLog("[%d] [r: %p] [f:%p]\n", amdFrameCount, amdFrame->f_retaddr, amdFrame);
			amdFrame = amdFrame->f_frame;
			amdFrameCount++;
		}
	}
	vm_fault_enable_pagefaults(s_Saved);

	static const char *trap_msg[] = {
		"",					/*  0 unused */
		"privileged instruction fault",		/*  1 T_PRIVINFLT */
		"",					/*  2 unused */
		"breakpoint instruction fault",		/*  3 T_BPTFLT */
		"",					/*  4 unused */
		"",					/*  5 unused */
		"arithmetic trap",			/*  6 T_ARITHTRAP */
		"",					/*  7 unused */
		"",					/*  8 unused */
		"general protection fault",		/*  9 T_PROTFLT */
		"trace trap",				/* 10 T_TRCTRAP */
		"",					/* 11 unused */
		"page fault",				/* 12 T_PAGEFLT */
		"",					/* 13 unused */
		"alignment fault",			/* 14 T_ALIGNFLT */
		"",					/* 15 unused */
		"",					/* 16 unused */
		"",					/* 17 unused */
		"integer divide fault",			/* 18 T_DIVIDE */
		"non-maskable interrupt trap",		/* 19 T_NMI */
		"overflow trap",			/* 20 T_OFLOW */
		"FPU bounds check fault",		/* 21 T_BOUND */
		"FPU device not available",		/* 22 T_DNA */
		"double fault",				/* 23 T_DOUBLEFLT */
		"FPU operand fetch fault",		/* 24 T_FPOPFLT */
		"invalid TSS fault",			/* 25 T_TSSFLT */
		"segment not present fault",		/* 26 T_SEGNPFLT */
		"stack fault",				/* 27 T_STKFLT */
		"machine check trap",			/* 28 T_MCHK */
		"SIMD floating-point exception",	/* 29 T_XMMFLT */
		"reserved (unknown) fault",		/* 30 T_RESERVED */
		"",					/* 31 unused (reserved) */
		"DTrace pid return trap",		/* 32 T_DTRACE_RET */
		"DTrace fasttrap probe trap",		/* 33 T_DTRACE_PROBE */
	};

	int code, ss;
	u_int type;
	long esp;
	struct soft_segment_descriptor softseg;
	const char *msg;

	code = frame->tf_err;
	type = frame->tf_trapno;
	sdtossd(&gdt[NGDT * PCPU_GET(cpuid) + IDXSEL(frame->tf_cs & 0xffff)],
	    &softseg);

	if (type <= MAX_TRAP_MSG)
		msg = trap_msg[type];
	else
		msg = "UNKNOWN";
	
	WriteKernelFileLog("\n\nFatal trap %d: %s while in %s mode\n", type, msg,
	    ISPL(frame->tf_cs) == SEL_UPL ? "user" : "kernel");
#ifdef SMP
	/* two separate prints in case of a trap on an unmapped page */
	WriteKernelFileLog("cpuid = %d; ", PCPU_GET(cpuid));
	WriteKernelFileLog("apic id = %02x\n", PCPU_GET(apic_id));
#endif
	if (type == T_PAGEFLT) {
		WriteKernelFileLog("fault virtual address	= 0x%lx\n", eva);
		WriteKernelFileLog("fault code		= %s %s %s, %s\n",
			code & PGEX_U ? "user" : "supervisor",
			code & PGEX_W ? "write" : "read",
			code & PGEX_I ? "instruction" : "data",
			code & PGEX_P ? "protection violation" : "page not present");
	}
	WriteKernelFileLog("instruction pointer	= 0x%lx:0x%lx\n",
	       frame->tf_cs & 0xffff, frame->tf_rip);
        if (ISPL(frame->tf_cs) == SEL_UPL) {
		ss = frame->tf_ss & 0xffff;
		esp = frame->tf_rsp;
	} else {
		ss = GSEL(GDATA_SEL, SEL_KPL);
		esp = (long)&frame->tf_rsp;
	}

	WriteKernelFileLog("stack pointer	        = 0x%x:0x%lx\n", ss, esp);
	WriteKernelFileLog("frame pointer	        = 0x%x:0x%lx\n", ss, frame->tf_rbp);
	WriteKernelFileLog("code segment		= base 0x%lx, limit 0x%lx, type 0x%x\n",
	       softseg.ssd_base, softseg.ssd_limit, softseg.ssd_type);
	WriteKernelFileLog("			= DPL %d, pres %d, long %d, def32 %d, gran %d\n",
	       softseg.ssd_dpl, softseg.ssd_p, softseg.ssd_long, softseg.ssd_def32,
	       softseg.ssd_gran);
	WriteKernelFileLog("processor eflags	= ");
	if (frame->tf_rflags & PSL_T)
		WriteKernelFileLog("trace trap, ");
	if (frame->tf_rflags & PSL_I)
		WriteKernelFileLog("interrupt enabled, ");
	if (frame->tf_rflags & PSL_NT)
		WriteKernelFileLog("nested task, ");
	if (frame->tf_rflags & PSL_RF)
		WriteKernelFileLog("resume, ");
	WriteKernelFileLog("IOPL = %ld\n", (frame->tf_rflags & PSL_IOPL) >> 12);
	WriteKernelFileLog("current process		= ");
	if (curproc) {
		WriteKernelFileLog("%lu (%s)\n",
		    (u_long)curproc->p_pid, curthread->td_name/* ?
		    curthread->td_name : ""*/);
	} else {
		WriteKernelFileLog("Idle\n");
	}

#ifdef KDB
	if (debugger_on_panic || kdb_active)
		if (kdb_trap(type, 0, frame))
			return;
#endif

	WriteKernelFileLog("trap number		= %d\n", type);
	if (type <= MAX_TRAP_MSG)
	{	
		WriteKernelFileLog("%s\n", trap_msg[type]);
	}
	else
		WriteKernelFileLog("unknown/reserved trap");
	
	// Intentionally hang the thread
	/*for (;;)
		__asm__("nop");*/

	/*auto s_TrapFatalHook = ((Debugger*)s_Framework->GetPluginManager()->GetDebugger())->m_TrapFatalHook;
	if (s_TrapFatalHook != nullptr)
	{
		
		uint32_t s_TrampSize = 0;
		auto s_OrigTrapFatal = (void(*)(struct trapframe* frame, vm_offset_t eva))s_TrapFatalHook->GetTrampolineFunctionAddress(&s_TrampSize);
		
		printf("tramp: %p orig: %p this: %p\n", s_OrigTrapFatal, kdlsym(trap_fatal), OnTrapFatal);

		for (auto i = 0; i < s_TrampSize; ++i)
			printf("0x%02X ", ((uint8_t*)s_OrigTrapFatal)[i]);
		printf("\n\n");

		printf("calling original trap fatal\n");
		s_OrigTrapFatal(frame, eva);
	}

	
	
	s_TrapFatalHook->Disable();*/

	printf("exiting crashed thread\n");
	kthread_exit();

	// Allow the debugger to be placed here manually and continue exceution
	__asm__("pop %rbp;leave;ret;");
}