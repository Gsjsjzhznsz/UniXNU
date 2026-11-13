/*
 * raphael (Redmi K20 Pro, SM8150) DSI0 DCS brightness.
 *
 * raphael's AMOLED takes its brightness over MIPI DCS 0x51 (two bytes, high
 * byte first), not over the PM8150L WLED: Xiaomi's raphael DTS marks the panel
 * bl_ctrl_dcs with min 2 / max 2047 (xiaomi-sm8150-common.dtsi:329-337,
 * dsi-panel-ss-fhd-ea8076-cmd.dtsi), and dsi_panel_set_backlight() only ever
 * calls the WLED path for a panel whose bl_config.type is WLED.
 *
 * Everything here is opt-in: without the boot argument raphael_dcs=1 the
 * module maps nothing, reads no register and writes no register. With it, the
 * read sysctl returns a snapshot and a write to raphael_dcs_level sends one
 * DCS 0x51 packet.
 *
 * The packet layout and the FIFO sequence are from MiCode/Xiaomi_Kernel_
 * OpenSource raphael-p-oss @ 663db50e195a8a54c8565ac4ab0c6d9ba7d80db0:
 *   dsi_ctrl_copy_and_pad_cmd()      pad to 32 bits with 0xFF, set BIT(6) in
 *                                    header byte 3 when a payload follows
 *   dsi_ctrl_cmd_tx_trigger()        set BIT(7) in byte 3 for the last command
 *   mipi_dsi_create_packet()         long write: header[0..1] = word count LE,
 *                                    header[2] = (channel << 6) | type
 *   dsi_ctrl_hw_cmn_kickoff_fifo_command()
 *                                    TPG ctrl, push dwords, EMBEDDED_MODE,
 *                                    DMA_CMD_LENGTH, SW_TRIGGER
 *   dsi_ctrl_hw_cmn_wait_for_cmd_mode_mdp_idle()
 *                                    poll DSI_STATUS bit 2 before touching the
 *                                    FIFO (200 ms, 2 ms steps)
 *   dsi_ctrl_hw_cmn_get_interrupt_status()
 *                                    DSI_INT_CTRL bit 0 = CMD_MODE_DMA_DONE
 *
 * This runs FIFO transfers only. It never resets the controller, never
 * changes lane/PHY/clock/timing configuration, never acks an error interrupt
 * other than clearing the completion bit it consumed, and restores
 * COMMAND_MODE_DMA_CTRL to the value it found. The panel keeps its
 * bootloader-programmed mode and refresh.
 *
 * The controller does not report a usable hardware version: downstream picks
 * v2.3 from the DT compatible string, so the version register can only be
 * logged, never used to prove "this is v2.3".
 */
#ifndef _RAPHAEL_DSI_H_
#define _RAPHAEL_DSI_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define RAPHAEL_DSI_PA          0x0ae94000u     /* sm8150-sde.dtsi: DSI0 ctrl */
#define RAPHAEL_DSI_LEN         0x400u
#define RAPHAEL_DSI_LEVEL_MAX   2047u
#define RAPHAEL_DSI_TEXT_MAX    1024

/* dsi_ctrl_reg.h, offsets from the controller base. */
#define RDS_HW_VERSION          0x0000u
#define RDS_STATUS              0x0008u
#define RDS_FIFO_STATUS         0x000cu
#define RDS_COMMAND_MODE_DMA_CTRL 0x003cu
#define RDS_DMA_CMD_LENGTH      0x004cu
#define RDS_ACK_ERR_STATUS      0x0068u
#define RDS_TRIG_CTRL           0x0084u
#define RDS_CMD_MODE_DMA_SW_TRIGGER 0x0090u
#define RDS_INT_CTRL            0x0110u
#define RDS_TEST_PATTERN_GEN_CTRL 0x015cu
#define RDS_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL 0x017cu

/* DSI_STATUS bit 2: the command-mode MDP has data in flight. */
#define RDS_STATUS_CMD_MODE_MDP_BUSY (1u << 2)
/* DSI_INT_CTRL bit 0 / bit 24. */
#define RDS_INT_CMD_MODE_DMA_DONE (1u << 0)
#define RDS_INT_ERROR             (1u << 24)

/* dsi_ctrl_hw_cmn_kickoff_fifo_command(): BIT(1)|BIT(2)|(0x3 << 16). */
#define RDS_TPG_CTRL_FIFO_MODE   ((1u << 1) | (1u << 2) | (0x3u << 16))
/* DSI_COMMAND_MODE_DMA_CTRL bits we touch, all restored afterwards. */
#define RDS_DMA_CTRL_BROADCAST   (1u << 31)
#define RDS_DMA_CTRL_MASTER      (1u << 30)
#define RDS_DMA_CTRL_USE_LPM     (1u << 26)
#define RDS_DMA_CTRL_EMBEDDED    (1u << 28)

#define RDS_BOOTARG             "raphael_dcs"

/*
 * One MIPI DCS long write of 51 <hi> <lo>, as the two 32-bit little-endian
 * dwords the FIFO expects. Size is always 8 bytes: the 4-byte packet header
 * plus three payload bytes padded with 0xFF, so the odd-dword padding rule in
 * kickoff_fifo_command() never applies.
 *
 *   byte 0  word count LSB            3
 *   byte 1  word count MSB            0
 *   byte 2  (channel << 6) | DT       0x39 = DCS long write, channel 0
 *   byte 3  BIT(7) last | BIT(6) payload | ECC (hardware computes it)
 *   byte 4.. 51, hi, lo, 0xFF pad
 */
static inline int
raphael_dsi_pack_brightness(uint32_t level, uint32_t words[2])
{
	if (words == 0 || level > RAPHAEL_DSI_LEVEL_MAX) {
		return -1;
	}
	words[0] = 0x03u | (0x39u << 16) | (0xc0u << 24);              /* 0xc0390003 */
	words[1] = 0x51u | (((level >> 8) & 0xffu) << 8) |
	    ((level & 0xffu) << 16) | (0xffu << 24);                   /* 0xff..0051 */
	return 0;
}

/* Boot argument raphael_dcs=1 was given and the controller is mapped. */
bool raphael_dsi_available(void);

/* Read-only snapshot of the controller registers, into TEXT. 0 on success. */
int raphael_dsi_snapshot(char *text, size_t len);

/*
 * Send one DCS 0x51. Thread context, one caller at a time, returns 0 only when
 * the controller reported CMD_MODE_DMA_DONE without an error bit.
 */
int raphael_dsi_set_brightness(uint32_t level);

#endif /* _RAPHAEL_DSI_H_ */
