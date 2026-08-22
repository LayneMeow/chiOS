#include <console.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FLAG_LEFT  (1u << 0)
#define FLAG_ZERO  (1u << 1)
#define FLAG_PLUS  (1u << 2)
#define FLAG_SPACE (1u << 3)
#define FLAG_ALT   (1u << 4)

#define MAX_FIELD 4096

enum length {
    LEN_INT,
    LEN_CHAR,
    LEN_SHORT,
    LEN_LONG,
    LEN_LLONG,
    LEN_SIZE,
    LEN_PTRDIFF,
    LEN_INTMAX,
};

struct sink {
    char *buf;
    size_t cap;
    size_t len;
    bool to_console;
};

static void emit(struct sink *s, char c) {
    if (s->to_console) {
        console_putchar(c);
    } else if (s->buf != NULL && s->len + 1 < s->cap) {

        s->buf[s->len] = c;
    }

    s->len++;
}

static void emit_pad(struct sink *s, char c, size_t n) {
    for (size_t i = 0; i < n; i++) {
        emit(s, c);
    }
}

static void emit_number(struct sink *s, unsigned long long value, unsigned base,
                        bool upper, char sign, const char *prefix,
                        unsigned flags, int width, int prec) {
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char *digits = upper ? upper_digits : lower_digits;

    char tmp[24];
    size_t ndigits = 0;

    if (value == 0) {

        if (prec != 0) {
            tmp[ndigits++] = '0';
        }
    } else {
        while (value != 0 && ndigits < sizeof tmp) {
            tmp[ndigits++] = digits[value % base];
            value /= base;
        }
    }

    size_t nprefix = prefix != NULL ? strlen(prefix) : 0;
    size_t nzeros = (prec > 0 && (size_t)prec > ndigits) ? (size_t)prec - ndigits : 0;
    size_t body = ndigits + nzeros + nprefix + (sign != '\0' ? 1u : 0u);
    size_t pad = (size_t)width > body ? (size_t)width - body : 0;

    bool zero_pad = (flags & FLAG_ZERO) && !(flags & FLAG_LEFT) && prec < 0;

    if (!(flags & FLAG_LEFT) && !zero_pad) {
        emit_pad(s, ' ', pad);
    }
    if (sign != '\0') {
        emit(s, sign);
    }
    for (size_t i = 0; i < nprefix; i++) {
        emit(s, prefix[i]);
    }
    if (zero_pad) {
        emit_pad(s, '0', pad);
    }
    emit_pad(s, '0', nzeros);
    while (ndigits > 0) {
        emit(s, tmp[--ndigits]);
    }
    if (flags & FLAG_LEFT) {
        emit_pad(s, ' ', pad);
    }
}

static void emit_string(struct sink *s, const char *str, unsigned flags,
                        int width, int prec) {
    if (str == NULL) {
        str = "(null)";
    }

    size_t slen = prec >= 0 ? strnlen(str, (size_t)prec) : strlen(str);
    size_t pad = (size_t)width > slen ? (size_t)width - slen : 0;

    if (!(flags & FLAG_LEFT)) {
        emit_pad(s, ' ', pad);
    }
    for (size_t i = 0; i < slen; i++) {
        emit(s, str[i]);
    }
    if (flags & FLAG_LEFT) {
        emit_pad(s, ' ', pad);
    }
}

static unsigned long long magnitude(long long v) {
    if (v < 0) {
        return ~(unsigned long long)v + 1ULL;
    }
    return (unsigned long long)v;
}

static int format(struct sink *s, const char *fmt, va_list ap) {
    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            emit(s, *p);
            continue;
        }
        p++;

        unsigned flags = 0;
        for (bool scanning = true; scanning; ) {
            switch (*p) {
            case '-': flags |= FLAG_LEFT;  p++; break;
            case '0': flags |= FLAG_ZERO;  p++; break;
            case '+': flags |= FLAG_PLUS;  p++; break;
            case ' ': flags |= FLAG_SPACE; p++; break;
            case '#': flags |= FLAG_ALT;   p++; break;
            default:  scanning = false;         break;
            }
        }

        int width = 0;
        if (*p == '*') {
            p++;
            width = va_arg(ap, int);
            if (width < 0) {
                flags |= FLAG_LEFT;
                width = -width;
            }
        } else {
            while (isdigit((unsigned char)*p) && width < MAX_FIELD) {
                width = width * 10 + (*p++ - '0');
            }
        }
        if (width > MAX_FIELD) {
            width = MAX_FIELD;
        }

        int prec = -1;
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') {
                p++;
                prec = va_arg(ap, int);
                if (prec < 0) {
                    prec = -1;
                }
            } else {
                while (isdigit((unsigned char)*p) && prec < MAX_FIELD) {
                    prec = prec * 10 + (*p++ - '0');
                }
            }
        }
        if (prec > MAX_FIELD) {
            prec = MAX_FIELD;
        }

        enum length len = LEN_INT;
        switch (*p) {
        case 'h':
            p++;
            if (*p == 'h') {
                p++;
                len = LEN_CHAR;
            } else {
                len = LEN_SHORT;
            }
            break;
        case 'l':
            p++;
            if (*p == 'l') {
                p++;
                len = LEN_LLONG;
            } else {
                len = LEN_LONG;
            }
            break;
        case 'z': p++; len = LEN_SIZE;    break;
        case 't': p++; len = LEN_PTRDIFF; break;
        case 'j': p++; len = LEN_INTMAX;  break;
        default: break;
        }

        if (*p == '\0') {
            emit(s, '%');
            break;
        }

        switch (*p) {
        case 'd':
        case 'i': {
            long long v;

            switch (len) {
            case LEN_CHAR:    v = (signed char)va_arg(ap, int); break;
            case LEN_SHORT:   v = (short)va_arg(ap, int);       break;
            case LEN_LONG:    v = va_arg(ap, long);             break;
            case LEN_LLONG:   v = va_arg(ap, long long);        break;
            case LEN_SIZE:    v = va_arg(ap, long);             break;
            case LEN_PTRDIFF: v = va_arg(ap, ptrdiff_t);        break;
            case LEN_INTMAX:  v = va_arg(ap, intmax_t);         break;
            case LEN_INT:
            default:          v = va_arg(ap, int);              break;
            }

            char sign = '\0';
            if (v < 0) {
                sign = '-';
            } else if (flags & FLAG_PLUS) {
                sign = '+';
            } else if (flags & FLAG_SPACE) {
                sign = ' ';
            }

            emit_number(s, magnitude(v), 10, false, sign, NULL, flags, width, prec);
            break;
        }

        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            unsigned long long v;

            switch (len) {
            case LEN_CHAR:    v = (unsigned char)va_arg(ap, unsigned int);  break;
            case LEN_SHORT:   v = (unsigned short)va_arg(ap, unsigned int); break;
            case LEN_LONG:    v = va_arg(ap, unsigned long);                break;
            case LEN_LLONG:   v = va_arg(ap, unsigned long long);           break;
            case LEN_SIZE:    v = va_arg(ap, size_t);                       break;
            case LEN_PTRDIFF: v = (unsigned long long)va_arg(ap, ptrdiff_t);break;
            case LEN_INTMAX:  v = va_arg(ap, uintmax_t);                    break;
            case LEN_INT:
            default:          v = va_arg(ap, unsigned int);                 break;
            }

            unsigned base = 10;
            bool upper = false;
            const char *prefix = NULL;

            if (*p == 'o') {
                base = 8;
                if ((flags & FLAG_ALT) && v != 0) {
                    prefix = "0";
                }
            } else if (*p == 'x' || *p == 'X') {
                base = 16;
                upper = (*p == 'X');
                if ((flags & FLAG_ALT) && v != 0) {
                    prefix = upper ? "0X" : "0x";
                }
            }

            emit_number(s, v, base, upper, '\0', prefix, flags, width, prec);
            break;
        }

        case 'c': {
            char c = (char)va_arg(ap, int);
            size_t pad = width > 1 ? (size_t)width - 1 : 0;

            if (!(flags & FLAG_LEFT)) {
                emit_pad(s, ' ', pad);
            }
            emit(s, c);
            if (flags & FLAG_LEFT) {
                emit_pad(s, ' ', pad);
            }
            break;
        }

        case 's':
            emit_string(s, va_arg(ap, const char *), flags, width, prec);
            break;

        case 'p': {
            void *ptr = va_arg(ap, void *);

            if (ptr == NULL) {
                emit_string(s, "(nil)", flags, width, -1);
            } else {

                emit_number(s, (unsigned long long)(uintptr_t)ptr, 16, false,
                            '\0', "0x", flags, width, prec);
            }
            break;
        }

        case '%':
            emit(s, '%');
            break;

        default:

            emit(s, '%');
            emit(s, *p);
            break;
        }
    }

    return s->len > (size_t)INT32_MAX ? -1 : (int)s->len;
}

int vprintf(const char *fmt, va_list ap) {
    struct sink s = { .buf = NULL, .cap = 0, .len = 0, .to_console = true };

    return format(&s, fmt, ap);
}

int printf(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);

    return n;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct sink s = { .buf = buf, .cap = size, .len = 0, .to_console = false };
    int n = format(&s, fmt, ap);

    if (buf != NULL && size > 0) {
        buf[s.len < size ? s.len : size - 1] = '\0';
    }

    return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);

    return n;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsnprintf(buf, SIZE_MAX, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);

    return n;
}

int putchar(int c) {
    console_putchar((char)c);

    return c;
}

int puts(const char *s) {
    size_t n = strlen(s);

    console_write(s, n);
    console_putchar('\n');

    return (int)n + 1;
}
