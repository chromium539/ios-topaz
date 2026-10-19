/*
 * marie milestone 2 probe: can we talk to the eMMC Huawei fastboot booted us
 * from, using the state it left the controller in?
 *
 * Kirin 710 eMMC host: sdhci@FF3E1000 ("mshc,sdhci", Linux
 * sdhci_mshc_libra.c). Its first reg range, 0xFF3E6000, is the standard
 * SDHCI register set; 0xFF3E1000 (the node name) is the eMMC PHY control. Runs with the MMU off
 * (Device memory) and uses PIO only. READ-ONLY: the only commands sent are
 * CMD13 (status), CMD8 (EXT_CSD) and CMD17 (read one block); there is no
 * write command in this file.
 */
#include <stdint.h>
#include "console.h"
#include "emmc.h"

#define EMMC_BASE	0xff3e6000UL	/* kirin710_mmc.dtsi reg[0] "HCI standard" */
#define RCA		1		/* Linux: mmc0:0001 */

/* SDHCI (Linux drivers/mmc/host/sdhci.h) */
#define SD_BLKSIZE	0x04	/* 16-bit size + 16-bit count at 0x06 */
#define SD_ARG		0x08
#define SD_XFER		0x0c	/* 16-bit transfer mode + 16-bit command at 0x0e */
#define SD_RESP0	0x10
#define SD_BUFFER	0x20
#define SD_PSTATE	0x24
#define SD_HOSTCTL	0x28
#define SD_CLKCTL	0x2c	/* + timeout 0x2e, reset 0x2f */
#define SD_INTSTS	0x30	/* normal 16 + error 16 */
#define SD_INTEN	0x34
#define SD_SIGEN	0x38
#define SD_HOSTCTL2	0x3c	/* auto CMD12 err 16 + host control 2 at 0x3e */
#define SD_CAPS		0x40
#define SD_VERSION	0xfc	/* slot int 16 + host version at 0xfe */

#define INT_CMD_DONE	(1u << 0)
#define INT_XFER_DONE	(1u << 1)
#define INT_BUF_RD	(1u << 5)
#define INT_ERROR	(1u << 15)

#define CMD_RESP48	0x02
#define CMD_CRC		0x08
#define CMD_INDEX	0x10
#define CMD_DATA	0x20
#define CMD_R1		(CMD_RESP48 | CMD_CRC | CMD_INDEX)
#define XFER_READ	0x10

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

static inline uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(EMMC_BASE + off); }
static inline void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(EMMC_BASE + off) = v; }

static uint8_t blk[512] __attribute__((aligned(16)));

static uint64_t ticks(void)
{
	__asm__ volatile("isb" ::: "memory");
	return rsr("cntpct_el0");
}

/* Wait for any of `bits` (or an error) in INT_STATUS; 0 on timeout. */
static uint32_t wait_int(uint32_t bits, unsigned ms)
{
	uint64_t end = ticks() + rsr("cntfrq_el0") / 1000 * ms;
	uint32_t s;

	do {
		s = rd(SD_INTSTS);
		if (s & (bits | INT_ERROR))
			return s;
	} while (ticks() < end);
	return 0;
}

/* One command; returns 0 or the INT_STATUS value that failed (bit 31 set on timeout). */
static uint32_t cmd(uint32_t idx, uint32_t arg, uint32_t flags, uint32_t mode, uint32_t *resp)
{
	uint64_t end = ticks() + rsr("cntfrq_el0") / 10;

	while (rd(SD_PSTATE) & ((flags & CMD_DATA) ? 3u : 1u))	/* CMD/DAT inhibit */
		if (ticks() > end)
			return 0x80000000u | rd(SD_PSTATE);
	wr(SD_INTSTS, 0xffffffff);
	wr(SD_ARG, arg);
	wr(SD_XFER, (idx << 24) | (flags << 16) | mode);
	uint32_t s = wait_int(INT_CMD_DONE, 100);
	if (s == 0)
		return 0x80000000u;
	if (s & INT_ERROR)
		return s;
	*resp = rd(SD_RESP0);
	wr(SD_INTSTS, INT_CMD_DONE);
	return 0;
}

/* Single-block PIO read with CMD (8 = EXT_CSD, 17 = sector). */
static uint32_t read512(uint32_t idx, uint32_t arg, uint32_t *r1)
{
	wr(SD_BLKSIZE, (1u << 16) | (7u << 12) | 512);
	uint32_t e = cmd(idx, arg, CMD_R1 | CMD_DATA, XFER_READ, r1);
	if (e)
		return e;
	uint32_t s = wait_int(INT_BUF_RD, 500);
	if (s == 0 || (s & INT_ERROR))
		return s ? s : 0x80000001u;
	for (int i = 0; i < 128; i++)
		((uint32_t *)blk)[i] = rd(SD_BUFFER);
	wr(SD_INTSTS, INT_BUF_RD);
	s = wait_int(INT_XFER_DONE, 500);
	if (s == 0 || (s & INT_ERROR))
		return s ? s : 0x80000002u;
	wr(SD_INTSTS, 0xffffffff);
	return 0;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static int name_is(const uint8_t *utf16, const char *want)
{
	for (int i = 0; i < 36; i++) {
		char c = (char)utf16[2 * i];
		if (utf16[2 * i + 1] || c != want[i])
			return 0;
		if (!c)
			return 1;
	}
	return 0;
}

void test_emmc(void)
{
	uint32_t r1, e;

	con_hex2("EMMC", "ver=", rd(SD_VERSION) >> 16, "caps=", rd(SD_CAPS));
	con_hex2(" regs", "pstate=", rd(SD_PSTATE), "hostctl=", rd(SD_HOSTCTL));
	con_hex2(" regs", "clkctl=", rd(SD_CLKCTL), "hostctl2=", rd(SD_HOSTCTL2));
	con_hex2(" regs", "inten=", rd(SD_INTEN), "sigen=", rd(SD_SIGEN));
	con_hex2(" regs", "intsts=", rd(SD_INTSTS), "blk=", rd(SD_BLKSIZE));

	/* Latch every status bit; no interrupt signals (we poll). */
	uint32_t old_en = rd(SD_INTEN), old_sig = rd(SD_SIGEN);
	wr(SD_SIGEN, 0);
	wr(SD_INTEN, 0xffffffff);

	e = cmd(13, RCA << 16, CMD_R1, 0, &r1);
	con_hex2("CMD13", "err=", e, "r1=", r1);
	if (e)
		goto out;
	con_hex2(" state", "cur=", (r1 >> 9) & 0xf, "4=tran", 0);

	e = read512(8, 0, &r1);
	con_hex2("EXTCSD", "err=", e, "r1=", r1);
	if (!e) {
		con_hex2(" csd", "cmdq_en=", blk[15], "part_cfg=", blk[179]);
		con_hex2(" csd", "bus_w=", blk[183], "hs_tim=", blk[185]);
		con_hex2(" csd", "sectors=", le32(blk + 212), "rev=", blk[192]);
	}

	e = read512(17, 1, &r1);
	con_hex2("GPT", "err=", e, "r1=", r1);
	if (e)
		goto out;
	if (le64(blk) != 0x5452415020494645ULL) {	/* "EFI PART" */
		con_hex("GPT", le64(blk));
		goto out;
	}
	uint64_t ent_lba = le64(blk + 72);
	uint32_t nent = le32(blk + 80), esz = le32(blk + 84);
	con_hex2(" hdr", "entries=", nent, "size=", esz);
	if (esz != 128 || nent > 256)
		goto out;
	for (uint32_t i = 0; i < nent; i++) {
		if (i % 4 == 0 && (e = read512(17, (uint32_t)(ent_lba + i / 4), &r1)) != 0) {
			con_hex2("GPT ent", "err=", e, "i=", i);
			goto out;
		}
		const uint8_t *ent = blk + (i % 4) * 128;
		if (name_is(ent + 56, "userdata")) {
			con_hex2("USERDATA", "index=", i + 1, "first=", (uint32_t)le64(ent + 32));
			con_hex2(" last", "lba=", (uint32_t)le64(ent + 40), "hi=", (uint32_t)(le64(ent + 40) >> 32));
			break;
		}
	}
out:
	wr(SD_INTSTS, 0xffffffff);
	wr(SD_INTEN, old_en);
	wr(SD_SIGEN, old_sig);
}
