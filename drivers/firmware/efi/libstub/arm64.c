// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2013, 2014 Linaro Ltd;  <roy.franz@linaro.org>
 *
 * This file implements the EFI boot stub for the arm64 kernel.
 * Adapted from ARM version by Mark Salter <msalter@redhat.com>
 */


#include <linux/efi.h>
#include <linux/screen_info.h>
#include <asm/efi.h>
#include <asm/image.h>
#include <asm/memory.h>
#include <asm/sysreg.h>

#include "efistub.h"

static u32 *sheng_phase_fb;
static unsigned long sheng_phase_pixels;

void efi_sheng_phase_probe_init(const struct screen_info *si)
{
	unsigned long size;

	/* This experiment is limited to Sheng's known 32-bit GOP mode. */
	if (si->orig_video_isVGA != VIDEO_TYPE_EFI ||
	    si->lfb_width != 3048 || si->lfb_height != 2032 ||
	    si->lfb_depth != 32 || si->lfb_linelength != 3048 * 4)
		return;

	size = (unsigned long)si->lfb_linelength * si->lfb_height;
	if (si->lfb_size < size || !__screen_info_lfb_base(si))
		return;

	sheng_phase_fb = (u32 *)(unsigned long)__screen_info_lfb_base(si);
	sheng_phase_pixels = size / sizeof(u32);
}

void efi_sheng_phase_probe_mark(u32 color, bool boot_services_available)
{
	u64 start, ticks;
	unsigned long addr, end, i, lsize;

	if (!sheng_phase_fb)
		return;

	for (i = 0; i < sheng_phase_pixels; i++)
		WRITE_ONCE(sheng_phase_fb[i], color);

	/* U-Boot maps the hand-off framebuffer cacheably; make each mark visible. */
	lsize = 4 << cpuid_feature_extract_unsigned_field(
		read_cpuid_effective_cachetype(), CTR_EL0_DminLine_SHIFT);
	addr = (unsigned long)sheng_phase_fb & ~(lsize - 1);
	end = (unsigned long)(sheng_phase_fb + sheng_phase_pixels);
	for (; addr < end; addr += lsize)
		asm volatile("dc cvac, %0" :: "r"(addr) : "memory");
	dsb(sy);

	if (boot_services_available) {
		efi_bs_call(stall, 2 * EFI_USEC_PER_SEC);
		return;
	}

	/* Boot services are gone; use the architectural counter for the last mark. */
	ticks = read_sysreg(cntfrq_el0) * 2;
	start = read_sysreg(cntvct_el0);
	while (read_sysreg(cntvct_el0) - start < ticks)
		asm volatile("yield");
}

static bool system_needs_vamap(void)
{
	const struct efi_smbios_type4_record *record;
	const u32 __aligned(1) *socid;
	const u8 *version;

	/*
	 * Ampere eMAG, Altra, and Altra Max machines crash in SetTime() if
	 * SetVirtualAddressMap() has not been called prior. Most Altra systems
	 * can be identified by the SMCCC soc ID, which is conveniently exposed
	 * via the type 4 SMBIOS records. Otherwise, test the processor version
	 * field. eMAG systems all appear to have the processor version field
	 * set to "eMAG".
	 */
	record = (struct efi_smbios_type4_record *)efi_get_smbios_record(4);
	if (!record)
		return false;

	socid = (u32 *)record->processor_id;
	switch (*socid & 0xffff000f) {
		static char const altra[] = "Ampere(TM) Altra(TM) Processor";
		static char const emag[] = "eMAG";

	default:
		version = efi_get_smbios_string(record, processor_version);
		if (!version || (strncmp(version, altra, sizeof(altra) - 1) &&
				 strncmp(version, emag, sizeof(emag) - 1)))
			break;

		fallthrough;

	case 0x0a160001:	// Altra
	case 0x0a160002:	// Altra Max
		efi_warn("Working around broken SetVirtualAddressMap()\n");
		return true;
	}

	return false;
}

efi_status_t check_platform_features(void)
{
	u64 tg;

	/*
	 * If we have 48 bits of VA space for TTBR0 mappings, we can map the
	 * UEFI runtime regions 1:1 and so calling SetVirtualAddressMap() is
	 * unnecessary.
	 */
	if (VA_BITS_MIN >= 48 && !system_needs_vamap())
		efi_novamap = true;

	/* UEFI mandates support for 4 KB granularity, no need to check */
	if (IS_ENABLED(CONFIG_ARM64_4K_PAGES))
		return EFI_SUCCESS;

	tg = (read_cpuid(ID_AA64MMFR0_EL1) >> ID_AA64MMFR0_EL1_TGRAN_SHIFT) & 0xf;
	if (tg < ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MIN || tg > ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MAX) {
		if (IS_ENABLED(CONFIG_ARM64_64K_PAGES))
			efi_err("This 64 KB granular kernel is not supported by your CPU\n");
		else
			efi_err("This 16 KB granular kernel is not supported by your CPU\n");
		return EFI_UNSUPPORTED;
	}
	return EFI_SUCCESS;
}

#ifdef CONFIG_ARM64_WORKAROUND_CLEAN_CACHE
#define DCTYPE	"civac"
#else
#define DCTYPE	"cvau"
#endif

u32 __weak code_size;

void efi_cache_sync_image(unsigned long image_base,
			  unsigned long alloc_size)
{
	u32 ctr = read_cpuid_effective_cachetype();
	u64 lsize = 4 << cpuid_feature_extract_unsigned_field(ctr,
						CTR_EL0_DminLine_SHIFT);

	/* only perform the cache maintenance if needed for I/D coherency */
	if (!(ctr & BIT(CTR_EL0_IDC_SHIFT))) {
		unsigned long base = image_base;
		unsigned long size = code_size;

		do {
			asm("dc " DCTYPE ", %0" :: "r"(base));
			base += lsize;
			size -= lsize;
		} while (size >= lsize);
	}

	asm("ic ialluis");
	dsb(ish);
	isb();

	efi_remap_image(image_base, alloc_size, code_size);
}

unsigned long __weak primary_entry_offset(void)
{
	/*
	 * By default, we can invoke the kernel via the branch instruction in
	 * the image header, so offset #0. This will be overridden by the EFI
	 * stub build that is linked into the core kernel, as in that case, the
	 * image header may not have been loaded into memory, or may be mapped
	 * with non-executable permissions.
	 */
       return 0;
}

void __noreturn efi_enter_kernel(unsigned long entrypoint,
				 unsigned long fdt_addr,
				 unsigned long fdt_size)
{
	void (* __noreturn enter_kernel)(u64, u64, u64, u64);

	enter_kernel = (void *)entrypoint + primary_entry_offset();
	enter_kernel(fdt_addr, 0, 0, 0);
}
