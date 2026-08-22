#ifndef _INTERRUPTS_H
#define _INTERRUPTS_H

#include <stdbool.h>
#include <stdint.h>

#define IRQ_BASE     0x20
#define IRQ_COUNT    16
#define IRQ_TIMER    0
#define IRQ_KEYBOARD 1

bool interrupts_init(void);

bool interrupts_ready(void);

void interrupts_enable(void);
void interrupts_disable(void);
bool interrupts_enabled(void);

void irq_install(uint8_t irq, void (*handler)(void));
void irq_mask(uint8_t irq);
void irq_unmask(uint8_t irq);

#endif
