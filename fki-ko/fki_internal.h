/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared between fki's own files (not part of the interface in fki.h). */
#ifndef FKI_INTERNAL_H
#define FKI_INTERNAL_H
/*
 * Run fn(arg) where Linux may run it (a Linux-compliant stack, Linux's address space), keeping the caller's exception mask.
 * size bytes at arg are copied to a per-CPU scratch area first (fn gets that copy) and back after: arg may be on the
 * foreign kernel's stack, which Linux's address space does not map. At most 128 bytes.
 */
void fki_linux_runs_ext(void (*fn)(void *), void *arg, unsigned long size);
int fki_hostcall_init(void);
void fki_hostcall_exit(void);
#endif
