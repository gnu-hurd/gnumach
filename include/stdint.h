/*
 * Copyright (C) 2016 Free Software Foundation, Inc.
 *
 * This file is part of GNU Mach.
 *
 * GNU Mach is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef _STDINT_H_
#define _STDINT_H_

/*
 * These types are _exactly_ as wide as indicated in their names.
 */

#include <mach/machine/stdint.h>

typedef __mach_int8_t	int8_t;
typedef __mach_int16_t	int16_t;
typedef __mach_int32_t	int32_t;
typedef __mach_int64_t	int64_t;

typedef __mach_uint8_t	uint8_t;
typedef __mach_uint16_t	uint16_t;
typedef __mach_uint32_t	uint32_t;
typedef __mach_uint64_t	uint64_t;

/* Types for `void *' pointers.  */
typedef __mach_intptr_t		intptr_t;
typedef __mach_uintptr_t	uintptr_t;

#endif /* _STDINT_H_ */
