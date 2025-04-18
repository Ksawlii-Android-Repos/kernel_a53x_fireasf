// SPDX-License-Identifier: GPL-2.0-only
/*
 * HyperV  Detection code.
 *
 * Copyright (C) 2010, Novell, Inc.
 * Author : K. Y. Srinivasan <ksrinivasan@novell.com>
 */

#include <linux/types.h>
#include <linux/time.h>
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/hardirq.h>
#include <linux/efi.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kexec.h>
#include <linux/random.h>
#include <asm/processor.h>
#include <asm/hypervisor.h>
#include <asm/hyperv-tlfs.h>
#include <asm/mshyperv.h>
#include <asm/desc.h>
#include <asm/idtentry.h>
#include <asm/irq_regs.h>
#include <asm/i8259.h>
#include <asm/apic.h>
#include <asm/timer.h>
#include <asm/reboot.h>
#include <asm/nmi.h>
#include <clocksource/hyperv_timer.h>

struct ms_hyperv_info ms_hyperv;
EXPORT_SYMBOL_GPL(ms_hyperv);

#if IS_ENABLED(CONFIG_HYPERV)
static void (*vmbus_handler)(void);
static void (*hv_stimer0_handler)(void);
static void (*hv_kexec_handler)(void);
static void (*hv_crash_handler)(struct pt_regs *regs);

DEFINE_IDTENTRY_SYSVEC(sysvec_hyperv_callback)
{
	struct pt_regs *old_regs = set_irq_regs(regs);

	inc_irq_stat(irq_hv_callback_count);
	if (vmbus_handler)
		vmbus_handler();

	if (ms_hyperv.hints & HV_DEPRECATING_AEOI_RECOMMENDED)
		ack_APIC_irq();

	set_irq_regs(old_regs);
}

int hv_setup_vmbus_irq(int irq, void (*handler)(void))
{
	/*
	 * The 'irq' argument is ignored on x86/x64 because a hard-coded
	 * interrupt vector is used for Hyper-V interrupts.
	 */
	vmbus_handler = handler;
	return 0;
}

void hv_remove_vmbus_irq(void)
{
	/* We have no way to deallocate the interrupt gate */
	vmbus_handler = NULL;
}
EXPORT_SYMBOL_GPL(hv_setup_vmbus_irq);
EXPORT_SYMBOL_GPL(hv_remove_vmbus_irq);

/*
 * Routines to do per-architecture handling of stimer0
 * interrupts when in Direct Mode
 */
DEFINE_IDTENTRY_SYSVEC(sysvec_hyperv_stimer0)
{
	struct pt_regs *old_regs = set_irq_regs(regs);

	inc_irq_stat(hyperv_stimer0_count);
	if (hv_stimer0_handler)
		hv_stimer0_handler();
	add_interrupt_randomness(HYPERV_STIMER0_VECTOR);
	ack_APIC_irq();

	set_irq_regs(old_regs);
}

int hv_setup_stimer0_irq(int *irq, int *vector, void (*handler)(void))
{
	*vector = HYPERV_STIMER0_VECTOR;
	*irq = -1;   /* Unused on x86/x64 */
	hv_stimer0_handler = handler;
	return 0;
}
EXPORT_SYMBOL_GPL(hv_setup_stimer0_irq);

void hv_remove_stimer0_irq(int irq)
{
	/* We have no way to deallocate the interrupt gate */
	hv_stimer0_handler = NULL;
}
EXPORT_SYMBOL_GPL(hv_remove_stimer0_irq);

void hv_setup_kexec_handler(void (*handler)(void))
{
	hv_kexec_handler = handler;
}
EXPORT_SYMBOL_GPL(hv_setup_kexec_handler);

void hv_remove_kexec_handler(void)
{
	hv_kexec_handler = NULL;
}
EXPORT_SYMBOL_GPL(hv_remove_kexec_handler);

void hv_setup_crash_handler(void (*handler)(struct pt_regs *regs))
{
	hv_crash_handler = handler;
}
EXPORT_SYMBOL_GPL(hv_setup_crash_handler);

void hv_remove_crash_handler(void)
{
	hv_crash_handler = NULL;
}
EXPORT_SYMBOL_GPL(hv_remove_crash_handler);

#ifdef CONFIG_KEXEC_CORE
static void hv_machine_shutdown(void)
{
	if (kexec_in_progress && hv_kexec_handler)
		hv_kexec_handler();

	/*
	 * Call hv_cpu_die() on all the CPUs, otherwise later the hypervisor
	 * corrupts the old VP Assist Pages and can crash the kexec kernel.
	 */
	if (kexec_in_progress && hyperv_init_cpuhp > 0)
		cpuhp_remove_state(hyperv_init_cpuhp);

	/* The function calls stop_other_cpus(). */
	native_machine_shutdown();

	/* Disable the hypercall page when there is only 1 active CPU. */
	if (kexec_in_progress)
		hyperv_cleanup();
}

static void hv_machine_crash_shutdown(struct pt_regs *regs)
{
	if (hv_crash_handler)
		hv_crash_handler(regs);

	/* The function calls crash_smp_send_stop(). */
	native_machine_crash_shutdown(regs);

	/* Disable the hypercall page when there is only 1 active CPU. */
	hyperv_cleanup();
}
#endif /* CONFIG_KEXEC_CORE */

static u64 hv_ref_counter_at_suspend;
static void (*old_save_sched_clock_state)(void);
static void (*old_restore_sched_clock_state)(void);

/*
 * Hyper-V clock counter resets during hibernation. Save and restore clock
 * offset during suspend/resume, while also considering the time passed
 * before suspend. This is to make sure that sched_clock using hv tsc page
 * based clocksource, proceeds from where it left off during suspend and
 * it shows correct time for the timestamps of kernel messages after resume.
 */
static void save_hv_clock_tsc_state(void)
{
	hv_ref_counter_at_suspend = hv_read_reference_counter();
}

static void restore_hv_clock_tsc_state(void)
{
	/*
	 * Adjust the offsets used by hv tsc clocksource to
	 * account for the time spent before hibernation.
	 * adjusted value = reference counter (time) at suspend
	 *                - reference counter (time) now.
	 */
	hv_adj_sched_clock_offset(hv_ref_counter_at_suspend - hv_read_reference_counter());
}

/*
 * Functions to override save_sched_clock_state and restore_sched_clock_state
 * functions of x86_platform. The Hyper-V clock counter is reset during
 * suspend-resume and the offset used to measure time needs to be
 * corrected, post resume.
 */
static void hv_save_sched_clock_state(void)
{
	old_save_sched_clock_state();
	save_hv_clock_tsc_state();
}

static void hv_restore_sched_clock_state(void)
{
	restore_hv_clock_tsc_state();
	old_restore_sched_clock_state();
}

static void __init x86_setup_ops_for_tsc_pg_clock(void)
{
	if (!(ms_hyperv.features & HV_MSR_REFERENCE_TSC_AVAILABLE))
		return;

	old_save_sched_clock_state = x86_platform.save_sched_clock_state;
	x86_platform.save_sched_clock_state = hv_save_sched_clock_state;

	old_restore_sched_clock_state = x86_platform.restore_sched_clock_state;
	x86_platform.restore_sched_clock_state = hv_restore_sched_clock_state;
}
#endif /* CONFIG_HYPERV */

static uint32_t  __init ms_hyperv_platform(void)
{
	u32 eax;
	u32 hyp_signature[3];

	if (!boot_cpu_has(X86_FEATURE_HYPERVISOR))
		return 0;

	cpuid(HYPERV_CPUID_VENDOR_AND_MAX_FUNCTIONS,
	      &eax, &hyp_signature[0], &hyp_signature[1], &hyp_signature[2]);

	if (eax >= HYPERV_CPUID_MIN &&
	    eax <= HYPERV_CPUID_MAX &&
	    !memcmp("Microsoft Hv", hyp_signature, 12))
		return HYPERV_CPUID_VENDOR_AND_MAX_FUNCTIONS;

	return 0;
}

static unsigned char hv_get_nmi_reason(void)
{
	return 0;
}

#ifdef CONFIG_X86_LOCAL_APIC
/*
 * Prior to WS2016 Debug-VM sends NMIs to all CPUs which makes
 * it dificult to process CHANNELMSG_UNLOAD in case of crash. Handle
 * unknown NMI on the first CPU which gets it.
 */
static int hv_nmi_unknown(unsigned int val, struct pt_regs *regs)
{
	static atomic_t nmi_cpu = ATOMIC_INIT(-1);

	if (!unknown_nmi_panic)
		return NMI_DONE;

	if (atomic_cmpxchg(&nmi_cpu, -1, raw_smp_processor_id()) != -1)
		return NMI_HANDLED;

	return NMI_DONE;
}
#endif

static unsigned long hv_get_tsc_khz(void)
{
	unsigned long freq;

	rdmsrl(HV_X64_MSR_TSC_FREQUENCY, freq);

	return freq / 1000;
}

#if defined(CONFIG_SMP) && IS_ENABLED(CONFIG_HYPERV)
static void __init hv_smp_prepare_boot_cpu(void)
{
	native_smp_prepare_boot_cpu();
#if defined(CONFIG_X86_64) && defined(CONFIG_PARAVIRT_SPINLOCKS)
	hv_init_spinlocks();
#endif
}
#endif

static void __init ms_hyperv_init_platform(void)
{
	int hv_host_info_eax;
	int hv_host_info_ebx;
	int hv_host_info_ecx;
	int hv_host_info_edx;

#ifdef CONFIG_PARAVIRT
	pv_info.name = "Hyper-V";
#endif

	/*
	 * Extract the features and hints
	 */
	ms_hyperv.features = cpuid_eax(HYPERV_CPUID_FEATURES);
	ms_hyperv.misc_features = cpuid_edx(HYPERV_CPUID_FEATURES);
	ms_hyperv.hints    = cpuid_eax(HYPERV_CPUID_ENLIGHTMENT_INFO);

	pr_info("Hyper-V: features 0x%x, hints 0x%x, misc 0x%x\n",
		ms_hyperv.features, ms_hyperv.hints, ms_hyperv.misc_features);

	ms_hyperv.max_vp_index = cpuid_eax(HYPERV_CPUID_IMPLEMENT_LIMITS);
	ms_hyperv.max_lp_index = cpuid_ebx(HYPERV_CPUID_IMPLEMENT_LIMITS);

	pr_debug("Hyper-V: max %u virtual processors, %u logical processors\n",
		 ms_hyperv.max_vp_index, ms_hyperv.max_lp_index);

	/*
	 * Extract host information.
	 */
	if (cpuid_eax(HYPERV_CPUID_VENDOR_AND_MAX_FUNCTIONS) >=
	    HYPERV_CPUID_VERSION) {
		hv_host_info_eax = cpuid_eax(HYPERV_CPUID_VERSION);
		hv_host_info_ebx = cpuid_ebx(HYPERV_CPUID_VERSION);
		hv_host_info_ecx = cpuid_ecx(HYPERV_CPUID_VERSION);
		hv_host_info_edx = cpuid_edx(HYPERV_CPUID_VERSION);

		pr_info("Hyper-V Host Build:%d-%d.%d-%d-%d.%d\n",
			hv_host_info_eax, hv_host_info_ebx >> 16,
			hv_host_info_ebx & 0xFFFF, hv_host_info_ecx,
			hv_host_info_edx >> 24, hv_host_info_edx & 0xFFFFFF);
	}

	if (ms_hyperv.features & HV_ACCESS_FREQUENCY_MSRS &&
	    ms_hyperv.misc_features & HV_FEATURE_FREQUENCY_MSRS_AVAILABLE) {
		x86_platform.calibrate_tsc = hv_get_tsc_khz;
		x86_platform.calibrate_cpu = hv_get_tsc_khz;
		setup_force_cpu_cap(X86_FEATURE_TSC_KNOWN_FREQ);
	}

	if (ms_hyperv.hints & HV_X64_ENLIGHTENED_VMCS_RECOMMENDED) {
		ms_hyperv.nested_features =
			cpuid_eax(HYPERV_CPUID_NESTED_FEATURES);
	}

	/*
	 * Hyper-V expects to get crash register data or kmsg when
	 * crash enlightment is available and system crashes. Set
	 * crash_kexec_post_notifiers to be true to make sure that
	 * calling crash enlightment interface before running kdump
	 * kernel.
	 */
	if (ms_hyperv.misc_features & HV_FEATURE_GUEST_CRASH_MSR_AVAILABLE)
		crash_kexec_post_notifiers = true;

#ifdef CONFIG_X86_LOCAL_APIC
	if (ms_hyperv.features & HV_ACCESS_FREQUENCY_MSRS &&
	    ms_hyperv.misc_features & HV_FEATURE_FREQUENCY_MSRS_AVAILABLE) {
		/*
		 * Get the APIC frequency.
		 */
		u64	hv_lapic_frequency;

		rdmsrl(HV_X64_MSR_APIC_FREQUENCY, hv_lapic_frequency);
		hv_lapic_frequency = div_u64(hv_lapic_frequency, HZ);
		lapic_timer_period = hv_lapic_frequency;
		pr_info("Hyper-V: LAPIC Timer Frequency: %#x\n",
			lapic_timer_period);
	}

	register_nmi_handler(NMI_UNKNOWN, hv_nmi_unknown, NMI_FLAG_FIRST,
			     "hv_nmi_unknown");
#endif

#ifdef CONFIG_X86_IO_APIC
	no_timer_check = 1;
#endif

#if IS_ENABLED(CONFIG_HYPERV) && defined(CONFIG_KEXEC_CORE)
	machine_ops.shutdown = hv_machine_shutdown;
	machine_ops.crash_shutdown = hv_machine_crash_shutdown;
#endif
	if (ms_hyperv.features & HV_ACCESS_TSC_INVARIANT) {
		wrmsrl(HV_X64_MSR_TSC_INVARIANT_CONTROL, 0x1);
		setup_force_cpu_cap(X86_FEATURE_TSC_RELIABLE);
	}

	/*
	 * Generation 2 instances don't support reading the NMI status from
	 * 0x61 port.
	 */
	if (efi_enabled(EFI_BOOT))
		x86_platform.get_nmi_reason = hv_get_nmi_reason;

#if IS_ENABLED(CONFIG_HYPERV)
	/*
	 * Setup the hook to get control post apic initialization.
	 */
	x86_platform.apic_post_init = hyperv_init;
	hyperv_setup_mmu_ops();
	/* Setup the IDT for hypervisor callback */
	alloc_intr_gate(HYPERVISOR_CALLBACK_VECTOR, asm_sysvec_hyperv_callback);

	/* Setup the IDT for reenlightenment notifications */
	if (ms_hyperv.features & HV_ACCESS_REENLIGHTENMENT) {
		alloc_intr_gate(HYPERV_REENLIGHTENMENT_VECTOR,
				asm_sysvec_hyperv_reenlightenment);
	}

	/* Setup the IDT for stimer0 */
	if (ms_hyperv.misc_features & HV_STIMER_DIRECT_MODE_AVAILABLE) {
		alloc_intr_gate(HYPERV_STIMER0_VECTOR,
				asm_sysvec_hyperv_stimer0);
	}

# ifdef CONFIG_SMP
	smp_ops.smp_prepare_boot_cpu = hv_smp_prepare_boot_cpu;
# endif

	/*
	 * Hyper-V doesn't provide irq remapping for IO-APIC. To enable x2apic,
	 * set x2apic destination mode to physcial mode when x2apic is available
	 * and Hyper-V IOMMU driver makes sure cpus assigned with IO-APIC irqs
	 * have 8-bit APIC id.
	 */
# ifdef CONFIG_X86_X2APIC
	if (x2apic_supported())
		x2apic_phys = 1;
# endif

	/* Register Hyper-V specific clocksource */
	hv_init_clocksource();
	x86_setup_ops_for_tsc_pg_clock();
#endif
	/*
	 * TSC should be marked as unstable only after Hyper-V
	 * clocksource has been initialized. This ensures that the
	 * stability of the sched_clock is not altered.
	 */
	if (!(ms_hyperv.features & HV_ACCESS_TSC_INVARIANT))
		mark_tsc_unstable("running on Hyper-V");
}

const __initconst struct hypervisor_x86 x86_hyper_ms_hyperv = {
	.name			= "Microsoft Hyper-V",
	.detect			= ms_hyperv_platform,
	.type			= X86_HYPER_MS_HYPERV,
	.init.init_platform	= ms_hyperv_init_platform,
};
