/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* VESPER 2 on the FM-1: the few string routines VESPER uses (felucca-bsp's chip/libc.c, vesper/libc.c) */
#pragma once
#include <stddef.h>
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, unsigned n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
char *strcat(char *d, const char *s);
char *strncat(char *d, const char *s, size_t n);
char *strchr(const char *s, int c);
void *memchr(const void *s, int c, size_t n);
