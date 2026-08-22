#ifndef _IO_H
#define _IO_H

#include <stdint.h>

static inline uint8_t inb(uint16_t port) {
    uint8_t value;

    asm volatile ("inb %w1, %b0" : "=a" (value) : "Nd" (port) : "memory");

    return value;
}

static inline void outb(uint16_t port, uint8_t value) {
    asm volatile ("outb %b0, %w1" : : "a" (value), "Nd" (port) : "memory");
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;

    asm volatile ("inw %w1, %w0" : "=a" (value) : "Nd" (port) : "memory");

    return value;
}

static inline void outw(uint16_t port, uint16_t value) {
    asm volatile ("outw %w0, %w1" : : "a" (value), "Nd" (port) : "memory");
}

static inline uint32_t inl(uint16_t port) {
    uint32_t value;

    asm volatile ("inl %w1, %k0" : "=a" (value) : "Nd" (port) : "memory");

    return value;
}

static inline void outl(uint16_t port, uint32_t value) {
    asm volatile ("outl %k0, %w1" : : "a" (value), "Nd" (port) : "memory");
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

#endif
