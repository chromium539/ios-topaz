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

void test_touch(void)
{
	uint32_t g0 = peek(PERI_CRG + PERSTAT2);

	/* --- clocks --- */
	wr(PERI_CRG + PEREN2, SPI1_GATE);
	wr(PERI_CRG + CLKGT_SPI, (1u << (13 + 16)) | (1u << 13));
	con_hex2("CRG", "stat2 was=", g0, "now=", peek(PERI_CRG + PERSTAT2));
	con_hex2(" crg", "rst2=", peek(PERI_CRG + PERRSTSTAT2), "gt=", peek(PERI_CRG + CLKGT_SPI));
	con_hex2(" crg", "div=", peek(PERI_CRG + CLKDIV_SPI), "mux=", peek(PERI_CRG + CLKMUX_SPI));

	/* --- pins --- */
	con_hex2("IOMG", "spi7=", peek(IOMG_SPI1), "irq210=", peek(IOMG_TP_IRQ));
	con_hex2(" iocg", "spi7=", peek(IOCG_SPI1), "spi8=", peek(IOCG_SPI1 + 4));
	for (int i = 0; i < 3; i++)
		wr(IOMG_SPI1 + 4 * i, 1);
	con_hex2(" iomg", "spi now=", peek(IOMG_SPI1), "spi9=", peek(IOMG_SPI1 + 8));

	/* --- GPIOs: CS output high; IRQ and reset only reported --- */
	con_hex2("GPIO", "csdir=", peek(GPIO_CS + GPIODIR), "irq=", peek(GPIO_BIT(GPIO_IRQ, 2)));
	con_hex2(" gpio", "rstdir=", peek(GPIO_RST + GPIODIR), "rst=", peek(GPIO_BIT(GPIO_RST, 0)));
	cs(1);
	wr(GPIO_CS + GPIODIR, rd(GPIO_CS + GPIODIR) | (1u << 2));

	/* --- PL022 --- */
	con_hex2("PL022", "pid0=", peek(SPI1 + SSPPID0), "cr0=", peek(SPI1 + SSPCR0));
	con_hex2(" pl022", "cr1=", peek(SPI1 + SSPCR1), "sr=", peek(SPI1 + SSPSR));
	if (peek(SPI1 + SSPPID0) == 0xdeadbeef) {
		con_str("PL022", "abort: still unclocked or in reset");
		return;
	}
	spi_config(1, 4, 9);		/* mode 1, SSPCLK / 40 (~1 MHz if 41 MHz) */

	/* --- release the touch reset (nvt_kit_hw_reset on SPI: high, 5 ms) --- */
	wr(GPIO_BIT(GPIO_RST, 0), 0xff);
	wr(GPIO_RST + GPIODIR, rd(GPIO_RST + GPIODIR) | 1u);
	udelay(20000);
	con_hex2("RST", "now=", peek(GPIO_BIT(GPIO_RST, 0)), "irq=", peek(GPIO_BIT(GPIO_IRQ, 2)));

	nvt_write(0x03f0fe, 0x69);		/* bootloader reset */
	udelay(15000);
	/* rx[1..7]: dummy, then the 6 ID bytes; last byte ee = transfer error */
	read_id("M1 fast");
	static const char *const lbl[4] = { "M0 slow", "M1 slow", "M2 slow", "M3 slow" };
	for (unsigned m = 0; m < 4; m++) {
		spi_config(m, 16, 9);		/* SSPCLK / 160 */
		read_id(lbl[m]);
	}
	con_str(" want", "dd 0A FF FF  82 66 03 00 (dd = dummy)");
}
