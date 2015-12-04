/*
 * Performance events - AMD Processor Power Reporting Mechanism
 *
 * Copyright (C) 2016 Advanced Micro Devices, Inc.
 *
 * Author: Huang Rui <ray.huang@amd.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/perf_event.h>
#include <asm/cpu_device_id.h>
#include "perf_event.h"

#define MSR_F15H_CU_PWR_ACCUMULATOR     0xc001007a
#define MSR_F15H_CU_MAX_PWR_ACCUMULATOR 0xc001007b
#define MSR_F15H_PTSC			0xc0010280

/*
 * event code: LSB 8 bits, passed in attr->config
 * any other bit is reserved
 */
#define AMD_POWER_EVENT_MASK	0xFFULL

#define MAX_CUS	8

/*
 * Acc power status counters
 */
#define AMD_POWER_PKG_ID		0
#define AMD_POWER_EVENTSEL_PKG		1

/*
 * the ratio of compute unit power accumulator sample period to the
 * PTSC period
 */
static unsigned int cpu_pwr_sample_ratio;
static unsigned int cores_per_cu;
static unsigned int cu_num;

/* maximum accumulated power of a compute unit */
static u64 max_cu_acc_power;

struct power_pmu {
	spinlock_t		lock;
	struct list_head	active_list;
	struct pmu		*pmu; /* pointer to power_pmu_class */
	local64_t		cpu_sw_pwr_ptsc;
};

static struct pmu pmu_class;

/*
 * Accumulated power is to measure the sum of each compute unit's
 * power consumption. So it picks only one core from each compute unit
 * to get the power with MSR_F15H_CU_PWR_ACCUMULATOR. The cpu_mask
 * represents CPU bit map of all cores which are picked to measure the
 * power for the compute units that they belong to.
 */
static cpumask_t cpu_mask;

static DEFINE_PER_CPU(struct power_pmu *, amd_power_pmu);

static u64 event_update(struct perf_event *event, struct power_pmu *pmu)
{
	struct hw_perf_event *hwc = &event->hw;
	u64 prev_raw_count, new_raw_count, prev_ptsc, new_ptsc;
	u64 delta, tdelta;

again:
	prev_raw_count = local64_read(&hwc->prev_count);
	prev_ptsc = local64_read(&pmu->cpu_sw_pwr_ptsc);
	rdmsrl(event->hw.event_base, new_raw_count);
	rdmsrl(MSR_F15H_PTSC, new_ptsc);

	if (local64_cmpxchg(&hwc->prev_count, prev_raw_count,
			    new_raw_count) != prev_raw_count) {
		cpu_relax();
		goto again;
	}

	/*
	 * calculate the power comsumption for each compute unit over
	 * a time period, the unit of final value (delta) is
	 * micro-Watts. Then add it into event count.
	 */
	if (new_raw_count < prev_raw_count) {
		delta = max_cu_acc_power + new_raw_count;
		delta -= prev_raw_count;
	} else
		delta = new_raw_count - prev_raw_count;

	delta *= cpu_pwr_sample_ratio * 1000;
	tdelta = new_ptsc - prev_ptsc;

	do_div(delta, tdelta);
	local64_add(delta, &event->count);

	return new_raw_count;
}

static void __pmu_event_start(struct power_pmu *pmu,
			      struct perf_event *event)
{
	u64 ptsc, counts;

	if (WARN_ON_ONCE(!(event->hw.state & PERF_HES_STOPPED)))
		return;

	event->hw.state = 0;

	list_add_tail(&event->active_entry, &pmu->active_list);

	rdmsrl(MSR_F15H_PTSC, ptsc);
	local64_set(&pmu->cpu_sw_pwr_ptsc, ptsc);
	rdmsrl(event->hw.event_base, counts);
	local64_set(&event->hw.prev_count, counts);
}

static void pmu_event_start(struct perf_event *event, int mode)
{
	struct power_pmu *pmu = __this_cpu_read(amd_power_pmu);
	unsigned long flags;

	spin_lock_irqsave(&pmu->lock, flags);
	__pmu_event_start(pmu, event);
	spin_unlock_irqrestore(&pmu->lock, flags);
}

static void pmu_event_stop(struct perf_event *event, int mode)
{
	struct power_pmu *pmu = __this_cpu_read(amd_power_pmu);
	struct hw_perf_event *hwc = &event->hw;
	unsigned long flags;

	spin_lock_irqsave(&pmu->lock, flags);

	/* mark event as deactivated and stopped */
	if (!(hwc->state & PERF_HES_STOPPED)) {
		list_del(&event->active_entry);
		hwc->state |= PERF_HES_STOPPED;
	}

	/* check if update of sw counter is necessary */
	if ((mode & PERF_EF_UPDATE) && !(hwc->state & PERF_HES_UPTODATE)) {
		/*
		 * Drain the remaining delta count out of a event
		 * that we are disabling:
		 */
		event_update(event, pmu);
		hwc->state |= PERF_HES_UPTODATE;
	}

	spin_unlock_irqrestore(&pmu->lock, flags);
}

static int pmu_event_add(struct perf_event *event, int mode)
{
	struct power_pmu *pmu = __this_cpu_read(amd_power_pmu);
	struct hw_perf_event *hwc = &event->hw;
	unsigned long flags;

	spin_lock_irqsave(&pmu->lock, flags);

	hwc->state = PERF_HES_UPTODATE | PERF_HES_STOPPED;

	if (mode & PERF_EF_START)
		__pmu_event_start(pmu, event);

	spin_unlock_irqrestore(&pmu->lock, flags);

	return 0;
}

static void pmu_event_del(struct perf_event *event, int flags)
{
	pmu_event_stop(event, PERF_EF_UPDATE);
}

static int pmu_event_init(struct perf_event *event)
{
	u64 cfg = event->attr.config & AMD_POWER_EVENT_MASK;
	int bit, ret = 0;

	/* only look at AMD power events */
	if (event->attr.type != pmu_class.type)
		return -ENOENT;

	/* unsupported modes and filters */
	if (event->attr.exclude_user   ||
	    event->attr.exclude_kernel ||
	    event->attr.exclude_hv     ||
	    event->attr.exclude_idle   ||
	    event->attr.exclude_host   ||
	    event->attr.exclude_guest  ||
	    event->attr.sample_period) /* no sampling */
		return -EINVAL;

	if (cfg == AMD_POWER_EVENTSEL_PKG)
		bit = AMD_POWER_PKG_ID;
	else
		return -EINVAL;

	event->hw.event_base = MSR_F15H_CU_PWR_ACCUMULATOR;
	event->hw.config = cfg;
	event->hw.idx = bit;

	return ret;
}

static void pmu_event_read(struct perf_event *event)
{
	struct power_pmu *pmu = __this_cpu_read(amd_power_pmu);

	event_update(event, pmu);
}

static ssize_t get_attr_cpumask(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return cpumap_print_to_pagebuf(true, buf, &cpu_mask);
}

static DEVICE_ATTR(cpumask, S_IRUGO, get_attr_cpumask, NULL);

static struct attribute *pmu_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static struct attribute_group pmu_attr_group = {
	.attrs = pmu_attrs,
};


/*
 * at current, it only supports to report the power of each
 * processor/package
 */
EVENT_ATTR_STR(power-pkg, power_pkg, "event=0x01");

EVENT_ATTR_STR(power-pkg.unit, power_pkg_unit, "mWatts");

/* convert the count from micro-Watts to milli-Watts */
EVENT_ATTR_STR(power-pkg.scale, power_pkg_scale, "1.000000e-3");


static struct attribute *events_attr[] = {
	EVENT_PTR(power_pkg),
	EVENT_PTR(power_pkg_unit),
	EVENT_PTR(power_pkg_scale),
	NULL,
};

static struct attribute_group pmu_events_group = {
	.name = "events",
	.attrs = events_attr,
};

PMU_FORMAT_ATTR(event, "config:0-7");

static struct attribute *formats_attr[] = {
	&format_attr_event.attr,
	NULL,
};

static struct attribute_group pmu_format_group = {
	.name = "format",
	.attrs = formats_attr,
};

static const struct attribute_group *attr_groups[] = {
	&pmu_attr_group,
	&pmu_format_group,
	&pmu_events_group,
	NULL,
};

static struct pmu pmu_class = {
	.attr_groups	= attr_groups,
	.task_ctx_nr	= perf_invalid_context, /* system-wide only */
	.event_init	= pmu_event_init,
	.add		= pmu_event_add, /* must have */
	.del		= pmu_event_del, /* must have */
	.start		= pmu_event_start,
	.stop		= pmu_event_stop,
	.read		= pmu_event_read,
};


static int power_cpu_exit(int cpu)
{
	struct power_pmu *pmu = per_cpu(amd_power_pmu, cpu);
	int i, cu, ret = 0;
	int target = nr_cpumask_bits;
	cpumask_var_t mask, tmp_mask;

	cu = cpu / cores_per_cu;

	if (!zalloc_cpumask_var(&mask, GFP_KERNEL))
		return -ENOMEM;

	if (!zalloc_cpumask_var(&tmp_mask, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < cores_per_cu; i++)
		cpumask_set_cpu(i, mask);

	cpumask_shift_left(mask, mask, cu * cores_per_cu);

	cpumask_clear_cpu(cpu, &cpu_mask);
	cpumask_clear_cpu(cpu, mask);

	if (!cpumask_and(tmp_mask, mask, cpu_online_mask))
		goto out1;

	/*
	 * find a new CPU on same compute unit, if was set in cpumask
	 * and still some CPUs on compute unit, then move to the new
	 * CPU
	 */
	target = cpumask_any(tmp_mask);
	if (target < nr_cpumask_bits && target != cpu)
		cpumask_set_cpu(target, &cpu_mask);

	WARN_ON(cpumask_empty(&cpu_mask));

out1:
	/*
	 * migrate events and context to new CPU
	 */
	if (target < nr_cpumask_bits)
		perf_pmu_migrate_context(pmu->pmu, cpu, target);

	free_cpumask_var(tmp_mask);
out:
	free_cpumask_var(mask);

	return ret;

}

static int power_cpu_init(int cpu)
{
	int i, cu, ret = 0;
	cpumask_var_t mask, dummy_mask;

	cu = cpu / cores_per_cu;

	if (!zalloc_cpumask_var(&mask, GFP_KERNEL))
		return -ENOMEM;

	if (!zalloc_cpumask_var(&dummy_mask, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < cores_per_cu; i++)
		cpumask_set_cpu(i, mask);

	cpumask_shift_left(mask, mask, cu * cores_per_cu);

	if (!cpumask_and(dummy_mask, mask, &cpu_mask))
		cpumask_set_cpu(cpu, &cpu_mask);

	free_cpumask_var(dummy_mask);
out:
	free_cpumask_var(mask);

	return ret;
}

static int power_cpu_prepare(int cpu)
{
	struct power_pmu *pmu = per_cpu(amd_power_pmu, cpu);
	int phys_id = topology_physical_package_id(cpu);

	if (pmu)
		return 0;

	if (phys_id < 0)
		return -EINVAL;

	pmu = kzalloc_node(sizeof(*pmu), GFP_KERNEL, cpu_to_node(cpu));
	if (!pmu)
		return -ENOMEM;

	spin_lock_init(&pmu->lock);

	INIT_LIST_HEAD(&pmu->active_list);

	pmu->pmu = &pmu_class;

	per_cpu(amd_power_pmu, cpu) = pmu;

	return 0;
}

static void power_cpu_kfree(int cpu)
{
	struct power_pmu *pmu = per_cpu(amd_power_pmu, cpu);

	kfree(pmu);
}

static int power_cpu_notifier(struct notifier_block *self,
			      unsigned long action, void *hcpu)
{
	unsigned int cpu = (long)hcpu;

	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_UP_PREPARE:
		if (power_cpu_prepare(cpu))
			return NOTIFY_BAD;
		break;
	case CPU_STARTING:
		if (power_cpu_init(cpu))
			return NOTIFY_BAD;
		break;
	case CPU_ONLINE:
	case CPU_DEAD:
		power_cpu_kfree(cpu);
		break;
	case CPU_DOWN_PREPARE:
		if (power_cpu_exit(cpu))
			return NOTIFY_BAD;
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

static const struct x86_cpu_id cpu_match[] = {
	{ .vendor = X86_VENDOR_AMD, .family = 0x15 },
	{},
};

static int __init amd_power_pmu_init(void)
{
	int i, ret;
	u64 tmp;
	cpumask_var_t tmp_mask, res_mask;

	if (!x86_match_cpu(cpu_match))
		return 0;

	if (!boot_cpu_has(X86_FEATURE_ACC_POWER))
		return -ENODEV;

	cores_per_cu = amd_get_cores_per_cu();
	cu_num = boot_cpu_data.x86_max_cores / cores_per_cu;

	if (WARN_ON_ONCE(cu_num > MAX_CUS))
		return -EINVAL;

	cpu_pwr_sample_ratio = cpuid_ecx(0x80000007);

	if (rdmsrl_safe(MSR_F15H_CU_MAX_PWR_ACCUMULATOR, &tmp)) {
		pr_err("Failed to read max compute unit power accumulator MSR\n");
		return -ENODEV;
	}
	max_cu_acc_power = tmp;

	if (!zalloc_cpumask_var(&tmp_mask, GFP_KERNEL))
		return -ENOMEM;

	if (!zalloc_cpumask_var(&res_mask, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < cores_per_cu; i++)
		cpumask_set_cpu(i, tmp_mask);

	cpu_notifier_register_begin();

	/*
	 * Choose the one online core of each compute unit
	 */
	for (i = 0; i < cu_num; i++) {
		/* WARN_ON for empty CU masks */
		WARN_ON(!cpumask_and(res_mask, tmp_mask, cpu_online_mask));
		cpumask_set_cpu(cpumask_any(res_mask), &cpu_mask);
		cpumask_shift_left(tmp_mask, tmp_mask, cores_per_cu);
	}

	for_each_present_cpu(i) {
		ret = power_cpu_prepare(i);
		if (ret) {
			/* unwind on [0 ... i-1] CPUs */
			while (i--)
				power_cpu_kfree(i);
			goto out1;
		}
		ret = power_cpu_init(i);
		if (ret) {
			/* unwind on [0 ... i] CPUs */
			while (i >= 0)
				power_cpu_kfree(i--);
			goto out1;
		}
	}

	__perf_cpu_notifier(power_cpu_notifier);

	ret = perf_pmu_register(&pmu_class, "power", -1);
	if (WARN_ON(ret)) {
		pr_warn("AMD Power PMU registration failed\n");
		goto out1;
	}

	pr_info("AMD Power PMU detected, %d compute units\n", cu_num);

out1:
	cpu_notifier_register_done();

	free_cpumask_var(res_mask);
out:
	free_cpumask_var(tmp_mask);

	return ret;
}
device_initcall(amd_power_pmu_init);
