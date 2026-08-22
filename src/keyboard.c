#include <ctype.h>
#include <interrupts.h>
#include <io.h>
#include <keyboard.h>
#include <stdint.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define PS2_OUTPUT_FULL 0x01
#define PS2_INPUT_FULL  0x02
#define PS2_FROM_MOUSE  0x20

#define PS2_READ_CONFIG   0x20
#define PS2_WRITE_CONFIG  0x60
#define PS2_DISABLE_PORT1 0xAD
#define PS2_ENABLE_PORT1  0xAE

#define CONFIG_PORT1_IRQ   (1u << 0)
#define CONFIG_PORT1_CLOCK (1u << 4)
#define CONFIG_TRANSLATE   (1u << 6)

#define IO_TIMEOUT 100000

#define RING_SIZE 128

static const char keymap_plain[128] =
     "\0\x1b" "1234567890-=" "\b\t"
     "qwertyuiop[]" "\n" "\0" "as"
     "dfghjkl;'`" "\0" "\\zxcv"
     "bnm,./" "\0" "*" "\0" " " "\0\0\0\0\0\0"
     "\0\0\0\0\0\0\0" "789-456+1"
     "230.";

static const char keymap_shift[128] =
     "\0\x1b" "!@#$%^&*()_+" "\b\t"
     "QWERTYUIOP{}" "\n" "\0" "AS"
     "DFGHJKL:\"~" "\0" "|ZXCV"
     "BNM<>?" "\0" "*" "\0" " " "\0\0\0\0\0\0"
     "\0\0\0\0\0\0\0" "789-456+1"
     "230.";

#define SCAN_LSHIFT 0x2A
#define SCAN_RSHIFT 0x36
#define SCAN_CTRL   0x1D
#define SCAN_CAPS   0x3A

static struct {
    volatile char ring[RING_SIZE];
    volatile size_t head;
    volatile size_t tail;
    volatile size_t dropped;

    bool extended;
    unsigned skip;
    bool shift;
    bool ctrl;
    bool caps;

    bool ready;
} kbd;

static void ring_push(char c) {
    size_t next = (kbd.head + 1) % RING_SIZE;

    if (next == kbd.tail) {
        kbd.dropped++;
        return;
    }

    kbd.ring[kbd.head] = c;
    kbd.head = next;
}

static int ring_pop(void) {
    if (kbd.tail == kbd.head) {
        return KEY_NONE;
    }

    char c = kbd.ring[kbd.tail];

    kbd.tail = (kbd.tail + 1) % RING_SIZE;

    return (unsigned char)c;
}

static void decode(uint8_t code) {
    if (kbd.skip > 0) {
        kbd.skip--;
        return;
    }

    if (code == 0xE1) {
        kbd.skip = 5;
        return;
    }
    if (code == 0xE0) {
        kbd.extended = true;
        return;
    }

    bool released = (code & 0x80) != 0;
    uint8_t key = code & 0x7F;
    bool extended = kbd.extended;

    kbd.extended = false;

    if (extended) {
        switch (key) {
        case SCAN_CTRL:
            kbd.ctrl = !released;
            return;
        case 0x1C:
            if (!released) {
                ring_push('\n');
            }
            return;
        case 0x35:
            if (!released) {
                ring_push('/');
            }
            return;
        default:
            return;
        }
    }

    switch (key) {
    case SCAN_LSHIFT:
    case SCAN_RSHIFT:
        kbd.shift = !released;
        return;
    case SCAN_CTRL:
        kbd.ctrl = !released;
        return;
    case SCAN_CAPS:
        if (!released) {
            kbd.caps = !kbd.caps;
        }
        return;
    default:
        break;
    }

    if (released) {
        return;
    }

    char c = kbd.shift ? keymap_shift[key] : keymap_plain[key];

    if (c == '\0') {
        return;
    }

    if (kbd.caps && isalpha((unsigned char)c)) {
        c ^= 0x20;
    }

    if (kbd.ctrl) {

        if (isalpha((unsigned char)c) || (c >= '[' && c <= '_')) {
            c = (char)(c & 0x1F);
        } else if (c == '?') {
            c = 0x7F;
        } else {
            return;
        }
    }

    ring_push(c);
}

static void keyboard_service(void) {
    uint8_t status;

    while (((status = inb(PS2_STATUS)) & PS2_OUTPUT_FULL) != 0) {
        uint8_t data = inb(PS2_DATA);

        if ((status & PS2_FROM_MOUSE) == 0) {
            decode(data);
        }
    }
}

static bool wait_writable(void) {
    for (int i = 0; i < IO_TIMEOUT; i++) {
        if ((inb(PS2_STATUS) & PS2_INPUT_FULL) == 0) {
            return true;
        }
    }

    return false;
}

static bool wait_readable(void) {
    for (int i = 0; i < IO_TIMEOUT; i++) {
        if ((inb(PS2_STATUS) & PS2_OUTPUT_FULL) != 0) {
            return true;
        }
    }

    return false;
}

static bool send_command(uint8_t cmd) {
    if (!wait_writable()) {
        return false;
    }
    outb(PS2_CMD, cmd);

    return true;
}

static void flush_output(void) {
    for (int i = 0; i < 32 && (inb(PS2_STATUS) & PS2_OUTPUT_FULL) != 0; i++) {
        inb(PS2_DATA);
    }
}

bool keyboard_init(void) {
    bool configured = false;

    interrupts_disable();

    kbd.head = kbd.tail = 0;
    kbd.dropped = 0;
    kbd.extended = false;
    kbd.skip = 0;
    kbd.shift = kbd.ctrl = kbd.caps = false;

    send_command(PS2_DISABLE_PORT1);
    flush_output();

    if (send_command(PS2_READ_CONFIG) && wait_readable()) {
        uint8_t config = inb(PS2_DATA);

        config |= CONFIG_PORT1_IRQ | CONFIG_TRANSLATE;
        config &= (uint8_t)~CONFIG_PORT1_CLOCK;

        if (send_command(PS2_WRITE_CONFIG) && wait_writable()) {
            outb(PS2_DATA, config);
            configured = true;
        }
    }

    send_command(PS2_ENABLE_PORT1);
    flush_output();

    irq_install(IRQ_KEYBOARD, keyboard_service);
    irq_unmask(IRQ_KEYBOARD);

    kbd.ready = true;

    return configured;
}

bool keyboard_ready(void) {
    return kbd.ready;
}

static bool irq_driven(void) {
    return interrupts_ready() && interrupts_enabled();
}

int keyboard_poll(void) {
    bool live = irq_driven();

    interrupts_disable();

    if (!live) {
        keyboard_service();
    }

    int c = ring_pop();

    if (live) {
        interrupts_enable();
    }

    return c;
}

int keyboard_getchar(void) {
    for (;;) {
        bool live = irq_driven();

        interrupts_disable();

        int c = ring_pop();

        if (c != KEY_NONE) {
            if (live) {
                interrupts_enable();
            }
            return c;
        }

        if (live) {

            asm volatile ("sti; hlt");
        } else {
            keyboard_service();
            asm volatile ("pause");
        }
    }
}

bool keyboard_has_input(void) {
    bool live = irq_driven();

    interrupts_disable();

    if (!live) {
        keyboard_service();
    }

    bool available = kbd.head != kbd.tail;

    if (live) {
        interrupts_enable();
    }

    return available;
}

void keyboard_flush(void) {
    bool were_enabled = interrupts_enabled();

    interrupts_disable();
    kbd.tail = kbd.head;

    if (were_enabled) {
        interrupts_enable();
    }
}

size_t keyboard_dropped(void) {
    return kbd.dropped;
}
