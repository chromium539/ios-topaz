/*
 * marie (Huawei P30 Lite, Kirin 710) milestone 0: one boot answers the
 * questions that decide the XNU port.
 *   - where Huawei fastboot's DSS scans out (read channel DMA registers)
 *   - CurrentEL, MIDR, ID_AA64MMFR0 (16K granule) on the A53 boot core
 *   - PSCI CPU_ON to A73 CPU 0x100 and the same registers there
 *   - GIC-400 (GICv2): Group 1 IRQ from the virtual timer (PPI 27) and SGI 1
 *   - nothing resets the AP for 5 minutes with the SP805 disabled
 * With an xnu payload appended (build.sh $1) the A73 boots it instead of
 * running the probes (loader.c). Any unexpected exception paints a blue
 * panic below the last good line.
 */
#include <stdint.h>
#include "console.h"
#include "loader.h"
#include "emmc.h"
#include "touch.h"

#define SP805_BASE		0xe8a06000UL	/* kirin710_cs_config.dtsi watchdog0 */
#define SP805_CONTROL		0x008
#define SP805_LOCK		0xc00
#define SP805_UNLOCK_KEY	0x1acce551U

#define DSS_BASE		0xe8600000UL	/* kirin710_framebuffer.dtsi */
/* drivers/hisi/ap/platform/kirin710/soc_dss_interface.h */
#define DMA_CTRL		0x1c
#define DMA_DATA_ADDR0		0x60
#define DMA_STRIDE0		0x64	/* in DMA_ALIGN_BYTES (16) units, bits 15:0 */
#define DMA_ALIGN_BYTES		16
#define DSS_OVL0_OFFSET		0x60000
#define OVL_SIZE		0x00	/* (h - 1) << 16 | (w - 1) */

#define GICD_BASE		0xe82b1000UL	/* live DT interrupt-controller@e82b0000 */
#define GICC_BASE		0xe82b2000UL
#define GICD_CTLR		0x000
#define GICD_TYPER		0x004
#define GICD_IIDR		0x008
#define GICD_IGROUPR0		0x080
#define GICD_ISENABLER0		0x100
#define GICD_ICENABLER0		0x180
#define GICD_ISPENDR0		0x200
#define GICD_ISACTIVER0		0x300
#define GICD_IPRIORITYR		0x400
#define GICD_ITARGETSR		0x800
#define GICD_SGIR		0xf00
#define GICC_CTLR		0x000
#define GICC_PMR		0x004
#define GICC_BPR		0x008
#define GICC_IAR		0x00c
#define GICC_EOIR		0x010
#define GICC_HPPIR		0x018
#define GICC_IIDR		0x0fc
#define VTIMER_INTID		27	/* DT timer: <1 11 ..> */
#define PTIMER_INTID		30	/* DT timer: <1 14 ..> (non-secure physical) */
#define SGI_INTID		1

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define SMCCC_VERSION		0x80000000UL

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint8_t rd8(uintptr_t a) { return *(volatile uint8_t *)a; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];
extern char xnu_psci_tramp[], xnu_psci_mailbox[];

/*
 * Reads of powered-down/clock-gated blocks (e.g. idle DSS read channels)
 * end in a synchronous external abort. probe32() turns that into ok = 0:
 * exc_handler() skips the faulting load while probe_on is set.
 */
volatile int probe_on, probe_fault;

uint32_t probe32(uintptr_t a, int *ok)
{
	uint32_t v = 0xdeadbeef;

	probe_fault = 0;
	probe_on = 1;
	__asm__ volatile("dsb sy; ldr %w0, [%1]; dsb sy" : "+r"(v) : "r"(a) : "memory");
	probe_on = 0;
	*ok = !probe_fault;
	return *ok ? v : 0xdeadbeef;
}

static const char *stage;	/* set at runtime: no absolute pointers in data */

static volatile uint32_t irq_count, irq_other, irq_last;
static volatile int irq_src;
static volatile int gold_up, gold_go;
static volatile uint32_t a53_mhz;
static volatile uint64_t sib_mpidr[8];	/* CPU_ON test: woken A73 siblings */
static volatile uint64_t gold_dtb;

/* build.sh appends the payload at the first 4K boundary after _end */
extern char _end[];

static int payload_present(void)
{
	static const char want[] = "SWEETXNU";
	const volatile char *m = (const volatile char *)(((uintptr_t)_end + 0xfff) & ~0xfffUL);

	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

static void __attribute__((noreturn)) halt(void)
{
	con_skip();
	con_str("HALT", "hold VOL- + POWER (USB in) for fastboot");
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- exceptions ---- */

static void irq_handler(void)
{
	uint32_t iar = rd32(GICC_BASE + GICC_IAR);
	uint32_t id = iar & 0x3ff;

	if (id >= 1020)
		return;	/* spurious */
	irq_last = id;
	if (id == (uint32_t)irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* 10 ms */
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
	wr32(GICC_BASE + GICC_EOIR, iar);
	isb();
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (slot == 5) {	/* IRQ from current EL, SPx */
		irq_handler();
		return;
	}
	if (slot == 4 && probe_on && (rsr("esr_el1") >> 26) == 0x25) {	/* data abort */
		probe_fault = 1;
		wsr("elr_el1", rsr("elr_el1") + 4);
		return;
	}
	/* keep what is already on screen: the panic goes below it */
	con_skip();
	con_bg_set(BLUE);
	switch (slot & 3) {
	case 0: con_str("PANIC", "SYNC"); break;
	case 1: con_str("PANIC", "IRQ"); break;
	case 2: con_str("PANIC", "FIQ"); break;
	default: con_str("PANIC", "SERROR"); break;
	}
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */
	halt();
}

/* ---- CPU clock estimate ---- */

/*
 * 20M dependent adds with the I-cache on (fine with the MMU off), timed
 * against the system counter: about one add per cycle on A53 and A73, so
 * MHz ~= iterations / microseconds. Rough, but enough to see whether the
 * A73s run at fastboot's boot clock or near their 2.2 GHz maximum.
 */
static uint32_t cpu_mhz(void)
{
	uint64_t sctlr = rsr("sctlr_el1");
	wsr("sctlr_el1", sctlr | (1UL << 12));
	isb();
	uint64_t t = now(), x = 0;
	for (uint64_t i = 0; i < 20000000; i++)
		__asm__ volatile("add %0, %0, #1" : "+r"(x));
	t = now() - t;
	wsr("sctlr_el1", sctlr);
	isb();
	uint64_t us = t * 1000000 / rsr("cntfrq_el0");
	return us ? (uint32_t)(20000000 / us) : 0;
}

/* Decimal digits packed as hex nibbles, so con_hex prints n in decimal. */
static uint32_t bcd(uint32_t n)
{
	uint32_t r = 0;
	for (int sh = 0; n && sh < 32; sh += 4, n /= 10)
		r |= (n % 10) << sh;
	return r;
}

/*
 * Big-cluster DVFS. Fastboot leaves the A73s far below their lowest OPP;
 * Linux (drivers/hisi/hw_vote, kirin710_hw_vote.dtsi big-freq vote-src-1)
 * asks LPM3 for a frequency by writing MHz/16 into PMCTRL 0x270[7:0]
 * (bits 16-23 are the write enables); LPM3 reports the result in
 * 0x2dc[7:0]. The vote stays in place for xnu.
 */
#define PMCTRL		0xfff31000UL
#define HV_BIG_VOTE	0x270
#define HV_RESULT	0x2dc
#define BIG_MHZ		1824		/* OPP 5 of 7, 1.0 V; max is 2189 */

static void big_freq_vote(void)
{
	int ok1, ok2;
	uint32_t v = probe32(PMCTRL + HV_BIG_VOTE, &ok1);
	uint32_t r = probe32(PMCTRL + HV_RESULT, &ok2);
	con_hex2("HVOTE", "big=", v, "res=", r);
	if (!ok1 || !ok2)
		return;
	wr32(PMCTRL + HV_BIG_VOTE, 0x00ff0000 | (BIG_MHZ / 16));
	uint64_t t0 = now();
	while (((rd32(PMCTRL + HV_RESULT) & 0xff) != BIG_MHZ / 16) &&
	       now() - t0 < rsr("cntfrq_el0") / 10)
		;
	con_hex2("HVOTE", "big=", rd32(PMCTRL + HV_BIG_VOTE), "res=", rd32(PMCTRL + HV_RESULT));
}

/* ---- watchdog ---- */

static void sp805_disable(void)
{
	wr32(SP805_BASE + SP805_LOCK, SP805_UNLOCK_KEY);
	wr32(SP805_BASE + SP805_CONTROL, 0);
	wr32(SP805_BASE + SP805_LOCK, 1);
	(void)rd32(SP805_BASE + SP805_LOCK);
}

/* ---- caches ---- */

/*
 * Fastboot ran on this A53 with the D-cache on; we run with it off and the
 * A73 writes the kernel straight to DRAM. Clean+invalidate every data cache
 * level by set/way so no dirty line from fastboot can later be written back
 * over xnu, or be served to the A73 by a coherent snoop once xnu turns its
 * caches on. (The Linux boot protocol only cleans the kernel image itself.)
 */
static void dcache_cisw_all(void)
{
	uint64_t clidr = rsr("clidr_el1");
	unsigned int loc = (clidr >> 24) & 7;

	for (unsigned int level = 0; level < loc; level++) {
		if (((clidr >> (level * 3)) & 7) < 2)
			continue;	/* no data or unified cache at this level */
		wsr("csselr_el1", level << 1);
		isb();
		uint64_t ccsidr = rsr("ccsidr_el1");
		unsigned int line_shift = (ccsidr & 7) + 4;
		uint32_t ways = (ccsidr >> 3) & 0x3ff;
		uint32_t sets = (ccsidr >> 13) & 0x7fff;
		unsigned int way_shift = ways ? __builtin_clz(ways) : 0;

		for (uint32_t set = 0; set <= sets; set++)
			for (uint32_t way = 0; way <= ways; way++) {
				uint64_t v = (uint64_t)way << way_shift | (uint64_t)set << line_shift | level << 1;
				__asm__ volatile("dc cisw, %0" : : "r"(v) : "memory");
			}
	}
	__asm__ volatile("dsb sy; isb" ::: "memory");
}

/* ---- display: find what the DSS read channels scan out ---- */

static const uint32_t rch_off[] = {
	0x20000, 0x28000, 0x30000,	/* VG0 VG1 VG2 */
	0x38000, 0x40000,		/* G0 G1 */
	0x52000, 0x53000, 0x50000, 0x51000,	/* D0 D1 D2 D3 */
};
#define NR_RCH (sizeof(rch_off) / sizeof(rch_off[0]))

static uint32_t rch_addr[NR_RCH], rch_stride[NR_RCH], rch_ctrl[NR_RCH];
static int rch_ok[NR_RCH];
static int fb_rch = -1;
static uint32_t ovl_size;
static int ovl_ok;

static int ram_addr(uint32_t a)
{
	return a >= 0x00400000U && a < 0xe0000000U;
}

static void fb_find(void)
{
	ovl_size = probe32(DSS_BASE + DSS_OVL0_OFFSET + OVL_SIZE, &ovl_ok);
	for (unsigned i = 0; i < NR_RCH; i++) {
		uintptr_t b = DSS_BASE + rch_off[i];
		int ok1, ok2, ok3;
		rch_addr[i] = probe32(b + DMA_DATA_ADDR0, &ok1);
		rch_stride[i] = probe32(b + DMA_STRIDE0, &ok2);
		rch_ctrl[i] = probe32(b + DMA_CTRL, &ok3);
		rch_ok[i] = ok1 && ok2 && ok3;
		/* first channel with a physical (SMMU bypass, CTRL bit 8 clear) RAM address */
		if (fb_rch < 0 && rch_ok[i] && ram_addr(rch_addr[i]) && !(rch_ctrl[i] & (1U << 8)) &&
		    (rch_stride[i] & 0xffff))
			fb_rch = i;
	}
	/*
	 * Only report: drawing at FB_BASE is confirmed visible (milestone 0
	 * run 1). The channel values show what xnu's scanout must match.
	 */
}

static void fb_report(void)
{
	con_hex2("FB", "drawing at=", (uint32_t)(uintptr_t)con_fb, "stride=", con_stride);
	if (fb_rch >= 0)
		con_hex2("FB", "rch=", fb_rch, "line bytes=", (rch_stride[fb_rch] & 0xffff) * DMA_ALIGN_BYTES);
	else
		con_str("FB", "no read channel with a physical RAM address");
	con_hex2("OVL0", "size=", ovl_size, "ok=", ovl_ok);
	/* per-channel dump: milestone 0 photos (only D2 = RCH7 is live) */
}

/* ---- GIC-400 (GICv2) + timer ---- */

/* Run one interrupt source for ~300 ms and report what the GIC saw. */
static void irq_window(const char *label, int src)
{
	uint64_t f = rsr("cntfrq_el0");
	uint32_t pend, act, hppi, ist;

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", f / 100);
		wsr("cntv_ctl_el0", 1);
	} else if (src == PTIMER_INTID) {
		wsr("cntp_tval_el0", f / 100);
		wsr("cntp_ctl_el0", 1);
	}
	isb();
	/* sample state with IRQs still masked, after the first period */
	uint64_t t = now() + f / 50;
	while (now() < t)
		;
	if (src == SGI_INTID)
		wr32(GICD_BASE + GICD_SGIR, (2U << 24) | SGI_INTID);	/* to self */
	__asm__ volatile("dsb sy" ::: "memory");
	ist = src == PTIMER_INTID ? rsr("cntp_ctl_el0") : rsr("cntv_ctl_el0");
	pend = rd32(GICD_BASE + GICD_ISPENDR0);
	act = rd32(GICD_BASE + GICD_ISACTIVER0);
	hppi = rd32(GICC_BASE + GICC_HPPIR);

	__asm__ volatile("msr daifclr, #2" ::: "memory");
	t = now() + f * 3 / 10;
	/* SGI: one already sent above, send 9 more 10 ms apart -> expect 10 */
	uint64_t next = now() + f / 100;
	for (int sent = 1; now() < t;)
		if (src == SGI_INTID && sent < 10 && now() >= next) {
			wr32(GICD_BASE + GICD_SGIR, (2U << 24) | SGI_INTID);
			sent++;
			next += f / 100;
		}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	wsr("cntp_ctl_el0", 0);
	isb();

	con_hex2(label, "irqs=", irq_count, "other=", irq_other);
	con_hex2(" pre", "pend=", pend, "act=", act);
	con_hex2(" pre", "hppi=", hppi, "tctl=", ist);
	con_hex2(" pre", "last=", irq_last, 0, 0);
}

static void __attribute__((unused)) test_gic(void)
{
	stage = "gic regs";
	con_hex2("GICD", "ctlr=", rd32(GICD_BASE + GICD_CTLR), "typer=", rd32(GICD_BASE + GICD_TYPER));
	con_hex2("GICD", "iidr=", rd32(GICD_BASE + GICD_IIDR), "grp0=", rd32(GICD_BASE + GICD_IGROUPR0));
	/* ITARGETSR0..7 are banked and read back this CPU's interface bit */
	con_hex2("GICD", "itgt0=", rd32(GICD_BASE + GICD_ITARGETSR), "en0=", rd32(GICD_BASE + GICD_ISENABLER0));
	con_hex2("GICC", "ctlr=", rd32(GICC_BASE + GICC_CTLR), "pmr=", rd32(GICC_BASE + GICC_PMR));
	con_hex2("GICC", "iidr=", rd32(GICC_BASE + GICC_IIDR), 0, 0);

	stage = "gic config";
	/* Non-secure view: GICD_CTLR bit0 / GICC_CTLR bit0 = EnableGrp1 */
	wr32(GICD_BASE + GICD_CTLR, rd32(GICD_BASE + GICD_CTLR) | 1);
	for (int id = 0; id < 32; id++)
		wr8(GICD_BASE + GICD_IPRIORITYR + id, 0xa0);
	wr32(GICD_BASE + GICD_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));
	wr32(GICC_BASE + GICC_PMR, 0xf0);
	wr32(GICC_BASE + GICC_BPR, 0);
	wr32(GICC_BASE + GICC_CTLR, rd32(GICC_BASE + GICC_CTLR) | 1);
	__asm__ volatile("dsb sy" ::: "memory");
	con_hex2("GIC on", "dctlr=", rd32(GICD_BASE + GICD_CTLR), "cctlr=", rd32(GICC_BASE + GICC_CTLR));
	con_hex2("GIC on", "en0=", rd32(GICD_BASE + GICD_ISENABLER0), "pri27=", rd8(GICD_BASE + GICD_IPRIORITYR + 27));

	stage = "gic vtimer";
	irq_window("VTIMER", VTIMER_INTID);
	stage = "gic sgi";
	irq_window("SGI1", SGI_INTID);
	stage = "gic ptimer";	/* last: CNTP_* may trap if EL3 kept it */
	irq_window("PTIMER", PTIMER_INTID);
	wr32(GICD_BASE + GICD_ICENABLER0, 0xffffffff);
	__asm__ volatile("dsb sy" ::: "memory");
}

/* ---- CPU ---- */

static void cpu_report(const char *label)
{
	uint64_t mmfr0 = rsr("id_aa64mmfr0_el1");

	con_hex2(label, "midr=", (uint32_t)rsr("midr_el1"), "mpidr=", (uint32_t)rsr("mpidr_el1"));
	con_hex2(" el", "cur=", (uint32_t)(rsr("currentel") >> 2), "sctlr=", (uint32_t)rsr("sctlr_el1"));
	con_hex2(" mmfr0", "lo=", (uint32_t)mmfr0, "tg16=", (mmfr0 >> 20) & 0xf);
	con_hex2(" isar0", "lo=", (uint32_t)rsr("id_aa64isar0_el1"), "pfr0=", (uint32_t)rsr("id_aa64pfr0_el1"));
}

/*
 * The boot CPU is A53 CPU 0 (no 16K granule if Cortex-A53 behaves as on
 * topaz). The A73 at MPIDR 0x100 is where xnu will have to run.
 */
static void __attribute__((unused)) test_sibling_cpu_on(void);

static void __attribute__((noreturn)) gold_main(void)
{
	gold_up = 1;
	__asm__ volatile("dsb sy" ::: "memory");
	while (!gold_go)	/* the boot CPU is still drawing */
		;
	con_skip();
	cpu_report("A73");
	con_hex2(" mhz", "a73~", bcd(cpu_mhz()), "a53~", bcd(a53_mhz));
	big_freq_vote();
	con_hex2(" mhz", "a73~", bcd(cpu_mhz()), "a53~", bcd(a53_mhz));
	/* hold the screen so the clock lines can be photographed */
	for (uint64_t t0 = now(); now() - t0 < 5 * rsr("cntfrq_el0");)
		;
	if (payload_present()) {
		con_skip();
		/* loader.c writes these into the ADT for xnu's secondary CPUs */
		con_hex2("TRAMP", "pa=", (uintptr_t)xnu_psci_tramp, "mbox=", (uintptr_t)xnu_psci_mailbox);
		stage = "touch";
		touch_prepare();
		stage = "loader";
		con_str("LOADER", loader_boot(gold_dtb));
		halt();
	}
	/* Milestone 2: sibling CPU_ON and eMMC probes instead of the GIC/watchdog checks */
	con_skip();
	stage = "touch";
	test_touch();
	halt();

	/* Watchdog check: nothing pets anything from here on. */
	stage = "idle";
	con_skip();
	int r = con_row();
	uint64_t f = rsr("cntfrq_el0"), t0 = now();
	for (uint32_t s = 0; s <= 300; s++) {
		while (now() < t0 + f * s)
			;
		con_set_row(r);
		con_hex2("ALIVE", "sec=", s, "of=", 300);
	}
	con_str("ALIVE", "5 min without reset: no hidden watchdog");
	halt();
}

void secondary_main(uint64_t idx)
{
	if (idx >= 5 && idx < 8) {	/* A73 sibling from the CPU_ON test: report and power off */
		sib_mpidr[idx] = rsr("mpidr_el1") | (1ULL << 63);
		__asm__ volatile("dsb sy" ::: "memory");
		smc(PSCI_CPU_OFF, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfi");
	}
	gold_main();
}

/*
 * xnu's CPU_ON returned -9 (INVALID_ADDRESS) for its entry (0x414b8000) and
 * the trampoline at 0x80400, while 0x80848 (secondary_entry) works. Map what
 * Huawei's PSCI accepts: a 16-byte stub (ldr x1, =secondary_entry; br x1) at
 * each address, CPU_ON 0x101 there; the sibling records itself and powers
 * off again (secondary_main), so the next address can be tried.
 */
static const uint64_t onaddr[] = {
	0x80400, 0x80848, 0x98000, 0x100000, 0x200000, 0x1000000, 0x4000000,
	0xff00000, 0x2e300000, 0x40000000, 0x80000000, 0xc0000000, 0x200000000,
};

static void __attribute__((unused)) test_sibling_cpu_on(void)
{
	/* mailbox stays 0: secondary_entry is xnu's trampoline once it is set */
	for (unsigned i = 0; i < sizeof(onaddr) / sizeof(onaddr[0]); i++) {
		uint64_t a = onaddr[i];
		if (a != 0x80400 && a != 0x80848) {
			volatile uint32_t *w = (volatile uint32_t *)a;
			w[0] = 0x58000041;	/* ldr x1, #8 */
			w[1] = 0xd61f0020;	/* br x1 */
			w[2] = (uint32_t)(uintptr_t)secondary_entry;
			w[3] = 0;
		}
		sib_mpidr[5] = 0;
		__asm__ volatile("dsb sy; isb" ::: "memory");
		int64_t r = smc(PSCI_CPU_ON, 0x101, a, 5);
		uint64_t end = now() + rsr("cntfrq_el0") / 5;	/* 200 ms */
		while (r == 0 && !sib_mpidr[5] && now() < end)
			;
		/* wait for it to be OFF again (AFFINITY_INFO 1) */
		end = now() + rsr("cntfrq_el0") / 5;
		while (r == 0 && smc(0xc4000004UL, 0x101, 0, 0) != 1 && now() < end)
			;
		char label[] = "ON 00";
		label[3] = "0123456789abcdef"[i >> 4];
		label[4] = "0123456789abcdef"[i & 15];
		con_hex2(label, "a=", (uint32_t)(a >> 4), "ret/up=", (uint32_t)r ^ (sib_mpidr[5] ? 0x10000000u : 0));
	}
	con_str("ON", "a= is addr>>4; ret/up=1xxxxxxx: came up");
}

void shim_main(uint64_t dtb)
{
	sp805_disable();

	stage = "fb";
	fb_find();
	con_clear(RED);
	con_str("SHIM", payload_present() ? "marie milestone 1: xnu loader" : "marie milestone 0: probes");
	fb_report();
	con_skip();
	con_hex2("DTB", "at=", dtb, "magic=", dtb ? __builtin_bswap32(rd32(dtb)) : 0);
	con_hex2("CNTFRQ", "hz=", (uint32_t)rsr("cntfrq_el0"), 0, 0);
	stage = "psci";
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), "smccc=", smc(SMCCC_VERSION, 0, 0, 0));
	cpu_report("A53");
	a53_mhz = cpu_mhz();

	stage = "dcache";
	dcache_cisw_all();
	con_hex2("DCACHE", "clidr=", (uint32_t)rsr("clidr_el1"), "cleaned=", 1);

	stage = "cpu_on";
	gold_dtb = dtb;
	__asm__ volatile("dsb sy" ::: "memory");
	int64_t ret = smc(PSCI_CPU_ON, 0x100, (uintptr_t)secondary_entry, 4);
	con_hex2("CPU_ON", "0x100 ret=", (uint32_t)ret, 0, 0);
	if (ret == 0) {
		delay_ms(2000);
		if (!gold_up)
			con_str("CPU_ON", "A73 never reached secondary_main");
		__asm__ volatile("dsb sy" ::: "memory");
		gold_go = 1;
		for (;;)	/* parked with DAIF masked (start.S) */
			__asm__ volatile("wfi");
	}
	/* No A73: run the GIC and watchdog checks here instead. */
	gold_go = 1;
	gold_main();
}
