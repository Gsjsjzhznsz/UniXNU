/*
 * raphael DSI0 DCS brightness. See raphael_dsi.h for the sources and for why
 * this is DCS and not the PM8150L WLED.
 *
 * Boot argument raphael_dcs=1 arms everything. Without it this file maps
 * nothing and touches no register: the image behaves exactly like one built
 * without this patch.
 */
#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
#include <string.h>

#include "sweet_board.h"
#include "raphael_dsi.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void IOSleep(unsigned int milliseconds);
extern void IODelay(unsigned int microseconds);

/* Try-lock, not a sleep lock: ml_io_map and the bounded polls may sleep. */
static unsigned rd_busy;
static unsigned rd_armed;               /* raphael_dcs=1 was given */
static vm_offset_t rd_base;             /* 0 until the first armed use */
static unsigned rd_sequence;
static int rd_last_error;
static uint32_t rd_version, rd_status, rd_fifo_status, rd_dma_ctrl;
static uint32_t rd_last_level;
static unsigned rd_writes;
static char rd_text[RAPHAEL_DSI_TEXT_MAX] =
    "raphael_dsi v1 mode=off armed=0 writes=0\n";

static bool
rd_enter(void)
{
	return __atomic_exchange_n(&rd_busy, 1u, __ATOMIC_ACQUIRE) == 0;
}

static void
rd_leave(void)
{
	__atomic_store_n(&rd_busy, 0u, __ATOMIC_RELEASE);
}

static uint32_t
rd_rd(uint32_t off)
{
	return *(volatile uint32_t *)(rd_base + off);
}

static void
rd_wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(rd_base + off) = val;
}

/* Parse the boot argument once. Called from sysctl context: cheap and idempotent. */
static bool
rd_check_armed(void)
{
	static unsigned once;
	int v = 0;

	if (__atomic_load_n(&once, __ATOMIC_ACQUIRE) == 0) {
		__atomic_store_n(&once, 1u, __ATOMIC_RELEASE);
		if (PE_parse_boot_argn(RDS_BOOTARG, &v, sizeof(v)) && v == 1) {
			__atomic_store_n(&rd_armed, 1u, __ATOMIC_RELEASE);
		}
	}
	return __atomic_load_n(&rd_armed, __ATOMIC_ACQUIRE) != 0;
}

/*
 * Map once. The controller is inside the always-on MDSS window that the
 * display is actively driving, so this only happens after an explicit opt-in.
 */
static int
rd_map(void)
{
	if (rd_base != 0) {
		return 0;
	}
	if (!sweet_board_raphael()) {
		return ENOTSUP;
	}
	rd_base = ml_io_map((vm_offset_t)RAPHAEL_DSI_PA, (vm_size_t)RAPHAEL_DSI_LEN);
	if (rd_base == 0) {
		return ENXIO;
	}
	return 0;
}

bool
raphael_dsi_available(void)
{
	return rd_check_armed();
}

/* Collect the read-only state. Caller holds rd_busy. */
static void
rd_collect(void)
{
	/*
	 * DSI_STATUS bit 2: the command-mode MDP is pushing a frame. Reading the
	 * other registers while it does is what the downstream dump does too, so
	 * this is a note in the output, not a refusal.
	 */
	rd_status = rd_rd(RDS_STATUS);
	rd_fifo_status = rd_rd(RDS_FIFO_STATUS);
	rd_dma_ctrl = rd_rd(RDS_COMMAND_MODE_DMA_CTRL);
	rd_version = rd_rd(RDS_HW_VERSION);
}

/*
 * Wait for the command-mode MDP to be idle, mirroring
 * dsi_ctrl_hw_cmn_wait_for_cmd_mode_mdp_idle(): bit 2 of DSI_STATUS must be
 * clear. 200 ms in 2 ms steps, no reset on timeout.
 */
static int
rd_wait_idle(void)
{
	for (unsigned i = 0; i < 100; i++) {
		if ((rd_rd(RDS_STATUS) & RDS_STATUS_CMD_MODE_MDP_BUSY) == 0) {
			return 0;
		}
		IOSleep(2);
	}
	return ETIMEDOUT;
}

/*
 * One DCS 0x51 through the command FIFO. Returns 0 only when the controller
 * reported CMD_MODE_DMA_DONE without DSI_INT_CTRL bit 24 (error).
 */
int
raphael_dsi_set_brightness(uint32_t level)
{
	uint32_t words[2];
	uint32_t saved_ctrl;
	int error;

	if (!rd_check_armed()) {
		return ENOTSUP;
	}
	if (level > RAPHAEL_DSI_LEVEL_MAX || raphael_dsi_pack_brightness(level, words) != 0) {
		return EINVAL;
	}
	if (!rd_enter()) {
		return EBUSY;
	}
	error = rd_map();
	if (error == 0) {
		error = rd_wait_idle();
	}
	if (error != 0) {
		rd_last_error = error;
		rd_leave();
		return error;
	}

	saved_ctrl = rd_rd(RDS_COMMAND_MODE_DMA_CTRL);

	/* Drop a stale completion so the poll below cannot read the previous one. */
	rd_wr(RDS_INT_CTRL, RDS_INT_CMD_MODE_DMA_DONE);

	rd_wr(RDS_TEST_PATTERN_GEN_CTRL, RDS_TPG_CTRL_FIFO_MODE);
	rd_wr(RDS_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, words[0]);
	rd_wr(RDS_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, words[1]);
	/* 8 bytes = 2 dwords, even, so no odd-dword zero padding is needed. */
	rd_wr(RDS_COMMAND_MODE_DMA_CTRL,
	    (saved_ctrl & ~(RDS_DMA_CTRL_BROADCAST | RDS_DMA_CTRL_MASTER |
	    RDS_DMA_CTRL_USE_LPM)) | RDS_DMA_CTRL_EMBEDDED);
	rd_wr(RDS_DMA_CMD_LENGTH, 8);
	__asm__ volatile ("dsb sy" ::: "memory");
	rd_wr(RDS_CMD_MODE_DMA_SW_TRIGGER, 1);

	error = ETIMEDOUT;
	for (unsigned i = 0; i < 500; i++) {
		const uint32_t ints = rd_rd(RDS_INT_CTRL);
		if (ints & RDS_INT_ERROR) {
			error = EIO;
			break;
		}
		if (ints & RDS_INT_CMD_MODE_DMA_DONE) {
			error = 0;
			break;
		}
		IODelay(100);
	}

	/* Put the frame path's DMA control back; the embedded bit is ours alone. */
	rd_wr(RDS_COMMAND_MODE_DMA_CTRL, saved_ctrl);
	__asm__ volatile ("dsb sy" ::: "memory");
	if (error == 0) {
		rd_wr(RDS_INT_CTRL, RDS_INT_CMD_MODE_DMA_DONE);
		rd_last_level = level;
		rd_writes++;
		rd_collect();
	}
	rd_last_error = error;
	rd_leave();
	return error;
}

int
raphael_dsi_snapshot(char *text, size_t len)
{
	int n = 0;
	int error = 0;

	if (text == NULL || len == 0) {
		return EINVAL;
	}
	if (!rd_check_armed()) {
		snprintf(text, len, "raphael_dsi v1 mode=off armed=0 writes=0\n");
		return 0;
	}
	if (!rd_enter()) {
		return EBUSY;
	}
	error = rd_map();
	if (error == 0) {
		rd_sequence++;
		rd_collect();
	}
	if (error == 0) {
		n = snprintf(text, len,
		    "raphael_dsi v1 mode=snapshot armed=1 seq=%u writes=%u last_level=%u "
		    "last_error=%d busy=%u base=0x%08x len=0x%x\n",
		    rd_sequence, rd_writes, rd_last_level, rd_last_error,
		    (rd_status & RDS_STATUS_CMD_MODE_MDP_BUSY) ? 1u : 0u,
		    RAPHAEL_DSI_PA, RAPHAEL_DSI_LEN);
	}
	if (error == 0 && n > 0 && (size_t)n < len) {
		const int m = snprintf(text + n, len - (size_t)n,
		    "hw_version=0x%08x status=0x%08x fifo_status=0x%08x dma_ctrl=0x%08x\n",
		    rd_version, rd_status, rd_fifo_status, rd_dma_ctrl);
		if (m < 0 || (size_t)m >= len - (size_t)n) {
			error = EOVERFLOW;
		}
	}
	if (error != 0) {
		snprintf(text, len, "raphael_dsi v1 mode=error armed=1 errno=%d\n", error);
	}
	rd_leave();
	return error;
}

static int
raphael_dsi_sysctl SYSCTL_HANDLER_ARGS
{
	char buf[RAPHAEL_DSI_TEXT_MAX];
	int error;

	if (!sweet_board_raphael()) {
		snprintf(buf, sizeof(buf),
		    "raphael_dsi v1 mode=unsupported-board armed=0 writes=0\n");
	} else {
		error = raphael_dsi_snapshot(buf, sizeof(buf));
		if (error != 0) {
			return error;
		}
	}
	return sysctl_handle_string(oidp, buf, sizeof(buf), req);
}

static int
raphael_dcs_level_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	int level = 0;
	int error;

	if (req->newptr == NULL) {
		return EINVAL;
	}
	error = sysctl_handle_int(oidp, &level, 0, req);
	if (error != 0) {
		return error;
	}
	if (level < 0) {
		return EINVAL;
	}
	return raphael_dsi_set_brightness((uint32_t)level);
}

SYSCTL_PROC(_kern, OID_AUTO, raphael_dsi,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    raphael_dsi_sysctl, "A",
    "Raphael DSI0 state; needs boot argument raphael_dcs=1");
SYSCTL_PROC(_kern, OID_AUTO, raphael_dcs_level,
    CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_LOCKED, 0, 0,
    raphael_dcs_level_sysctl, "I",
    "Write 0..2047 to send one DCS 0x51 brightness write; needs raphael_dcs=1");
#endif /* ARM64_BOARD_CONFIG_SWEET */
