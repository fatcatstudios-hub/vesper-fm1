/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* VESPER 2: included first in every VESPER unit (fm1.toml o2_include) */
#pragma once
#include "build_flags.h"     /* VESPER_TEST (tools/build.sh) */
#if defined(VESPER_FMB) && !defined(PLAT_HOST)
#define VESPER_BIG __attribute__((section(".pool")))   /* big zero-initialised buffers: the pool, beside the 96 KiB RAM */
#else
#define VESPER_BIG
#endif
/* the heap: VESPER's own, in the pool (vesper/libc.c), on the FM-1 and in the simulator alike */
#define malloc vesper_malloc
#define calloc vesper_calloc
#define free vesper_free
