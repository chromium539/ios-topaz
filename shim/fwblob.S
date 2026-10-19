/*
 * Novatek touch firmware for the touch test (shim/touch.c). Huawei's file,
 * not in git: extracted from the vendor image into research/touch/ (PLAN.md);
 * build.sh passes its path as NVT_FW_PATH when it exists.
 */
	.section .rodata
	.balign	16
	.globl	nvt_fw
	.globl	nvt_fw_end
nvt_fw:
#ifdef NVT_FW_PATH
	.incbin	NVT_FW_PATH
#endif
nvt_fw_end:
