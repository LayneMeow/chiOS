#include <console.h>
#include <ctype.h>
#include <keyboard.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LINE_MAX 256

#define CTRL(c) ((c) & 0x1F)

static char line[LINE_MAX];
static size_t line_len;
static size_t line_pos;

static bool line_refill(void) {
    line_len = 0;
    line_pos = 0;

    for (;;) {
        int c = keyboard_getchar();

        switch (c) {
        case '\n':
        case '\r':
            line[line_len++] = '\n';
            putchar('\n');
            return true;

        case '\b':
        case 0x7F:
            if (line_len > 0) {
                line_len--;
                putchar('\b');
            }
            break;

        case CTRL('U'):
            while (line_len > 0) {
                line_len--;
                putchar('\b');
            }
            break;

        case CTRL('D'):
            if (line_len == 0) {
                putchar('\n');
                return false;
            }
            break;

        case CTRL('C'):
            console_write("^C", 2);
            putchar('\n');
            line_len = 0;
            return false;

        default:
            if (c == '\t') {
                c = ' ';
            }
            if (c < 0x20) {
                break;
            }
            if (line_len + 1 < LINE_MAX) {
                line[line_len++] = (char)c;
                putchar(c);
            }
            break;
        }
    }
}

int getchar(void) {
    if (line_pos >= line_len && !line_refill()) {
        return EOF;
    }

    return (unsigned char)line[line_pos++];
}

char *gets_s(char *s, size_t size) {
    if (s == NULL || size == 0) {
        return NULL;
    }

    size_t i = 0;
    int c;

    while ((c = getchar()) != EOF && c != '\n') {
        if (i + 1 < size) {
            s[i++] = (char)c;
        }
    }

    if (c == EOF && i == 0) {
        s[0] = '\0';
        return NULL;
    }

    s[i] = '\0';

    return s;
}

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

#define PUSHBACK_MAX 4

struct source {
    const char *str;
    size_t pos;
    int pushback[PUSHBACK_MAX];
    size_t npushback;
    size_t consumed;
    bool eof;
};

static int src_getc(struct source *s) {
    int c;

    if (s->npushback > 0) {
        c = s->pushback[--s->npushback];
    } else if (s->str != NULL) {
        c = (unsigned char)s->str[s->pos];
        if (c == '\0') {
            s->eof = true;
            return EOF;
        }
        s->pos++;
    } else {
        c = getchar();
        if (c == EOF) {
            s->eof = true;
            return EOF;
        }
    }

    s->consumed++;

    return c;
}

static void src_ungetc(struct source *s, int c) {
    if (c == EOF) {
        return;
    }

    if (s->str == NULL && line_pos > 0 && line[line_pos - 1] == (char)c) {
        line_pos--;
        s->consumed--;
        return;
    }

    if (s->npushback >= PUSHBACK_MAX) {
        return;
    }

    s->pushback[s->npushback++] = c;
    s->consumed--;
}

static int take(struct source *s, int width, int *used) {
    if (width > 0 && *used >= width) {
        return EOF;
    }

    int c = src_getc(s);

    if (c != EOF) {
        (*used)++;
    }

    return c;
}

static void give_back(struct source *s, int c, int *used) {
    if (c == EOF) {
        return;
    }

    src_ungetc(s, c);
    (*used)--;
}

static void skip_space(struct source *s) {
    int c;

    while ((c = src_getc(s)) != EOF && isspace(c)) {
        continue;
    }

    src_ungetc(s, c);
}

static int digit_value(int c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }

    return -1;
}

static bool scan_integer(struct source *s, int base, int width,
                         unsigned long long *out, bool *negative) {
    unsigned long long value = 0;
    int used = 0;
    int ndigits = 0;
    int sign = 0;
    int c = take(s, width, &used);

    if (c == '+' || c == '-') {
        sign = c;
        c = take(s, width, &used);
    }

    if (c == '0' && (base == 0 || base == 16)) {
        ndigits = 1;

        int next = take(s, width, &used);

        if (next == 'x' || next == 'X') {
            int first = take(s, width, &used);

            if (digit_value(first) >= 0 && digit_value(first) < 16) {
                base = 16;
                c = first;
            } else {

                give_back(s, first, &used);
                give_back(s, next, &used);
                c = EOF;
            }
        } else {
            if (base == 0) {
                base = 8;
            }
            c = next;
        }
    } else if (base == 0) {
        base = 10;
    }

    while (c != EOF) {
        int d = digit_value(c);

        if (d < 0 || d >= base) {
            give_back(s, c, &used);
            break;
        }

        value = value * (unsigned long long)base + (unsigned long long)d;
        ndigits++;
        c = take(s, width, &used);
    }

    if (ndigits == 0) {
        give_back(s, sign, &used);
        return false;
    }

    *out = value;
    *negative = (sign == '-');

    return true;
}

static void store_signed(void *dest, enum length len, long long value) {
    switch (len) {
    case LEN_CHAR:    *(signed char *)dest = (signed char)value; break;
    case LEN_SHORT:   *(short *)dest      = (short)value;        break;
    case LEN_LONG:    *(long *)dest       = (long)value;         break;
    case LEN_LLONG:   *(long long *)dest  = value;               break;
    case LEN_SIZE:    *(size_t *)dest     = (size_t)value;       break;
    case LEN_PTRDIFF: *(ptrdiff_t *)dest  = (ptrdiff_t)value;    break;
    case LEN_INTMAX:  *(intmax_t *)dest   = (intmax_t)value;     break;
    case LEN_INT:
    default:          *(int *)dest        = (int)value;          break;
    }
}

static void store_unsigned(void *dest, enum length len, unsigned long long value) {
    switch (len) {
    case LEN_CHAR:    *(unsigned char *)dest      = (unsigned char)value;  break;
    case LEN_SHORT:   *(unsigned short *)dest     = (unsigned short)value; break;
    case LEN_LONG:    *(unsigned long *)dest      = (unsigned long)value;  break;
    case LEN_LLONG:   *(unsigned long long *)dest = value;                 break;
    case LEN_SIZE:    *(size_t *)dest             = (size_t)value;         break;
    case LEN_PTRDIFF: *(ptrdiff_t *)dest          = (ptrdiff_t)value;      break;
    case LEN_INTMAX:  *(uintmax_t *)dest          = (uintmax_t)value;      break;
    case LEN_INT:
    default:          *(unsigned int *)dest       = (unsigned int)value;   break;
    }
}

static const char *parse_scanset(const char *p, bool set[256]) {
    bool negate = false;

    memset(set, 0, 256 * sizeof set[0]);

    if (*p == '^') {
        negate = true;
        p++;
    }
    if (*p == ']') {
        set[(unsigned char)']'] = true;
        p++;
    }

    while (*p != '\0' && *p != ']') {
        unsigned char lo = (unsigned char)*p++;

        if (*p == '-' && p[1] != ']' && p[1] != '\0') {
            unsigned char hi = (unsigned char)p[1];

            p += 2;
            if (lo <= hi) {
                for (unsigned c = lo; c <= hi; c++) {
                    set[c] = true;
                }
            } else {
                set[lo] = set[hi] = set[(unsigned char)'-'] = true;
            }
        } else {
            set[lo] = true;
        }
    }

    if (negate) {
        for (size_t c = 0; c < 256; c++) {
            set[c] = !set[c];
        }
    }

    return p;
}

static int scan(struct source *s, const char *fmt, va_list ap) {
    int assigned = 0;
    bool input_failure = false;

    for (const char *p = fmt; *p != '\0'; p++) {

        if (isspace((unsigned char)*p)) {
            skip_space(s);
            continue;
        }

        if (*p != '%') {
            int c = src_getc(s);

            if (c == EOF) {
                input_failure = true;
                break;
            }
            if (c != *p) {
                src_ungetc(s, c);
                goto done;
            }
            continue;
        }

        p++;

        bool suppress = false;
        if (*p == '*') {
            suppress = true;
            p++;
        }

        int width = 0;
        while (isdigit((unsigned char)*p)) {
            width = width * 10 + (*p++ - '0');
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
            break;
        }

        switch (*p) {
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            int base = 10;

            switch (*p) {
            case 'i': base = 0;  break;
            case 'o': base = 8;  break;
            case 'x':
            case 'X': base = 16; break;
            default:  base = 10; break;
            }

            skip_space(s);

            unsigned long long value;
            bool negative;

            if (!scan_integer(s, base, width, &value, &negative)) {
                input_failure = s->eof;
                goto done;
            }

            if (!suppress) {
                void *dest = va_arg(ap, void *);

                if (*p == 'd' || *p == 'i') {
                    long long signed_value = negative
                        ? -(long long)value : (long long)value;

                    store_signed(dest, len, signed_value);
                } else {
                    store_unsigned(dest, len, negative ? 0ULL - value : value);
                }
                assigned++;
            }
            break;
        }

        case 'p': {
            skip_space(s);

            unsigned long long value = 0;
            bool negative = false;
            int c = src_getc(s);

            if (c == '(') {
                static const char nil[] = "nil)";

                for (const char *q = nil; *q != '\0'; q++) {
                    if (src_getc(s) != *q) {
                        input_failure = s->eof;
                        goto done;
                    }
                }
            } else {
                src_ungetc(s, c);
                if (!scan_integer(s, 16, width, &value, &negative)) {
                    input_failure = s->eof;
                    goto done;
                }
            }

            if (!suppress) {
                *(void **)va_arg(ap, void **) = (void *)(uintptr_t)value;
                assigned++;
            }
            break;
        }

        case 'c': {
            int count = width > 0 ? width : 1;
            char *dest = suppress ? NULL : va_arg(ap, char *);

            for (int i = 0; i < count; i++) {
                int c = src_getc(s);

                if (c == EOF) {
                    input_failure = (i == 0);
                    goto done;
                }
                if (dest != NULL) {
                    dest[i] = (char)c;
                }
            }

            if (!suppress) {
                assigned++;
            }
            break;
        }

        case 's': {
            char *dest = suppress ? NULL : va_arg(ap, char *);
            int count = 0;

            skip_space(s);

            for (;;) {
                if (width > 0 && count >= width) {
                    break;
                }

                int c = src_getc(s);

                if (c == EOF) {
                    input_failure = (count == 0);
                    break;
                }
                if (isspace(c)) {
                    src_ungetc(s, c);
                    break;
                }
                if (dest != NULL) {
                    dest[count] = (char)c;
                }
                count++;
            }

            if (count == 0) {
                goto done;
            }
            if (dest != NULL) {
                dest[count] = '\0';
                assigned++;
            }
            break;
        }

        case '[': {
            bool set[256];
            char *dest = suppress ? NULL : va_arg(ap, char *);
            int count = 0;

            p = parse_scanset(p + 1, set);

            if (*p == '\0') {
                goto done;
            }

            for (;;) {
                if (width > 0 && count >= width) {
                    break;
                }

                int c = src_getc(s);

                if (c == EOF) {
                    input_failure = (count == 0);
                    break;
                }
                if (!set[(unsigned char)c]) {
                    src_ungetc(s, c);
                    break;
                }
                if (dest != NULL) {
                    dest[count] = (char)c;
                }
                count++;
            }

            if (count == 0) {
                goto done;
            }
            if (dest != NULL) {
                dest[count] = '\0';
                assigned++;
            }
            break;
        }

        case 'n':

            if (!suppress) {
                store_signed(va_arg(ap, void *), len, (long long)s->consumed);
            }
            break;

        case '%': {
            int c = src_getc(s);

            if (c == EOF) {
                input_failure = true;
                goto done;
            }
            if (c != '%') {
                src_ungetc(s, c);
                goto done;
            }
            break;
        }

        default:
            goto done;
        }
    }

done:
    return (assigned == 0 && input_failure) ? EOF : assigned;
}

int vsscanf(const char *str, const char *fmt, va_list ap) {
    struct source s = { .str = str };

    if (str == NULL) {
        return EOF;
    }

    return scan(&s, fmt, ap);
}

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    int n = vsscanf(str, fmt, ap);
    va_end(ap);

    return n;
}

int vscanf(const char *fmt, va_list ap) {
    struct source s = { .str = NULL };

    return scan(&s, fmt, ap);
}

int scanf(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    int n = vscanf(fmt, ap);
    va_end(ap);

    return n;
}
