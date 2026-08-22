#ifndef _KEYBOARD_H
#define _KEYBOARD_H

#include <stdbool.h>
#include <stddef.h>

#define KEY_NONE (-1)

bool keyboard_init(void);
bool keyboard_ready(void);

int keyboard_getchar(void);

int keyboard_poll(void);
bool keyboard_has_input(void);
void keyboard_flush(void);

size_t keyboard_dropped(void);

#endif
