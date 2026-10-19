# iOS 16 on the Huawei P30 Lite (marie) with 64-bit XNU

## Context
The 32-bit Galaxy J1 port (kOtusin/uniloader + xnu-j1xlte) is ruled out. The P30 Lite's bootloader starts the kernel at EL1 (`CPU: All CPU(s) started at EL1`), so a 32-bit kernel can't run. The 64-bit route has a working precedent with nearly the same CPU:

- **[dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet)**: Apple's open-source **xnu-8796**, booted on a Redmi Note 10 Pro (SM7150), running the **unmodified iPhone X iOS 16.7.x userland** up to SpringBoard.
- **[MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz)**: a port of sweet to the Redmi Note 12 4G (SM6225: 4×A53-class + 4×A73-class, **ARMv8.0**). It reaches the iOS lock screen. It has already solved the ARMv8.0 problems we would hit:
  - building with `-mcpu=cortex-a53`
  - no PAN
  - emulating userland LSE atomics and `ldapr` (patch 0081)
  - booting XNU on the A73 cores because the A53 cores have no 16K translation granule (shim PSCI `CPU_ON` + patch 0082)

The marie port therefore **forks ios-topaz** and replaces the Qualcomm-specific parts with Kirin 710 ones. The two SoCs differ mainly in the interrupt controller (GIC-400/GICv2 instead of GICv3), the storage (eMMC/SDHCI instead of UFS), the display, and the touch/SPI hardware.

## Marie hardware facts (read from the running LineageOS 20 over adb on 2026-10-19)
| Item | Value | Notes vs topaz |
| --- | --- | --- |
| CPUs | cpu@0–3 Cortex-A53 (`0xd03`, MIDR `410fd034`), cpu@100–103 Cortex-A73 (`0xd09`) | Same split as topaz. **Boots on A53 CPU 0** → hand off to `0x100` |
| PSCI | `cpu_on`/`cpu_off`/`system_reset`/`system_off` present | Same mechanism topaz uses |
| GIC | GIC-400, **GICD `0xE82B1000`, GICC `0xE82B2000`** | **GICv2, memory-mapped CPU interface**; topaz uses GICv3 system registers |
| Timer | `arm,armv8-timer`, PPIs secure 13 / NS-phys 14 / virt 11 / hyp 10 (INTID +16 → **vtimer 27**), **CNTFRQ 1.92 MHz** (`0x1d4c00`) | topaz: vtimer INTID 19 at 19.2 MHz |
| RAM | 4 GB model. Usable: `0x2E200000–0xDFFFFFFF` (~2.8 GiB contiguous), `0x2_00000000–0x2_1FFFFFFF` | Carve-outs (bl31, secos, modem, hifi, bbox) all lie below `0x2E200000` |
| Storage | eMMC `mmcblk0` (model `DA4128`), host **`sdhci@FF3E1000`** (`mshc,sdhci`, Linux `drivers/mmc/host/sdhci_mshc_libra.c`), `userdata` = `mmcblk0p70`, `kernel` = `mmcblk0p43` | topaz: UFS |
| SD slot | `dwmmc1@FF37F000` (DesignWare) | optional later |
| Display | 1080×2312, 32 bpp, stride 4320, panel `MARI_130 203` 6.15"; fastboot logo buffer `0x18C00000` (+`0x3000000`) | Scanout address still to be confirmed (Phase 0) |
| Touch | Huawei `ts_kit` → **Novatek** over **SPI (PL022)**, 10 MHz, mode 1, CS GPIO 10, 1080×2312 | topaz: FocalTech over I²C |
| Watchdog | SP805 at `0xE8A06000`, 24 s (same as uniLoader's `board-agassi2.c`) | the shim must disable it |
| Boot image | Android boot v1, kernel at `0x80000`, page `0x800`, flashed to `kernel` partition (see `../marie/clean/kernel_boot.img`) | topaz: boot v4 via `fastboot boot` |

## Prerequisites
- A GitHub account for the macOS runner build (`.github/workflows/xnu.yml`: macos-14, Xcode 15, blacktop/darwin-xnu-build, xnu-8796). No Mac is needed.
- The iPhone X (iPhone10,3) iOS 16.7.x IPSW from Apple. `apfs-fuse`, `hfsprogs`, `img2simg`, `aarch64-linux-gnu-gcc`, AOSP `mkbootimg`.
- **Accept that `userdata` gets wiped** when the iOS root is flashed there. Back up `kernel`, `userdata` and the GPT first. `../marie/clean/` already holds the stock `kernel_boot.img`, `dts.img`, `dto.img` and `vbmeta.img`.
- Recovery path: Vol-Down + USB → fastboot → `fastboot flash kernel clean/kernel_boot.img`. This is already proven by the ~30 marie test images.

## Phase 0 results (2026-10-19, `shim/` on branch `marie`, photos IMG_1907/1908)
All gates passed:
- **`fastboot boot` is refused** (`Command not allowed`), so each test means flashing the `kernel` partition. The boot v1 header must match stock (`shim/build.sh`). Fastboot jumps to the gzip'd Image at `0x80000`, EL1, DTB in x0 = `0x7A00000`.
- **Display:**
  - fastboot leaves DSS read channel **D2** (`DSS_BASE+0x50000`) scanning out **`0x18C00000`**, stride 4320 B, OVL0 1080×2312, rdma ctrl `0x28` (no SMMU); CPU writes show up directly
  - idle read channels (VG0, VG2, G0) **abort on read**, so register probes must survive external aborts (`probe32()`)
- **CPUs:**
  - A53 CPU0: MIDR `410FD034`, TGran16 = 0
  - A73 at 0x100: MIDR `410FD092`, **TGran16 = 1**, reached with PSCI `CPU_ON` (PSCI 1.0, SMCCC 1.1)
  - neither core has LSE (`isar0 = 0x11120`)
- **GIC-400:** GICD_TYPER `0xFCEB` (384 IRQs, 8 CPU interfaces, SecurityExtn). The CPU interface on A73 0x100 is **#4**. GICD/GICC are already enabled for Group 1 with PMR `0xF0`. vtimer PPI 27, ptimer PPI 30 and SGI 1 are delivered as NS Group 1 IRQs (30/30/10 in 300 ms).
- **Watchdog:** after disabling the SP805, 300 s idle with no reset.

## Touch hardware (live DT via ../marie busybox USB serial, 2026-10-19; tools/marie-serial.py --props)
- **Controller:** Novatek NT36682A TDDI (`/ts_kit/novatek@62`, ic_type 2, x/y max 1080x2312); vci/vddio type 0, powered with the panel.
- **Bus:** `/ts_kit`: `bus_type spi`, `bus_id 1` = **PL022 `spi@fdf08000`** (4 CS, DMA, source 41 MHz), 10 MHz, `spi-mode 1`.
  - pl022 com-mode 1, ctrl-len 7, rx/tx-level-trig 3
  - cs_reset_low_delay 10, high_delay 200
- **GPIOs** (all PL061):
  - **CS** = `gpio@e8a0c000` pin 2 (GPIO-driven chip select)
  - **IRQ** = `gpio@fff0f000` pin 2 (GIC SPI 110 for the bank; pin = GPIO 210)
  - **reset** = `gpio@e8a10000` pin 0
- **Pin mux** (pinctrl-single, 32-bit, function mask 7):
  - SPI1 = GPIO 7/8/9 function 1: IOMG `0xE896C708/70C/710`, IOCG `pinmux@e896c800` +0x708.. = 0
  - touch IRQ GPIO 210 function 0: IOMG `0xFFF11708`, IOCG `pinmux@fff11800` +0x708 = 0
- **SPI1 clocks** (PERI_CRG `0xFFF35000`):
  - clk_spi1/pclk_spi1 gate = PEREN2 (0x20, dis 0x24, stat 0x28) bit 9
  - clkgt_spi = hiword 0xF4 bit 13 (always on)
  - clkdiv_spi = hiword 0xC4 mask 0xF000
  - clkmux_spi = hiword 0xAC bit 8
- **Protocol:** Linux driver `kernel-marie/drivers/devkit/tpkit/panel/novatek/nt36xxx*.c`.
- **Note:** the ../marie busybox image has only ash/ls/cat/echo/dmesg… (no grep/dd/base64), so `tools/marie-log-pull.py` needs a richer image.

## Phase 3 milestone: iOS home screen on the phone (2026-10-19)
- **Kernel:** xnu 0207 (CI 39305111981, `xnu/out-0207`), image `shim/out/marie-0207-gui.img`. Built with `CPUS=4 MARIE_DISPLAY=1 tools/marie-image.sh ... --args "rd=ufsroot0 ufsrw=1 rootrw=1 unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1 sweetfb_hz=30 sweetfb_noconsole=1"`.
- **Display:** AppleCLCD copy path (xnu 0206) into the fastboot logo buffer; iPhone X canvas 1125x2436 cropped to the 1080x2312 panel.
- **First GUI boot:** Apple logo for well over 10 minutes (console on, then off) before SpringBoard appeared. The first boot builds the LaunchServices database. Likely slow because of polled eMMC PIO and the A73s at fastboot's clock (no cpufreq).
- **Debugging without a screen:** `tools/marie-log-pull.py` reads the persistent console log (last 8193 sectors of userdata) over the ../marie busybox USB-serial image (not needed this time).
- **Not working yet:** touch (Novatek NT36682A TDDI over SPI), buttons, brightness, CPU frequency, eMMC DMA, Wi-Fi and the rest.

## Phase 2/3 milestone: iOS 16.7.16 userland on the phone (2026-10-19, photo IMG_1924)
- **Root:** `research/ios16-root.img` (`tools/marie-mkroot.sh`: 16 GB journaled HFS+, topaz root tweaks), flashed with `fastboot flash userdata research/ios16-root.img.simg` (27 sparse chunks, 7 min; Huawei fastboot allows it and erases the Android crypt info in oeminfo).
- **Kernel:** xnu 0205 (CI 39300558240), `CPUS=4 tools/marie-image.sh xnu/out-0205/kernel.development.sweet 0205-ios-rw --args "rd=ufsroot0 ufsrw=1 rootrw=1 unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1"`.
- **Read-only first:** a read-only mount fails because the fresh journal needs one write to initialise (`jnl: write_journal_header` EROFS), so the first boot has to be read-write.
- **Result:** launchd, xpcproxy, bluetoothd, medialibraryd, fairplayd and backboardd run. SpringBoard crash-loops with `failed to find the main CADisplay` (no display node), so the display (xnu 0206 + `MARIE_DISPLAY=1`) is next.
- **Topaz touch/keys:** `sweet_ufs.c` starts topaz's touch/keys drivers with the root device. Their Qualcomm addresses are low DRAM on the Kirin, so `marie-image.sh` passes `sweettouch=0 sweetkeys=0`.

## SMP and eMMC findings (2026-10-19, photos IMG_1912-1920)
- **4 A73 cores up** (`cpus=4`, photo IMG_1920).
  - Huawei PSCI returns -9 INVALID_ADDRESS for every CPU_ON entry point except the one used by the **first CPU_ON after boot** (the shim's A53 -> 0x100 call at `secondary_entry`, 0x80848). Tested: 0x80400, 0x80878, 0x98000 and RAM up to 0x200000000 all rejected.
  - Fix: `secondary_entry` is xnu's trampoline. xnu writes its entry into the mailbox (ADT CPU property `sweet,psci-trampoline`, patched in by `shim/loader.c`) and calls CPU_ON at 0x80848 (xnu 0204).
- **eMMC** (shim probe `shim/emmc.c`, photo IMG_1915):
  - SDHCI registers at **0xFF3E6000** (DT reg[0]; 0xFF3E1000 is the PHY), SDHCI 4.10
  - card RCA 1 in transfer state, HS400 (HS_TIMING 3), CMDQ off, PARTITION_CONFIG 0x08 (user area), 0x0E8F6000 sectors
  - GPT entry 70 `userdata` = LBA 0x0111C000-0x0E8F5FFF (~116 GB)
  - PIO CMD13/CMD8/CMD17 work from fastboot's state. xnu 0205 = SDHCI PIO backend for sweet_ufs.c (`rd=ufsroot0`).

## Phase 1 milestone: XNU to PID 1 on the phone (2026-10-19, photos IMG_1910/1911)
Kernel xnu 0201+0202+0203 (CI run 39294487743, `xnu/out-0203`), image `shim/out/marie-0203c.img`. Boot-args: `debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress sweetbattery=0 sweetcpufreq=0 wdt=-1 wdt_test=no_user_checkin rd=md0`. ADT: `mkadt.py --marie`. RAM disk: `research/init/sweet-init` (mockfs root).
Result: A73 0x100 alive, SGI1 verified through the GICv2 backend, `BSD root: md0`, `*** SWEET userspace: init running as PID 1 ***` and 5 s heartbeats.
What it took:
- **0200** GICv2 backend
- **0201** boot CPU taken from the ADT
- **0202** early console on `0x18C00000`: topaz's was hard-wired to `0x9C000000`, which on marie is RAM inside the kernel window
- **0203** SPAN set: QEMU only
- **shim:** clean+invalidate the A53's D-caches by set/way before CPU_ON. Without it XNU hung right after `start_first_cpu` (stage 0 band only): dirty fastboot lines on the A53.
QEMU (`tools/marie-image.sh ... --qemu`, GICv2) reaches the same point, except that the early console is not visible there.

## Phase 0: Bare-metal probes (shim only, no XNU)
Fork ios-topaz into this directory and add a `marie` branch. Adapt `shim/` (`start.S`, `shim.c`, `loader.c`, `mmu.c`, `console.c`, `build.sh`):
1. **Boot format:** keep a Linux arm64 Image header and pack it as **boot v1** with kernel at `0x80000`. Put the payload after `_end`, as topaz already does. First test whether Huawei fastboot accepts `fastboot boot` with the stock `clean/kernel_boot.img`. If it does, use RAM boots for every iteration; if not, flash to `kernel`.
2. **Watchdog:** disable the SP805 at `0xE8A06000` first thing; the code can be lifted from `board-agassi2.c`. Then let the shim idle for 5 minutes with no Linux running, to rule out an LPM3/system watchdog.
3. **Framebuffer:** paint test bars at `0x18C00000` (1080×2312×4). If nothing appears, read the DSS overlay/RDMA address registers (DSS base from the `dss`/`hisifb` DT node in `kernel-marie`) to find the real scanout buffer. Put the shim console there.
4. **CPU checks:** print `CurrentEL`, MIDR, and `ID_AA64MMFR0_EL1` on CPU 0 and on CPU `0x100` after PSCI `CPU_ON`. **Gate: the A73 must report TGran16 ≠ 0.** If it doesn't, XNU needs a 4K-page build (topaz roadmap item 6), which is much riskier.
5. **GIC:** on CPU `0x100`, read `GICD_TYPER`, `GICD_IGROUPRn`, `GICD_ITARGETSR0` (this CPU's interface bit) and `GICC_IIDR`. Fire a test SGI and a vtimer PPI 27 and confirm they arrive as Group 1 IRQs.
6. **Toolchain:** run topaz's unmodified `xnu.yml` on the fork to confirm the CI build works.

## Phase 1: `MARIE` machine config in XNU
Base it on topaz's patch series. Keep 0001–0027 (toolchain, console, PE, mockfs, AMFI/HFS/bearssl), 0035–0059, 0064–0068 and 0081–0082. Drop or gate the Qualcomm drivers: UFS 0028/0030/0034/0069, Goodix/FT touch, MDP scanout 0092+, battery 0091, USB 0072–0077.

New patches, starting at 0200:
- **0200 machine config:** `SUPPORTED_ARM64_MACHINE_CONFIGS += MARIE`, `-mcpu=cortex-a53`, plain arm64 (no arm64e), and `pexpert/pexpert/arm64/MARIE.h` modelled on `SWEET.h`.
- **0201 GICv2 backend.** This replaces the `ICC_*_EL1` sysreg code in `osfmk/arm64/sleh.c`, `pexpert/arm/pe_fiq.c` and `iokit/Kernel/arm/SweetCPU.cpp` (topaz patch 0004/0018):
  - IRQ ack/EOI: `GICC_IAR` (+0x0C) / `GICC_EOIR` (+0x10)
  - CPU interface setup: `GICC_PMR` (+0x04) = 0xF0, `GICC_CTLR` (+0x00) enable Group 1
  - IPIs: `GICD_SGIR` (+0xF00), target list taken from each CPU's `GICD_ITARGETSR0` byte (not from MPIDR)
  - SPI enable/priority/target: `GICD_ISENABLER`/`IPRIORITYR`/`ITARGETSR`
  - map both register blocks through the ADT `reg` entries
  - vtimer is INTID 27
- **0202 timer/CPU topology:** 1.92 MHz timebase in the ADT. Boot on `0x100` (reuse 0082). List cluster 0x100–0x103 as P-cores only; the A53s stay parked.
- **0203 framebuffer console** with the marie geometry from Phase 0, and the iPhone X display identity scaled to 1080×2312 (as topaz 0061 does for 1080×2400).
- `tools/mkadt.py`:
  - add a `--board marie` profile with the memory window. Start conservatively at `0x40000000` + 2 GiB, then grow to the full `0x2E200000–0xDFFFFFFF` range.
  - add GIC, timer and SP805 nodes, the A73 CPU nodes, and the `/product` node copied from the iPhone X ADT (`d22ap`).
- **Milestone:** XNU verbose boot text on the panel, with all 4 A73s up via sweet's PSCI SMP patch 0065. Boot it with the mockfs ramdisk root (0021/0022/0024) and a tiny `launchd` test (`tools/mkinit.py`) before any storage work.

## Phase 2: eMMC root
- **0210 SDHCI eMMC driver** for `sdhci@FF3E1000`. It replaces UFS as the root device provider used by `rd=` (topaz 0028):
  - start with PIO or SDMA, single and multi-block CMD17/18/24/25
  - reuse the controller and card state fastboot leaves behind (clocks, bus width, HS200/HS400 timing) instead of reinitialising; keep a slow re-init fallback modelled on `sdhci_mshc_libra.c`
  - the userdata start LBA comes from `/sys/block/mmcblk0/mmcblk0p70/start` and is passed as a boot-arg (`emmcroot=<lba>`), or the driver parses the GPT
- Writes and journaling (topaz 0030/0031 equivalents), then a performance pass (ADMA2).
- **Milestone:** `rd=emmcroot0 rootrw=1` mounts the HFS+ iOS root, and launchd plus the daemons start. Verify in the persistent log ring (0047) and with `tools/sweet-log.py`.

## Phase 3: iOS userland to SpringBoard
- Build the root exactly as topaz does: `tools/mkiosroot.sh` with the iPhone X 16.7.x OS/SystemOS/AppOS images, then `tools/sweet-root-tweaks.py`, then `img2simg`. Flash it with `fastboot flash userdata`. The Huawei fastboot sparse/size limits may need `-S 256M`.
- Make the kernel IOSurface/IOMobileFramebuffer path (0046, plus topaz's copy/flip patches 0100–0103) present into the marie scanout buffer.
- **Milestone:** Apple logo, then backboardd, then SpringBoard lock screen.

## Phase 4: Input and basics
- **Touch:**
  - PL022 SPI driver and Kirin GPIO (CS GPIO 10, IRQ/reset GPIOs from the `novatek` node in the marie DT)
  - Novatek protocol driver, ported from `kernel-marie/drivers/devkit/tpkit/` (the novatek ts_kit backend)
  - feed it into sweet's HID event services (0058/0060)
  - check whether fastboot leaves the touch rail powered; if not, the PMIC LDO has to be driven (hi6555 PMU)
- **Power/volume keys:** `hisi_gpio_key` (volume) and the PMIC power key `hisi_on`.
- **SMP polish, cpufreq, brightness** (`lm36274` backlight over I²C 3-0011), RTC.

## Verification (per phase)
- **Phase 0:** test bars visible; MIDR/MMFR0/GIC dump shown on the panel; the shim survives 5 minutes idle; SGI and PPI 27 delivered on CPU `0x100`.
- **Phase 1:** CI produces `kernel.development.marie` with no LSE instructions left (`llvm-objdump -d --mattr=+v8.5a | grep -P '\t(cas|ldadd|ldset|ldclr|swp|ldapr)'`). The panel shows the XNU banner, `cpus=4`, and the ramdisk launchd test output.
- **Phase 2:** an HFS+ root mounted read/write from `mmcblk0p70`; after reboot back to Lineage/TWRP, `fsck.hfsplus` on the image comes up clean.
- **Phase 3:** lock screen rendered.
- **Phase 4:** touch unlocks the lock screen.

## Main risks
1. **A73 16K granule** (checked in Phase 0). If it's missing, a 4K-page XNU is needed, against an iOS userland only ever tested on 16K pages.
2. **Hidden watchdogs**: LPM3/system or Huawei RDR "AP alive" checks that reboot the phone without Linux running.
3. **Scanout continuity**: Huawei fastboot may stop DSS scanout at handoff. In that case a minimal DSS/DSI bring-up is needed, based on `kernel-marie` hisifb/lcdkit for panel `MARI_130 203`.
4. **GICv2 security setup**: interrupts the secure firmware has configured as Group 0 can't be used. This only matters if the vtimer or SGIs are among them.
5. **Low timer frequency** (1.92 MHz) exposing XNU timing assumptions.
6. **Fastboot quirks**: no `fastboot boot`, which makes every iteration a flash; sparse size limits for the multi-GB userdata image.

## Where things live
- This directory: the topaz fork (`shim/`, `xnu/patches/`, `tools/`, `.github/workflows/xnu.yml`) on a `marie` branch
- `../marie/kernel-marie/`: reference drivers and DT (`sdhci_mshc_libra.c`, hisifb, tpkit/novatek, `arch/arm64/boot/dts/auto-generate/{config,overlay}/kirin710/`)
- `../marie/clean/`: stock images for recovery
