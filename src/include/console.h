#ifndef _CONSOLE_H
#define _CONSOLE_H

#include <limine.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONSOLE_BLACK   0x000000u
#define CONSOLE_WHITE   0xd6d6d6u
#define CONSOLE_BRIGHT  0xffffffu
#define CONSOLE_GRAY    0x8a8a8au
#define CONSOLE_RED     0xe0574fu
#define CONSOLE_GREEN   0x59c46eu
#define CONSOLE_YELLOW  0xd9b44au
#define CONSOLE_BLUE    0x5b9bd6u
#define CONSOLE_MAGENTA 0xc078d0u
#define CONSOLE_CYAN    0x4fc0c0u

bool console_init(struct limine_framebuffer *fb, size_t scale);
bool console_ready(void);

void console_clear(void);
void console_putchar(char c);
void console_write(const char *s, size_t len);

void console_set_color(uint32_t fg, uint32_t bg);
void console_get_color(uint32_t *fg, uint32_t *bg);

size_t console_cols(void);
size_t console_rows(void);
void console_set_cursor(size_t col, size_t row);
void console_get_cursor(size_t *col, size_t *row);

#endif
