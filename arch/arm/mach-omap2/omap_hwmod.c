/*
 * omap_hwmod implementation for OMAP2/3/4
 *
 * Copyright (C) 2009-2011 Nokia Corporation
 * Copyright (C) 2011 Texas Instruments, Inc.
 *
 * Paul Walmsley, Benoît Cousson, Kevin Hilman
 *
 * Created in collaboration with (alphabetical order): Thara Gopinath,
 * Tony Lindgren, Rajendra Nayak, Vikram Pandita, Sakari Poussa, Anand
 * Sawant, Santosh Shilimkar, Richard Woodruff
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Introduction
 * ------------
 * One way to view an OMAP SoC is as a collection of largely unrelated
 * IP blocks connected by interconnects.  The IP blocks include
 * devices such as ARM processors, audio serial interfaces, UARTs,
 * etc.  Some of these devices, like the DSP, are created by TI;
 * others, like the SGX, largely originate from external vendors.  In
 * TI's documentation, on-chip devices are referred to as "OMAP
 * modules."  Some of these IP blocks are identical across several
 * OMAP versions.  Others are revised frequently.
 *
 * These OMAP modules are tied together by various interconnects.
 * Most of the address and data flow between modules is via OCP-based
 * interconnects such as the L3 and L4 buses; but there are other
 * interconnects that distribute the hardware clock tree, handle idle
 * and reset signaling, supply power, and connect the modules to
 * various pads or balls on the OMAP package.
 *
 * OMAP hwmod provides a consistent way to describe the on-chip
 * hardware blocks and their integration into the rest of the chip.
 * This description can be automatically generated from the TI
 * hardware database.  OMAP hwmod provides a standard, consistent API
 * to reset, enable, idle, and disable these hardware blocks.  And
 * hwmod provides a way for other core code, such as the Linux device
 * code or the OMAP power management and address space mapping code,
 * to query the hardware database.
 *
 * Using hwmod
 * -----------
 * Drivers won't call hwmod functions directly.  That is done by the
 * omap_device code, and in rare occasions, by custom integration code
 * in arch/arm/ *omap*.  The omap_device code includes functions to
 * build a struct platform_device using omap_hwmod data, and that is
 * currently how hwmod data is communicated to drivers and to the
 * Linux driver model.  Most drivers will call omap_hwmod functions only
 * indirectly, via pm_runtime*() functions.
 *
 * From a layering perspective, here is where the OMAP hwmod code
 * fits into the kernel software stack:
 *
 *            +-------------------------------+
 *            |      Device driver code       |
 *            |      (e.g., drivers/)         |
 *            +-------------------------------+
 *            |      Linux driver model       |
 *            |     (platform_device /        |
 *            |  platform_driver data/code)   |
 *            +-------------------------------+
 *            | OMAP core-driver integration  |
 *            |(arch/arm/mach-omap2/devices.c)|
 *            +-------------------------------+
 *            |      omap_device code         |
 *            | (../plat-omap/omap_device.c)  |
 *            +-------------------------------+
 *   ---->    |    omap_hwmod code/data       |    <-----
 *            | (../mach-omap2/omap_hwmod*)   |
 *            +-------------------------------+
 *            | OMAP clock/PRCM/register fns  |
 *            | (__raw_{read,write}l, clk*)   |
 *            +-------------------------------+
 *
 * Device drivers should not contain any OMAP-specific code or data in
 * them.  They should only contain code to operate the IP block that
 * the driver is responsible for.  This is because these IP blocks can
 * also appear in other SoCs, either from TI (such as DaVinci) or from
 * other manufacturers; and drivers should be reusable across other
 * platforms.
 *
 * The OMAP hwmod code also will attempt to reset and idle all on-chip
 * devices upon boot.  The goal here is for the kernel to be
 * completely self-reliant and independent from bootloaders.  This is
 * to ensure a repeatable configuration, both to ensure consistent
 * runtime behavior, and to make it easier for others to reproduce
 * bugs.
 *
 * OMAP module activity states
 * ---------------------------
 * The hwmod code considers modules to be in one of several activity
 * states.  IP blocks start out in an UNKNOWN state, then once they
 * are registered via the hwmod code, proceed to the REGISTERED state.
 * Once their clock names are resolved to clock pointers, the module
 * enters the CLKS_INITED state; and finally, once the module has been
 * reset and the integration registers programmed, the INITIALIZED state
 * is entered.  The hwmod code will then place the module into either
 * the IDLE state to save power, or in the case of a critical system
 * module, the ENABLED state.
 *
 * OMAP core integration code can then call omap_hwmod*() functions
 * directly to move the module between the IDLE, ENABLED, and DISABLED
 * states, as needed.  This is done during both the PM idle loop, and
 * in the OMAP core integration code's implementation of the PM runtime
 * functions.
 *
 * References
 * ----------
 * This is a partial list.
 * - OMAP2420 Multimedia Processor Silicon Revision 2.1.1, 2.2 (SWPU064)
 * - OMAP2430 Multimedia Device POP Silicon Revision 2.1 (SWPU090)
 * - OMAP34xx Multimedia Device Silicon Revision 3.1 (SWPU108)
 * - OMAP4430 Multimedia Device Silicon Revision 1.0 (SWPU140)
 * - Open Core Protocol Specification 2.2
 *
 * To do:
 * - handle IO mapping
 * - bus throughput & module latency measurement code
 *
 * XXX add tests at the beginning of each function to ensure the hwmod is
 * in the appropriate state
 * XXX error return values should be checked to ensure that they are
 * appropriate
 */
#undef DEBUG

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include "common.h"
#include <plat/cpu.h>
#include "clockdomain.h"
#include "powerdomain.h"
#include <plat/clock.h>
#include <plat/omap_hwmod.h>
#include <plat/omap_device.h>
#include <plat/prcm.h>

#include "cm2xxx_3xxx.h"
#include "cminst44xx.h"
#include "prm2xxx_3xxx.h"
#include "prm44xx.h"
#include "prminst44xx.h"
#include "mux.h"
#include "pm.h"

/* Maximum microseconds to wait for OMAP module to softreset */
#define MAX_MODULE_SOFTRESET_WAIT	10000

/* Name of the OMAP hwmod for the MPU */
#define MPU_INITIATOR_NAME		"mpu"

/* omap_hwmod_list contains all registered struct omap_hwmods */
static LIST_HEAD(omap_hwmod_list);

struct hwmod_ops {
	void	(*hwmod_update_context_lost)(struct omap_hwmod *oh);
	int	(*hwmod_get_context_lost)(struct omap_hwmod *oh);
};

static struct hwmod_ops *arch_hwmod;

/* mpu_oh: used to add/remove MPU initiator from sleepdep list */
static struct omap_hwmod *mpu_oh;

/* io_chain_lock: used to serialize reconfigurations of the I/O chain */
static DEFINE_SPINLOCK(io_chain_lock);

static const ktime_t invalkt = {0};	/* Invalid kernel time */

/* Private functions */

/**
 * _update_sysc_cache - return the module OCP_SYSCONFIG register, keep copy
 * @oh: struct omap_hwmod *
 *
 * Load the current value of the hwmod OCP_SYSCONFIG register into the
 * struct omap_hwmod for later use.  Returns -EINVAL if the hwmod has no
 * OCP_SYSCONFIG register or 0 upon success.
 */
static int _update_sysc_cache(struct omap_hwmod *oh)
{
	if (!oh->class->sysc) {
		WARN(1, "omap_hwmod: %s: cannot read OCP_SYSCONFIG: not defined on hwmod's class\n", oh->name);
		return -EINVAL;
	}

	/* XXX ensure module interface clock is up */

	oh->_sysc_cache = omap_hwmod_read(oh, oh->class->sysc->sysc_offs);

	if (!(oh->class->sysc->sysc_flags & SYSC_NO_CACHE))
		oh->_int_flags |= _HWMOD_SYSCONFIG_LOADED;

	return 0;
}

/**
 * _write_sysconfig - write a value to the module's OCP_SYSCONFIG register
 * @v: OCP_SYSCONFIG value to write
 * @oh: struct omap_hwmod *
 *
 * Write @v into the module class' OCP_SYSCONFIG register, if it has
 * one.  No return value.
 */
static void _write_sysconfig(u32 v, struct omap_hwmod *oh)
{
	if (!oh->class->sysc) {
		WARN(1, "omap_hwmod: %s: cannot write OCP_SYSCONFIG: not defined on hwmod's class\n", oh->name);
		return;
	}

	/* XXX ensure module interface clock is up */

	/* Module might have lost context, always update cache and register */
	oh->_sysc_cache = v;
	omap_hwmod_write(v, oh, oh->class->sysc->sysc_offs);
}

/**
 * _set_master_standbymode: set the OCP_SYSCONFIG MIDLEMODE field in @v
 * @oh: struct omap_hwmod *
 * @standbymode: MIDLEMODE field bits
 * @v: pointer to register contents to modify
 *
 * Update the master standby mode bits in @v to be @standbymode for
 * the @oh hwmod.  Does not write to the hardware.  Returns -EINVAL
 * upon error or 0 upon success.
 */
static int _set_master_standbymode(struct omap_hwmod *oh, u8 standbymode,
				   u32 *v)
{
	u32 mstandby_mask;
	u8 mstandby_shift;

	if (!oh->class->sysc ||
	    !(oh->class->sysc->sysc_flags & SYSC_HAS_MIDLEMODE))
		return -EINVAL;

	if (!oh->class->sysc->sysc_fields) {
		WARN(1, "omap_hwmod: %s: offset struct for sysconfig not provided in class\n", oh->name);
		return -EINVAL;
	}

	mstandby_shift = oh->class->sysc->sysc_fields->midle_shift;
	mstandby_mask = (0x3 << mstandby_shift);

	*v &= ~mstandby_mask;
	*v |= __ffs(standbymode) << mstandby_shift;

	return 0;
}

/**
 * _set_slave_idlemode: set the OCP_SYSCONFIG SIDLEMODE field in @v
 * @oh: struct omap_hwmod *
 * @idlemode: SIDLEMODE field bits
 * @v: pointer to register contents to modify
 *
 * Update the slave idle mode bits in @v to be @idlemode for the @oh
 * hwmod.  Does not write to the hardware.  Returns -EINVAL upon error
 * or 0 upon success.
 */
static int _set_slave_idlemode(struct omap_hwmod *oh, u8 idlemode, u32 *v)
{
	u32 sidle_mask;
	u8 sidle_shift;

	if (!oh->class->sysc ||
	    !(oh->class->sysc->sysc_flags & SYSC_HAS_SIDLEMODE))
		return -EINVAL;

	if (!oh->class->sysc->sysc_fields) {
		WARN(1, "omap_hwmod: %s: offset struct for sysconfig not provided in class\n", oh->name);
		return -EINVAL;
	}

	sidle_shift = oh->class->sysc->sysc_fields->sidle_shift;
	sidle_mask = (0x3 << sidle_shift);

	*v &= ~sidle_mask;
	*v |= __ffs(idlemode) << sidle_shift;

	return 0;
}

/**
 * _set_clockactivity: set OCP_SYSCONFIG.CLOCKACTIVITY bits in @v
 * @oh: struct omap_hwmod *
 * @clockact: CLOCKACTIVITY field bits
 * @v: pointer to register contents to modify
 *
 * Update the clockactivity mode bits in @v to be @clockact for the
 * @oh hwmod.  Used for additional powersaving on some modules.  Does
 * not write to the hardware.  Returns -EINVAL upon error or 0 upon
 * success.
 */
static int _set_clockactivity(struct omap_hwmod *oh, u8 clockact, u32 *v)
{
	u32 clkact_mask;
	u8  clkact_shift;

	if (!oh->class->sysc ||
	    !(oh->class->sysc->sysc_flags & SYSC_HAS_CLOCKACTIVITY))
		return -EINVAL;

	if (!oh->class->sysc->sysc_fields) {
		WARN(1, "omap_hwmod: %s: offset struct for sysconfig not provided in class\n", oh->name);
		return -EINVAL;
	}

	clkact_shift = oh->class->sysc->sysc_fields->clkact_shift;
	clkact_mask = (0x3 << clkact_shift);

	*v &= ~clkact_mask;
	*v |= clockact << clkact_shift;

	return 0;
}

/**
 * _set_softreset: set OCP_SYSCONFIG.SOFTRESET bit in @v
 * @oh: struct omap_hwmod *
 * @v: pointer to register contents to modify
 *
 * Set the SOFTRESET bit in @v for hwmod @oh.  Returns -EINVAL upon
 * error or 0 upon success.
 */
static int _set_softreset(struct omap_hwmod *oh, u32 *v)
{
	u32 softrst_mask;

	if (!oh->class->sysc ||
	    !(oh->class->sysc->sysc_flags & SYSC_HAS_SOFTRESET))
		return -EINVAL;

	if (!oh->class->sysc->sysc_fields) {
		WARN(1, "omap_hwmod: %s: offset struct for sysconfig not provided in class\n", oh->name);
		return -EINVAL;
	}

	softrst_mask = (0x1 << oh->class->sysc->sysc_fields->srst_shift);

	*v |= softrst_mask;

	return 0;
}

 /**
 * _set_dmadisable: set OCP_SYSCONFIG.DMADISABLE bit in @v
 * @oh: struct omap_hwmod *
 *
 * Set the DMADISABLE bit in @v for hwmod @oh.  Returns -EINVAL upon
 * error or 0 upon success.
 */
static int _set_dmadisable(struct omap_hwmod *oh)
{
	u32 v;
	u32 dmadisable_mask;

	if (!oh->class->sysc ||
		!(oh->class->sysc->sysc_flags & SYSC_HAS_DMADISABLE))
		return -EINVAL;

	if (!oh->class->sysc->sysc_fields) {
		WARN(1, "omap_hwmod: %s: offset struct for sysconfig not provided in class\n", oh->name);
		return -EINVAL;
	}

	/* clocks must be on for this operation */
	if (oh->_state != _HWMOD_STATE_ENABLED) {
		pr_warning("omap_hwmod: %s: dma can be disabled only from enabled state\n", oh->name);
		return -EINVAL;
	}

	pr_info("omap_hwmod: %s: setting DMADISABLE\n", oh->name);

	v = oh->_sysc_cache;
	dmadisable_mask =
		(0x1 << oh->class->sysc->sysc_fields->dmadisable_shift);
	v |= dmadisable_mask;
	_write_sysconfig(v, oh);

	return 0;
}

/**
 * _set_module_autoidle: set the OCP_SYSCONFIG AUTOIDLE field in @v
 * @oh: struct omap_hwmod *
 * @autoidle: desired AUTOIDLE bitfield value (0 or 1)
 * @v: pointer to register contents to modify
 *
 * Update the module autoidle bit in @v to be @autoidle for the @oh
 * hwmod.  The autoidle bit controls whether the module can gate
 * internal clocks automatically when it isn't doing anything; the
 * exact function of this bit varies on a per-module basis.  This
 * function does not write to the hardware.  Returns -EINVAL upon
 * error or 0 upon success.
 */
static int _set_module_autoidle(struct omap_hwmod *oh, u8 autoidle,
				u32 *v)
{
	u32 autoidle_mask;
	u8 autoidle_shift;

	if (!oh->class->sysc ||
	    !(oh->class->sysc->sysc_flags & SYSC_HAS_AUTOIDLE))
		return -EINVAL;

	if (!oh->class->sysc->sysc_fields) {
		WARN(1, "omap_hwmod: %s: offset struct for sysconfig not provided in class\n", oh->name);
		return -EINVAL;
	}

	autoidle_shift = oh->class->sysc->sysc_fields->autoidle_shift;
	autoidle_mask = (0x1 << autoidle_shift);

	*v &= ~autoidle_mask;
	*v |= autoidle << autoidle_shift;

	return 0;
}

/**
 * _enable_module_level_wakeup - enable/disable module level wakeup on hwmod.
 * @oh: struct omap_hwmod *
 * @set_wake: bool value indicating to set (true) or clear (false) module level
 *		wakeup enable
 *
 * Set or clear the  module level wakeup capability the
 * hwmod @oh. This function configures th PM_WKEN reg bits if they
 * are available from hwmod. No return value
 */
static void _enable_module_level_wakeup(struct omap_hwmod *oh, bool set_wake)
{
	if (oh->prcm.omap2.module_offs && oh->prcm.omap2.prcm_reg_id &&
			oh->prcm.omap2.idlest_idle_bit)
		omap2_prm_enable_prcm_module_wakeup(
				oh->prcm.omap2.module_offs,
				oh->prcm.omap2.prcm_reg_id,
				oh->prcm.omap2.idlest_idle_bit,
				set_wake);
}

/**
 * _set_idle_ioring_wakeup - enable/disable IO pad wakeup on hwmod idle for mux
 * @oh: struct omap_hwmod *
 * @set_wake: bool value indicating to set (true) or clear (false) wakeup enable
 *
 * Set or clear the I/O pad wakeup flag in the mux entries for the
 * hwmod @oh.  This function changes the @oh->mux->pads_dynamic array
 * in memory.  If the hwmod is currently idled, and the new idle
 * values don't match the previous ones, this function will also
 * update the SCM PADCTRL registers.  Otherwise, if the hwmod is not
 * currently idled, this function won't touch the hardware: the new
 * mux settings are written to the SCM PADCTRL registers when the
 * hwmod is idled.  No return value.
 */
static void _set_idle_ioring_wakeup(struct omap_hwmod *oh, bool set_wake)
{
	struct omap_device_pad *pad;
	bool change = false;
	u16 prev_idle;
	int j;

	if (!oh->mux || !oh->mux->enabled)
		return;

	for (j = 0; j < oh->mux->nr_pads_dynamic; j++) {
		pad = oh->mux->pads_dynamic[j];

		if (!(pad->flags & OMAP_DEVICE_PAD_WAKEUP))
			continue;

		prev_idle = pad->idle;

		if (set_wake)
			pad->idle |= OMAP_WAKEUP_EN;
		else
			pad->idle &= ~OMAP_WAKEUP_EN;

		if (prev_idle != pad->idle)
			change = true;
	}

	if (change && oh->_state == _HWMOD_STATE_IDLE)
		omap_hwmod_mux(oh->mux, _HWMOD_STATE_IDLE);
}

/**
 * _enable_wakeup: set OCP_SYSCONFIG.ENAWAKEUP bit in the hardware
 * @oh: struct omap_hwmod *
 *
 * Allow the hardware module @oh to send wakeups.  Returns -EINVAL
 * upon error or 0 upon success.
 */
static int _enable_wakeup(struct omap_hwmod *oh, u32 *v)
{
	if (!oh->class->sysc ||
	    !((oh->class->sysc->sysc_flags & SYSC_HAS_ENAWAKEUP) ||
	      (oh->class->sysc->idlemodes & SIDLE_SMART_WKUP) ||
	      (oh->class->sysc->idlemodes & MSTANDBY_SMART_WKUP)))
		return -EINVAL;

	if (!oh->class->sysc->sysc_fields) {
		WARN(1, "omap_hwmod: %s: offset struct for sysconfig not provided in class\n", oh->name);
		return -EINVAL;
	}

	if (oh->class->sysc->sysc_flags & SYSC_HAS_ENAWAKEUP)
		*v |= 0x1 << oh->class->sysc->sysc_fields->enwkup_shift;

	if (oh->class->sysc->idlemodes & SIDLE_SMART_WKUP)
		_set_slave_idlemode(oh, HWMOD_IDLEMODE_SMART_WKUP, v);
	if (oh->class->sysc->idlemodes & MSTANDBY_SMART_WKUP)
		_set_master_standbymode(oh, HWMOD_IDLEMODE_SMART_WKUP, v);

	/* XXX test pwrdm_get_wken for this hwmod's subsystem */

	oh->_int_flags |= _HWMOD_WAKEUP_ENABLED;

	return 0;
}

/**
 * _disable_wakeup: clear OCP_SYSCONFIG.ENAWAKEUP bit in the hardware
 * @oh: struct omap_hwmod *
 *
 * Prevent the hardware module @oh to send wakeups.  Returns -EINVAL
 * upon error or 0 upon success.
 */
static int _disable_wakeup(struct omap_hwmod *oh, u32 *v)
{
	if (!oh->class->sysc ||
	    !((oh->class->sysc->sysc_flags & SYSC_HAS_ENAWAKEUP) ||
	      (oh->class->sysc->idlemodes & SIDLE_SMART_WKUP) ||
	      (oh->class->sysc->idlemodes & MSTANDBY_SMART_WKUP)))
		return -EINVAL;

	if (!oh->class->sysc->sysc_fields) {
		WARN(1, "omap_hwmod: %s: offset struct for sysconfig not provided in class\n", oh->name);
		return -EINVAL;
	}

	if (oh->class->sysc->sysc_flags & SYSC_HAS_ENAWAKEUP)
		*v &= ~(0x1 << oh->class->sysc->sysc_fields->enwkup_shift);

	if (oh->class->sysc->idlemodes & SIDLE_SMART_WKUP)
		_set_slave_idlemode(oh, HWMOD_IDLEMODE_SMART, v);
	if (oh->class->sysc->idlemodes & MSTANDBY_SMART_WKUP)
		_set_master_standbymode(oh, HWMOD_IDLEMODE_SMART_WKUP, v);

	/* XXX test pwrdm_get_wken for this hwmod's subsystem */

	oh->_int_flags &= ~_HWMOD_WAKEUP_ENABLED;

	return 0;
}

/**
 * _add_initiator_dep: prevent @oh from smart-idling while @init_oh is active
 * @oh: struct omap_hwmod *
 *
 * Prevent the hardware module @oh from entering idle while the
 * hardare module initiator @init_oh is active.  Useful when a module
 * will be accessed by a particular initiator (e.g., if a module will
 * be accessed by the IVA, there should be a sleepdep between the IVA
 * initiator and the module).  Only applies to modules in smart-idle
 * mode.  If the clockdomain is marked as not needing autodeps, return
 * 0 without doing anything.  Otherwise, returns -EINVAL upon error or
 * passes along clkdm_add_sleepdep() value upon success.
 */
static int _add_initiator_dep(struct omap_hwmod *oh, struct omap_hwmod *init_oh)
{
	if (!oh->_clk)
		return -EINVAL;

	if (oh->_clk->clkdm && oh->_clk->clkdm->flags & CLKDM_NO_AUTODEPS)
		return 0;

	return clkdm_add_sleepdep(oh->_clk->clkdm, init_oh->_clk->clkdm);
}

/**
 * _del_initiator_dep: allow @oh to smart-idle even if @init_oh is active
 * @oh: struct omap_hwmod *
 *
 * Allow the hardware module @oh to enter idle while the hardare
 * module initiator @init_oh is active.  Useful when a module will not
 * be accessed by a particular initiator (e.g., if a module will not
 * be accessed by the IVA, there should be no sleepdep between the IVA
 * initiator and the module).  Only applies to modules in smart-idle
 * mode.  If the clockdomain is marked as not needing autodeps, return
 * 0 without doing anything.  Returns -EINVAL upon error or passes
 * along clkdm_del_sleepdep() value upon success.
 */
static int _del_initiator_dep(struct omap_hwmod *oh, struct omap_hwmod *init_oh)
{
	if (!oh->_clk)
		return -EINVAL;

	if (oh->_clk->clkdm && oh->_clk->clkdm->flags & CLKDM_NO_AUTODEPS)
		return 0;

	return clkdm_del_sleepdep(oh->_clk->clkdm, init_oh->_clk->clkdm);
}

/**
 * _init_main_clk - get a struct clk * for the the hwmod's main functional clk
 * @oh: struct omap_hwmod *
 *
 * Called from _init_clocks().  Populates the @oh _clk (main
 * functional clock pointer) if a main_clk is present.  Returns 0 on
 * success or -EINVAL on error.
 */
static int _init_main_clk(struct omap_hwmod *oh)
{
	int ret = 0;

	if (!oh->main_clk)
		return 0;

	oh->_clk = omap_clk_get_by_name(oh->main_clk);
	if (!oh->_clk) {
		pr_warning("omap_hwmod: %s: cannot clk_get main_clk %s\n",
			   oh->name, oh->main_clk);
		return -EINVAL;
	}
#ifndef CONFIG_MACH_OMAP_5430ZEBU
	if (!oh->_clk->clkdm && !oh->clkdm_name)
		pr_warning("omap_hwmod: %s: missing clockdomain for %s.\n",
			   oh->name, oh->_clk->name);
#endif
	return ret;
}

/**
 * _init_interface_clks - get a struct clk * for the the hwmod's interface clks
 * @oh: struct omap_hwmod *
 *
 * Called from _init_clocks().  Populates the @oh OCP slave interface
 * clock pointers.  Returns 0 on success or -EINVAL on error.
 */
static int _init_interface_clks(struct omap_hwmod *oh)
{
	struct clk *c;
	int i;
	int ret = 0;

	if (oh->slaves_cnt == 0)
		return 0;

	for (i = 0; i < oh->slaves_cnt; i++) {
		struct omap_hwmod_ocp_if *os = oh->slaves[i];

		if (!os->clk)
			continue;

		c = omap_clk_get_by_name(os->clk);
		if (!c) {
			pr_warning("omap_hwmod: %s: cannot clk_get interface_clk %s\n",
				   oh->name, os->clk);
			ret = -EINVAL;
		}
		os->_clk = c;
	}

	return ret;
}

/**
 * _init_opt_clk - get a struct clk * for the the hwmod's optional clocks
 * @oh: struct omap_hwmod *
 *
 * Called from _init_clocks().  Populates the @oh omap_hwmod_opt_clk
 * clock pointers.  Returns 0 on success or -EINVAL on error.
 */
static int _init_opt_clks(struct omap_hwmod *oh)
{
	struct omap_hwmod_opt_clk *oc;
	struct clk *c;
	int i;
	int ret = 0;

	for (i = oh->opt_clks_cnt, oc = oh->opt_clks; i > 0; i--, oc++) {
		c = omap_clk_get_by_name(oc->clk);
		if (!c) {
			pr_warning("omap_hwmod: %s: cannot clk_get opt_clk %s\n",
				   oh->name, oc->clk);
			ret = -EINVAL;
		}
		oc->_clk = c;
	}

	return ret;
}

/**
 * _enable_clocks - enable hwmod main clock and interface clocks
 * @oh: struct omap_hwmod *
 *
 * Enables all clocks necessary for register reads and writes to succeed
 * on the hwmod @oh.  Returns 0.
 */
static int _enable_clocks(struct omap_hwmod *oh)
{
	int i;

	pr_debug("omap_hwmod: %s: enabling clocks\n", oh->name);

	if (oh->_clk)
		clk_enable(oh->_clk);

	if (oh->slaves_cnt > 0) {
		for (i = 0; i < oh->slaves_cnt; i++) {
			struct omap_hwmod_ocp_if *os = oh->slaves[i];
			struct clk *c = os->_clk;

			if (c && (os->flags & OCPIF_SWSUP_IDLE))
				clk_enable(c);
		}
	}

	/* The opt clocks are controlled by the device driver. */

	return 0;
}

/**
 * _disable_clocks - disable hwmod main clock and interface clocks
 * @oh: struct omap_hwmod *
 *
 * Disables the hwmod @oh main functional and interface clocks.  Returns 0.
 */
static int _disable_clocks(struct omap_hwmod *oh)
{
	int i;

	pr_debug("omap_hwmod: %s: disabling clocks\n", oh->name);

	if (oh->_clk)
		clk_disable(oh->_clk);

	if (oh->slaves_cnt > 0) {
		for (i = 0; i < oh->slaves_cnt; i++) {
			struct omap_hwmod_ocp_if *os = oh->slaves[i];
			struct clk *c = os->_clk;

			if (c && (os->flags & OCPIF_SWSUP_IDLE))
				clk_disable(c);
		}
	}

	/* The opt clocks are controlled by the device driver. */

	return 0;
}

static void _enable_optional_clocks(struct omap_hwmod *oh)
{
	struct omap_hwmod_opt_clk *oc;
	int i;

	pr_debug("omap_hwmod: %s: enabling optional clocks\n", oh->name);

	for (i = oh->opt_clks_cnt, oc = oh->opt_clks; i > 0; i--, oc++)
		if (oc->_clk) {
			pr_debug("omap_hwmod: enable %s:%s\n", oc->role,
				 oc->_clk->name);
			clk_enable(oc->_clk);
		}
}

static void _disable_optional_clocks(struct omap_hwmod *oh)
{
	struct omap_hwmod_opt_clk *oc;
	int i;

	pr_debug("omap_hwmod: %s: disabling optional clocks\n", oh->name);

	for (i = oh->opt_clks_cnt, oc = oh->opt_clks; i > 0; i--, oc++)
		if (oc->_clk) {
			pr_debug("omap_hwmod: disable %s:%s\n", oc->role,
				 oc->_clk->name);
			clk_disable(oc->_clk);
		}
}

/**
 * _enable_module - enable CLKCTRL modulemode on OMAP4
 * @oh: struct omap_hwmod *
 *
 * Enables the PRCM module mode related to the hwmod @oh.
 * No return value.
 */
static void _enable_module(struct omap_hwmod *oh)
{
	/* The module mode does not exist prior OMAP4 */
	if (cpu_is_omap24xx() || cpu_is_omap34xx())
		return;

	if (!oh->clkdm || !oh->prcm.omap4.modulemode)
		return;

	pr_debug("omap_hwmod: %s: _enable_module: %d\n",
		 oh->name, oh->prcm.omap4.modulemode);

	omap4_cminst_module_enable(oh->prcm.omap4.modulemode,
				   oh->clkdm->prcm_partition,
				   oh->clkdm->cm_inst,
				   oh->clkdm->clkdm_offs,
				   oh->prcm.omap4.clkctrl_offs);
}

/**
 * _omap4_wait_target_disable - wait for a module to be disabled on OMAP4
 * @oh: struct omap_hwmod *
 *
 * Wait for a module @oh to enter slave idle.  Returns 0 if the module
 * does not have an IDLEST bit or if the module successfully enters
 * slave idle; otherwise, pass along the return value of the
 * appropriate *_cm*_wait_module_idle() function.
 */
static int _omap4_wait_target_disable(struct omap_hwmod *oh)
{
	if (cpu_is_omap24xx() || cpu_is_omap34xx())
		return 0;

	if (!oh)
		return -EINVAL;

	if (oh->_int_flags & _HWMOD_NO_MPU_PORT)
		return 0;

	if (oh->flags & HWMOD_NO_IDLEST)
		return 0;

	return omap4_cminst_wait_module_idle(oh->clkdm->prcm_partition,
					     oh->clkdm->cm_inst,
					     oh->clkdm->clkdm_offs,
					     oh->prcm.omap4.clkctrl_offs);
}

/**
 * _omap4_disable_module - enable CLKCTRL modulemode on OMAP4
 * @oh: struct omap_hwmod *
 *
 * Disable the PRCM module mode related to the hwmod @oh.
 * Return EINVAL if the modulemode is not supported and 0 in case of success.
 */
static int _omap4_disable_module(struct omap_hwmod *oh)
{
	int v;

	/* The module mode does not exist prior OMAP4 */
	if (cpu_is_omap24xx() || cpu_is_omap34xx())
		return -EINVAL;

	if (!oh->clkdm || !oh->prcm.omap4.modulemode)
		return -EINVAL;

	pr_debug("omap_hwmod: %s: %s\n", oh->name, __func__);

	omap4_cminst_module_disable(oh->clkdm->prcm_partition,
				    oh->clkdm->cm_inst,
				    oh->clkdm->clkdm_offs,
				    oh->prcm.omap4.clkctrl_offs);

	v = _omap4_wait_target_disable(oh);
	if (v)
		pr_warn("omap_hwmod: %s: _wait_target_disable failed\n",
			oh->name);

	return 0;
}

/**
 * _count_mpu_irqs - count the number of MPU IRQ lines associated with @oh
 * @oh: struct omap_hwmod *oh
 *
 * Count and return the number of MPU IRQs associated with the hwmod
 * @oh.  Used to allocate struct resource data.  Returns 0 if @oh is
 * NULL.
 */
static int _count_mpu_irqs(struct omap_hwmod *oh)
{
	struct omap_hwmod_irq_info *ohii;
	int i = 0;

	if (!oh || !oh->mpu_irqs)
		return 0;

	do {
		ohii = &oh->mpu_irqs[i++];
	} while (ohii->irq != -1);

	return i-1;
}

/**
 * _count_sdma_reqs - count the number of SDMA request lines associated with @oh
 * @oh: struct omap_hwmod *oh
 *
 * Count and return the number of SDMA request lines associated with
 * the hwmod @oh.  Used to allocate struct resource data.  Returns 0
 * if @oh is NULL.
 */
static int _count_sdma_reqs(struct omap_hwmod *oh)
{
	struct omap_hwmod_dma_info *ohdi;
	int i = 0;

	if (!oh || !oh->sdma_reqs)
		return 0;

	do {
		ohdi = &oh->sdma_reqs[i++];
	} while (ohdi->dma_req != -1);

	return i-1;
}

/**
 * _count_ocp_if_addr_spaces - count the number of address space entries for @oh
 * @oh: struct omap_hwmod *oh
 *
 * Count and return the number of address space ranges associated with
 * the hwmod @oh.  Used to allocate struct resource data.  Returns 0
 * if @oh is NULL.
 */
static int _count_ocp_if_addr_spaces(struct omap_hwmod_ocp_if *os)
{
	struct omap_hwmod_addr_space *mem;
	int i = 0;

	if (!os || !os->addr)
		return 0;

	do {
		mem = &os->addr[i++];
	} while (mem->pa_start != mem->pa_end);

	return i-1;
}

/**
 * _find_mpu_port_index - find hwmod OCP slave port ID intended for MPU use
 * @oh: struct omap_hwmod *
 *
 * Returns the array index of the OCP slave port that the MPU
 * addresses the device on, or -EINVAL upon error or not found.
 */
static int __init _find_mpu_port_index(struct omap_hwmod *oh)
{
	int i;
	int found = 0;

	if (!oh || oh->slaves_cnt == 0)
		return -EINVAL;

	for (i = 0; i < oh->slaves_cnt; i++) {
		struct omap_hwmod_ocp_if *os = oh->slaves[i];

		if (os->user & OCP_USER_MPU) {
			found = 1;
			break;
		}
	}

	if (found)
		pr_debug("omap_hwmod: %s: MPU OCP slave port ID  %d\n",
			 oh->name, i);
	else
		pr_debug("omap_hwmod: %s: no MPU OCP slave port found\n",
			 oh->name);

	return (found) ? i : -EINVAL;
}

/**
 * _find_mpu_rt_base - find hwmod register target base addr accessible by MPU
 * @oh: struct omap_hwmod *
 *
 * Return the virtual address of the base of the register target of
 * device @oh, or NULL on error.
 */
static void __iomem * __init _find_mpu_rt_base(struct omap_hwmod *oh, u8 index)
{
	struct omap_hwmod_ocp_if *os;
	struct omap_hwmod_addr_space *mem;
	int i = 0, found = 0;
	void __iomem *va_start;

	if (!oh || oh->slaves_cnt == 0)
		return NULL;

	os = oh->slaves[index];

	if (!os->addr)
		return NULL;

	do {
		mem = &os->addr[i++];
		if (mem->flags & ADDR_TYPE_RT)
			found = 1;
	} while (!found && mem->pa_start != mem->pa_end);

	if (found) {
		va_start = ioremap(mem->pa_start, mem->pa_end - mem->pa_start);
		if (!va_start) {
			pr_err("omap_hwmod: %s: Could not ioremap\n", oh->name);
			return NULL;
		}
		pr_debug("omap_hwmod: %s: MPU register target at va %p\n",
			 oh->name, va_start);
	} else {
		pr_debug("omap_hwmod: %s: no MPU register target found\n",
			 oh->name);
	}

	return (found) ? va_start : NULL;
}

/**
 * _enable_sysc - try to bring a module out of idle via OCP_SYSCONFIG
 * @oh: struct omap_hwmod *
 *
 * Ensure that the OCP_SYSCONFIG register for the IP block represented
 * by @oh is set to indicate to the PRCM that the IP block is active.
 * Usually this means placing the module into smart-idle mode and
 * smart-standby, but if there is a bug in the automatic idle handling
 * for the IP block, it may need to be placed into the force-idle or
 * no-idle variants of these modes.  No return value.
 */
static void _enable_sysc(struct omap_hwmod *oh)
{
	u8 idlemode, sf;
	u32 v;
	bool clkdm_act;

	if (!oh->class->sysc)
		return;

	v = oh->_sysc_cache;
	sf = oh->class->sysc->sysc_flags;

	if (sf & SYSC_HAS_SIDLEMODE) {
		clkdm_act = ((oh->clkdm &&
			      oh->clkdm->flags & CLKDM_ACTIVE_WITH_MPU) ||
			     (oh->_clk && oh->_clk->clkdm &&
			      oh->_clk->clkdm->flags & CLKDM_ACTIVE_WITH_MPU));
		if (clkdm_act && !(oh->class->sysc->idlemodes &
				   (SIDLE_SMART | SIDLE_SMART_WKUP)))
			idlemode = HWMOD_IDLEMODE_FORCE;
		else
			idlemode = (oh->flags & HWMOD_SWSUP_SIDLE) ?
				HWMOD_IDLEMODE_NO : HWMOD_IDLEMODE_SMART;
		_set_slave_idlemode(oh, idlemode, &v);
	}

	if (sf & SYSC_HAS_MIDLEMODE) {
		if (oh->flags & HWMOD_SWSUP_MSTANDBY) {
			idlemode = HWMOD_IDLEMODE_NO;
		} else {
			if (sf & SYSC_HAS_ENAWAKEUP)
				_enable_wakeup(oh, &v);
			if (oh->class->sysc->idlemodes & MSTANDBY_SMART_WKUP)
				idlemode = HWMOD_IDLEMODE_SMART_WKUP;
			else
				idlemode = HWMOD_IDLEMODE_SMART;
		}
		_set_master_standbymode(oh, idlemode, &v);
	}

	/*
	 * XXX The clock framework should handle this, by
	 * calling into this code.  But this must wait until the
	 * clock structures are tagged with omap_hwmod entries
	 */
	if ((oh->flags & HWMOD_SET_DEFAULT_CLOCKACT) &&
	    (sf & SYSC_HAS_CLOCKACTIVITY))
		_set_clockactivity(oh, oh->class->sysc->clockact, &v);

	/* If slave is in SMARTIDLE, also enable wakeup */
	if ((sf & SYSC_HAS_SIDLEMODE) && !(oh->flags & HWMOD_SWSUP_SIDLE))
		_enable_wakeup(oh, &v);

	_write_sysconfig(v, oh);

	/*
	 * Set the autoidle bit only after setting the smartidle bit
	 * Setting this will not have any impact on the other modules.
	 */
	if (sf & SYSC_HAS_AUTOIDLE) {
		idlemode = (oh->flags & HWMOD_NO_OCP_AUTOIDLE) ?
			0 : 1;
		_set_module_autoidle(oh, idlemode, &v);
		_write_sysconfig(v, oh);
	}
}

/**
 * _idle_sysc - try to put a module into idle via OCP_SYSCONFIG
 * @oh: struct omap_hwmod *
 *
 * If module is marked as SWSUP_SIDLE, force the module into slave
 * idle; otherwise, configure it for smart-idle.  If module is marked
 * as SWSUP_MSUSPEND, force the module into master standby; otherwise,
 * configure it for smart-standby.  No return value.
 */
static void _idle_sysc(struct omap_hwmod *oh)
{
	u8 idlemode, sf;
	u32 v;

	if (!oh->class->sysc)
		return;

	v = oh->_sysc_cache;
	sf = oh->class->sysc->sysc_flags;

	if (sf & SYSC_HAS_SIDLEMODE) {
		/* XXX What about HWMOD_IDLEMODE_SMART_WKUP? */
		if (oh->flags & HWMOD_SWSUP_SIDLE ||
		    !(oh->class->sysc->idlemodes &
		      (SIDLE_SMART | SIDLE_SMART_WKUP)))
			idlemode = HWMOD_IDLEMODE_FORCE;
		else
			idlemode = HWMOD_IDLEMODE_SMART;
		_set_slave_idlemode(oh, idlemode, &v);
	}

	if (sf & SYSC_HAS_MIDLEMODE) {
		if (oh->flags & HWMOD_SWSUP_MSTANDBY) {
			idlemode = HWMOD_IDLEMODE_FORCE;
		} else {
			if (sf & SYSC_HAS_ENAWAKEUP)
				_enable_wakeup(oh, &v);
			if (oh->class->sysc->idlemodes & MSTANDBY_SMART_WKUP)
				idlemode = HWMOD_IDLEMODE_SMART_WKUP;
			else
				idlemode = HWMOD_IDLEMODE_SMART;
		}
		_set_master_standbymode(oh, idlemode, &v);
	}

	/* If slave is in SMARTIDLE, also enable wakeup */
	if ((sf & SYSC_HAS_SIDLEMODE) && !(oh->flags & HWMOD_SWSUP_SIDLE))
		_enable_wakeup(oh, &v);

	_write_sysconfig(v, oh);
}

/**
 * _shutdown_sysc - force a module into idle via OCP_SYSCONFIG
 * @oh: struct omap_hwmod *
 *
 * Force the module into slave idle and master suspend. No return
 * value.
 */
static void _shutdown_sysc(struct omap_hwmod *oh)
{
	u32 v;
	u8 sf;

	if (!oh->class->sysc)
		return;

	v = oh->_sysc_cache;
	sf = oh->class->sysc->sysc_flags;

	if (sf & SYSC_HAS_SIDLEMODE)
		_set_slave_idlemode(oh, HWMOD_IDLEMODE_FORCE, &v);

	if (sf & SYSC_HAS_MIDLEMODE)
		_set_master_standbymode(oh, HWMOD_IDLEMODE_FORCE, &v);

	if (sf & SYSC_HAS_AUTOIDLE)
		_set_module_autoidle(oh, 1, &v);

	_write_sysconfig(v, oh);
}

/**
 * _lookup - find an omap_hwmod by name
 * @name: find an omap_hwmod by name
 *
 * Return a pointer to an omap_hwmod by name, or NULL if not found.
 */
static struct omap_hwmod *_lookup(const char *name)
{
	struct omap_hwmod *oh, *temp_oh;

	oh = NULL;

	list_for_each_entry(temp_oh, &omap_hwmod_list, node) {
		if (!strcmp(name, temp_oh->name)) {
			oh = temp_oh;
			break;
		}
	}

	return oh;
}
/**
 * _init_clkdm - look up a clockdomain name, store pointer in omap_hwmod
 * @oh: struct omap_hwmod *
 *
 * Convert a clockdomain name stored in a struct omap_hwmod into a
 * clockdomain pointer, and save it into the struct omap_hwmod.
 * return -EINVAL if clkdm_name does not exist or if the lookup failed.
 */
static int _init_clkdm(struct omap_hwmod *oh)
{
	if (cpu_is_omap24xx() || cpu_is_omap34xx())
		return 0;

	if (!oh->clkdm_name) {
		pr_warning("omap_hwmod: %s: no clkdm_name\n", oh->name);
		return -EINVAL;
	}

	oh->clkdm = clkdm_lookup(oh->clkdm_name);
	if (!oh->clkdm) {
		pr_warning("omap_hwmod: %s: could not associate to clkdm %s\n",
			oh->name, oh->clkdm_name);
		return -EINVAL;
	}

	pr_debug("omap_hwmod: %s: associated to clkdm %s\n",
		oh->name, oh->clkdm_name);

	return 0;
}

/**
 * _init_clocks - clk_get() all clocks associated with this hwmod. Retrieve as
 * well the clockdomain.
 * @oh: struct omap_hwmod *
 * @data: not used; pass NULL
 *
 * Called by omap_hwmod_setup_*() (after omap2_clk_init()).
 * Resolves all clock names embedded in the hwmod.  Returns 0 on
 * success, or a negative error code on failure.
 */
static int _init_clocks(struct omap_hwmod *oh, void *data)
{
	int ret = 0;

	if (oh->_state != _HWMOD_STATE_REGISTERED)
		return 0;

	pr_debug("omap_hwmod: %s: looking up clocks\n", oh->name);
	ret |= _init_main_clk(oh);
	ret |= _init_interface_clks(oh);
	ret |= _init_opt_clks(oh);
	ret |= _init_clkdm(oh);

	if (!ret)
		oh->_state = _HWMOD_STATE_CLKS_INITED;
	else
		pr_warning("omap_hwmod: %s: cannot _init_clocks\n", oh->name);

	return ret;
}

/**
 * _wait_target_ready - wait for a module to leave slave idle
 * @oh: struct omap_hwmod *
 *
 * Wait for a module @oh to leave slave idle.  Returns 0 if the module
 * does not have an IDLEST bit or if the module successfully leaves
 * slave idle; otherwise, pass along the return value of the
 * appropriate *_cm*_wait_module_ready() function.
 */
static int _wait_target_ready(struct omap_hwmod *oh)
{
	struct omap_hwmod_ocp_if *os;
	int ret;

	if (!oh)
		return -EINVAL;

	if (oh->_int_flags & _HWMOD_NO_MPU_PORT)
		return 0;

	os = oh->slaves[oh->_mpu_port_index];

	if (oh->flags & HWMOD_NO_IDLEST)
		return 0;

	/* XXX check module SIDLEMODE */

	/* XXX check clock enable states */

	if (cpu_is_omap24xx() || cpu_is_omap34xx()) {
		ret = omap2_cm_wait_module_ready(oh->prcm.omap2.module_offs,
						 oh->prcm.omap2.idlest_reg_id,
						 oh->prcm.omap2.idlest_idle_bit);
	} else if (cpu_is_omap44xx() || cpu_is_omap54xx()) {
		if (!oh->clkdm)
			return -EINVAL;

		ret = omap4_cminst_wait_module_ready(oh->clkdm->prcm_partition,
						     oh->clkdm->cm_inst,
						     oh->clkdm->clkdm_offs,
						     oh->prcm.omap4.clkctrl_offs);
	} else {
		BUG();
	};

	return ret;
}

/**
 * _lookup_hardreset - fill register bit info for this hwmod/reset line
 * @oh: struct omap_hwmod *
 * @name: name of the reset line in the context of this hwmod
 * @ohri: struct omap_hwmod_rst_info * that this function will fill in
 *
 * Return the bit position of the reset line that match the
 * input name. Return -ENOENT if not found.
 */
static u8 _lookup_hardreset(struct omap_hwmod *oh, const char *name,
			    struct omap_hwmod_rst_info *ohri)
{
	int i;

	for (i = 0; i < oh->rst_lines_cnt; i++) {
		const char *rst_line = oh->rst_lines[i].name;
		if (!strcmp(rst_line, name)) {
			ohri->rst_shift = oh->rst_lines[i].rst_shift;
			ohri->st_shift = oh->rst_lines[i].st_shift;
			pr_debug("omap_hwmod: %s: %s: %s: rst %d st %d\n",
				 oh->name, __func__, rst_line, ohri->rst_shift,
				 ohri->st_shift);

			return 0;
		}
	}

	return -ENOENT;
}

/**
 * _assert_hardreset - assert the HW reset line of submodules
 * contained in the hwmod module.
 * @oh: struct omap_hwmod *
 * @name: name of the reset line to lookup and assert
 *
 * Some IP like dsp, ipu or iva contain processor that require
 * an HW reset line to be assert / deassert in order to enable fully
 * the IP.
 */
static int _assert_hardreset(struct omap_hwmod *oh, const char *name)
{
	struct omap_hwmod_rst_info ohri;
	u8 ret;

	if (!oh)
		return -EINVAL;

	ret = _lookup_hardreset(oh, name, &ohri);
	if (IS_ERR_VALUE(ret))
		return ret;

	if (cpu_is_omap24xx() || cpu_is_omap34xx())
		return omap2_prm_assert_hardreset(oh->prcm.omap2.module_offs,
						  ohri.rst_shift);
	else if (cpu_is_omap44xx() || cpu_is_omap54xx()) {
		return omap4_prminst_assert_hardreset(ohri.rst_shift,
				  oh->clkdm->pwrdm.ptr->prcm_partition,
				  oh->clkdm->pwrdm.ptr->prcm_offs,
				  oh->prcm.omap4.rstctrl_offs);
	} else {
		return -EINVAL;
	}

	return ret;
}

/**
 * _deassert_hardreset - deassert the HW reset line of submodules contained
 * in the hwmod module.
 * @oh: struct omap_hwmod *
 * @name: name of the reset line to look up and deassert
 *
 * Some IP like dsp, ipu or iva contain processor that require
 * an HW reset line to be assert / deassert in order to enable fully
 * the IP.
 */
static int _deassert_hardreset(struct omap_hwmod *oh, const char *name)
{
	struct omap_hwmod_rst_info ohri;
	int ret;

	if (!oh)
		return -EINVAL;

	ret = _lookup_hardreset(oh, name, &ohri);
	if (IS_ERR_VALUE(ret))
		return ret;

	if (cpu_is_omap24xx() || cpu_is_omap34xx()) {
		ret = omap2_prm_deassert_hardreset(oh->prcm.omap2.module_offs,
						   ohri.rst_shift,
						   ohri.st_shift);
	} else if (cpu_is_omap44xx() || cpu_is_omap54xx()) {
		if (ohri.st_shift)
			pr_err("omap_hwmod: %s: %s: hwmod data error: OMAP4 does not support st_shift\n",
			       oh->name, name);
		ret = omap4_prminst_deassert_hardreset(ohri.rst_shift,
				  oh->clkdm->pwrdm.ptr->prcm_partition,
				  oh->clkdm->pwrdm.ptr->prcm_offs,
				  oh->prcm.omap4.rstctrl_offs);
	} else {
		return -EINVAL;
	}

	if (ret == -EBUSY)
		pr_warning("omap_hwmod: %s: failed to hardreset\n", oh->name);

	return ret;
}

/**
 * _read_hardreset - read the HW reset line state of submodules
 * contained in the hwmod module
 * @oh: struct omap_hwmod *
 * @name: name of the reset line to look up and read
 *
 * Return the state of the reset line.
 */
static int _read_hardreset(struct omap_hwmod *oh, const char *name)
{
	struct omap_hwmod_rst_info ohri;
	u8 ret;

	if (!oh)
		return -EINVAL;

	ret = _lookup_hardreset(oh, name, &ohri);
	if (IS_ERR_VALUE(ret))
		return ret;

	if (cpu_is_omap24xx() || cpu_is_omap34xx()) {
		return omap2_prm_is_hardreset_asserted(oh->prcm.omap2.module_offs,
						       ohri.st_shift);
	} else if (cpu_is_omap44xx() || cpu_is_omap54xx()) {
		return omap4_prminst_is_hardreset_asserted(ohri.rst_shift,
				  oh->clkdm->pwrdm.ptr->prcm_partition,
				  oh->clkdm->pwrdm.ptr->prcm_offs,
				  oh->prcm.omap4.rstctrl_offs);
	} else {
		return -EINVAL;
	}

	return ret;
}

/**
 * _clear_softreset: clear OCP_SYSCONFIG.SOFTRESET bit in @v
 * @oh: struct omap_hwmod *
 * @v: pointer to register contents to modify
 *
 * Clear the SOFTRESET bit in @v for hwmod @oh.  Returns -EINVAL upon
 * error or 0 upon success.
 */
static int _clear_softreset(struct omap_hwmod *oh, u32 *v)
{
	u32 softrst_mask;

	if (!oh->class->sysc ||
	    !(oh->class->sysc->sysc_flags & SYSC_HAS_SOFTRESET))
		return -EINVAL;

	if (!oh->class->sysc->sysc_fields) {
		WARN(1,
		     "omap_hwmod: %s: sysc_fields absent for sysconfig class\n",
		     oh->name);
		return -EINVAL;
	}

	softrst_mask = (0x1 << oh->class->sysc->sysc_fields->srst_shift);

	*v &= ~softrst_mask;

	return 0;
}

/**
 * _ocp_softreset - reset an omap_hwmod via the OCP_SYSCONFIG bit
 * @oh: struct omap_hwmod *
 *
 * Resets an omap_hwmod @oh via the OCP_SYSCONFIG bit.  hwmod must be
 * enabled for this to work.  Returns -EINVAL if the hwmod cannot be
 * reset this way or if the hwmod is in the wrong state, -ETIMEDOUT if
 * the module did not reset in time, or 0 upon success.
 *
 * In OMAP3 a specific SYSSTATUS register is used to get the reset status.
 * Starting in OMAP4, some IPs do not have SYSSTATUS registers and instead
 * use the SYSCONFIG softreset bit to provide the status.
 *
 * Note that some IP like McBSP do have reset control but don't have
 * reset status.
 */
static int _ocp_softreset(struct omap_hwmod *oh)
{
	u32 v, softrst_mask;
	int c = 0;
	int ret = 0;

	if (!oh->class->sysc ||
	    !(oh->class->sysc->sysc_flags & SYSC_HAS_SOFTRESET))
		return -EINVAL;

	/* clocks must be on for this operation */
	if (oh->_state != _HWMOD_STATE_ENABLED) {
		pr_warning("omap_hwmod: %s: reset can only be entered from "
			   "enabled state\n", oh->name);
		return -EINVAL;
	}

	/* For some modules, all optionnal clocks need to be enabled as well */
	if (oh->flags & HWMOD_CONTROL_OPT_CLKS_IN_RESET)
		_enable_optional_clocks(oh);

	pr_debug("omap_hwmod: %s: resetting via OCP SOFTRESET\n", oh->name);

	v = oh->_sysc_cache;
	ret = _set_softreset(oh, &v);
	if (ret)
		goto dis_opt_clks;

	_write_sysconfig(v, oh);

	if (oh->class->sysc->srst_udelay)
		udelay(oh->class->sysc->srst_udelay);

	if (oh->class->sysc->sysc_flags & SYSS_HAS_RESET_STATUS)
		omap_test_timeout((omap_hwmod_read(oh,
						    oh->class->sysc->syss_offs)
				   & SYSS_RESETDONE_MASK),
				  MAX_MODULE_SOFTRESET_WAIT, c);
	else if (oh->class->sysc->sysc_flags & SYSC_HAS_RESET_STATUS) {
		softrst_mask = (0x1 << oh->class->sysc->sysc_fields->srst_shift);
		omap_test_timeout(!(omap_hwmod_read(oh,
						     oh->class->sysc->sysc_offs)
				   & softrst_mask),
				  MAX_MODULE_SOFTRESET_WAIT, c);
	}

	if (c == MAX_MODULE_SOFTRESET_WAIT) {
		pr_warning("omap_hwmod: %s: softreset failed (waited %d usec)\n",
			   oh->name, MAX_MODULE_SOFTRESET_WAIT);
		ret = -ETIMEDOUT;
		goto dis_opt_clks;
	} else {
		pr_debug("omap_hwmod: %s: softreset in %d usec\n", oh->name, c);
	}

	ret = _clear_softreset(oh, &v);
	if (ret)
		goto dis_opt_clks;
	_write_sysconfig(v, oh);

	/*
	 * XXX add _HWMOD_STATE_WEDGED for modules that don't come back from
	 * _wait_target_ready() or _reset()
	 */

dis_opt_clks:
	if (oh->flags & HWMOD_CONTROL_OPT_CLKS_IN_RESET)
		_disable_optional_clocks(oh);

	return ret;
}

/**
 * _reset - reset an omap_hwmod
 * @oh: struct omap_hwmod *
 *
 * Resets an omap_hwmod @oh.  The default software reset mechanism for
 * most OMAP IP blocks is triggered via the OCP_SYSCONFIG.SOFTRESET
 * bit.  However, some hwmods cannot be reset via this method: some
 * are not targets and therefore have no OCP header registers to
 * access; others (like the IVA) have idiosyncratic reset sequences.
 * So for these relatively rare cases, custom reset code can be
 * supplied in the struct omap_hwmod_class .reset function pointer.
 * Passes along the return value from either _reset() or the custom
 * reset function - these must return -EINVAL if the hwmod cannot be
 * reset this way or if the hwmod is in the wrong state, -ETIMEDOUT if
 * the module did not reset in time, or 0 upon success.
 */
static int _reset(struct omap_hwmod *oh)
{
	int ret;

	pr_debug("omap_hwmod: %s: resetting\n", oh->name);

	ret = (oh->class->reset) ? oh->class->reset(oh) : _ocp_softreset(oh);
	_set_dmadisable(oh);

	if (oh->class->sysc) {
		_update_sysc_cache(oh);
		_enable_sysc(oh);
	}

	return ret;
}

/**
 * omap_hwmod_reconfigure_io_chain - clear any I/O chain wakeups and
 *		reconfigure chain
 *
 * Call the appropriate PRM function to clear any logged I/O chain
 * wakeups and to reconfigure the chain.  This apparently needs to be
 * done upon every mux change.  Since hwmods can be concurrently
 * enabled and idled, hold a spinlock around the I/O chain
 * reconfiguration sequence.  No return value.
 *
 * XXX When the PRM code is moved to drivers, this function can be removed,
 * as the PRM infrastructure should abstract this.
 */
void omap_hwmod_reconfigure_io_chain(void)
{
	unsigned long flags;

	spin_lock_irqsave(&io_chain_lock, flags);

	if (cpu_is_omap34xx() && omap3_has_io_chain_ctrl())
		omap3xxx_prm_reconfigure_io_chain();
	else if (cpu_is_omap44xx() || cpu_is_omap54xx())
		omap44xx_prm_reconfigure_io_chain();

	spin_unlock_irqrestore(&io_chain_lock, flags);
}

static inline void _omap4_inc_context_loss(unsigned int *v)
{

	/*
	 * Context loss count has to be a non-negative value.
	 * Clear the sign bit to get a value range from 0 to
	 * INT_MAX.
	 */
	*v = (*v + 1) & INT_MAX;
	*v = *v ? *v : 1;
}

/**
 * _omap4_update_context_lost - increment hwmod context loss counter if
 * hwmod context was lost, and clear hardware context loss reg
 * @oh: hwmod to check for context loss
 *
 * If the PRCM indicates that the hwmod @oh lost context, increment
 * our in-memory context loss counter, and clear the RM_*_CONTEXT
 * bits. No return value.
 */
static void _omap4_update_context_lost(struct omap_hwmod *oh)
{
	u32 r;

	if (oh->prcm.omap4.context_offs == USHRT_MAX)
		return;

	r = omap4_prminst_read_inst_reg(oh->clkdm->pwrdm.ptr->prcm_partition,
					oh->clkdm->pwrdm.ptr->prcm_offs,
					oh->prcm.omap4.context_offs);

	if (!r)
		return;

	_omap4_inc_context_loss(&oh->prcm.omap4.context_lost_counter);

	omap4_prminst_write_inst_reg(r, oh->clkdm->pwrdm.ptr->prcm_partition,
				     oh->clkdm->pwrdm.ptr->prcm_offs,
				     oh->prcm.omap4.context_offs);
}

/**
 * _omap4_get_context_lost - get context loss counter for a hwmod
 * @oh:	hwmod to provide for context loss count for
 *
 * Returns the in-memory context loss counter for a hwmod.
 */
static int _omap4_get_context_lost(struct omap_hwmod *oh)
{
	if (oh->prcm.omap4.context_offs == USHRT_MAX)
		_omap4_inc_context_loss(&oh->prcm.omap4.context_lost_counter);

	return oh->prcm.omap4.context_lost_counter;
}

static struct hwmod_ops omap4_hwmod_ops = {
	.hwmod_update_context_lost	= _omap4_update_context_lost,
	.hwmod_get_context_lost		= _omap4_get_context_lost,
};

/**
 * _enable_preprogram - Pre-program an IP block during the _enable() process
 * @oh: struct omap_hwmod *
 *
 * Some IP blocks (such as AESS) require some additional programming
 * after enable before they can enter idle.  If a function pointer to
 * do so is present in the hwmod data, then call it and pass along the
 * return value; otherwise, return 0.
 */
static int _enable_preprogram(struct omap_hwmod *oh)
{
	if (!oh->class->enable_preprogram)
		return 0;

	return oh->class->enable_preprogram(oh);
}

/**
 * _enable - enable an omap_hwmod
 * @oh: struct omap_hwmod *
 *
 * Enables an omap_hwmod @oh such that the MPU can access the hwmod's
 * register target.  Returns -EINVAL if the hwmod is in the wrong
 * state or passes along the return value of _wait_target_ready().
 */
static int _enable(struct omap_hwmod *oh)
{
	int r;
	int hwsup = 0;

	pr_debug("omap_hwmod: %s: enabling\n", oh->name);

	if (oh->flags & HWMOD_MIN_TIME_STABLE && omap_pm_is_prepared()) {
		s64 tm;

		/* Mandatory apply this delay if last_switch is invallid */
		if (ktime_equal(oh->last_switch, invalkt))
			oh->last_switch = ktime_get();

		tm = ktime_to_us(ktime_sub(ktime_get(), oh->last_switch));

		if (tm > 0 && tm < oh->min_time_stable)
			udelay(oh->min_time_stable - tm);
	}

	/*
	 * hwmods with HWMOD_INIT_NO_IDLE flag set are left
	 * in enabled state at init.
	 * Now that someone is really trying to enable them,
	 * just ensure that the hwmod mux is set.
	 */
	if (oh->_int_flags & _HWMOD_SKIP_ENABLE) {
		/*
		 * If the caller has mux data populated, do the mux'ing
		 * which wouldn't have been done as part of the _enable()
		 * done during setup.
		 */
		if (oh->mux)
			omap_hwmod_mux(oh->mux, _HWMOD_STATE_ENABLED);

		oh->_int_flags &= ~_HWMOD_SKIP_ENABLE;
		return 0;
	}

	if (oh->_state != _HWMOD_STATE_INITIALIZED &&
	    oh->_state != _HWMOD_STATE_IDLE &&
	    oh->_state != _HWMOD_STATE_DISABLED) {
		WARN(1, "omap_hwmod: %s: enabled state can only be entered from initialized, idle, or disabled state\n",
			oh->name);
		return -EINVAL;
	}

	/* Mux pins for device runtime if populated */
	if (oh->mux && (!oh->mux->enabled ||
			((oh->_state == _HWMOD_STATE_IDLE) &&
			 oh->mux->pads_dynamic))) {
		omap_hwmod_mux(oh->mux, _HWMOD_STATE_ENABLED);
		omap_hwmod_reconfigure_io_chain();
	}

	_add_initiator_dep(oh, mpu_oh);

	if (oh->clkdm) {
		/*
		 * A clockdomain must be in SW_SUP before enabling
		 * completely the module. The clockdomain can be set
		 * in HW_AUTO only when the module become ready.
		 */
		hwsup = clkdm_in_hwsup(oh->clkdm);
		r = clkdm_hwmod_enable(oh->clkdm, oh);
		if (r) {
			WARN(1, "omap_hwmod: %s: could not enable clockdomain %s: %d\n",
			     oh->name, oh->clkdm->name, r);
			return r;
		}
	}

	_enable_clocks(oh);
	_enable_module(oh);

	/*
	 * If an IP contains only one HW reset line, then de-assert it to have
	 * the module functional. deassert_hardreset is currently limited only
	 * to processor device-like IPs - IPU, DSP and IVA, so we can safely
	 * call it after enabling clocks.
	 */
	if ((oh->_state == _HWMOD_STATE_INITIALIZED ||
	     oh->_state == _HWMOD_STATE_DISABLED) && oh->rst_lines_cnt == 1)
		_deassert_hardreset(oh, oh->rst_lines[0].name);

	if (arch_hwmod && arch_hwmod->hwmod_update_context_lost)
		arch_hwmod->hwmod_update_context_lost(oh);

	r = _wait_target_ready(oh);
	if (!r) {
		/*
		 * Set the clockdomain to HW_AUTO only if the target is ready,
		 * assuming that the previous state was HW_AUTO
		 */
		if (oh->clkdm && hwsup)
			clkdm_allow_idle(oh->clkdm);

		oh->_state = _HWMOD_STATE_ENABLED;

		/* Access the sysconfig only if the target is ready */
		if (oh->class->sysc) {
			if (!(oh->_int_flags & _HWMOD_SYSCONFIG_LOADED))
				_update_sysc_cache(oh);
			_enable_sysc(oh);
		}
		r = _enable_preprogram(oh);
	} else {
		_omap4_disable_module(oh);
		_disable_clocks(oh);
		pr_debug("omap_hwmod: %s: _wait_target_ready: %d\n",
			 oh->name, r);

		if (oh->clkdm)
			clkdm_hwmod_disable(oh->clkdm, oh);
	}

	if (oh->flags & HWMOD_MIN_TIME_STABLE)
		oh->last_switch = omap_pm_is_prepared() ? ktime_get() : invalkt;

	return r;
}

/**
 * _idle - idle an omap_hwmod
 * @oh: struct omap_hwmod *
 *
 * Idles an omap_hwmod @oh.  This should be called once the hwmod has
 * no further work.  Returns -EINVAL if the hwmod is in the wrong
 * state or returns 0.
 */
static int _idle(struct omap_hwmod *oh)
{
	int r, hwsup = 0;
	pr_debug("omap_hwmod: %s: idling\n", oh->name);

	if (oh->_state != _HWMOD_STATE_ENABLED) {
		WARN(1, "omap_hwmod: %s: idle state can only be entered from enabled state\n",
			oh->name);
		return -EINVAL;
	}

	if (oh->flags & HWMOD_MIN_TIME_STABLE && omap_pm_is_prepared()) {
		s64 tm;

		/* Mandatory apply this delay if last_switch is invallid */
		if (ktime_equal(oh->last_switch, invalkt))
			oh->last_switch = ktime_get();

		tm = ktime_to_us(ktime_sub(ktime_get(), oh->last_switch));

		if (tm > 0 && tm < oh->min_time_stable)
			udelay(oh->min_time_stable - tm);
	}

	if (oh->class->sysc)
		_idle_sysc(oh);
	_del_initiator_dep(oh, mpu_oh);
	if (oh->clkdm && oh->clkdm->flags &&
	    (oh->clkdm->flags & CLKDM_CAN_ENABLE_AUTO) &&
	    (oh->prcm.omap4.modulemode == MODULEMODE_HWCTRL)) {
		/*
		 * For modules with modulemode configured as "auto" and
		 * if this clockdomain supports HW_AUTO mode
		 * the clockdomain must be in SW_WKUP before disabling
		 * a module completely.
		 */
		hwsup = clkdm_in_hwsup(oh->clkdm);
		r = clkdm_hwmod_enable(oh->clkdm, oh);
		if (r) {
			WARN(1, "omap_hwmod: %s: could not enable clockdomain %s: %d\n",
			     oh->name, oh->clkdm->name, r);
			return r;
		}
	}

	_omap4_disable_module(oh);

	/*
	 * The module must be in idle mode before disabling any parents
	 * clocks. Otherwise, the parent clock might be disabled before
	 * the module transition is done, and thus will prevent the
	 * transition to complete properly.
	 */
	_disable_clocks(oh);
	if (oh->clkdm) {
		if (hwsup)
			clkdm_allow_idle(oh->clkdm);
		clkdm_hwmod_disable(oh->clkdm, oh);
	}

	/* Mux pins for device idle if populated */
	if (oh->mux && oh->mux->pads_dynamic) {
		omap_hwmod_mux(oh->mux, _HWMOD_STATE_IDLE);
		omap_hwmod_reconfigure_io_chain();
	}

	oh->_state = _HWMOD_STATE_IDLE;

	if (oh->flags & HWMOD_MIN_TIME_STABLE)
		oh->last_switch = omap_pm_is_prepared() ? ktime_get() : invalkt;

	return 0;
}

/**
 * omap_hwmod_set_ocp_autoidle - set the hwmod's OCP autoidle bit
 * @oh: struct omap_hwmod *
 * @autoidle: desired AUTOIDLE bitfield value (0 or 1)
 *
 * Sets the IP block's OCP autoidle bit in hardware, and updates our
 * local copy. Intended to be used by drivers that require
 * direct manipulation of the AUTOIDLE bits.
 * Returns -EINVAL if @oh is null or is not in the ENABLED state, or passes
 * along the return value from _set_module_autoidle().
 *
 * Any users of this function should be scrutinized carefully.
 */
int omap_hwmod_set_ocp_autoidle(struct omap_hwmod *oh, u8 autoidle)
{
	u32 v;
	int retval = 0;
	unsigned long flags;

	if (!oh || oh->_state != _HWMOD_STATE_ENABLED)
		return -EINVAL;

	spin_lock_irqsave(&oh->_lock, flags);

	v = oh->_sysc_cache;

	retval = _set_module_autoidle(oh, autoidle, &v);

	if (!retval)
		_write_sysconfig(v, oh);

	spin_unlock_irqrestore(&oh->_lock, flags);

	return retval;
}

/**
 * _shutdown - shutdown an omap_hwmod
 * @oh: struct omap_hwmod *
 *
 * Shut down an omap_hwmod @oh.  This should be called when the driver
 * used for the hwmod is removed or unloaded or if the driver is not
 * used by the system.  Returns -EINVAL if the hwmod is in the wrong
 * state or returns 0.
 */
static int _shutdown(struct omap_hwmod *oh)
{
	int ret, hwsup = 0;
	u8 prev_state;

	if (oh->_state != _HWMOD_STATE_IDLE &&
	    oh->_state != _HWMOD_STATE_ENABLED) {
		WARN(1, "omap_hwmod: %s: disabled state can only be entered from idle, or enabled state\n",
			oh->name);
		return -EINVAL;
	}

	pr_debug("omap_hwmod: %s: disabling\n", oh->name);

	if (oh->class->pre_shutdown) {
		prev_state = oh->_state;
		if (oh->_state == _HWMOD_STATE_IDLE)
			_enable(oh);
		ret = oh->class->pre_shutdown(oh);
		if (ret) {
			if (prev_state == _HWMOD_STATE_IDLE)
				_idle(oh);
			return ret;
		}
	}

	if (oh->class->sysc) {
		if (oh->_state == _HWMOD_STATE_IDLE)
			_enable(oh);
		_shutdown_sysc(oh);
	}

	/* clocks and deps are already disabled in idle */
	if (oh->_state == _HWMOD_STATE_ENABLED) {
		_del_initiator_dep(oh, mpu_oh);
		if (oh->clkdm && oh->clkdm->flags &&
		    (oh->clkdm->flags & CLKDM_CAN_ENABLE_AUTO) &&
		    (oh->prcm.omap4.modulemode == MODULEMODE_HWCTRL)) {
			/*
			 * For modules with modulemode configured as "auto" and
			 * if this clockdomain supports HW_AUTO mode
			 * the clockdomain must be in SW_WKUP before disabling
			 * a module completely.
			 */
			hwsup = clkdm_in_hwsup(oh->clkdm);
			ret = clkdm_hwmod_enable(oh->clkdm, oh);
			if (ret) {
				WARN(1, "omap_hwmod: %s: could not enable clockdomain %s: %d\n",
				     oh->name, oh->clkdm->name, ret);
				return ret;
			}
		}
		/* XXX what about the other system initiators here? dma, dsp */
		_omap4_disable_module(oh);
		_disable_clocks(oh);
		if (oh->clkdm) {
			if (hwsup)
				clkdm_allow_idle(oh->clkdm);
			clkdm_hwmod_disable(oh->clkdm, oh);
		}
	}
	/* XXX Should this code also force-disable the optional clocks? */

	/*
	 * If an IP contains only one HW reset line, then assert it
	 * after disabling the clocks and before shutting down the IP.
	 */
	if (oh->rst_lines_cnt == 1)
		_assert_hardreset(oh, oh->rst_lines[0].name);

	/* Mux pins to safe mode or use populated off mode values */
	if (oh->mux)
		omap_hwmod_mux(oh->mux, _HWMOD_STATE_DISABLED);

	oh->_state = _HWMOD_STATE_DISABLED;

	return 0;
}

/**
 * _setup - do initial configuration of omap_hwmod
 * @oh: struct omap_hwmod *
 *
 * Writes the CLOCKACTIVITY bits @clockact to the hwmod @oh
 * OCP_SYSCONFIG register.  Returns 0.
 */
static int _setup(struct omap_hwmod *oh, void *data)
{
	int i, r;
	u8 postsetup_state;

	if (oh->_state != _HWMOD_STATE_CLKS_INITED)
		return 0;

	/* Set iclk autoidle mode */
	if (oh->slaves_cnt > 0) {
		for (i = 0; i < oh->slaves_cnt; i++) {
			struct omap_hwmod_ocp_if *os = oh->slaves[i];
			struct clk *c = os->_clk;

			if (!c)
				continue;

			if (os->flags & OCPIF_SWSUP_IDLE) {
				/* XXX omap_iclk_deny_idle(c); */
			} else {
				/* XXX omap_iclk_allow_idle(c); */
				clk_enable(c);
			}
		}
	}

	oh->_state = _HWMOD_STATE_INITIALIZED;

	/*
	 * In the case of hwmod with hardreset that should not be
	 * de-assert at boot time, we have to keep the module
	 * initialized, because we cannot enable it properly with the
	 * reset asserted. Exit without warning because that behavior is
	 * expected.
	 */
	if ((oh->flags & HWMOD_INIT_NO_RESET) && oh->rst_lines_cnt == 1)
		return 0;

	r = _enable(oh);
	if (r) {
#ifndef CONFIG_MACH_OMAP_5430ZEBU
		pr_warning("omap_hwmod: %s: cannot be enabled (%d)\n",
			   oh->name, oh->_state);
#endif
		return 0;
	}

	if (!(oh->flags & HWMOD_INIT_NO_RESET))
		_reset(oh);

	postsetup_state = oh->_postsetup_state;
	if (postsetup_state == _HWMOD_STATE_UNKNOWN)
		postsetup_state = _HWMOD_STATE_ENABLED;

	/*
	 * XXX HWMOD_INIT_NO_IDLE does not belong in hwmod data -
	 * it should be set by the core code as a runtime flag during startup
	 */
	if ((oh->flags & HWMOD_INIT_NO_IDLE) &&
	    (postsetup_state == _HWMOD_STATE_IDLE)) {
		oh->_int_flags |= _HWMOD_SKIP_ENABLE;
		postsetup_state = _HWMOD_STATE_ENABLED;
	}

	/*
	 * Process HWMOD_NO_ACCESS: module should be disabled and not accessible
	 */
	if (oh->flags & HWMOD_ACCESS_DISABLED)
		postsetup_state = _HWMOD_STATE_DISABLED;

	if (postsetup_state == _HWMOD_STATE_IDLE)
		_idle(oh);
	else if (postsetup_state == _HWMOD_STATE_DISABLED)
		_shutdown(oh);
	else if (postsetup_state != _HWMOD_STATE_ENABLED)
		WARN(1, "hwmod: %s: unknown postsetup state %d! defaulting to enabled\n",
		     oh->name, postsetup_state);

	return 0;
}

/**
 * _register - register a struct omap_hwmod
 * @oh: struct omap_hwmod *
 *
 * Registers the omap_hwmod @oh.  Returns -EEXIST if an omap_hwmod
 * already has been registered by the same name; -EINVAL if the
 * omap_hwmod is in the wrong state, if @oh is NULL, if the
 * omap_hwmod's class field is NULL; if the omap_hwmod is missing a
 * name, or if the omap_hwmod's class is missing a name; or 0 upon
 * success.
 *
 * XXX The data should be copied into bootmem, so the original data
 * should be marked __initdata and freed after init.  This would allow
 * unneeded omap_hwmods to be freed on multi-OMAP configurations.  Note
 * that the copy process would be relatively complex due to the large number
 * of substructures.
 */
static int __init _register(struct omap_hwmod *oh)
{
	int ms_id;

	if (!oh || !oh->name || !oh->class || !oh->class->name ||
	    (oh->_state != _HWMOD_STATE_UNKNOWN))
		return -EINVAL;

	pr_debug("omap_hwmod: %s: registering\n", oh->name);

	if (_lookup(oh->name))
		return -EEXIST;

	ms_id = _find_mpu_port_index(oh);
	if (!IS_ERR_VALUE(ms_id))
		oh->_mpu_port_index = ms_id;
	else
		oh->_int_flags |= _HWMOD_NO_MPU_PORT;

	list_add_tail(&oh->node, &omap_hwmod_list);

	spin_lock_init(&oh->_lock);

	oh->_state = _HWMOD_STATE_REGISTERED;

	/*
	 * XXX Rather than doing a strcmp(), this should test a flag
	 * set in the hwmod data, inserted by the autogenerator code.
	 */
	if (!strcmp(oh->name, MPU_INITIATOR_NAME))
		mpu_oh = oh;

	return 0;
}


/* Public functions */

u32 omap_hwmod_read(struct omap_hwmod *oh, u16 reg_offs)
{
	if (oh->flags & HWMOD_16BIT_REG)
		return __raw_readw(oh->_mpu_rt_va + reg_offs);
	else
		return __raw_readl(oh->_mpu_rt_va + reg_offs);
}

void omap_hwmod_write(u32 v, struct omap_hwmod *oh, u16 reg_offs)
{
	if (oh->flags & HWMOD_16BIT_REG)
		__raw_writew(v, oh->_mpu_rt_va + reg_offs);
	else
		__raw_writel(v, oh->_mpu_rt_va + reg_offs);
}

/**
 * omap_hwmod_softreset - reset a module via SYSCONFIG.SOFTRESET bit
 * @oh: struct omap_hwmod *
 *
 * This is a public function exposed to drivers. Some drivers may need to do
 * some settings before and after resetting the device.  Those drivers after
 * doing the necessary settings could use this function to start a reset by
 * setting the SYSCONFIG.SOFTRESET bit.
 */
int omap_hwmod_softreset(struct omap_hwmod *oh)
{
	u32 v;
	int ret;

	if (!oh || !(oh->_sysc_cache))
		return -EINVAL;

	v = oh->_sysc_cache;
	ret = _set_softreset(oh, &v);
	if (ret)
		goto error;
	_write_sysconfig(v, oh);

	ret = _clear_softreset(oh, &v);
	if (ret)
		goto error;
	_write_sysconfig(v, oh);

error:
	return ret;
}

/**
 * omap_hwmod_set_slave_idlemode - set the hwmod's OCP slave idlemode
 * @oh: struct omap_hwmod *
 * @idlemode: SIDLEMODE field bits (shifted to bit 0)
 *
 * Sets the IP block's OCP slave idlemode in hardware, and updates our
 * local copy.  Intended to be used by drivers that have some erratum
 * that requires direct manipulation of the SIDLEMODE bits.  Returns
 * -EINVAL if @oh is null, or passes along the return value from
 * _set_slave_idlemode().
 *
 * XXX Does this function have any current users?  If not, we should
 * remove it; it is better to let the rest of the hwmod code handle this.
 * Any users of this function should be scrutinized carefully.
 */
int omap_hwmod_set_slave_idlemode(struct omap_hwmod *oh, u8 idlemode)
{
	u32 v;
	int retval = 0;

	if (!oh)
		return -EINVAL;

	v = oh->_sysc_cache;

	retval = _set_slave_idlemode(oh, idlemode, &v);
	if (!retval)
		_write_sysconfig(v, oh);

	return retval;
}

/**
 * omap_hwmod_lookup - look up a registered omap_hwmod by name
 * @name: name of the omap_hwmod to look up
 *
 * Given a @name of an omap_hwmod, return a pointer to the registered
 * struct omap_hwmod *, or NULL upon error.
 */
struct omap_hwmod *omap_hwmod_lookup(const char *name)
{
	struct omap_hwmod *oh;

	if (!name)
		return NULL;

	oh = _lookup(name);
	/* check access flag */
	if (oh && oh->flags & HWMOD_ACCESS_DISABLED) {
		pr_warning("omap_hwmod: %s: access denied\n", oh->name);
		oh = NULL;
	}

	return oh;
}

/**
 * omap_hwmod_for_each - call function for each registered omap_hwmod
 * @fn: pointer to a callback function
 * @data: void * data to pass to callback function
 *
 * Call @fn for each registered omap_hwmod, passing @data to each
 * function.  @fn must return 0 for success or any other value for
 * failure.  If @fn returns non-zero, the iteration across omap_hwmods
 * will stop and the non-zero return value will be passed to the
 * caller of omap_hwmod_for_each().  @fn is called with
 * omap_hwmod_for_each() held.
 */
int omap_hwmod_for_each(int (*fn)(struct omap_hwmod *oh, void *data),
			void *data)
{
	struct omap_hwmod *temp_oh;
	int ret = 0;

	if (!fn)
		return -EINVAL;

	list_for_each_entry(temp_oh, &omap_hwmod_list, node) {
		ret = (*fn)(temp_oh, data);
		if (ret)
			break;
	}

	return ret;
}

/**
 * omap_hwmod_register - register an array of hwmods
 * @ohs: pointer to an array of omap_hwmods to register
 *
 * Intended to be called early in boot before the clock framework is
 * initialized.  If @ohs is not null, will register all omap_hwmods
 * listed in @ohs that are valid for this chip.  Returns 0.
 */
int __init omap_hwmod_register(struct omap_hwmod **ohs)
{
	int r, i;

	if (!ohs)
		return 0;

	i = 0;
	do {
		r = _register(ohs[i]);
		WARN(r, "omap_hwmod: %s: _register returned %d\n", ohs[i]->name,
		     r);
	} while (ohs[++i]);

	return 0;
}


/**
 * omap_hwmod_register_flags - set/clear hwmod flags
 * @ohs: pointer to an array of omap_hwmods to register new flags
 * @set_flags: flags which have to be set
 * @clear_flags: flags which have to be cleared
 *
 * Intended to be called early in boot before the clock framework is
 * initialized.  If @ohs is not null, will set/clear flags.
 * Allowed to be called only in REGISTERED state.
 * Returns 0.
 */
int __init omap_hwmod_register_flags(struct omap_hwmod **ohs,
					u32 set_flags, u32 clear_flags)
{
	int i = 0;

	if (!ohs)
		return 0;

	/* scan through list of modules */
	do {
		/* allow to be called only in registered state */
		if (ohs[i]->_state != _HWMOD_STATE_REGISTERED) {
			i++;
			continue;
		}

		ohs[i]->flags = (ohs[i]->flags & ~clear_flags) | set_flags;
	} while (ohs[++i]);

	return 0;
}

/*
 * _populate_mpu_rt_base - populate the virtual address for a hwmod
 *
 * Must be called only from omap_hwmod_setup_*() so ioremap works properly.
 * Assumes the caller takes care of locking if needed.
 */
static int __init _populate_mpu_rt_base(struct omap_hwmod *oh, void *data)
{
	if (oh->_state != _HWMOD_STATE_REGISTERED)
		return 0;

	if (oh->_int_flags & _HWMOD_NO_MPU_PORT)
		return 0;

	oh->_mpu_rt_va = _find_mpu_rt_base(oh, oh->_mpu_port_index);

	return 0;
}

/**
 * omap_hwmod_setup_one - set up a single hwmod
 * @oh_name: const char * name of the already-registered hwmod to set up
 *
 * Must be called after omap2_clk_init().  Resolves the struct clk
 * names to struct clk pointers for each registered omap_hwmod.  Also
 * calls _setup() on each hwmod.  Returns -EINVAL upon error or 0 upon
 * success.
 */
int __init omap_hwmod_setup_one(const char *oh_name)
{
	struct omap_hwmod *oh;
	int r;

	pr_debug("omap_hwmod: %s: %s\n", oh_name, __func__);

	if (!mpu_oh) {
		pr_err("omap_hwmod: %s: cannot setup_one: MPU initiator hwmod %s not yet registered\n",
		       oh_name, MPU_INITIATOR_NAME);
		return -EINVAL;
	}

	oh = _lookup(oh_name);
	if (!oh) {
		WARN(1, "omap_hwmod: %s: hwmod not yet registered\n", oh_name);
		return -EINVAL;
	}

	if (mpu_oh->_state == _HWMOD_STATE_REGISTERED && oh != mpu_oh)
		omap_hwmod_setup_one(MPU_INITIATOR_NAME);

	r = _populate_mpu_rt_base(oh, NULL);
	if (IS_ERR_VALUE(r)) {
		WARN(1, "omap_hwmod: %s: couldn't set mpu_rt_base\n", oh_name);
		return -EINVAL;
	}

	r = _init_clocks(oh, NULL);
	if (IS_ERR_VALUE(r)) {
		WARN(1, "omap_hwmod: %s: couldn't init clocks\n", oh_name);
		return -EINVAL;
	}

	_setup(oh, NULL);

	return 0;
}

#if defined(CONFIG_DEBUG_FS)

static struct dentry *omap_hwmod_dbg_dir;

/* internal hwmod states */
static const char *hwmod_states[_HWMOD_STATE_COUNT] = {
	[_HWMOD_STATE_UNKNOWN]          = "unknown",
	[_HWMOD_STATE_REGISTERED]       = "registered",
	[_HWMOD_STATE_CLKS_INITED]      = "clks_inited",
	[_HWMOD_STATE_INITIALIZED]      = "initialized",
	[_HWMOD_STATE_ENABLED]          = "enabled",
	[_HWMOD_STATE_IDLE]             = "idle",
	[_HWMOD_STATE_DISABLED]         = "disabled",
};

const char *_state_str(u8 state)
{
	if (state >= _HWMOD_STATE_COUNT)
		return "invalid_state";
	return hwmod_states[state];
}

static int omap_hwmod_dbg_show(struct seq_file *s, void *unused)
{
	struct omap_hwmod *oh;

	list_for_each_entry(oh, &omap_hwmod_list, node) {
		seq_printf(s, "name: %16s, state %d/%s\n", oh->name,
			oh->_state, _state_str(oh->_state));
	}

	return 0;
}

static int omap_hwmod_dbg_open(struct inode *inode, struct file *file)
{
	return single_open(file, omap_hwmod_dbg_show, inode->i_private);
}

static const struct file_operations omap_hwmod_dbg_fops = {
	.open		= omap_hwmod_dbg_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void __init omap_hwmod_dbg_init(void)
{
	omap_hwmod_dbg_dir = debugfs_create_dir("omap_hwmod", NULL);
	if (!omap_hwmod_dbg_dir)
		return;

	(void)debugfs_create_file("state", S_IRUGO, omap_hwmod_dbg_dir,
					NULL, &omap_hwmod_dbg_fops);
}

#endif	/* CONFIG_DEBUG_FS */


/**
 * omap_hwmod_setup - do some post-clock framework initialization
 *
 * Must be called after omap2_clk_init().  Resolves the struct clk names
 * to struct clk pointers for each registered omap_hwmod.  Also calls
 * _setup() on each hwmod.  Returns 0 upon success.
 */
static int __init omap_hwmod_setup_all(void)
{
	int r;

	if (cpu_is_omap44xx() || cpu_is_omap54xx())
		arch_hwmod = &omap4_hwmod_ops;

	if (!mpu_oh) {
		pr_err("omap_hwmod: %s: MPU initiator hwmod %s not yet registered\n",
		       __func__, MPU_INITIATOR_NAME);
		return -EINVAL;
	}

	r = omap_hwmod_for_each(_populate_mpu_rt_base, NULL);

	r = omap_hwmod_for_each(_init_clocks, NULL);
	WARN(IS_ERR_VALUE(r),
	     "omap_hwmod: %s: _init_clocks failed\n", __func__);

	omap_hwmod_for_each(_setup, NULL);

#ifdef CONFIG_DEBUG_FS
	omap_hwmod_dbg_init();
#endif

	return 0;
}
core_initcall(omap_hwmod_setup_all);

/**
 * omap_hwmod_enable - enable an omap_hwmod
 * @oh: struct omap_hwmod *
 *
 * Enable an omap_hwmod @oh.  Intended to be called by omap_device_enable().
 * Returns -EINVAL on error or passes along the return value from _enable().
 */
int omap_hwmod_enable(struct omap_hwmod *oh)
{
	int r;
	unsigned long flags;

	if (!oh)
		return -EINVAL;

	spin_lock_irqsave(&oh->_lock, flags);
	r = _enable(oh);
	spin_unlock_irqrestore(&oh->_lock, flags);

	return r;
}

/**
 * omap_hwmod_idle - idle an omap_hwmod
 * @oh: struct omap_hwmod *
 *
 * Idle an omap_hwmod @oh.  Intended to be called by omap_device_idle().
 * Returns -EINVAL on error or passes along the return value from _idle().
 */
int omap_hwmod_idle(struct omap_hwmod *oh)
{
	unsigned long flags;

	if (!oh)
		return -EINVAL;

	spin_lock_irqsave(&oh->_lock, flags);
	_idle(oh);
	spin_unlock_irqrestore(&oh->_lock, flags);

	return 0;
}

/**
 * omap_hwmod_shutdown - shutdown an omap_hwmod
 * @oh: struct omap_hwmod *
 *
 * Shutdown an omap_hwmod @oh.  Intended to be called by
 * omap_device_shutdown().  Returns -EINVAL on error or passes along
 * the return value from _shutdown().
 */
int omap_hwmod_shutdown(struct omap_hwmod *oh)
{
	unsigned long flags;

	if (!oh)
		return -EINVAL;

	spin_lock_irqsave(&oh->_lock, flags);
	_shutdown(oh);
	spin_unlock_irqrestore(&oh->_lock, flags);

	return 0;
}

/**
 * omap_hwmod_enable_clocks - enable main_clk, all interface clocks
 * @oh: struct omap_hwmod *oh
 *
 * Intended to be called by the omap_device code.
 */
int omap_hwmod_enable_clocks(struct omap_hwmod *oh)
{
	unsigned long flags;

	spin_lock_irqsave(&oh->_lock, flags);
	_enable_clocks(oh);
	_enable_module(oh);
	spin_unlock_irqrestore(&oh->_lock, flags);

	return 0;
}

/**
 * omap_hwmod_disable_clocks - disable main_clk, all interface clocks
 * @oh: struct omap_hwmod *oh
 *
 * Intended to be called by the omap_device code.
 */
int omap_hwmod_disable_clocks(struct omap_hwmod *oh)
{
	unsigned long flags;

	spin_lock_irqsave(&oh->_lock, flags);
	_omap4_disable_module(oh);

	/*
	 * The module must be in idle mode before disabling any parents
	 * clocks. Otherwise, the parent clock might be disabled before
	 * the module transition is done, and thus will prevent the
	 * transition to complete properly.
	 */
	_disable_clocks(oh);
	spin_unlock_irqrestore(&oh->_lock, flags);

	return 0;
}

/**
 * omap_hwmod_ocp_barrier - wait for posted writes against the hwmod to complete
 * @oh: struct omap_hwmod *oh
 *
 * Intended to be called by drivers and core code when all posted
 * writes to a device must complete before continuing further
 * execution (for example, after clearing some device IRQSTATUS
 * register bits)
 *
 * XXX what about targets with multiple OCP threads?
 */
void omap_hwmod_ocp_barrier(struct omap_hwmod *oh)
{
	BUG_ON(!oh);

	if (!oh->class->sysc || !oh->class->sysc->sysc_flags) {
		WARN(1, "omap_device: %s: OCP barrier impossible due to device configuration\n",
			oh->name);
		return;
	}

	/*
	 * Forces posted writes to complete on the OCP thread handling
	 * register writes
	 */
	omap_hwmod_read(oh, oh->class->sysc->sysc_offs);
}

/**
 * omap_hwmod_reset - reset the hwmod
 * @oh: struct omap_hwmod *
 *
 * Under some conditions, a driver may wish to reset the entire device.
 * Called from omap_device code.  Returns -EINVAL on error or passes along
 * the return value from _reset().
 */
int omap_hwmod_reset(struct omap_hwmod *oh)
{
	int r;
	unsigned long flags;

	if (!oh)
		return -EINVAL;

	spin_lock_irqsave(&oh->_lock, flags);
	r = _reset(oh);
	spin_unlock_irqrestore(&oh->_lock, flags);

	return r;
}

/**
 * omap_hwmod_count_resources - count number of struct resources needed by hwmod
 * @oh: struct omap_hwmod *
 * @res: pointer to the first element of an array of struct resource to fill
 *
 * Count the number of struct resource array elements necessary to
 * contain omap_hwmod @oh resources.  Intended to be called by code
 * that registers omap_devices.  Intended to be used to determine the
 * size of a dynamically-allocated struct resource array, before
 * calling omap_hwmod_fill_resources().  Returns the number of struct
 * resource array elements needed.
 *
 * XXX This code is not optimized.  It could attempt to merge adjacent
 * resource IDs.
 *
 */
int omap_hwmod_count_resources(struct omap_hwmod *oh)
{
	int ret, i;

	ret = _count_mpu_irqs(oh) + _count_sdma_reqs(oh);

	for (i = 0; i < oh->slaves_cnt; i++)
		ret += _count_ocp_if_addr_spaces(oh->slaves[i]);

	return ret;
}

/**
 * omap_hwmod_fill_resources - fill struct resource array with hwmod data
 * @oh: struct omap_hwmod *
 * @res: pointer to the first element of an array of struct resource to fill
 *
 * Fill the struct resource array @res with resource data from the
 * omap_hwmod @oh.  Intended to be called by code that registers
 * omap_devices.  See also omap_hwmod_count_resources().  Returns the
 * number of array elements filled.
 */
int omap_hwmod_fill_resources(struct omap_hwmod *oh, struct resource *res)
{
	int i, j, mpu_irqs_cnt, sdma_reqs_cnt;
	int r = 0;

	/* For each IRQ, DMA, memory area, fill in array.*/

	mpu_irqs_cnt = _count_mpu_irqs(oh);
	for (i = 0; i < mpu_irqs_cnt; i++) {
		(res + r)->name = (oh->mpu_irqs + i)->name;
		(res + r)->start = (oh->mpu_irqs + i)->irq;
		(res + r)->end = (oh->mpu_irqs + i)->irq;
		(res + r)->flags = IORESOURCE_IRQ;
		r++;
	}

	sdma_reqs_cnt = _count_sdma_reqs(oh);
	for (i = 0; i < sdma_reqs_cnt; i++) {
		(res + r)->name = (oh->sdma_reqs + i)->name;
		(res + r)->start = (oh->sdma_reqs + i)->dma_req;
		(res + r)->end = (oh->sdma_reqs + i)->dma_req;
		(res + r)->flags = IORESOURCE_DMA;
		r++;
	}

	for (i = 0; i < oh->slaves_cnt; i++) {
		struct omap_hwmod_ocp_if *os;
		int addr_cnt;

		os = oh->slaves[i];
		addr_cnt = _count_ocp_if_addr_spaces(os);

		for (j = 0; j < addr_cnt; j++) {
			(res + r)->name = (os->addr + j)->name;
			(res + r)->start = (os->addr + j)->pa_start;
			(res + r)->end = (os->addr + j)->pa_end;
			(res + r)->flags = IORESOURCE_MEM;
			r++;
		}
	}

	return r;
}

/**
 * omap_hwmod_get_pwrdm - return pointer to this module's main powerdomain
 * @oh: struct omap_hwmod *
 *
 * Return the powerdomain pointer associated with the OMAP module
 * @oh's main clock.  If @oh does not have a main clk, return the
 * powerdomain associated with the interface clock associated with the
 * module's MPU port. (XXX Perhaps this should use the SDMA port
 * instead?)  Returns NULL on error, or a struct powerdomain * on
 * success.
 */
struct powerdomain *omap_hwmod_get_pwrdm(struct omap_hwmod *oh)
{
	struct clk *c;

	if (!oh)
		return NULL;

	if (oh->clkdm) {
		return oh->clkdm->pwrdm.ptr;
	} else if (oh->_clk) {
		c = oh->_clk;
	} else {
		if (oh->_int_flags & _HWMOD_NO_MPU_PORT)
			return NULL;
		c = oh->slaves[oh->_mpu_port_index]->_clk;
	}

	if (!c->clkdm)
		return NULL;

	return c->clkdm->pwrdm.ptr;

}

/**
 * omap_hwmod_get_mpu_rt_va - return the module's base address (for the MPU)
 * @oh: struct omap_hwmod *
 *
 * Returns the virtual address corresponding to the beginning of the
 * module's register target, in the address range that is intended to
 * be used by the MPU.  Returns the virtual address upon success or NULL
 * upon error.
 */
void __iomem *omap_hwmod_get_mpu_rt_va(struct omap_hwmod *oh)
{
	if (!oh)
		return NULL;

	if (oh->_int_flags & _HWMOD_NO_MPU_PORT)
		return NULL;

	if (oh->_state == _HWMOD_STATE_UNKNOWN)
		return NULL;

	return oh->_mpu_rt_va;
}

/**
 * omap_hwmod_add_initiator_dep - add sleepdep from @init_oh to @oh
 * @oh: struct omap_hwmod *
 * @init_oh: struct omap_hwmod * (initiator)
 *
 * Add a sleep dependency between the initiator @init_oh and @oh.
 * Intended to be called by DSP/Bridge code via platform_data for the
 * DSP case; and by the DMA code in the sDMA case.  DMA code, *Bridge
 * code needs to add/del initiator dependencies dynamically
 * before/after accessing a device.  Returns the return value from
 * _add_initiator_dep().
 *
 * XXX Keep a usecount in the clockdomain code
 */
int omap_hwmod_add_initiator_dep(struct omap_hwmod *oh,
				 struct omap_hwmod *init_oh)
{
	return _add_initiator_dep(oh, init_oh);
}

/*
 * XXX what about functions for drivers to save/restore ocp_sysconfig
 * for context save/restore operations?
 */

/**
 * omap_hwmod_del_initiator_dep - remove sleepdep from @init_oh to @oh
 * @oh: struct omap_hwmod *
 * @init_oh: struct omap_hwmod * (initiator)
 *
 * Remove a sleep dependency between the initiator @init_oh and @oh.
 * Intended to be called by DSP/Bridge code via platform_data for the
 * DSP case; and by the DMA code in the sDMA case.  DMA code, *Bridge
 * code needs to add/del initiator dependencies dynamically
 * before/after accessing a device.  Returns the return value from
 * _del_initiator_dep().
 *
 * XXX Keep a usecount in the clockdomain code
 */
int omap_hwmod_del_initiator_dep(struct omap_hwmod *oh,
				 struct omap_hwmod *init_oh)
{
	return _del_initiator_dep(oh, init_oh);
}

/**
 * omap_hwmod_enable_wakeup - allow device to wake up the system
 * @oh: struct omap_hwmod *
 *
 * Sets the module OCP socket ENAWAKEUP bit to allow the module to
 * send wakeups to the PRCM, and enable I/O ring wakeup events for
 * this IP block if it has dynamic mux entries.  Eventually this
 * should set PRCM wakeup registers to cause the PRCM to receive
 * wakeup events from the module.  Does not set any wakeup routing
 * registers beyond this point - if the module is to wake up any other
 * module or subsystem, that must be set separately.  Called by
 * omap_device code.  Returns -EINVAL on error or 0 upon success.
 */
int omap_hwmod_enable_wakeup(struct omap_hwmod *oh)
{
	unsigned long flags;
	u32 v;

	spin_lock_irqsave(&oh->_lock, flags);

	if (oh->class->sysc &&
	    (oh->class->sysc->sysc_flags & SYSC_HAS_ENAWAKEUP)) {
		v = oh->_sysc_cache;
		_enable_wakeup(oh, &v);
		_write_sysconfig(v, oh);
	}

	_enable_module_level_wakeup(oh, true);
	_set_idle_ioring_wakeup(oh, true);
	spin_unlock_irqrestore(&oh->_lock, flags);

	return 0;
}

/**
 * omap_hwmod_disable_wakeup - prevent device from waking the system
 * @oh: struct omap_hwmod *
 *
 * Clears the module OCP socket ENAWAKEUP bit to prevent the module
 * from sending wakeups to the PRCM, and disable I/O ring wakeup
 * events for this IP block if it has dynamic mux entries.  Eventually
 * this should clear PRCM wakeup registers to cause the PRCM to ignore
 * wakeup events from the module.  Does not set any wakeup routing
 * registers beyond this point - if the module is to wake up any other
 * module or subsystem, that must be set separately.  Called by
 * omap_device code.  Returns -EINVAL on error or 0 upon success.
 */
int omap_hwmod_disable_wakeup(struct omap_hwmod *oh)
{
	unsigned long flags;
	u32 v;

	spin_lock_irqsave(&oh->_lock, flags);

	if (oh->class->sysc &&
	    (oh->class->sysc->sysc_flags & SYSC_HAS_ENAWAKEUP)) {
		v = oh->_sysc_cache;
		_disable_wakeup(oh, &v);
		_write_sysconfig(v, oh);
	}

	_enable_module_level_wakeup(oh, false);
	_set_idle_ioring_wakeup(oh, false);
	spin_unlock_irqrestore(&oh->_lock, flags);

	return 0;
}

/**
 * omap_hwmod_assert_hardreset - assert the HW reset line of submodules
 * contained in the hwmod module.
 * @oh: struct omap_hwmod *
 * @name: name of the reset line to lookup and assert
 *
 * Some IP like dsp, ipu or iva contain processor that require
 * an HW reset line to be assert / deassert in order to enable fully
 * the IP.  Returns -EINVAL if @oh is null or if the operation is not
 * yet supported on this OMAP; otherwise, passes along the return value
 * from _assert_hardreset().
 */
int omap_hwmod_assert_hardreset(struct omap_hwmod *oh, const char *name)
{
	int ret;
	unsigned long flags;

	if (!oh)
		return -EINVAL;

	spin_lock_irqsave(&oh->_lock, flags);
	ret = _assert_hardreset(oh, name);
	spin_unlock_irqrestore(&oh->_lock, flags);

	return ret;
}

/**
 * omap_hwmod_deassert_hardreset - deassert the HW reset line of submodules
 * contained in the hwmod module.
 * @oh: struct omap_hwmod *
 * @name: name of the reset line to look up and deassert
 *
 * Some IP like dsp, ipu or iva contain processor that require
 * an HW reset line to be assert / deassert in order to enable fully
 * the IP.  Returns -EINVAL if @oh is null or if the operation is not
 * yet supported on this OMAP; otherwise, passes along the return value
 * from _deassert_hardreset().
 */
int omap_hwmod_deassert_hardreset(struct omap_hwmod *oh, const char *name)
{
	int ret;
	unsigned long flags;

	if (!oh)
		return -EINVAL;

	spin_lock_irqsave(&oh->_lock, flags);
	ret = _deassert_hardreset(oh, name);
	spin_unlock_irqrestore(&oh->_lock, flags);

	return ret;
}

/**
 * omap_hwmod_read_hardreset - read the HW reset line state of submodules
 * contained in the hwmod module
 * @oh: struct omap_hwmod *
 * @name: name of the reset line to look up and read
 *
 * Return the current state of the hwmod @oh's reset line named @name:
 * returns -EINVAL upon parameter error or if this operation
 * is unsupported on the current OMAP; otherwise, passes along the return
 * value from _read_hardreset().
 */
int omap_hwmod_read_hardreset(struct omap_hwmod *oh, const char *name)
{
	int ret;
	unsigned long flags;

	if (!oh)
		return -EINVAL;

	spin_lock_irqsave(&oh->_lock, flags);
	ret = _read_hardreset(oh, name);
	spin_unlock_irqrestore(&oh->_lock, flags);

	return ret;
}

/**
 * omap_hwmod_for_each_by_class - call @fn for each hwmod of class @classname
 * @classname: struct omap_hwmod_class name to search for
 * @fn: callback function pointer to call for each hwmod in class @classname
 * @user: arbitrary context data to pass to the callback function
 *
 * For each omap_hwmod of class @classname, call @fn.
 * If the callback function returns something other than
 * zero, the iterator is terminated, and the callback function's return
 * value is passed back to the caller.  Returns 0 upon success, -EINVAL
 * if @classname or @fn are NULL, or passes back the error code from @fn.
 */
int omap_hwmod_for_each_by_class(const char *classname,
				 int (*fn)(struct omap_hwmod *oh,
					   void *user),
				 void *user)
{
	struct omap_hwmod *temp_oh;
	int ret = 0;

	if (!classname || !fn)
		return -EINVAL;

	pr_debug("omap_hwmod: %s: looking for modules of class %s\n",
		 __func__, classname);

	list_for_each_entry(temp_oh, &omap_hwmod_list, node) {
		if (!strcmp(temp_oh->class->name, classname)) {
			pr_debug("omap_hwmod: %s: %s: calling callback fn\n",
				 __func__, temp_oh->name);
			ret = (*fn)(temp_oh, user);
			if (ret)
				break;
		}
	}

	if (ret)
		pr_debug("omap_hwmod: %s: iterator terminated early: %d\n",
			 __func__, ret);

	return ret;
}

/**
 * omap_hwmod_set_postsetup_state - set the post-_setup() state for this hwmod
 * @oh: struct omap_hwmod *
 * @state: state that _setup() should leave the hwmod in
 *
 * Sets the hwmod state that @oh will enter at the end of _setup()
 * (called by omap_hwmod_setup_*()).  Only valid to call between
 * calling omap_hwmod_register() and omap_hwmod_setup_*().  Returns
 * 0 upon success or -EINVAL if there is a problem with the arguments
 * or if the hwmod is in the wrong state.
 */
int omap_hwmod_set_postsetup_state(struct omap_hwmod *oh, u8 state)
{
	int ret;
	unsigned long flags;

	if (!oh)
		return -EINVAL;

	if (state != _HWMOD_STATE_DISABLED &&
	    state != _HWMOD_STATE_ENABLED &&
	    state != _HWMOD_STATE_IDLE)
		return -EINVAL;

	spin_lock_irqsave(&oh->_lock, flags);

	if (oh->_state != _HWMOD_STATE_REGISTERED) {
		ret = -EINVAL;
		goto ohsps_unlock;
	}

	oh->_postsetup_state = state;
	ret = 0;

ohsps_unlock:
	spin_unlock_irqrestore(&oh->_lock, flags);

	return ret;
}

/**
 * omap_hwmod_set_wakeuplat_constraint - Set or update a wake-up latency
 * constraint
 * @oh: struct omap_hwmod* to which the target device belongs to.
 * @cookie: identifier of the constraints list for @oh.
 * @min_latency: the minimum allowed wake-up latency for @oh.
 *
 * Returns the return value from pwrdm_wakeuplat_update_constraint(),
 * or -EINVAL in case of invalid parameters.
 */
int omap_hwmod_set_wakeuplat_constraint(struct omap_hwmod *oh, void *cookie,
					long min_latency)
{
	struct powerdomain *pwrdm = omap_hwmod_get_pwrdm(oh);

	if (!pwrdm)
		return -EINVAL;

	return pwrdm_wakeuplat_update_constraint(pwrdm, cookie, min_latency);
}

/**
 * omap_hwmod_remove_wakeuplat_constraint - Release a wake-up latency
 * constraint
 * @oh: struct omap_hwmod* to which the target device belongs to.
 * @cookie: identifier of the constraints list for @oh.
 *
 * Removes a wakeup latency contraint.  Returns the return value from
 * pwrdm_wakeuplat_remove_constraint(), or -EINVAL in case of invalid
 * parameters.
 */
int omap_hwmod_remove_wakeuplat_constraint(struct omap_hwmod *oh, void *cookie)
{
	struct powerdomain *pwrdm = omap_hwmod_get_pwrdm(oh);

	if (!pwrdm)
		return -EINVAL;

	return pwrdm_wakeuplat_remove_constraint(pwrdm, cookie);
}

/**
 * omap_hwmod_get_context_loss_count - get lost context count
 * @oh: struct omap_hwmod *
 *
 * Returns the context loss count of associated @oh
 * upon success, or zero if no context loss data is available.
 *
 * On OMAP4, this queries the per-hwmod context loss register,
 * assuming one exists.  If not, or on OMAP2/3, this queries the
 * enclosing powerdomain context loss count.
 */
int omap_hwmod_get_context_loss_count(struct omap_hwmod *oh)
{
	struct powerdomain *pwrdm;
	int ret = 0;

	if (arch_hwmod && arch_hwmod->hwmod_get_context_lost)
		return arch_hwmod->hwmod_get_context_lost(oh);

	pwrdm = omap_hwmod_get_pwrdm(oh);
	if (pwrdm)
		ret = pwrdm_get_context_loss_count(pwrdm);

	return ret;
}

/**
 * omap_hwmod_no_setup_reset - prevent a hwmod from being reset upon setup
 * @oh: struct omap_hwmod *
 *
 * Prevent the hwmod @oh from being reset during the setup process.
 * Intended for use by board-*.c files on boards with devices that
 * cannot tolerate being reset.  Must be called before the hwmod has
 * been set up.  Returns 0 upon success or negative error code upon
 * failure.
 */
int omap_hwmod_no_setup_reset(struct omap_hwmod *oh)
{
	if (!oh)
		return -EINVAL;

	if (oh->_state != _HWMOD_STATE_REGISTERED) {
		pr_err("omap_hwmod: %s: cannot prevent setup reset; in wrong state\n",
			oh->name);
		return -EINVAL;
	}

	oh->flags |= HWMOD_INIT_NO_RESET;

	return 0;
}

/**
 * omap_hwmod_pad_route_irq - route an I/O pad wakeup to a particular MPU IRQ
 * @oh: struct omap_hwmod * containing hwmod mux entries
 * @pad_idx: array index in oh->mux of the hwmod mux entry to route wakeup
 * @irq_idx: the hwmod mpu_irqs array index of the IRQ to trigger on wakeup
 *
 * When an I/O pad wakeup arrives for the dynamic or wakeup hwmod mux
 * entry number @pad_idx for the hwmod @oh, trigger the interrupt
 * service routine for the hwmod's mpu_irqs array index @irq_idx.  If
 * this function is not called for a given pad_idx, then the ISR
 * associated with @oh's first MPU IRQ will be triggered when an I/O
 * pad wakeup occurs on that pad.  Note that @pad_idx is the index of
 * the _dynamic or wakeup_ entry: if there are other entries not
 * marked with OMAP_DEVICE_PAD_WAKEUP or OMAP_DEVICE_PAD_REMUX, these
 * entries are NOT COUNTED in the dynamic pad index.  This function
 * must be called separately for each pad that requires its interrupt
 * to be re-routed this way.  Returns -EINVAL if there is an argument
 * problem or if @oh does not have hwmod mux entries or MPU IRQs;
 * returns -ENOMEM if memory cannot be allocated; or 0 upon success.
 *
 * XXX This function interface is fragile.  Rather than using array
 * indexes, which are subject to unpredictable change, it should be
 * using hwmod IRQ names, and some other stable key for the hwmod mux
 * pad records.
 */
int omap_hwmod_pad_route_irq(struct omap_hwmod *oh, int pad_idx, int irq_idx)
{
	int nr_irqs;

	might_sleep();

	if (!oh || !oh->mux || !oh->mpu_irqs || pad_idx < 0 ||
	    pad_idx >= oh->mux->nr_pads_dynamic)
		return -EINVAL;

	/* Check the number of available mpu_irqs */
	for (nr_irqs = 0; oh->mpu_irqs[nr_irqs].irq >= 0; nr_irqs++)
		;

	if (irq_idx >= nr_irqs)
		return -EINVAL;

	if (!oh->mux->irqs) {
		/* XXX What frees this? */
		oh->mux->irqs = kzalloc(sizeof(int) * oh->mux->nr_pads_dynamic,
			GFP_KERNEL);
		if (!oh->mux->irqs)
			return -ENOMEM;
	}
	oh->mux->irqs[pad_idx] = irq_idx;

	return 0;
}

/**
 * omap_hwmod_pad_wakeup_handler - add wakeup handler for an I/O pad wakeup
 * @oh: struct omap_hwmod * containing hwmod mux entries
 * @pad_idx: array index in oh->mux of the hwmod mux entry to handle wakeup
 * @wakeup_handler: the wakeup_handler function to trigger on wakeup
 */
int omap_hwmod_pad_wakeup_handler(struct omap_hwmod *oh, int pad_idx,
		int (*wakeup_handler)(struct omap_hwmod_mux_info *hmux))
{
	might_sleep();

	if (!oh || !oh->mux || pad_idx < 0 ||
			pad_idx >= oh->mux->nr_pads_dynamic)
		return -EINVAL;

	if (!oh->mux->wakeup_handler) {
		/* XXX What frees this? */
		oh->mux->wakeup_handler =
				kzalloc(sizeof(*(oh->mux->wakeup_handler)) *
				oh->mux->nr_pads_dynamic, GFP_KERNEL);

		if (!oh->mux->wakeup_handler)
			return -ENOMEM;
	}
	oh->mux->wakeup_handler[pad_idx] = wakeup_handler;

	return 0;
}

/**
 * omap_hwmod_get_main_clk - get pointer to main clock name
 * @oh: struct omap_hwmod *
 *
 * Returns the main clock name assocated with @oh upon success,
 * or NULL if @oh is NULL.
 */
const char *omap_hwmod_get_main_clk(struct omap_hwmod *oh)
{
	if (!oh)
		return NULL;

	return oh->main_clk;
}