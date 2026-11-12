/*
 * sweet_scanout: the display reads iOS's frames itself (patch 0268).
 * What, why and whose design: sweet_scanout.h.
 *
 * The hardware as the ABL leaves it and as the 0263/0265 probe read it on
 * raphael (Redmi K20 Pro, SM8150):
 *
 *   MDP 0x0ae00000 (DPU 5.0): VIG0 (+0x5000) fetches the panel memory
 *   0x9c000000 (1080x2340, stride 4320, ARGB8888 = iOS's BGRA, no scaling)
 *   for CTL0 (+0x2000) -> LM0 -> INTF1 (DSI0, +0x6b800), command mode; INTF1's
 *   tear check is on (+0x284 = 1) with autorefresh every frame (+0x2b4 =
 *   0x80000001), so the panel is refreshed without a CTL_START and a CTL
 *   flush is taken at the next frame. Register offsets: Linux dpu1
 *   (dpu_5_0_sm8150.h), the same ones the beryllium scaler patch 0126 uses.
 *
 *   apps SMMU 0x15000000 (MMU-500, 4 KiB pages, 128 global pages, 56 banks):
 *   SMR 3 = 0x84200800 (SID 0x800 mask 0x420, the DT's display stream),
 *   S2CR 3 = 0x00000002 (translate, bank 2), bank 2 with SCTLR.M = 0: the
 *   pipe reads physical addresses, and only below 4 GiB. iOS's surfaces are
 *   wired kernel memory anywhere in RAM.
 *
 * Bank 2 gets 4 KiB stage-1 tables with a 32-bit input space (start level 1):
 * the panel memory at its own address (the ABL's frame, the console, the
 * blank of display off), surfaces in 16 MiB slots from 0x80000000. Leaves
 * are read-only, non-cacheable (MAIR attr 0 = 0x44), non-global (ASID = the
 * bank, ASIDPNE), the walk is non-cacheable, so every table write is cleaned
 * to the point of coherency by VA (sweet_dma_sync_va: this build defines
 * __ARM_COHERENT_IO__, which makes flush_dcache64 a bare dsb). The register
 * values are Linux arm-smmu's for a stage-1 bank: CBAR type 1 with
 * BPSHCFG/MEMATTR, CBA2R AArch64, TCR T0SZ 32 / SH0 outer / TG0 4K / EPD1,
 * TCR2 PASize 40 bits.
 * If the bank faults or a flush is not taken within 100 ms of turning
 * translation on, everything goes back as it was (SCTLR.M = 0) and the
 * framebuffer keeps its CPU copy for the boot.
 */
#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include <kdp/kdp_callout.h>

#include "sweet_board.h"        /* sweet_dma_sync_va() */
#include "sweet_scanout.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern void IOSleep(unsigned int milliseconds);

#define SC_TAG            "SWEET SCANOUT:"

/* MDP (our SWEET_MDP_PA 0x0ae00000 in SweetFramebuffer.cpp; block offsets are Linux catalog dpu_5_0_sm8150.h) */
#define SC_MDP_PA         0x0ae00000u
#define SC_MDP_LEN        0x00082000u
#define SC_VIG0           0x5000u
#define SC_VIG1           0x7000u
#define SC_QSEED          (SC_VIG0 + 0xa00u)
#define SC_QSEED1         (SC_VIG1 + 0xa00u)
#define SC_CTL0           0x2000u
#define SC_LM0            0x45000u
#define SC_INTF1          0x6b800u
/* SSPP (dpu_hw_sspp.c) */
#define SS_SRC_SIZE       0x00u
#define SS_SRC_XY         0x08u
#define SS_OUT_SIZE       0x0cu
#define SS_OUT_XY         0x10u
#define SS_SRC0_ADDR      0x14u
#define SS_YSTRIDE0       0x24u
#define SS_SRC_FORMAT     0x30u
#define SS_OP_MODE        0x38u
#define SS_PE_C0_LR       0x100u
#define SS_PE_C0_TB       0x104u
#define SS_PE_C0_REQ      0x108u
#define SS_PE_C12_LR      0x110u
#define SS_PE_C12_TB      0x114u
#define SS_PE_C12_REQ     0x118u
#define SS_PE_C3_LR       0x120u
#define SS_PE_C3_TB       0x124u
#define SS_PE_C3_REQ      0x128u
/* QSEED3 (dpu_hw_util.c) */
#define QS_HW_VERSION     0x00u
#define QS_OP_MODE        0x04u
#define QS_PHASE_INIT     0x0cu
#define QS_STEP_Y_H       0x10u
#define QS_STEP_Y_V       0x14u
#define QS_STEP_UV_H      0x18u
#define QS_STEP_UV_V      0x1cu
#define QS_PRELOAD        0x20u
#define QS_SRC_SIZE_Y     0x40u
#define QS_SRC_SIZE_UV    0x44u
#define QS_DST_SIZE       0x48u
#define QS_INIT_Y_H       0x90u
#define QS_INIT_Y_V       0x94u
#define QS_INIT_UV_H      0x98u
#define QS_INIT_UV_V      0x9cu
/* CTL (dpu_hw_ctl.c): flush VIG0 (bit 0), VIG1 (bit 1), LM0 (bit 6) and the CTL itself (bit 17) */
#define CT_FLUSH          0x18u
#define CT_FLUSH_VIG0     (1u << 0)
#define CT_FLUSH_VIG1     (1u << 1)
#define CT_FLUSH_LM0      (1u << 6)
#define CT_FLUSH_CTL      (1u << 17)
#define CT_FLUSH_BITS     (CT_FLUSH_VIG0 | CT_FLUSH_CTL)   /* the single-VIG0 path (0268) */
/* CTL LAYER0: every pipe's 3-bit mixer-stage field (dpu_hw_ctl.c); VIG1 sits at shift 3 */
#define CT_LAYER0         0x00u
#define SC_MIX_VIG1_SHIFT 3u
/*
 * LM0 blend (dpu_hw_lm.c, sdm845_lm_sblk shared by SM8150): blendstage_base 0x20, stride 0x18,
 * mixer value 2 = the first blend stage above the base (value 1 = base, no blend block).
 * CONST_ALPHA is the sdm845 packed register (fg << 16) | bg; the blend op 0 uses the constants.
 */
#define SC_LM_BLEND0       0x20u
#define SC_LM_BLEND_STRIDE 0x18u
#define SC_LM_BLEND_OP     0x00u
#define SC_LM_BLEND_ALPHA  0x04u
#define SC_BLEND_OP_CONST  0x000u        /* FG alpha from the constant, BG from the constant, no invert */
#define SC_BLEND_VIG1_ON   0x00ff0000u    /* CONST_ALPHA: fg in bits 23:16, bg in bits 7:0 */
#define SC_BLEND_VIG0_ON   0x000000ffu    /* fg 0x00, bg 0xff (VIG1 transparent, base shows) */
/* INTF tear check, DPU >= 5 (dpu_hw_intf.c) */
#define IF_TEAR_CHECK_EN  0x284u
#define IF_TEAR_LINE      0x2b0u
#define IF_AUTOREFRESH    0x2b4u

/* apps SMMU (DT apps_smmu reg) and the display's stream (DT iommus <&apps_smmu 0x800 0x420>) */
#define SC_SMMU_PA        0x15000000u
#define SC_SMMU_LEN       0x00100000u
#define SC_SID            0x800u
#define SM_IDR0           0x20u
#define SM_IDR1           0x24u
#define SM_IDR2           0x28u
#define SM_SMR(n)         (0x800u + 4u * (n))
#define SM_S2CR(n)        (0xc00u + 4u * (n))
#define SM_CBAR(n)        (4u * (n))             /* in global space 1 */
#define SM_CBA2R(n)       (0x800u + 4u * (n))    /* in global space 1 */
#define CB_SCTLR          0x00u
#define CB_ACTLR          0x04u
#define CB_TCR2           0x10u
#define CB_TTBR0          0x20u
#define CB_TTBR1          0x28u
#define CB_TCR            0x30u
#define CB_MAIR0          0x38u
#define CB_MAIR1          0x3cu
#define CB_FSR            0x58u
#define CB_FAR            0x60u
#define CB_FSYNR0         0x68u
#define CB_TLBIASID       0x610u
#define CB_TLBSYNC        0x7f0u
#define CB_TLBSTATUS      0x7f4u
/* Fault bits of CB_FSR as Linux arm-smmu reads them (TF..UUT, SS, MULTI); bits 10:9 are FORMAT. */
#define CB_FSR_FAULT      (0x1feu | (1u << 30) | (1u << 31))

/* What the bank is set to (see the top of the file). */
#define SC_CBAR           0x0001f300u     /* type 1 (S1, S2 bypass), BPSHCFG non-shareable, MEMATTR WB */
#define SC_CBA2R          0x1u            /* AArch64 descriptors */
#define SC_TCR            0x00802020u     /* T0SZ 32, NC walk, SH0 outer, TG0 4K, EPD1 */
#define SC_TCR2_SEP       (7u << 15)
#define SC_MAIR0          0x0000ff44u     /* attr 0 normal non-cacheable, attr 1 write-back */
#define SC_SCTLR          0x00001027u     /* M, TRE, AFE, CFRE, ASIDPNE; no fault interrupt */
/* Leaf: page, AttrIndx 0, AP read-only, SH outer, AF, nG, PXN, UXN */
#define SC_LEAF_ATTR      ((1ull << 11) | (1ull << 10) | (2ull << 8) | (1ull << 7) | (1ull << 6) | 3ull | (3ull << 53))

/* Tables and IOVA slots */
#define SC_POOL_PAGES     128u            /* 4 KiB tables: 1 + 1 + splash (24) + 8 slots x 9, with room */
#define SC_SLOT_BASE      0x80000000u
#define SC_SLOT_BYTES     (16u << 20)
#define SC_SLOTS          8u
/*
 * The ABL's continuous-splash carve-out (36 MiB from the panel PA) is mapped at its own address in
 * the display's tables, so the copy fallback and the console keep reading physical addresses after
 * translation is on. Its tail is also where the tables themselves go when the kernel's own memory is
 * above 4 GiB (the SMMU's 32-bit output size would not reach them).
 */
#define SC_SPLASH_SPAN    0x02400000u

struct sc_tables {
	uint64_t *va;                   /* pool, SC_POOL_PAGES x 4 KiB, contiguous */
	uint64_t pa;
	unsigned used;                  /* pages taken (page 0 is the level-1 table) */
};

/*
 * A table write has reached DRAM once the barrier completes: the tables live in the splash
 * carve-out, mapped uncached (ml_io_map is device memory), so the stores bypass the cache and
 * there is nothing to clean. "dc cvac" must NOT be used here: cache maintenance on device
 * memory is unpredictable and raised an unaligned data abort at the 9th mapped page (L3 entry 8)
 * on raphael. A data-synchronisation barrier is all this needs.
 */
static void
sc_clean(void *va, unsigned len)
{
	(void)va;
	(void)len;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* Physical address of the 4 KiB table holding entry E. */
static inline uint64_t
sc_pt_pa(const struct sc_tables *t, const uint64_t *e)
{
	return t->pa + (uint64_t)((const uint8_t *)e - (const uint8_t *)t->va);
}

/* The level-3 entry for IOVA, its tables made when MAKE; NULL when the pool is full or (no MAKE) not mapped. */
static uint64_t *
sc_pt_leaf(struct sc_tables *t, uint32_t iova, bool make, void (*clean)(void *va, unsigned len))
{
	uint64_t *table = t->va;
	for (unsigned level = 1; level < 3; level++) {
		const unsigned shift = level == 1 ? 30 : 21;
		uint64_t *e = &table[(iova >> shift) & (level == 1 ? 3u : 511u)];
		if ((*e & 3) != 3) {
			if (!make || t->used >= SC_POOL_PAGES) {
				return NULL;
			}
			*e = (t->pa + 0x1000ull * t->used++) | 3;
			clean(e, 8);
		}
		table = (uint64_t *)((uint8_t *)t->va + ((*e & 0xfffffffff000ull) - t->pa));
	}
	return &table[(iova >> 12) & 511];
}

/* Map NPAGES at IOVA: PAGES[i] or (PAGES NULL) PA + i x 4 KiB. False when the pool ran out. */
static bool
sc_pt_map(struct sc_tables *t, uint32_t iova, const uint64_t *pages, uint64_t pa, uint32_t npages,
    void (*clean)(void *va, unsigned len))
{
	for (uint32_t i = 0; i < npages; i++) {
		uint64_t *leaf = sc_pt_leaf(t, iova + 0x1000u * i, true, clean);
		if (leaf == NULL) {
			return false;
		}
		const uint64_t page = pages ? pages[i] : pa + 0x1000ull * i;
		*leaf = (page & 0xfffffffff000ull) | SC_LEAF_ATTR;
		clean(leaf, 8);
	}
	return true;
}

static void
sc_pt_unmap(struct sc_tables *t, uint32_t iova, uint32_t npages, void (*clean)(void *va, unsigned len))
{
	for (uint32_t i = 0; i < npages; i++) {
		uint64_t *leaf = sc_pt_leaf(t, iova + 0x1000u * i, false, clean);
		if (leaf != NULL) {
			*leaf = 0;
			clean(leaf, 8);
		}
	}
}

/* Pipe and scaler values for a frame (pure, host-tested). */
struct sc_regs {
	uint32_t src_size, src_xy, out_size, out_xy, addr, stride, req;
	bool scaled;
	uint32_t step_h, step_v, preload, qsrc, qdst, qop;
};

static bool
sc_frame_regs(const struct sweet_scan_frame *f, uint32_t qseed_version, struct sc_regs *r)
{
	if (f->src_w < 2 || f->src_h < 2 || f->dst_w < 2 || f->dst_h < 2 || f->src_w > 4096 || f->src_h > 4096 ||
	    f->dst_w > 4096 || f->dst_h > 4096 || f->stride == 0 || f->stride > 0xffff || (f->iova & 0xf) != 0 ||
	    f->src_x > 0xffff || f->src_y > 0xffff || f->dst_x > 0xffff || f->dst_y > 0xffff ||
	    /* QSEED3 limits as Linux's catalog: up to 4x down, 20x up */
	    f->src_w > 4 * f->dst_w || f->src_h > 4 * f->dst_h || f->dst_w > 20 * f->src_w || f->dst_h > 20 * f->src_h) {
		return false;
	}
	r->src_size = (f->src_h << 16) | f->src_w;
	r->src_xy = (f->src_y << 16) | f->src_x;
	r->out_size = (f->dst_h << 16) | f->dst_w;
	r->out_xy = (f->dst_y << 16) | f->dst_x;
	r->addr = f->iova;
	r->stride = f->stride;
	/* SW pixel extension (SRC_OP_MODE bit 31, set by the ABL): no extra pixels, as dpu_plane with QSEED3 */
	r->req = r->src_size;
	r->scaled = f->src_w != f->dst_w || f->src_h != f->dst_h;
	/* dpu_plane: phase step = (1 << 21) * src / dst; preload 4 / 3; bilinear (3), blend, 8-bit, alpha bilinear */
	r->step_h = (uint32_t)(((uint64_t)f->src_w << 21) / f->dst_w) & 0xffffff;
	r->step_v = (uint32_t)(((uint64_t)f->src_h << 21) / f->dst_h) & 0xffffff;
	r->preload = 4u | (3u << 8) | (4u << 16) | (3u << 24);
	r->qsrc = (f->src_w & 0x1ffff) | ((f->src_h & 0x1ffff) << 16);
	r->qdst = (f->dst_w & 0x1ffff) | ((f->dst_h & 0x1ffff) << 16);
	r->qop = 1u | (3u << 16) | (1u << 31) | (1u << 14) | (1u << 10) |
	    (qseed_version == 0x1002 ? (1u << 30) : (1u << 29));
	return true;
}

#define SC_SAVED_SSPP 16u
static const uint32_t sc_sspp_saved[SC_SAVED_SSPP] = {
	SS_SRC_SIZE, SS_SRC_XY, SS_OUT_SIZE, SS_OUT_XY, SS_SRC0_ADDR, SS_YSTRIDE0, SS_OP_MODE,
	SS_PE_C0_LR, SS_PE_C0_TB, SS_PE_C0_REQ, SS_PE_C12_LR, SS_PE_C12_TB, SS_PE_C12_REQ,
	SS_PE_C3_LR, SS_PE_C3_TB, SS_PE_C3_REQ,
};
/* OP_MODE last: restored after the values it turns on. */
#define SC_SAVED_QSEED 14u
static const uint32_t sc_qseed_saved[SC_SAVED_QSEED] = {
	QS_PHASE_INIT, QS_STEP_Y_H, QS_STEP_Y_V, QS_STEP_UV_H, QS_STEP_UV_V, QS_PRELOAD, QS_SRC_SIZE_Y,
	QS_SRC_SIZE_UV, QS_DST_SIZE, QS_INIT_Y_H, QS_INIT_Y_V, QS_INIT_UV_H, QS_INIT_UV_V, QS_OP_MODE,
};

static struct {
	lck_grp_t *grp;
	lck_mtx_t lock;                 /* everything below and the registers */
	bool tried, ready, on_panel;
	int error;
	vm_offset_t mdp, smmu;
	uint32_t cb, gr1;               /* offsets of the bank and of global space 1 */
	unsigned bank, smr, asid;
	uint32_t qseed_version;
	uint64_t panel_pa;
	uint32_t panel_bytes;
	struct sc_tables t;
	bool slot_used[SC_SLOTS];
	uint32_t sspp[SC_SAVED_SSPP], qseed[SC_SAVED_QSEED];
	uint32_t sspp1[SC_SAVED_SSPP], qseed1[SC_SAVED_QSEED];   /* VIG1's boot values (0272) */
	uint32_t layer0;                /* CTL LAYER0 as the ABL left it */
	uint32_t blend_op, blend_alpha; /* VIG1's LM0 blend stage as the ABL left it */
	bool pp, vig1_on;               /* pp: VIG0/VIG1 ping-pong; vig1_on: VIG1 joined the mixer */
	unsigned cur;                   /* pipe that holds the newest frame (0 = VIG0, 1 = VIG1) */
	uint32_t vig1_mix;              /* VIG1's mixer stage = VIG0's + 1 (one stage above the base pipe) */
	uint32_t vig1_blend;            /* VIG1's LM0 blend block offset (BLEND0 + STRIDE*(mix-2)) */
	uint32_t flush_bits;            /* CTL flush bits of the last show() (for SwapWait) */
	uint64_t shows, waits, faults, replaced;   /* replaced: a flush was written over a pending one */
} sc;

static unsigned sc_mode = 0, sc_mode_parsed;

static inline uint32_t
mdp_rd(uint32_t off)
{
	return *(volatile uint32_t *)(sc.mdp + off);
}

static inline void
mdp_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(sc.mdp + off) = v;
}

static inline uint32_t
sm_rd(uint32_t off)
{
	return *(volatile uint32_t *)(sc.smmu + off);
}

static inline void
sm_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(sc.smmu + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static inline void
sm_wr64(uint32_t off, uint64_t v)
{
	*(volatile uint64_t *)(sc.smmu + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* Sleep about US microseconds. */
static void
sc_nap_us(unsigned us)
{
	static int chan;
	(void)assert_wait_timeout((event_t)&chan, THREAD_UNINT, us, NSEC_PER_USEC);
	(void)thread_block(THREAD_CONTINUE_NULL);
}

static void
sc_mode_args(void)
{
	if (__atomic_exchange_n(&sc_mode_parsed, 1u, __ATOMIC_ACQ_REL) == 0) {
		unsigned v = 0;
		(void)PE_parse_boot_argn("sweetfb_scanout", &v, sizeof(v));
		__atomic_store_n(&sc_mode, v ? 1u : 0u, __ATOMIC_RELAXED);
		printf(SC_TAG " sweetfb_scanout=%u: %s\n", v ? 1u : 0u, v ? "the display reads Metal frames itself from the first one" :
		    "CPU copy of every frame");
	}
}

unsigned
sweet_scanout_mode(void)
{
	sc_mode_args();
	return __atomic_load_n(&sc_mode, __ATOMIC_RELAXED);
}

/* TLB of the bank's ASID, then a sync (at most 10 ms). */
static bool
sc_tlb_flush(void)
{
	sm_wr(sc.cb + CB_TLBIASID, sc.asid);
	sm_wr(sc.cb + CB_TLBSYNC, 0);
	for (unsigned i = 0; i < 10000; i++) {
		if ((sm_rd(sc.cb + CB_TLBSTATUS) & 1) == 0) {
			return true;
		}
		delay(1);
	}
	printf(SC_TAG " TLB sync of bank %u not done after 10 ms\n", sc.bank);
	return false;
}

/*
 * Give the previous CTL flush a moment to be taken, but never a whole panel frame.
 *
 * The flush is a one-shot latch: if the previous one is still pending, the address and geometry
 * written next are what the pipe latches with it, so the newest frame replaces the queued one.
 * Waiting for it to clear costs a full 60 Hz frame (measured 16.2 ms per swap on raphael) and that
 * serial latency is what made the UI feel stuck even with zero CPU copies. 2 ms covers the normal
 * near-latch case; a replaced frame is counted instead of blocking. Always true: the caller has
 * nothing better to fall back to.
 */
static bool
sc_flush_idle(uint32_t bits)
{
	for (unsigned i = 0; i < 8; i++) {
		if ((mdp_rd(SC_CTL0 + CT_FLUSH) & bits) == 0) {
			if (i != 0) {
				sc.waits++;
			}
			return true;
		}
		sc_nap_us(250);
	}
	sc.replaced++;
	return true;
}

/* Fault status of the bank: counted, logged (first 8), cleared. True when there was a fault. */
static bool
sc_fault_check(void)
{
	const uint32_t fsr = sm_rd(sc.cb + CB_FSR);
	if ((fsr & CB_FSR_FAULT) == 0) {
		return false;
	}
	if (++sc.faults <= 8) {
		printf(SC_TAG " bank %u fault fsr %08x far %08x%08x fsynr0 %08x\n", sc.bank, fsr,
		    sm_rd(sc.cb + CB_FAR + 4), sm_rd(sc.cb + CB_FAR), sm_rd(sc.cb + CB_FSYNR0));
	}
	sm_wr(sc.cb + CB_FSR, fsr);
	return true;
}

static void
sc_lock_init(void)
{
	static unsigned once;
	if (__atomic_exchange_n(&once, 1u, __ATOMIC_ACQ_REL) == 0) {
		sc.grp = lck_grp_alloc_init("sweet_scanout", LCK_GRP_ATTR_NULL);
		lck_mtx_init(&sc.lock, sc.grp, LCK_ATTR_NULL);
	}
}

static int
sc_init_locked(uint64_t panel_pa, uint32_t rowbytes, uint32_t w, uint32_t h)
{
	sc.mdp = ml_io_map(SC_MDP_PA, SC_MDP_LEN);
	sc.smmu = ml_io_map(SC_SMMU_PA, SC_SMMU_LEN);
	if (!sc.mdp || !sc.smmu) {
		printf(SC_TAG " MDP or SMMU not mapped: CPU copy\n");
		return ENOMEM;
	}
	/* The pipe: VIG0 reads the panel memory unscaled; INTF1 refreshes it by itself. */
	const uint32_t addr = mdp_rd(SC_VIG0 + SS_SRC0_ADDR), stride = mdp_rd(SC_VIG0 + SS_YSTRIDE0) & 0xffff;
	const uint32_t size = mdp_rd(SC_VIG0 + SS_SRC_SIZE), out = mdp_rd(SC_VIG0 + SS_OUT_SIZE);
	const uint32_t te = mdp_rd(SC_INTF1 + IF_TEAR_CHECK_EN), ar = mdp_rd(SC_INTF1 + IF_AUTOREFRESH);
	sc.qseed_version = mdp_rd(SC_QSEED + QS_HW_VERSION);
	printf(SC_TAG " vig0 %08x stride %u size %08x out %08x format %08x op %08x; qseed3 version %08x op %08x; "
	    "intf1 tear check %08x autorefresh %08x\n", addr, stride, size, out, mdp_rd(SC_VIG0 + SS_SRC_FORMAT),
	    mdp_rd(SC_VIG0 + SS_OP_MODE), sc.qseed_version, mdp_rd(SC_QSEED + QS_OP_MODE), te, ar);
	/*
	 * The only thing that identifies the pipe is that it is fetching the panel memory: show() programs
	 * SRC_SIZE/XY and OUT_SIZE/XY itself, so those need not match what the ABL left. Tear check and
	 * autorefresh are informational too: they say how the panel is refreshed, not whether this pipe is
	 * the display, and a video-mode panel latches a CTL flush at its next frame without either. The
	 * trial flush below is the real functional test.
	 */
	if (addr != (uint32_t)panel_pa || panel_pa > 0xffffffffull || stride != rowbytes) {
		printf(SC_TAG " not the pipe fetching the panel (src0 %08x != %#llx, stride %u != %u): CPU copy\n",
		    addr, (unsigned long long)panel_pa, stride, rowbytes);
		return ENODEV;
	}
	/* The stream: one valid stream match for SID 0x800, translating context, its bank off. */
	const uint32_t idr0 = sm_rd(SM_IDR0), idr1 = sm_rd(SM_IDR1), idr2 = sm_rd(SM_IDR2);
	const unsigned nsmr = idr0 & 0xffu, ncb = idr1 & 0xffu;
	const uint32_t page = (idr1 & (1u << 31)) ? 0x10000u : 0x1000u;
	const uint32_t numpage = 1u << (((idr1 >> 28) & 7u) + 1);
	unsigned hits = 0;
	uint32_t s2cr = 0;
	for (unsigned i = 0; i < nsmr && i < 128; i++) {
		const uint32_t smr = sm_rd(SM_SMR(i)), id = smr & 0x7fffu, mask = (smr >> 16) & 0x7fffu;
		if ((smr & (1u << 31)) && ((id ^ SC_SID) & ~mask & 0x7fffu) == 0) {
			sc.smr = i;
			s2cr = sm_rd(SM_S2CR(i));
			hits++;
		}
	}
	sc.bank = s2cr & 0xffu;
	sc.cb = numpage * page + sc.bank * page;
	sc.gr1 = page;
	if (hits != 1 || ((s2cr >> 16) & 3u) != 0 || sc.bank >= ncb || sc.cb + page > SC_SMMU_LEN) {
		printf(SC_TAG " stream %#x: %u matches, s2cr %08x (idr0 %08x idr1 %08x): CPU copy\n", SC_SID, hits, s2cr,
		    idr0, idr1);
		return ENODEV;
	}
	const uint32_t was_sctlr = sm_rd(sc.cb + CB_SCTLR), was_actlr = sm_rd(sc.cb + CB_ACTLR);
	const uint32_t was_cbar = sm_rd(sc.gr1 + SM_CBAR(sc.bank)), was_cba2r = sm_rd(sc.gr1 + SM_CBA2R(sc.bank));
	const uint32_t was_tcr = sm_rd(sc.cb + CB_TCR), was_tcr2 = sm_rd(sc.cb + CB_TCR2);
	const uint32_t was_mair0 = sm_rd(sc.cb + CB_MAIR0), was_mair1 = sm_rd(sc.cb + CB_MAIR1);
	printf(SC_TAG " stream match %u s2cr %08x -> bank %u: sctlr %08x actlr %08x cbar %08x cba2r %08x tcr %08x "
	    "tcr2 %08x mair %08x/%08x; idr2 %08x\n", sc.smr, s2cr, sc.bank, was_sctlr, was_actlr, was_cbar, was_cba2r,
	    was_tcr, was_tcr2, was_mair0, was_mair1, idr2);
	if (was_sctlr & 1) {
		printf(SC_TAG " bank %u already translates: CPU copy\n", sc.bank);
		return EBUSY;
	}
	/* Output size: the SMMU's, at most 40 bits (encodings 0..5 = 32, 36, 40, 42, 44, 48). */
	unsigned oas = (idr2 >> 4) & 15u;
	if (oas > 5) {
		return ENODEV;
	}
	if (oas > 2) {
		oas = 2;
	}

	/*
	 * Tables. The SMMU walks them from DRAM with its own output size (oas bits), and every byte xnu
	 * manages is above 4 GiB on this board, so a kernel allocation would be unreachable: the tables
	 * live in the tail of the splash carve-out (below 4 GiB, and not scanned by anything).
	 */
	const uint64_t tbl_pa = panel_pa + SC_SPLASH_SPAN - SC_POOL_PAGES * 0x1000;
	vm_offset_t tva = ml_io_map(tbl_pa, SC_POOL_PAGES * 0x1000);
	if (tva == 0) {
		printf(SC_TAG " no mapping for the tables at %#llx: CPU copy\n", (unsigned long long)tbl_pa);
		return ENOMEM;
	}
	sc.t.va = (uint64_t *)tva;
	sc.t.pa = tbl_pa;
	sc.t.used = 1;
	/* zero with plain 64-bit stores: a bzero may use 16-byte stores, which device memory need not take */
	for (uint32_t i = 0; i < SC_POOL_PAGES * (0x1000u / 8u); i++) {
		((volatile uint64_t *)tva)[i] = 0;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	const uint64_t lo = panel_pa;
	const uint64_t hi = panel_pa + SC_SPLASH_SPAN;
	sc.panel_pa = panel_pa;
	sc.panel_bytes = SC_SPLASH_SPAN;
	if (panel_pa > 0xffffffffull || (panel_pa & 0xfffull) != 0 ||
	    (lo < SC_SLOT_BASE + SC_SLOTS * SC_SLOT_BYTES && hi > SC_SLOT_BASE) ||
	    !sc_pt_map(&sc.t, (uint32_t)lo, NULL, lo, (uint32_t)((hi - lo) >> 12), sc_clean)) {
		printf(SC_TAG " tables at %#llx: splash %#llx-%#llx not mapped: CPU copy\n", (unsigned long long)sc.t.pa,
		    (unsigned long long)lo, (unsigned long long)hi);
		sc.t.va = NULL;
		return ENOMEM;
	}

	/* The bank, translation still off; then TLB, then on. */
	sc.asid = sc.bank;
	const uint64_t ttbr0 = ((uint64_t)sc.asid << 48) | sc.t.pa;
	const uint32_t tcr2 = SC_TCR2_SEP | (oas == 0 ? 0u : oas == 1 ? 1u : 2u);
	const uint32_t f0 = mdp_rd(SC_INTF1 + IF_TEAR_LINE);
	sm_wr(sc.cb + CB_SCTLR, 0);
	sm_wr(sc.gr1 + SM_CBA2R(sc.bank), SC_CBA2R);
	sm_wr(sc.gr1 + SM_CBAR(sc.bank), (was_cbar & 0xff000000u) | SC_CBAR);
	sm_wr(sc.cb + CB_TCR2, tcr2);
	sm_wr(sc.cb + CB_TCR, SC_TCR);
	sm_wr64(sc.cb + CB_TTBR0, ttbr0);
	sm_wr64(sc.cb + CB_TTBR1, (uint64_t)sc.asid << 48);
	sm_wr(sc.cb + CB_MAIR0, SC_MAIR0);
	sm_wr(sc.cb + CB_MAIR1, 0);
	sm_wr(sc.cb + CB_FSR, 0xffffffffu);
	const bool tlb = sc_tlb_flush();
	const uint32_t rb_tcr = sm_rd(sc.cb + CB_TCR), rb_cbar = sm_rd(sc.gr1 + SM_CBAR(sc.bank));
	const uint64_t rb_ttbr0 = *(volatile uint64_t *)(sc.smmu + sc.cb + CB_TTBR0);
	bool ok = tlb && rb_tcr == SC_TCR && (rb_cbar & 0x00ffffffu) == SC_CBAR && rb_ttbr0 == ttbr0;
	if (ok) {
		sm_wr(sc.cb + CB_SCTLR, SC_SCTLR);
		/* A flush of the unchanged pipe must be taken (frames run) and the bank must not fault. */
		mdp_wr(SC_CTL0 + CT_FLUSH, CT_FLUSH_BITS);
		IOSleep(100);
		ok = (mdp_rd(SC_CTL0 + CT_FLUSH) & CT_FLUSH_BITS) == 0 && !sc_fault_check();
	}
	const uint32_t f1 = mdp_rd(SC_INTF1 + IF_TEAR_LINE);
	printf(SC_TAG " bank %u: tables %u pages at %#llx, tlb %s, readback tcr %08x cbar %08x ttbr0 %#llx; "
	    "sctlr %08x fsr %08x flush %08x, tear line %u -> %u: %s\n", sc.bank, sc.t.used,
	    (unsigned long long)sc.t.pa, tlb ? "ok" : "TIMEOUT", rb_tcr, rb_cbar, (unsigned long long)rb_ttbr0,
	    sm_rd(sc.cb + CB_SCTLR), sm_rd(sc.cb + CB_FSR), mdp_rd(SC_CTL0 + CT_FLUSH), f0, f1,
	    ok ? "the display reads through its own tables" : "back as the ABL left it, CPU copy");
	if (!ok) {
		sm_wr(sc.cb + CB_SCTLR, was_sctlr);
		sm_wr(sc.gr1 + SM_CBAR(sc.bank), was_cbar);
		sm_wr(sc.gr1 + SM_CBA2R(sc.bank), was_cba2r);
		sm_wr(sc.cb + CB_TCR, was_tcr);
		sm_wr(sc.cb + CB_TCR2, was_tcr2);
		sm_wr(sc.cb + CB_MAIR0, was_mair0);
		sm_wr(sc.cb + CB_MAIR1, was_mair1);
		sm_wr(sc.cb + CB_FSR, 0xffffffffu);
		/* The tables stay allocated: the SMMU may still hold walks of them. */
		return EIO;
	}
	for (unsigned i = 0; i < SC_SAVED_SSPP; i++) {
		sc.sspp[i] = mdp_rd(SC_VIG0 + sc_sspp_saved[i]);
	}
	for (unsigned i = 0; i < SC_SAVED_QSEED; i++) {
		sc.qseed[i] = mdp_rd(SC_QSEED + sc_qseed_saved[i]);
	}
	/*
	 * 0272: probe VIG1 for the two-pipe ping-pong. On this board VIG0 sits at 0x5000 (measured),
	 * which is the mainline dpu_5_0_sm8150.h VIG0 0x4000 shifted by 0x1000, so VIG1 is the
	 * mainline 0x6000 + 0x1000 = 0x7000 and its QSEED3 is 0x7a00. A real QSEED3 hardware version
	 * (0x1002 lite / 0x1004 v1.4, the two values sc_frame_regs already knows) is the test that
	 * this offset really is a VIG pipe; any other value leaves VIG1 unused and scanout stays
	 * single-pipe, exactly as 0268 was.
	 */
	sc.pp = false;
	sc.vig1_on = false;
	sc.cur = 1;                 /* the first frame goes to VIG0, already the base pipe */
	sc.flush_bits = CT_FLUSH_BITS;
	sc.layer0 = mdp_rd(SC_CTL0 + CT_LAYER0);
	/*
	 * VIG0 sits at some mixer stage the ABL chose (measured: value 5 on raphael, not 1). VIG1
	 * must go one stage ABOVE it so the newest frame covers the previous one, and its blend
	 * block follows from that stage (BLEND0 + STRIDE * (mix - 2)).
	 */
	sc.vig1_mix = (sc.layer0 & 7u) + 1u;
	sc.vig1_blend = SC_LM_BLEND0 + SC_LM_BLEND_STRIDE * (sc.vig1_mix - 2u);
	sc.blend_op = mdp_rd(SC_LM0 + sc.vig1_blend + SC_LM_BLEND_OP);
	sc.blend_alpha = mdp_rd(SC_LM0 + sc.vig1_blend + SC_LM_BLEND_ALPHA);
	const uint32_t v1q = mdp_rd(SC_QSEED1 + QS_HW_VERSION);
	const uint32_t v1fmt = mdp_rd(SC_VIG1 + SS_SRC_FORMAT);
	const uint32_t v1op = mdp_rd(SC_VIG1 + SS_OP_MODE);
	const uint32_t v1src = mdp_rd(SC_VIG1 + SS_SRC0_ADDR);
	if ((v1q & 0xffffu) == 0x1002u || (v1q & 0xffffu) == 0x1004u) {
		for (unsigned i = 0; i < SC_SAVED_SSPP; i++) {
			sc.sspp1[i] = mdp_rd(SC_VIG1 + sc_sspp_saved[i]);
		}
		for (unsigned i = 0; i < SC_SAVED_QSEED; i++) {
			sc.qseed1[i] = mdp_rd(SC_QSEED1 + sc_qseed_saved[i]);
		}
		/*
		 * 0272 is not ready for hardware use. The existing framebuffer releases
		 * every hold except the newest after one latch, but both attached pipes
		 * can still fetch their surfaces. Keep the proven single-pipe path until
		 * per-pipe retirement and the full VIG1 fetch setup are implemented.
		 */
		sc.pp = false;
	}
	printf(SC_TAG " VIG1 qseed3 %08x fmt %08x op %08x src %08x; CTL layer0 %08x (VIG0 mix %u -> VIG1 mix %u); LM0 blend off %04x op %08x alpha %08x: %s\n",
	    v1q, v1fmt, v1op, v1src, sc.layer0, sc.layer0 & 7u, sc.vig1_mix,
	    sc.vig1_blend, sc.blend_op, sc.blend_alpha,
	    sc.pp ? "VIG0/VIG1 ping-pong" : "VIG1 not a VIG pipe, single-pipe scanout");
	sc.on_panel = true;
	return 0;
}

/*
 * Panic: the panic screen and the console draw into the panel memory, so the pipe goes back to it
 * (KDP_EVENT_PANICLOG comes only on the panic path, not for stackshots). Interrupts are off and the
 * other CPUs stopped: register writes only, no lock.
 */
static void
sc_panic_callout(void *arg, kdp_event_t event)
{
	(void)arg;
	if (event != KDP_EVENT_PANICLOG || !sc.ready || sc.on_panel) {
		return;
	}
	for (unsigned i = 0; i < SC_SAVED_SSPP; i++) {
		mdp_wr(SC_VIG0 + sc_sspp_saved[i], sc.sspp[i]);
	}
	for (unsigned i = 0; i < SC_SAVED_QSEED; i++) {
		mdp_wr(SC_QSEED + sc_qseed_saved[i], sc.qseed[i]);
	}
	if (sc.pp && sc.vig1_on) {
		mdp_wr(SC_LM0 + sc.vig1_blend + SC_LM_BLEND_OP, sc.blend_op);
		mdp_wr(SC_LM0 + sc.vig1_blend + SC_LM_BLEND_ALPHA, sc.blend_alpha);
		mdp_wr(SC_CTL0 + CT_LAYER0, sc.layer0);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	mdp_wr(SC_CTL0 + CT_FLUSH, CT_FLUSH_VIG0 | CT_FLUSH_LM0 | CT_FLUSH_CTL);
	sc.on_panel = true;
}

int
sweet_scanout_init(uint64_t panel_pa, uint32_t rowbytes, uint32_t w, uint32_t h)
{
	sc_lock_init();
	lck_mtx_lock(&sc.lock);
	if (!sc.tried) {
		sc.tried = true;
		sc.error = sc_init_locked(panel_pa, rowbytes, w, h);
		sc.ready = sc.error == 0;
		if (sc.ready) {
			kdp_register_callout(sc_panic_callout, NULL);
		}
	}
	const int error = sc.error;
	lck_mtx_unlock(&sc.lock);
	return error;
}

bool
sweet_scanout_ready(void)
{
	return __atomic_load_n(&sc.ready, __ATOMIC_ACQUIRE);
}

uint32_t
sweet_scanout_map(const uint64_t *pages, uint32_t npages)
{
	if (!sweet_scanout_ready() || pages == NULL || npages == 0 || npages > SC_SLOT_BYTES / 0x1000) {
		return 0;
	}
	for (uint32_t i = 0; i < npages; i++) {
		if (pages[i] == 0 || (pages[i] & 0xfff) != 0 || (pages[i] >> 40) != 0) {
			return 0;
		}
	}
	uint32_t iova = 0;
	lck_mtx_lock(&sc.lock);
	for (unsigned i = 0; i < SC_SLOTS; i++) {
		if (!sc.slot_used[i]) {
			const uint32_t at = SC_SLOT_BASE + i * SC_SLOT_BYTES;
			if (sc_pt_map(&sc.t, at, pages, 0, npages, sc_clean)) {
				sc.slot_used[i] = true;
				iova = at;
			} else {
				sc_pt_unmap(&sc.t, at, npages, sc_clean);
				printf(SC_TAG " table pool full (%u pages)\n", sc.t.used);
			}
			(void)sc_tlb_flush();
			break;
		}
	}
	lck_mtx_unlock(&sc.lock);
	return iova;
}

void
sweet_scanout_unmap(uint32_t iova, uint32_t npages)
{
	if (!sweet_scanout_ready() || iova < SC_SLOT_BASE || iova >= SC_SLOT_BASE + SC_SLOTS * SC_SLOT_BYTES ||
	    (iova - SC_SLOT_BASE) % SC_SLOT_BYTES != 0 || npages > SC_SLOT_BYTES / 0x1000) {
		return;
	}
	lck_mtx_lock(&sc.lock);
	sc_pt_unmap(&sc.t, iova, npages, sc_clean);
	(void)sc_tlb_flush();
	sc.slot_used[(iova - SC_SLOT_BASE) / SC_SLOT_BYTES] = false;
	lck_mtx_unlock(&sc.lock);
}

int
sweet_scanout_show(const struct sweet_scan_frame *f)
{
	struct sc_regs r;
	if (!sweet_scanout_ready()) {
		return ENODEV;
	}
	if (f == NULL || !sc_frame_regs(f, sc.qseed_version, &r)) {
		return EINVAL;
	}
	lck_mtx_lock(&sc.lock);

	/* 0272: alternate VIG0/VIG1 so a frame is written while the other pipe is still scanning */
	unsigned pipe = 0;
	if (sc.pp) {
		pipe = sc.cur ^ 1u;
	}

	/* the target pipe's own flush must not be pending (at most 2 ms, then replaced and counted) */
	uint32_t flush = (pipe == 0 ? CT_FLUSH_VIG0 : CT_FLUSH_VIG1) | CT_FLUSH_CTL;
	if (!sc_flush_idle(pipe == 0 ? CT_FLUSH_VIG0 : CT_FLUSH_VIG1)) {
		lck_mtx_unlock(&sc.lock);
		return EBUSY;
	}

	/* connect VIG1 to the mixer one stage above VIG0 the first time it is used */
	if (sc.pp && pipe == 1 && !sc.vig1_on) {
		mdp_wr(SC_CTL0 + CT_LAYER0, (sc.layer0 & ~(7u << SC_MIX_VIG1_SHIFT)) | (sc.vig1_mix << SC_MIX_VIG1_SHIFT));
		mdp_wr(SC_LM0 + sc.vig1_blend + SC_LM_BLEND_OP, SC_BLEND_OP_CONST);
		if (((mdp_rd(SC_CTL0 + CT_LAYER0) >> SC_MIX_VIG1_SHIFT) & 7u) == sc.vig1_mix) {
			sc.vig1_on = true;
			flush |= CT_FLUSH_LM0;
		} else {
			/* the mixer did not take VIG1: stay single-pipe and put this frame on VIG0 */
			mdp_wr(SC_CTL0 + CT_LAYER0, sc.layer0);
			sc.pp = false;
			pipe = 0;
			flush = CT_FLUSH_VIG0 | CT_FLUSH_CTL;
		}
	}

	const uint32_t p = pipe == 0 ? SC_VIG0 : SC_VIG1;
	const uint32_t q = pipe == 0 ? SC_QSEED : SC_QSEED1;
	mdp_wr(p + SS_SRC0_ADDR, r.addr);
	mdp_wr(p + SS_YSTRIDE0, r.stride);
	mdp_wr(p + SS_SRC_SIZE, r.src_size);
	mdp_wr(p + SS_SRC_XY, r.src_xy);
	mdp_wr(p + SS_OUT_SIZE, r.out_size);
	mdp_wr(p + SS_OUT_XY, r.out_xy);
	mdp_wr(p + SS_PE_C0_LR, 0);
	mdp_wr(p + SS_PE_C0_TB, 0);
	mdp_wr(p + SS_PE_C0_REQ, r.req);
	mdp_wr(p + SS_PE_C12_LR, 0);
	mdp_wr(p + SS_PE_C12_TB, 0);
	mdp_wr(p + SS_PE_C12_REQ, r.req);
	mdp_wr(p + SS_PE_C3_LR, 0);
	mdp_wr(p + SS_PE_C3_TB, 0);
	mdp_wr(p + SS_PE_C3_REQ, r.req);
	if (r.scaled) {
		if (sc.qseed_version == 0x1002) {
			mdp_wr(q + QS_PHASE_INIT, 0);
		} else {
			mdp_wr(q + QS_INIT_Y_H, 0);
			mdp_wr(q + QS_INIT_Y_V, 0);
			mdp_wr(q + QS_INIT_UV_H, 0);
			mdp_wr(q + QS_INIT_UV_V, 0);
		}
		mdp_wr(q + QS_STEP_Y_H, r.step_h);
		mdp_wr(q + QS_STEP_Y_V, r.step_v);
		mdp_wr(q + QS_STEP_UV_H, r.step_h);
		mdp_wr(q + QS_STEP_UV_V, r.step_v);
		mdp_wr(q + QS_PRELOAD, r.preload);
		mdp_wr(q + QS_SRC_SIZE_Y, r.qsrc);
		mdp_wr(q + QS_SRC_SIZE_UV, r.qsrc);
		mdp_wr(q + QS_DST_SIZE, r.qdst);
		mdp_wr(q + QS_OP_MODE, r.qop);
	} else {
		const uint32_t saved_op = pipe == 0 ? sc.qseed[SC_SAVED_QSEED - 1] : sc.qseed1[SC_SAVED_QSEED - 1];
		mdp_wr(q + QS_OP_MODE, saved_op);
	}

	/* blend alpha: the VIG1 stage covers the base when it is the newest, is transparent otherwise */
	if (sc.pp && sc.vig1_on) {
		mdp_wr(SC_LM0 + sc.vig1_blend + SC_LM_BLEND_ALPHA, pipe == 1 ? SC_BLEND_VIG1_ON : SC_BLEND_VIG0_ON);
		flush |= CT_FLUSH_LM0;
	}

	__asm__ volatile ("dsb sy" ::: "memory");
	mdp_wr(SC_CTL0 + CT_FLUSH, flush);
	sc.on_panel = false;
	sc.cur = pipe;
	sc.flush_bits = flush;
	sc.shows++;
	lck_mtx_unlock(&sc.lock);
	return 0;
}

int
sweet_scanout_panel(void)
{
	if (!sweet_scanout_ready()) {
		return 0;
	}
	lck_mtx_lock(&sc.lock);
	if (sc.on_panel) {
		lck_mtx_unlock(&sc.lock);
		return 0;
	}
	if (!sc_flush_idle(CT_FLUSH_VIG0 | CT_FLUSH_VIG1 | CT_FLUSH_LM0 | CT_FLUSH_CTL)) {
		lck_mtx_unlock(&sc.lock);
		return EBUSY;
	}
	for (unsigned i = 0; i < SC_SAVED_SSPP; i++) {
		mdp_wr(SC_VIG0 + sc_sspp_saved[i], sc.sspp[i]);
	}
	for (unsigned i = 0; i < SC_SAVED_QSEED; i++) {
		mdp_wr(SC_QSEED + sc_qseed_saved[i], sc.qseed[i]);
	}
	uint32_t flush = CT_FLUSH_VIG0 | CT_FLUSH_CTL;
	if (sc.pp && sc.vig1_on) {
		/* drop VIG1 from the mixer and put its blend block back; the pipe's own registers are
		 * left as show() programmed them because removing it from LAYER0 is what stops it. Its
		 * flush bit is deliberately NOT flushed: once the pipe is out of the mixer the hardware
		 * never takes a VIG1 flush, which made waitLatch spin on it for the full 25 ms. */
		mdp_wr(SC_LM0 + sc.vig1_blend + SC_LM_BLEND_OP, sc.blend_op);
		mdp_wr(SC_LM0 + sc.vig1_blend + SC_LM_BLEND_ALPHA, sc.blend_alpha);
		mdp_wr(SC_CTL0 + CT_LAYER0, sc.layer0);
		flush |= CT_FLUSH_LM0;
		sc.vig1_on = false;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	mdp_wr(SC_CTL0 + CT_FLUSH, flush);
	sc.on_panel = true;
	sc.cur = 1;                 /* the base (VIG0) is on the panel again; the next frame is VIG0 */
	sc.flush_bits = flush;
	lck_mtx_unlock(&sc.lock);
	return 0;
}

bool
sweet_scanout_latched(void)
{
	if (!sweet_scanout_ready()) {
		return true;
	}
	if (sc_fault_check() && sc.faults >= 3) {
		/* Something the tables do not map: back to the CPU copy (the framebuffer takes the panel back). */
		if (__atomic_exchange_n(&sc_mode, 0u, __ATOMIC_RELAXED) != 0) {
			printf(SC_TAG " %llu SMMU faults: scanout off (sweetfb_scanout=0), CPU copy\n", (unsigned long long)sc.faults);
		}
	}
	return (mdp_rd(SC_CTL0 + CT_FLUSH) & sc.flush_bits) == 0;
}

/* The CTL flush bits of the last show()/panel() (for SwapWait's honest answer). */
uint32_t
sweet_scanout_flush_bits(void)
{
	return sc.flush_bits;
}

bool
sweet_scanout_wait_latched(unsigned max_ms)
{
	if (!sweet_scanout_ready()) {
		return true;
	}
	const unsigned polls = max_ms * 4;
	for (unsigned i = 0; i < polls; i++) {
		if (sweet_scanout_latched()) {
			return true;
		}
		sc_nap_us(250);
	}
	return sweet_scanout_latched();
}

void
sweet_scanout_counts(uint64_t *shows, uint64_t *waits, uint64_t *faults)
{
	*shows = sc.shows;
	*waits = sc.waits;
	*faults = sc.faults;
}

int
sweet_scanout_ctl(unsigned mode, char *why, size_t len)
{
	sc_mode_args();
	if (mode > 1) {
		snprintf(why, len, "scanout: 0 or 1");
		return EINVAL;
	}
	__atomic_store_n(&sc_mode, mode, __ATOMIC_RELAXED);
	snprintf(why, len, "scanout %u (%s; hardware %s, %llu pipe updates, %llu waited, %llu replaced a "
	    "pending frame, %llu SMMU faults)", mode, mode == 0 ? "CPU copy" : "the display reads Metal frames",
	    sc.ready ? "set up" : sc.tried ? "refused, CPU copy" : "not set up yet", (unsigned long long)sc.shows,
	    (unsigned long long)sc.waits, (unsigned long long)sc.replaced, (unsigned long long)sc.faults);
	printf(SC_TAG " %s\n", why);
	return 0;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
