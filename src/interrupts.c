#include <console.h>
#include <interrupts.h>
#include <io.h>
#include <memory.h>
#include <stdio.h>
#include <string.h>

#define IDT_ENTRIES 256

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define PIC_EOI      0x20
#define PIC_READ_ISR 0x0B

#define ICW1_INIT 0x11
#define ICW4_8086 0x01

#define GATE_INTERRUPT 0x8E

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct interrupt_frame {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

static struct idt_entry idt[IDT_ENTRIES];
static void (*irq_handlers[IRQ_COUNT])(void);
static bool ready;

static _Noreturn void exception_panic(unsigned vector, const char *name,
                                      struct interrupt_frame *frame,
                                      uint64_t error, bool has_error) {
    uint64_t cr2;

    asm volatile ("mov %%cr2, %0" : "=r" (cr2));

    console_set_color(CONSOLE_RED, CONSOLE_BLACK);
    printf("\n-- exception %u: %s --\n", vector, name);
    printf("   rip    %#018llx  cs  %#llx\n",
           (unsigned long long)frame->rip, (unsigned long long)frame->cs);
    printf("   rsp    %#018llx  ss  %#llx\n",
           (unsigned long long)frame->rsp, (unsigned long long)frame->ss);
    printf("   rflags %#018llx\n", (unsigned long long)frame->rflags);

    if (has_error) {
        printf("   error  %#018llx\n", (unsigned long long)error);
    }
    if (vector == 14) {
        printf("   cr2    %#018llx  (the address that faulted)\n",
               (unsigned long long)cr2);
    }

    printf("   halted.\n");

    for (;;) {
        asm volatile ("cli; hlt");
    }
}

#define EXCEPTIONS(X, XE) \
    X (0,  "divide error") \
    X (1,  "debug") \
    X (2,  "non-maskable interrupt") \
    X (3,  "breakpoint") \
    X (4,  "overflow") \
    X (5,  "bound range exceeded") \
    X (6,  "invalid opcode") \
    X (7,  "device not available") \
    XE(8,  "double fault") \
    X (9,  "coprocessor segment overrun") \
    XE(10, "invalid TSS") \
    XE(11, "segment not present") \
    XE(12, "stack-segment fault") \
    XE(13, "general protection fault") \
    XE(14, "page fault") \
    X (15, "reserved") \
    X (16, "x87 floating-point exception") \
    XE(17, "alignment check") \
    X (18, "machine check") \
    X (19, "SIMD floating-point exception") \
    X (20, "virtualisation exception") \
    XE(21, "control protection exception") \
    X (22, "reserved") \
    X (23, "reserved") \
    X (24, "reserved") \
    X (25, "reserved") \
    X (26, "reserved") \
    X (27, "reserved") \
    X (28, "hypervisor injection exception") \
    XE(29, "VMM communication exception") \
    XE(30, "security exception") \
    X (31, "reserved")

#define EXC_STUB(vector, name) \
    static __attribute__((interrupt)) \
    void exception_stub_##vector(struct interrupt_frame *frame) { \
        exception_panic(vector, name, frame, 0, false); \
    }

#define EXC_STUB_EC(vector, name) \
    static __attribute__((interrupt)) \
    void exception_stub_##vector(struct interrupt_frame *frame, uint64_t error) { \
        exception_panic(vector, name, frame, error, true); \
    }

EXCEPTIONS(EXC_STUB, EXC_STUB_EC)

static void *const exception_stubs[] = {
#define EXC_ADDR(vector, name) (void *)exception_stub_##vector,
    EXCEPTIONS(EXC_ADDR, EXC_ADDR)
#undef EXC_ADDR
};

static void pic_send_eoi(unsigned irq) {
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}

static bool pic_spurious(unsigned irq) {
    if (irq != 7 && irq != 15) {
        return false;
    }

    uint16_t port = (irq == 7) ? PIC1_CMD : PIC2_CMD;

    outb(port, PIC_READ_ISR);
    if (inb(port) & (1u << (irq & 7))) {
        return false;
    }

    if (irq == 15) {
        outb(PIC1_CMD, PIC_EOI);
    }

    return true;
}

static void pic_remap(void) {
    outb(PIC1_CMD, ICW1_INIT);           io_wait();
    outb(PIC2_CMD, ICW1_INIT);           io_wait();
    outb(PIC1_DATA, IRQ_BASE);           io_wait();
    outb(PIC2_DATA, IRQ_BASE + 8);       io_wait();
    outb(PIC1_DATA, 1u << 2);            io_wait();
    outb(PIC2_DATA, 2);                  io_wait();
    outb(PIC1_DATA, ICW4_8086);          io_wait();
    outb(PIC2_DATA, ICW4_8086);          io_wait();

    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void irq_mask(uint8_t irq) {
    if (irq >= IRQ_COUNT) {
        return;
    }

    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;

    outb(port, inb(port) | (uint8_t)(1u << (irq & 7)));
}

void irq_unmask(uint8_t irq) {
    if (irq >= IRQ_COUNT) {
        return;
    }

    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;

    outb(port, inb(port) & (uint8_t)~(1u << (irq & 7)));

    if (irq >= 8) {
        irq_unmask(2);
    }
}

static void irq_dispatch(unsigned irq) {
    if (pic_spurious(irq)) {
        return;
    }

    void (*handler)(void) = irq_handlers[irq];

    if (handler != NULL) {
        handler();
    }

    pic_send_eoi(irq);
}

#define IRQS(X) \
    X(0)  X(1)  X(2)  X(3)  X(4)  X(5)  X(6)  X(7) \
    X(8)  X(9)  X(10) X(11) X(12) X(13) X(14) X(15)

#define IRQ_STUB(n) \
    static __attribute__((interrupt)) \
    void irq_stub_##n(struct interrupt_frame *frame) { \
        (void)frame; \
        irq_dispatch(n); \
    }

IRQS(IRQ_STUB)

static void *const irq_stubs[IRQ_COUNT] = {
#define IRQ_ADDR(n) (void *)irq_stub_##n,
    IRQS(IRQ_ADDR)
#undef IRQ_ADDR
};

void irq_install(uint8_t irq, void (*handler)(void)) {
    if (irq >= IRQ_COUNT) {
        return;
    }

    irq_handlers[irq] = handler;
}

#define IA32_APIC_BASE   0x1B
#define APIC_BASE_ENABLE (1ULL << 11)
#define APIC_BASE_MASK   0x000FFFFFFFFFF000ULL

#define LAPIC_SPURIOUS 0xF0
#define LAPIC_LVT0     0x350

#define LAPIC_SW_ENABLE (1u << 8)
#define LVT_LEVEL       (1u << 15)
#define LVT_EXTINT      (0x7u << 8)

#define SPURIOUS_VECTOR 0xFF
#define PAGE_SIZE       4096

static __attribute__((interrupt))
void spurious_stub(struct interrupt_frame *frame) {
    (void)frame;
}

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;

    asm volatile ("rdmsr" : "=a" (lo), "=d" (hi) : "c" (msr));

    return ((uint64_t)hi << 32) | lo;
}

static bool lapic_virtual_wire(void) {
    uint64_t apic_base = rdmsr(IA32_APIC_BASE);

    if ((apic_base & APIC_BASE_ENABLE) == 0) {
        return true;
    }

    volatile uint32_t *lapic = mmio_map(apic_base & APIC_BASE_MASK, PAGE_SIZE);

    if (lapic == NULL) {
        return false;
    }

    if ((lapic[LAPIC_SPURIOUS / 4] & LAPIC_SW_ENABLE) == 0) {

        lapic[LAPIC_SPURIOUS / 4] = LAPIC_SW_ENABLE | SPURIOUS_VECTOR;
    }

    lapic[LAPIC_LVT0 / 4] = LVT_EXTINT | LVT_LEVEL;

    return true;
}

static void idt_set(uint8_t vector, void *stub, uint16_t selector) {
    uint64_t addr = (uint64_t)(uintptr_t)stub;

    idt[vector].offset_low  = (uint16_t)addr;
    idt[vector].selector    = selector;
    idt[vector].ist         = 0;
    idt[vector].type_attr   = GATE_INTERRUPT;
    idt[vector].offset_mid  = (uint16_t)(addr >> 16);
    idt[vector].offset_high = (uint32_t)(addr >> 32);
    idt[vector].reserved    = 0;
}

bool interrupts_init(void) {
    uint16_t selector;

    interrupts_disable();
    asm volatile ("mov %%cs, %0" : "=r" (selector));

    memset(idt, 0, sizeof idt);

    for (size_t i = 0; i < sizeof exception_stubs / sizeof exception_stubs[0]; i++) {
        idt_set((uint8_t)i, exception_stubs[i], selector);
    }
    for (size_t i = 0; i < IRQ_COUNT; i++) {
        idt_set((uint8_t)(IRQ_BASE + i), irq_stubs[i], selector);
    }
    idt_set(SPURIOUS_VECTOR, (void *)spurious_stub, selector);

    struct idtr idtr = { .limit = sizeof idt - 1, .base = (uint64_t)(uintptr_t)idt };

    asm volatile ("lidt %0" : : "m" (idtr));

    pic_remap();
    ready = lapic_virtual_wire();

    return ready;
}

bool interrupts_ready(void) {
    return ready;
}

void interrupts_enable(void) {
    asm volatile ("sti");
}

void interrupts_disable(void) {
    asm volatile ("cli");
}

bool interrupts_enabled(void) {
    uint64_t rflags;

    asm volatile ("pushfq; pop %0" : "=r" (rflags));

    return (rflags & (1u << 9)) != 0;
}
