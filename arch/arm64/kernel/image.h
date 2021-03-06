/*
 * Linker script macros to generate Image header fields.
 *
 * Copyright (C) 2014 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __ASM_IMAGE_H
#define __ASM_IMAGE_H

#ifndef LINKER_SCRIPT
#error This file should only be included in vmlinux.lds.S
#endif

/*
 * There aren't any ELF relocations we can use to endian-swap values known only
 * at link time (e.g. the subtraction of two symbol addresses), so we must get
 * the linker to endian-swap certain values before emitting them.
 */
#ifdef CONFIG_CPU_BIG_ENDIAN
#define DATA_LE64(data)					\
	((((data) & 0x00000000000000ff) << 56) |	\
	 (((data) & 0x000000000000ff00) << 40) |	\
	 (((data) & 0x0000000000ff0000) << 24) |	\
	 (((data) & 0x00000000ff000000) << 8)  |	\
	 (((data) & 0x000000ff00000000) >> 8)  |	\
	 (((data) & 0x0000ff0000000000) >> 24) |	\
	 (((data) & 0x00ff000000000000) >> 40) |	\
	 (((data) & 0xff00000000000000) >> 56))
#else
#define DATA_LE64(data) ((data) & 0xffffffffffffffff)
#endif

#ifdef CONFIG_CPU_BIG_ENDIAN
#define __HEAD_FLAG_BE	1
#else
#define __HEAD_FLAG_BE	0
#endif

#define __HEAD_FLAGS	(__HEAD_FLAG_BE << 0)

/*
 * These will output as part of the Image header, which should be little-endian
 * regardless of the endianness of the kernel. While constant values could be
 * endian swapped in head.S, all are done here for consistency.
 */
#define HEAD_SYMBOLS						\
	_kernel_size_le		= DATA_LE64(_end - _text);	\
	_kernel_offset_le	= DATA_LE64(TEXT_OFFSET);	\
	_kernel_flags_le	= DATA_LE64(__HEAD_FLAGS);

#ifdef CONFIG_EFI

/*
 * Prevent the symbol aliases below from being emitted into the kallsyms
 * table, by forcing them to be absolute symbols (which are conveniently
 * ignored by scripts/kallsyms) rather than section relative symbols.
 * The distinction is only relevant for partial linking, and only for symbols
 * that are defined within a section declaration (which is not the case for
 * the definitions below) so the resulting values will be identical.
 */
#define KALLSYMS_HIDE(sym)	ABSOLUTE(sym)

/*
 * The EFI stub has its own symbol namespace prefixed by __efistub_, to
 * isolate it from the kernel proper. The following symbols are legally
 * accessed by the stub, so provide some aliases to make them accessible.
 * Only include data symbols here, or text symbols of functions that are
 * guaranteed to be safe when executed at another offset than they were
 * linked at. The routines below are all implemented in assembler in a
 * position independent manner
 */
__efistub_memcmp		= KALLSYMS_HIDE(__pi_memcmp);
__efistub_memchr		= KALLSYMS_HIDE(__pi_memchr);
__efistub_memcpy		= KALLSYMS_HIDE(__pi_memcpy);
__efistub_memmove		= KALLSYMS_HIDE(__pi_memmove);
__efistub_memset		= KALLSYMS_HIDE(__pi_memset);
__efistub_strlen		= KALLSYMS_HIDE(__pi_strlen);
__efistub_strcmp		= KALLSYMS_HIDE(__pi_strcmp);
__efistub_strncmp		= KALLSYMS_HIDE(__pi_strncmp);
__efistub___flush_dcache_area	= KALLSYMS_HIDE(__pi___flush_dcache_area);

#ifdef CONFIG_KASAN
__efistub___memcpy		= KALLSYMS_HIDE(__pi_memcpy);
__efistub___memmove		= KALLSYMS_HIDE(__pi_memmove);
__efistub___memset		= KALLSYMS_HIDE(__pi_memset);
#endif

__efistub__text			= KALLSYMS_HIDE(_text);
__efistub__end			= KALLSYMS_HIDE(_end);
__efistub__edata		= KALLSYMS_HIDE(_edata);

#endif

#endif /* __ASM_IMAGE_H */
