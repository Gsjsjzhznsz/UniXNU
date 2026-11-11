/*
 * sweet_scanout: the display reads iOS's frames itself (patch 0268).
 *
 * Until now every frame backboardd swapped was copied by the CPU into the
 * ABL's panel memory (0x9c000000), which VIG0 of the SM8150 display
 * controller (DPU 5.0) scans out: 1125x2436 scaled to 1080x2340, about 11 MB
 * read and 10 MB written per frame. Here the pipe is pointed at the IOSurface
 * itself at every swap, and its QSEED3 scaler does the fit, as an iPhone's
 * display pipe takes the frame buffer CoreAnimation hands it: GPU renders,
 * display scans, the CPU copies nothing.
 *
 * Adapted from Gianni Carollo's andromeda_scanout.c (UniXNU-andromeda, patch
 * 0207, itself from ShueiTsuki's data-a-live 0217-pandora-scanout): the same
 * SM8150 SoC, the same DPU 5.0, the same 1080x2340 panel and 1125x2436 canvas
 * as raphael (Redmi K20 Pro), so the register values transfer unchanged.
 *
 * The ABL leaves the apps SMMU stream for the display in translate mode with
 * its context bank's SCTLR.M = 0: the pipe then reads 32-bit physical
 * addresses and only below 4 GiB, while iOS's surfaces are wired kernel
 * memory anywhere in RAM. This file gives bank 2 4 KiB stage-1 tables with a
 * 32-bit input space: the panel memory at its own address (the console, the
 * blank of display off, panics), surfaces in 8 x 16 MiB IOVA slots from
 * 0x80000000. Leaves are read-only, non-cacheable, non-global; every table
 * write is cleaned to the point of coherency.
 *
 * Boot-arg sweetfb_scanout (default 0; also set at run time by "andctl scanout
 * 0|1" when andctl is available):
 *   1  scan out every Metal frame the pipe can take; any other frame is
 *      copied by the CPU as before
 *   0  CPU copy of every frame
 * The first Metal frame sets the hardware up: a stream match or bank not as
 * expected, a flush not taken or an SMMU fault within 100 ms leaves the CPU
 * copy for the rest of the boot. Nothing is written before that frame. A
 * panic puts the pipe back on the panel memory, where the panic screen goes.
 */
#ifndef SWEET_SCANOUT_H
#define SWEET_SCANOUT_H

#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* One frame for the pipe: source rectangle in the surface, output rectangle on the panel. */
struct sweet_scan_frame {
	uint32_t iova;          /* first byte of the surface for the display (slot + offset) */
	uint32_t stride;        /* bytes per row */
	uint32_t src_x, src_y, src_w, src_h;
	uint32_t dst_x, dst_y, dst_w, dst_h;
};

/* sweetfb_scanout (see above). */
unsigned sweet_scanout_mode(void);
int sweet_scanout_ctl(unsigned mode, char *why, size_t len);

/* Set the display stream up for scanout (once; thread context). 0 = ready. */
int sweet_scanout_init(uint64_t panel_pa, uint32_t panel_rowbytes, uint32_t panel_w, uint32_t panel_h);
bool sweet_scanout_ready(void);

/* Map NPAGES 4 KiB pages (physical addresses) into a free IOVA slot: its IOVA, or 0. */
uint32_t sweet_scanout_map(const uint64_t *pages, uint32_t npages);
void sweet_scanout_unmap(uint32_t iova, uint32_t npages);

/*
 * Give the pipe a frame (0), or back the ABL's panel memory (panel). Both
 * wait until the previous flush has been taken (at most one frame) and
 * return EBUSY when it was not.
 */
int sweet_scanout_show(const struct sweet_scan_frame *f);
int sweet_scanout_panel(void);

/* The last flush has been taken: the pipe reads what it was given last. */
bool sweet_scanout_latched(void);
/* The same, waiting for it at most MAX_MS (naps of 250 us, thread context). */
bool sweet_scanout_wait_latched(unsigned max_ms);

/* Frames shown, flushes that had to wait, SMMU faults seen (fault status read and cleared here). */
void sweet_scanout_counts(uint64_t *shows, uint64_t *waits, uint64_t *faults);

/*
 * SwapWait's test (iOS 16's swap_wait_gated), used by the framebuffer:
 * whether swap ID is done for TYPE, given the swap queued (QUEUED_VALID: one is), the one being
 * shown (ACTIVE, 0: none) and the one on screen (SHOWN, 0: none). 0: off the screen; 1: on screen or
 * taken over; 2 and up: nothing queued or active. Display off: everything is done.
 */
static inline bool
sweet_swap_done(uint32_t id, uint32_t type, bool queued_valid, uint32_t queued, uint32_t active, uint32_t shown,
    bool display_off)
{
	const bool q = queued_valid && id == queued, a = active != 0 && id == active;
	if (display_off) {
		return true;
	}
	switch (type) {
	case 0:
		return id == 0 || (!q && !a && id != shown);
	case 1:
		return id == 0 || (!q && !a);
	default:
		return !queued_valid && active == 0;
	}
}

#ifdef __cplusplus
}
#endif

#endif /* SWEET_SCANOUT_H */
