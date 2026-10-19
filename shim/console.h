#pragma once
#include <stdint.h>

/*
 * marie defaults: Huawei fastboot logo buffer (kirin710_memory.dtsi
 * logo-buffer) and the 1080x2312 panel. shim.c overrides con_fb/con_w/
 * con_h/con_stride with what the DSS read channel actually scans out.
 */
#define FB_BASE		0x18c00000UL
#define FB_W		1080
#define FB_H		2312
#define FB_STRIDE	(FB_W * 4)	/* a8r8g8b8 */

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint32_t *con_fb;
extern int con_w, con_h, con_stride;	/* pixels, pixels, bytes per line */

void con_clear(uint32_t bg);
void con_skip(void);
void con_bg_set(uint32_t bg);	/* text background only, screen kept */
int con_row(void);
void con_set_row(int r);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
