/*
 * marie milestone 4 probe: can we reach the Novatek NT36682A TDDI touch
 * controller on SPI1 from the state Huawei fastboot leaves?
 *
 * Hardware map from the live device tree (PLAN.md "Touch hardware"): PL022
 * spi@fdf08000 (bus 1), mode 1, chip select driven as GPIO (PL061
 * gpio@e8a0c000 pin 2), IRQ on gpio@fff0f000 pin 2, reset on
 * gpio@e8a10000 pin 0. SPI1 pins GPIO 7/8/9 = function 1.
 *
 * Writes: the SPI1 clock gates, the SPI1 pin mux, the CS GPIO, the PL022's
 * own registers, the touch reset GPIO (released high: fastboot leaves it
 * driven low, milestone 4 run 1) and on the touch IC the page-select command
 * plus Novatek's bootloader reset (0x69 to SWRST_N8 0x03F0FE), as Huawei's
 * Linux driver does before reading the chip ID.
 *
 * Milestone 4b: with the firmware embedded (fwblob.S), download it the way
 * Huawei's nt36xxx_fw_update_spi.c does (NT36672A memory map), then poll the
 * event buffer and draw a square wherever the panel is touched.
 */
#include <stdint.h>
#include "console.h"
#include "touch.h"

#define PERI_CRG	0xfff35000UL
#define PEREN2		0x020
#define PERSTAT2	0x028
#define PERRSTSTAT2	0x080
#define CLKDIV_SPI	0x0c4	/* clkdiv_spi: hiword, mask 0xf000 */
#define CLKMUX_SPI	0x0ac	/* clkmux_spi: hiword, bit 8 */
#define CLKGT_SPI	0x0f4	/* clkgt_spi: hiword, bit 13 */
#define SPI1_GATE	(1u << 9)

#define IOMG_SPI1	0xe896c708UL	/* GPIO 7, 8, 9: +0, +4, +8; function 1 = SPI1 */
#define IOCG_SPI1	0xe896cf08UL	/* pinmux@e896c800 + 0x708 */
#define IOMG_TP_IRQ	0xfff11708UL	/* GPIO 210 */

#define SPI1		0xfdf08000UL
#define SSPCR0		0x00
#define SSPCR1		0x04
#define SSPDR		0x08
#define SSPSR		0x0c
#define SSPCPSR		0x10
#define SSPPID0		0xfe0
#define SR_TFE		(1u << 0)
#define SR_TNF		(1u << 1)
#define SR_RNE		(1u << 2)
#define SR_BSY		(1u << 4)

#define GPIO_CS		0xe8a0c000UL
#define GPIO_IRQ	0xfff0f000UL
#define GPIO_RST	0xe8a10000UL
#define GPIODIR		0x400
#define GPIO_BIT(base, pin)	((base) + (4u << (pin)))	/* PL061 masked data access */

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; __asm__ volatile("dsb sy" ::: "memory"); }
static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }

static uint32_t peek(uintptr_t a)
{
	int ok;
	uint32_t v = probe32(a, &ok);
	return ok ? v : 0xdeadbeef;
}

static void udelay(unsigned us)
{
	uint64_t f = rsr("cntfrq_el0");
	__asm__ volatile("isb");
	uint64_t end = rsr("cntpct_el0") + f / 1000000 * us + 1;
	while (rsr("cntpct_el0") < end)
		__asm__ volatile("isb");
}

static void cs(int level)
{
	wr(GPIO_BIT(GPIO_CS, 2), level ? 0xff : 0);
}

/* One chip-select frame: n bytes out, n bytes in. 0 on success. */
static int xfer(const uint8_t *tx, uint8_t *rx, unsigned n)
{
	cs(0);
	udelay(5);
	for (unsigned i = 0; i < n; i++) {
		unsigned t = 100000;
		while (!(rd(SPI1 + SSPSR) & SR_TNF))
			if (!--t)
				goto fail;
		wr(SPI1 + SSPDR, tx[i]);
		t = 100000;
		while (!(rd(SPI1 + SSPSR) & SR_RNE))
			if (!--t)
				goto fail;
		rx[i] = (uint8_t)rd(SPI1 + SSPDR);
	}
	udelay(5);
	cs(1);
	return 0;
fail:
	cs(1);
	return -1;
}

/* Novatek SPI: 0xFF + addr[23:15] + addr[14:7] selects the page */
static int nvt_page(uint32_t addr)
{
	uint8_t tx[3] = { 0xff, (uint8_t)(addr >> 15), (uint8_t)(addr >> 7) }, rx[3];
	return xfer(tx, rx, 3);
}

static int nvt_write(uint32_t addr, uint8_t val)
{
	uint8_t tx[2] = { (uint8_t)((addr & 0x7f) | 0x80), val }, rx[2];
	int e = nvt_page(addr);
	udelay(100);
	return e ? e : xfer(tx, rx, 2);
}

/* One frame: a command byte, then n bytes from src (rx discarded). */
static int xfer_out(uint8_t first, const uint8_t *src, unsigned n)
{
	cs(0);
	udelay(5);
	for (unsigned i = 0; i <= n; i++) {
		unsigned t = 100000;
		while (!(rd(SPI1 + SSPSR) & SR_TNF))
			if (!--t)
				goto fail;
		wr(SPI1 + SSPDR, i ? src[i - 1] : first);
		t = 100000;
		while (!(rd(SPI1 + SSPSR) & SR_RNE))
			if (!--t)
				goto fail;
		(void)rd(SPI1 + SSPDR);
	}
	udelay(5);
	cs(1);
	return 0;
fail:
	cs(1);
	return -1;
}

/* Write n bytes at addr (page already selected when same_page). */
static int nvt_wbuf(uint32_t addr, const uint8_t *src, unsigned n, int same_page)
{
	if (!same_page && nvt_page(addr))
		return -1;
	return xfer_out((uint8_t)((addr & 0x7f) | 0x80), src, n);
}

/* Read n (<= 128) bytes at addr: offset byte, then data from rx[2] (Linux RX_DATA_START). */
static int nvt_rd(uint32_t addr, uint8_t *out, unsigned n, int same_page)
{
	uint8_t tx[130] = { (uint8_t)(addr & 0x7f) }, rx[130];
	if (n > 128 || (!same_page && nvt_page(addr)))
		return -1;
	if (xfer(tx, rx, n + 2))
		return -1;
	for (unsigned i = 0; i < n; i++)
		out[i] = rx[i + 2];
	return 0;
}

/* PL022: 8-bit Motorola, mode = CPOL << 1 | CPHA, SCK = SSPCLK / (cpsr * (1 + scr)) */
static void spi_config(unsigned mode, unsigned cpsr, unsigned scr)
{
	wr(SPI1 + SSPCR1, 0);
	wr(SPI1 + SSPCPSR, cpsr);
	wr(SPI1 + SSPCR0, (scr << 8) | ((mode & 1) << 7) | ((mode >> 1) << 6) | 7);
	wr(SPI1 + SSPCR1, 1u << 1);	/* SSE */
	while (rd(SPI1 + SSPSR) & SR_RNE)
		(void)rd(SPI1 + SSPDR);
}

/* Trim ID: page 0x1F600, 6 bytes at 0x4E; data starts at rx[2] (Linux RX_DATA_START) */
static void read_id(const char *label)
{
	uint8_t tx[8] = { 0x4e }, rx[8] = { 0 };
	int e = nvt_page(0x1f600);
	udelay(100);
	if (!e)
		e = xfer(tx, rx, 8);
	con_hex2(label, "", (uint32_t)rx[1] << 24 | rx[2] << 16 | rx[3] << 8 | rx[4],
		 "", (uint32_t)rx[5] << 24 | rx[6] << 16 | rx[7] << 8 | (e ? 0xee : 0));
}

extern const uint8_t nvt_fw[], nvt_fw_end[];

/* clang emits memset for zero-initialised buffers; there is no libc here */
void *memset(void *d, int c, unsigned long n);
void *memset(void *d, int c, unsigned long n)
{
	volatile uint8_t *p = d;
	while (n--)
		*p++ = (uint8_t)c;
	return d;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* NT36672A memory map (Huawei nt36xxx_mem_map.h) */
#define EVENT_BUF		0x21c00
#define EV_HOST_CMD		0x50
#define EV_RESET_COMPLETE	0x60
#define BOOT_RDY		0x3f10d
#define BLD_CRC_EN		0x3f30e
#define SWRST_N8		0x3f0fe
#define FW_SIZE			118784

/* ILM/DLM destination, length and golden checksum for the bootloader's HW CRC */
static void bld_crc_bank(uint32_t des, uint32_t sram, uint32_t len_addr, uint32_t size,
			 uint32_t ck_addr, uint32_t crc)
{
	uint8_t a[3] = { (uint8_t)sram, (uint8_t)(sram >> 8), (uint8_t)(sram >> 16) };
	uint8_t l[2] = { (uint8_t)size, (uint8_t)(size >> 8) };
	uint8_t c[4] = { (uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24) };
	nvt_wbuf(des, a, 3, 0);
	nvt_wbuf(len_addr, l, 2, 1);	/* same page as des (0x3F1xx) */
	nvt_wbuf(ck_addr, c, 4, 1);
}

/* Huawei nvt_download_firmware_hw_crc(); returns the reset state byte (>= 0xA0 = running) */
static int fw_download(void)
{
	const uint8_t *fw = nvt_fw;
	uint32_t fwsize = (uint32_t)(nvt_fw_end - nvt_fw);

	if (fwsize != FW_SIZE || fw[0x1a000] + fw[0x1a001] != 0xff) {
		con_hex2("FW", "bad size=", fwsize, "ver=", fwsize ? fw[0x1a000] : 0);
		return -1;
	}
	/* partitions: ILM, DLM, then the info sections (16 bytes each from 0x30) */
	uint32_t end = le32(fw), n = 2;
	for (uint32_t pos = 0x30; pos < end; pos += 0x10)
		n++;
	nvt_write(SWRST_N8, 0x69);		/* bootloader reset */
	udelay(5000);
	uint32_t ilm_sram = 0, ilm_size = 0, ilm_crc = 0, dlm_sram = 0, dlm_size = 0, dlm_crc = 0;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t bin, sram, size;
		if (i < 2) {
			bin = le32(fw + i * 12);
			sram = le32(fw + 4 + i * 12);
			size = le32(fw + 8 + i * 12);
			if (i == 0) {
				ilm_sram = sram; ilm_size = size; ilm_crc = le32(fw + 0x18);
			} else {
				dlm_sram = sram; dlm_size = size; dlm_crc = le32(fw + 0x1c);
			}
		} else {
			const uint8_t *h = fw + 0x30 + 0x10 * (i - 2);
			sram = le32(h);
			size = le32(h + 4);
			bin = le32(h + 8);
		}
		if (!size)
			continue;
		size += 1;			/* Huawei writes size + 1 bytes */
		if (bin + size > fwsize)
			size = fwsize - bin;
		for (uint32_t done = 0; done < size; done += 0x10000) {
			uint32_t len = size - done < 0x10000 ? size - done : 0x10000;
			if (nvt_wbuf(sram + done, fw + bin + done, len, 0))
				return -2;
		}
	}
	bld_crc_bank(0x3f128, ilm_sram, 0x3f118, ilm_size, 0x3f100, ilm_crc);
	bld_crc_bank(0x3f12c, dlm_sram, 0x3f130, dlm_size, 0x3f104, dlm_crc);
	uint8_t v;
	nvt_rd(BLD_CRC_EN, &v, 1, 0);		/* enable HW BLD CRC */
	v |= 0x80;
	nvt_wbuf(BLD_CRC_EN, &v, 1, 1);
	v = 0x00;				/* clear fw reset status, enable fw crc */
	nvt_wbuf(EVENT_BUF | EV_RESET_COMPLETE, &v, 1, 0);
	v = 0xae;
	nvt_wbuf(EVENT_BUF | EV_HOST_CMD, &v, 1, 1);
	nvt_write(BOOT_RDY, 1);			/* boot ready */
	udelay(5000);
	uint8_t st[5] = { 0 };
	for (int t = 0; t < 50; t++) {		/* Huawei: RESET_STATE_INIT within ~100 ms */
		nvt_rd(EVENT_BUF | EV_RESET_COMPLETE, st, 5, 0);
		if (st[0] >= 0xa0 && st[0] < 0xaf)
			break;
		udelay(10000);
	}
	return st[0];
}

static void square(uint32_t x, uint32_t y, uint32_t color)
{
	for (uint32_t dy = 0; dy < 24; dy++)
		for (uint32_t dx = 0; dx < 24; dx++) {
			uint32_t px = x + dx - 12, py = y + dy - 12;
			if (px < (uint32_t)con_w && py < (uint32_t)con_h)
				con_fb[py * (con_stride / 4) + px] = color;
		}
}

/* Poll the event buffer forever; 6 bytes per finger: id<<3|state, x[11:4], y[11:4], x[3:0]<<4|y[3:0] */
static void __attribute__((noreturn)) touch_loop(void)
{
	int row = con_row();
	uint32_t reports = 0;
	for (;;) {
		uint8_t pd[60];
		if (nvt_rd(EVENT_BUF, pd, 60, 0) == 0) {
			for (int i = 0; i < 10; i++) {
				const uint8_t *p = pd + 6 * i;
				uint32_t st = p[0] & 7, id = p[0] >> 3;
				if ((st == 1 || st == 2) && id >= 1 && id <= 10) {
					uint32_t x = (uint32_t)p[1] << 4 | p[3] >> 4;
					uint32_t y = (uint32_t)p[2] << 4 | (p[3] & 0xf);
					if (x < 1080 && y < 2312) {
						square(x, y, 0xffffffff);
						reports++;
						con_set_row(row);
						con_hex2("TOUCH", "n=", reports, "xy=", x << 16 | y);
					}
				}
			}
		}
		udelay(8000);
	}
}

/*
 * SPI1 clocks and pins, CS GPIO, PL022 in mode 0 (SSPCLK / 160), touch reset
 * released, bootloader reset. verbose: report the registers on the way.
 * 0, or -1 if the PL022 does not answer.
 */
static int touch_setup(int verbose)
{
	uint32_t g0 = peek(PERI_CRG + PERSTAT2);

	wr(PERI_CRG + PEREN2, SPI1_GATE);
	wr(PERI_CRG + CLKGT_SPI, (1u << (13 + 16)) | (1u << 13));
	if (verbose) {
		con_hex2("CRG", "stat2 was=", g0, "now=", peek(PERI_CRG + PERSTAT2));
		con_hex2(" crg", "rst2=", peek(PERI_CRG + PERRSTSTAT2), "gt=", peek(PERI_CRG + CLKGT_SPI));
		con_hex2(" crg", "div=", peek(PERI_CRG + CLKDIV_SPI), "mux=", peek(PERI_CRG + CLKMUX_SPI));
		con_hex2("IOMG", "spi7=", peek(IOMG_SPI1), "irq210=", peek(IOMG_TP_IRQ));
		con_hex2(" iocg", "spi7=", peek(IOCG_SPI1), "spi8=", peek(IOCG_SPI1 + 4));
	}
	for (int i = 0; i < 3; i++)
		wr(IOMG_SPI1 + 4 * i, 1);
	if (verbose) {
		con_hex2("GPIO", "csdir=", peek(GPIO_CS + GPIODIR), "irq=", peek(GPIO_BIT(GPIO_IRQ, 2)));
		con_hex2(" gpio", "rstdir=", peek(GPIO_RST + GPIODIR), "rst=", peek(GPIO_BIT(GPIO_RST, 0)));
	}
	cs(1);
	wr(GPIO_CS + GPIODIR, rd(GPIO_CS + GPIODIR) | (1u << 2));
	if (verbose)
		con_hex2("PL022", "pid0=", peek(SPI1 + SSPPID0), "sr=", peek(SPI1 + SSPSR));
	if (peek(SPI1 + SSPPID0) == 0xdeadbeef) {
		con_str("PL022", "abort: still unclocked or in reset");
		return -1;
	}
	/* release the touch reset (nvt_kit_hw_reset on SPI: high, 5 ms) */
	wr(GPIO_BIT(GPIO_RST, 0), 0xff);
	wr(GPIO_RST + GPIODIR, rd(GPIO_RST + GPIODIR) | 1u);
	udelay(20000);
	spi_config(0, 16, 9);		/* mode 0 (milestone 4 sweep), SSPCLK / 160 */
	nvt_write(SWRST_N8, 0x69);	/* bootloader reset */
	udelay(15000);
	return 0;
}

/* Probe mode (no xnu payload): registers, chip ID, firmware, then draw touches. */
void test_touch(void)
{
	if (touch_setup(1))
		return;
	read_id("ID");			/* dd 0A 00 00 / 82 66 03 00 */
	if ((uintptr_t)nvt_fw_end == (uintptr_t)nvt_fw) {
		con_str("FW", "not embedded (research/touch/ missing at build)");
		return;
	}
	int st = fw_download();
	con_hex2("FW", "state=", (uint32_t)st, "want>=", 0xa0);
	if (st < 0xa0 || st >= 0xaf)
		return;
	con_str("TOUCH", "firmware running: touch the screen");
	con_skip();
	touch_loop();
}

/*
 * Before xnu (loader path): bring the touch controller up with its firmware;
 * xnu (0208) then only reads the event buffer. Failure leaves touch off.
 */
void touch_prepare(void)
{
	if ((uintptr_t)nvt_fw_end == (uintptr_t)nvt_fw) {
		con_str("TOUCH", "no firmware embedded: touch off");
		return;
	}
	if (touch_setup(0))
		return;
	int st = fw_download();
	con_hex2("TOUCH", "fw state=", (uint32_t)st, "want>=", 0xa0);
}
