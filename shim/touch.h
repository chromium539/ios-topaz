#pragma once
#include <stdint.h>

/* Read-only probe of the Novatek TDDI touch controller on SPI1. */
void test_touch(void);

/* shim.c: MMIO read that survives an external abort (ok = 0) */
uint32_t probe32(uintptr_t a, int *ok);
