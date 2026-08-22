#include <console.h>
#include <font.h>

#define TAB_WIDTH 8

static struct {
    volatile uint32_t *pixels;
    size_t width;
    size_t height;
    size_t pitch_px;

    uint8_t r_size, r_shift;
    uint8_t g_size, g_shift;
    uint8_t b_size, b_shift;

    size_t scale;
    size_t cell_w, cell_h;
    size_t cols, rows;
    size_t cx, cy;

    uint32_t fg_rgb, bg_rgb;
    uint32_t fg, bg;

    bool ready;
} con;

static uint32_t pack_color(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8) & 0xFF;
    uint32_t b = rgb & 0xFF;

    return ((r >> (8 - con.r_size)) << con.r_shift)
         | ((g >> (8 - con.g_size)) << con.g_shift)
         | ((b >> (8 - con.b_size)) << con.b_shift);
}

static void fill_rect(size_t x, size_t y, size_t w, size_t h, uint32_t packed) {
    for (size_t j = 0; j < h; j++) {
        size_t py = y + j;

        if (py >= con.height) {
            break;
        }

        volatile uint32_t *row = con.pixels + py * con.pitch_px;
        for (size_t i = 0; i < w && x + i < con.width; i++) {
            row[x + i] = packed;
        }
    }
}

static void draw_cell(size_t col, size_t row, char c) {
    const uint8_t *glyph = font_glyph(c);
    size_t ox = col * con.cell_w;
    size_t oy = row * con.cell_h;

    fill_rect(ox, oy, con.cell_w, con.cell_h, con.bg);

    for (size_t gy = 0; gy < FONT_HEIGHT; gy++) {
        uint8_t bits = glyph[gy];

        if (bits == 0) {
            continue;
        }
        for (size_t gx = 0; gx < FONT_WIDTH; gx++) {
            if ((bits >> (FONT_WIDTH - 1 - gx)) & 1) {
                fill_rect(ox + gx * con.scale, oy + gy * con.scale,
                          con.scale, con.scale, con.fg);
            }
        }
    }
}

static void scroll(void) {
    size_t used_h = con.rows * con.cell_h;
    size_t shift = con.cell_h;

    for (size_t y = 0; y + shift < used_h; y++) {
        volatile uint32_t *dst = con.pixels + y * con.pitch_px;
        volatile const uint32_t *src = con.pixels + (y + shift) * con.pitch_px;

        for (size_t x = 0; x < con.width; x++) {
            dst[x] = src[x];
        }
    }

    fill_rect(0, used_h - shift, con.width, shift, con.bg);
}

static void newline(void) {
    con.cx = 0;
    con.cy++;

    if (con.cy >= con.rows) {
        scroll();
        con.cy = con.rows - 1;
    }
}

bool console_init(struct limine_framebuffer *fb, size_t scale) {
    if (fb == NULL || fb->address == NULL || fb->bpp != 32 || scale == 0) {
        return false;
    }

    con.pixels = fb->address;
    con.width = fb->width;
    con.height = fb->height;
    con.pitch_px = fb->pitch / 4;

    con.r_size = fb->red_mask_size;
    con.r_shift = fb->red_mask_shift;
    con.g_size = fb->green_mask_size;
    con.g_shift = fb->green_mask_shift;
    con.b_size = fb->blue_mask_size;
    con.b_shift = fb->blue_mask_shift;

    bool masks_sane = con.r_size >= 1 && con.r_size <= 8
                   && con.g_size >= 1 && con.g_size <= 8
                   && con.b_size >= 1 && con.b_size <= 8;
    if (!masks_sane) {
        con.r_size = con.g_size = con.b_size = 8;
        con.r_shift = 16;
        con.g_shift = 8;
        con.b_shift = 0;
    }

    con.scale = scale;
    con.cell_w = FONT_WIDTH * scale;
    con.cell_h = FONT_HEIGHT * scale;
    con.cols = con.width / con.cell_w;
    con.rows = con.height / con.cell_h;

    if (con.cols == 0 || con.rows == 0) {
        return false;
    }

    con.cx = 0;
    con.cy = 0;
    con.ready = true;

    console_set_color(CONSOLE_WHITE, CONSOLE_BLACK);
    console_clear();

    return true;
}

bool console_ready(void) {
    return con.ready;
}

void console_clear(void) {
    if (!con.ready) {
        return;
    }

    fill_rect(0, 0, con.width, con.height, con.bg);
    con.cx = 0;
    con.cy = 0;
}

void console_set_color(uint32_t fg, uint32_t bg) {
    if (!con.ready) {
        return;
    }

    con.fg_rgb = fg;
    con.bg_rgb = bg;
    con.fg = pack_color(fg);
    con.bg = pack_color(bg);
}

void console_get_color(uint32_t *fg, uint32_t *bg) {
    if (fg != NULL) {
        *fg = con.fg_rgb;
    }
    if (bg != NULL) {
        *bg = con.bg_rgb;
    }
}

size_t console_cols(void) {
    return con.cols;
}

size_t console_rows(void) {
    return con.rows;
}

void console_set_cursor(size_t col, size_t row) {
    if (!con.ready) {
        return;
    }

    con.cx = col < con.cols ? col : con.cols - 1;
    con.cy = row < con.rows ? row : con.rows - 1;
}

void console_get_cursor(size_t *col, size_t *row) {
    if (col != NULL) {
        *col = con.cx;
    }
    if (row != NULL) {
        *row = con.cy;
    }
}

void console_putchar(char c) {
    if (!con.ready) {
        return;
    }

    switch (c) {
    case '\n':
        newline();
        break;

    case '\r':
        con.cx = 0;
        break;

    case '\b':
        if (con.cx > 0) {
            con.cx--;
            draw_cell(con.cx, con.cy, ' ');
        }
        break;

    case '\t':

        do {
            console_putchar(' ');
        } while (con.cx % TAB_WIDTH != 0);
        break;

    default:
        if ((unsigned char)c < 0x20) {
            return;
        }
        draw_cell(con.cx, con.cy, c);
        con.cx++;
        if (con.cx >= con.cols) {
            newline();
        }
        break;
    }
}

void console_write(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        console_putchar(s[i]);
    }
}
