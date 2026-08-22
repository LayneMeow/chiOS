#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int digit_value(char c) {
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

static bool parse_prefix(const char **s, int *base, bool *negative) {
    const char *p = *s;

    while (isspace((unsigned char)*p)) {
        p++;
    }

    *negative = false;
    if (*p == '+' || *p == '-') {
        *negative = (*p == '-');
        p++;
    }

    if (*base < 0 || *base == 1 || *base > 36) {
        return false;
    }

    if ((*base == 0 || *base == 16)
        && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit_value(p[2]) >= 0
        && digit_value(p[2]) < 16) {
        p += 2;
        *base = 16;
    } else if (*base == 0) {
        *base = (*p == '0') ? 8 : 10;
    }

    *s = p;

    return true;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    const char *p = nptr;
    bool negative;

    if (!parse_prefix(&p, &base, &negative)) {
        if (endptr != NULL) {
            *endptr = (char *)nptr;
        }
        return 0;
    }

    unsigned long limit = ULONG_MAX / (unsigned long)base;
    int cutlim = (int)(ULONG_MAX % (unsigned long)base);
    unsigned long acc = 0;
    int state = 0;

    for (;; p++) {
        int d = digit_value(*p);

        if (d < 0 || d >= base) {
            break;
        }
        if (state < 0 || acc > limit || (acc == limit && d > cutlim)) {
            state = -1;
        } else {
            state = 1;
            acc = acc * (unsigned long)base + (unsigned long)d;
        }
    }

    if (state < 0) {
        acc = ULONG_MAX;
    } else if (negative) {
        acc = -acc;
    }

    if (endptr != NULL) {
        *endptr = (char *)(state != 0 ? p : nptr);
    }

    return acc;
}

long strtol(const char *nptr, char **endptr, int base) {
    const char *p = nptr;
    bool negative;

    if (!parse_prefix(&p, &base, &negative)) {
        if (endptr != NULL) {
            *endptr = (char *)nptr;
        }
        return 0;
    }

    unsigned long cutoff = negative ? -(unsigned long)LONG_MIN : (unsigned long)LONG_MAX;
    unsigned long limit = cutoff / (unsigned long)base;
    int cutlim = (int)(cutoff % (unsigned long)base);
    unsigned long acc = 0;
    int state = 0;

    for (;; p++) {
        int d = digit_value(*p);

        if (d < 0 || d >= base) {
            break;
        }
        if (state < 0 || acc > limit || (acc == limit && d > cutlim)) {
            state = -1;
        } else {
            state = 1;
            acc = acc * (unsigned long)base + (unsigned long)d;
        }
    }

    if (state < 0) {
        acc = cutoff;
    } else if (negative) {
        acc = -acc;
    }

    if (endptr != NULL) {
        *endptr = (char *)(state != 0 ? p : nptr);
    }

    return (long)acc;
}

int atoi(const char *nptr) {
    return (int)strtol(nptr, NULL, 10);
}

long atol(const char *nptr) {
    return strtol(nptr, NULL, 10);
}

int abs(int j) {
    return j < 0 ? -j : j;
}

long labs(long j) {
    return j < 0 ? -j : j;
}

div_t div(int numer, int denom) {
    return (div_t){ .quot = numer / denom, .rem = numer % denom };
}

ldiv_t ldiv(long numer, long denom) {
    return (ldiv_t){ .quot = numer / denom, .rem = numer % denom };
}

static uint32_t rand_state = 1;

void srand(unsigned int seed) {
    rand_state = seed;
}

int rand(void) {

    rand_state = rand_state * 1103515245u + 12345u;

    return (int)((rand_state >> 16) & (uint32_t)RAND_MAX);
}

static void swap_bytes(uint8_t *a, uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}

#define QSORT_THRESHOLD 8

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    uint8_t *a = base;

    if (size == 0) {
        return;
    }

    while (nmemb > QSORT_THRESHOLD) {

        uint8_t *mid = a + (nmemb / 2) * size;
        uint8_t *hi = a + (nmemb - 1) * size;

        if (compar(mid, a) < 0) {
            swap_bytes(mid, a, size);
        }
        if (compar(hi, a) < 0) {
            swap_bytes(hi, a, size);
        }
        if (compar(hi, mid) < 0) {
            swap_bytes(hi, mid, size);
        }
        swap_bytes(a, mid, size);

        size_t split = 0;
        for (size_t i = 1; i < nmemb; i++) {
            if (compar(a + i * size, a) < 0) {
                split++;
                swap_bytes(a + i * size, a + split * size, size);
            }
        }
        swap_bytes(a, a + split * size, size);

        size_t left = split;
        size_t right = nmemb - split - 1;

        if (left < right) {
            qsort(a, left, size, compar);
            a += (split + 1) * size;
            nmemb = right;
        } else {
            qsort(a + (split + 1) * size, right, size, compar);
            nmemb = left;
        }
    }

    for (size_t i = 1; i < nmemb; i++) {
        for (uint8_t *p = a + i * size; p > a && compar(p - size, p) > 0; p -= size) {
            swap_bytes(p - size, p, size);
        }
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
    const uint8_t *a = base;

    while (nmemb > 0) {
        size_t mid = nmemb / 2;
        const uint8_t *p = a + mid * size;
        int cmp = compar(key, p);

        if (cmp == 0) {
            return (void *)p;
        }
        if (cmp > 0) {
            a = p + size;
            nmemb -= mid + 1;
        } else {
            nmemb = mid;
        }
    }

    return NULL;
}

_Noreturn void abort(void) {
    printf("\n*** abort() called, halting ***\n");

    for (;;) {
        asm volatile ("cli; hlt");
    }
}
