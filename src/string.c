#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("no-tree-loop-distribute-patterns")
#endif

void *memcpy(void *restrict dest, const void *restrict src, size_t n) {
    uint8_t *restrict pdest = dest;
    const uint8_t *restrict psrc = src;

    for (size_t i = 0; i < n; i++) {
        pdest[i] = psrc[i];
    }

    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = s;

    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)c;
    }

    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *pdest = dest;
    const uint8_t *psrc = src;

    if ((uintptr_t)src > (uintptr_t)dest) {
        for (size_t i = 0; i < n; i++) {
            pdest[i] = psrc[i];
        }
    } else if ((uintptr_t)src < (uintptr_t)dest) {
        for (size_t i = n; i > 0; i--) {
            pdest[i-1] = psrc[i-1];
        }
    }

    return dest;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = s1;
    const uint8_t *p2 = s2;

    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] < p2[i] ? -1 : 1;
        }
    }

    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = s;
    uint8_t needle = (uint8_t)c;

    for (size_t i = 0; i < n; i++) {
        if (p[i] == needle) {
            return (void *)(p + i);
        }
    }

    return NULL;
}

size_t strlen(const char *s) {
    size_t n = 0;

    while (s[n] != '\0') {
        n++;
    }

    return n;
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t n = 0;

    while (n < maxlen && s[n] != '\0') {
        n++;
    }

    return n;
}

char *strcpy(char *restrict dest, const char *restrict src) {
    char *d = dest;

    while ((*d++ = *src++) != '\0') {

    }

    return dest;
}

char *strncpy(char *restrict dest, const char *restrict src, size_t n) {
    size_t i = 0;

    for (; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }

    for (; i < n; i++) {
        dest[i] = '\0';
    }

    return dest;
}

char *strcat(char *restrict dest, const char *restrict src) {
    char *d = dest + strlen(dest);

    while ((*d++ = *src++) != '\0') {

    }

    return dest;
}

char *strncat(char *restrict dest, const char *restrict src, size_t n) {
    char *d = dest + strlen(dest);
    size_t i = 0;

    for (; i < n && src[i] != '\0'; i++) {
        d[i] = src[i];
    }
    d[i] = '\0';

    return dest;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 != '\0' && *s1 == *s2) {
        s1++;
        s2++;
    }

    return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i] || s1[i] == '\0') {
            return (int)(unsigned char)s1[i] - (int)(unsigned char)s2[i];
        }
    }

    return 0;
}

char *strchr(const char *s, int c) {
    char needle = (char)c;

    for (;; s++) {
        if (*s == needle) {
            return (char *)s;
        }
        if (*s == '\0') {
            return NULL;
        }
    }
}

char *strrchr(const char *s, int c) {
    char needle = (char)c;
    const char *found = NULL;

    for (;; s++) {
        if (*s == needle) {
            found = s;
        }
        if (*s == '\0') {
            return (char *)found;
        }
    }
}

char *strstr(const char *haystack, const char *needle) {
    if (*needle == '\0') {
        return (char *)haystack;
    }

    for (; *haystack != '\0'; haystack++) {
        const char *h = haystack;
        const char *n = needle;

        while (*n != '\0' && *h == *n) {
            h++;
            n++;
        }
        if (*n == '\0') {
            return (char *)haystack;
        }
    }

    return NULL;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;

    while (s[n] != '\0' && strchr(accept, s[n]) != NULL) {
        n++;
    }

    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;

    while (s[n] != '\0' && strchr(reject, s[n]) == NULL) {
        n++;
    }

    return n;
}

char *strdup(const char *s) {
    size_t size = strlen(s) + 1;
    char *copy = malloc(size);

    if (copy == NULL) {
        return NULL;
    }

    return memcpy(copy, s, size);
}
