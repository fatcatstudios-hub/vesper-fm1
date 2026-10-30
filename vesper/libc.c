/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* VESPER 2 on the FM-1: string routines beyond felucca-bsp's chip/libc.c (memset, memcpy, memcmp). */
#ifndef PLAT_HOST
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *a = d; const unsigned char *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
size_t strlen(const char *s) { const char *p = s; while (*p) ++p; return (size_t)(p - s); }
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) ++a, ++b;
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; --n, ++a, ++b) if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b;
    return 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strncpy(char *d, const char *s, size_t n)
{
    char *r = d;
    for (; n && *s; --n) *d++ = *s++;
    while (n--) *d++ = 0;
    return r;
}
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strchr(const char *s, int c)
{
    for (;; ++s) { if (*s == (char)c) return (char *)s; if (!*s) return 0; }
}
int abs(int v) { return v < 0 ? -v : v; }
#endif
/* malloc: a small first-fit heap in the pool (the 1.x engine allocates each track's sound once, and
 * project transfers and tuning saves borrow a few KiB for a moment). Main loop only. */
#include <stddef.h>
#include <stdint.h>
#include "plat.h"
#include <string.h>
#ifndef VESPER_HEAP_KB
#define VESPER_HEAP_KB 96
#endif
typedef struct blk { uint32_t size; struct blk *next; } blk;   /* size in bytes incl. header; next: free list */
static uint32_t heap[VESPER_HEAP_KB * 256] PLAT_POOL;
static blk *free_list;
static int heap_ready;
uint32_t vesper_heap_used, vesper_heap_peak;
static void heap_init(void)
{
    free_list = (blk *)heap;
    free_list->size = sizeof heap;
    free_list->next = 0;
    heap_ready = 1;
}
void *malloc(size_t n)
{
    if (!heap_ready) heap_init();
    if (!n || n > sizeof heap) return 0;
    uint32_t need = (uint32_t)((n + sizeof(blk) + 7u) & ~7u);
    for (blk **pp = &free_list; *pp; pp = &(*pp)->next) {
        blk *b = *pp;
        if (b->size < need) continue;
        if (b->size - need >= 32u) {               /* split: the rest stays free */
            blk *r = (blk *)((uint8_t *)b + need);
            r->size = b->size - need; r->next = b->next;
            *pp = r; b->size = need;
        } else
            *pp = b->next;
        b->next = (blk *)0x5E5E5E5Eu;              /* in use */
        vesper_heap_used += b->size;
        if (vesper_heap_used > vesper_heap_peak) vesper_heap_peak = vesper_heap_used;
        return b + 1;
    }
    return 0;
}
void *calloc(size_t a, size_t b)
{
    size_t n = a * b;
    if (b && n / b != a) return 0;
    void *p = malloc(n);
    if (p) memset(p, 0, (unsigned)n);
    return p;
}
void free(void *p)
{
    if (!p) return;
    blk *b = (blk *)p - 1;
    if ((uint32_t *)b < heap || (uint32_t *)b >= heap + sizeof heap / 4 || b->next != (blk *)0x5E5E5E5Eu) return;
    vesper_heap_used -= b->size;
    blk **pp = &free_list;                          /* address order, so neighbours merge */
    while (*pp && *pp < b) pp = &(*pp)->next;
    b->next = *pp; *pp = b;
    if (b->next && (uint8_t *)b + b->size == (uint8_t *)b->next) { b->size += b->next->size; b->next = b->next->next; }
    if (pp != &free_list) {
        blk *prev = (blk *)((uint8_t *)pp - offsetof(blk, next));
        if ((uint8_t *)prev + prev->size == (uint8_t *)b) { prev->size += b->size; prev->next = b->next; }
    }
}
#ifdef PLAT_HOST
#include <stdio.h>
__attribute__((destructor)) static void heap_report(void)
{ fprintf(stderr, "vesper heap: peak %u B of %u, in use %u\n", (unsigned)vesper_heap_peak, (unsigned)sizeof heap, (unsigned)vesper_heap_used); }
#endif
#ifndef PLAT_HOST
void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (; n; --n, ++p) if (*p == (unsigned char)c) return (void *)p;
    return 0;
}
#endif
#ifndef PLAT_HOST
char *strncat(char *d, const char *s, size_t n)
{
    char *e = d + strlen(d);
    for (; n && *s; --n) *e++ = *s++;
    *e = 0;
    return d;
}
#endif
#ifndef PLAT_HOST
/* 64-bit division (the compiler's helpers; VESPER divides 64-bit only at control rate) */
static uint64_t udivmod64(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0, r = 0;
    if (!d) { if (rem) *rem = 0; return ~(uint64_t)0; }
    if (!(n >> 32) && !(d >> 32)) { uint32_t a = (uint32_t)n, b = (uint32_t)d; if (rem) *rem = a % b; return a / b; }
    for (int i = 63; i >= 0; --i) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) { r -= d; q |= (uint64_t)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}
uint64_t __udivdi3(uint64_t n, uint64_t d) { return udivmod64(n, d, 0); }
uint64_t __umoddi3(uint64_t n, uint64_t d) { uint64_t r; udivmod64(n, d, &r); return r; }
int64_t __divdi3(int64_t n, int64_t d)
{
    int neg = (n < 0) != (d < 0);
    uint64_t q = udivmod64(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, 0);
    return neg ? -(int64_t)q : (int64_t)q;
}
int64_t __moddi3(int64_t n, int64_t d)
{
    uint64_t r;
    udivmod64(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, &r);
    return n < 0 ? -(int64_t)r : (int64_t)r;
}
#endif

/* 64-bit multiplies, out of line (tools/mul64_outline.py moves every one here: felucca-bsp docs/CPU.md) */
__attribute__((noinline)) int64_t vesper_smul32(int32_t a, int32_t b) { return (int64_t)a * b; }
__attribute__((noinline)) uint64_t vesper_umul32(uint32_t a, uint32_t b) { return (uint64_t)a * b; }
__attribute__((noinline)) uint64_t vesper_mul64(uint64_t a, uint64_t b) { return a * b; }
