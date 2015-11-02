/*******************************************************************************
 * Copyright (c) 2013 Stanislav Yakovlev.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 * The Eclipse Public License is available at
 * http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 * You may elect to redistribute this code under either of these licenses.
 *
 * Contributors:
 *     Stanislav Yakovlev - initial API and implementation
 *******************************************************************************/

/*
 * This module provides CPU specific definitions for PowerPC.
 */

#if defined(__powerpc__) || defined(__ppc)

#include <tcf/regset.h>

extern RegisterDefinition * regs_index;
extern unsigned char BREAK_INST[4];

#if !defined(ENABLE_ini_cpudefs_mdep)
#define ENABLE_ini_cpudefs_mdep 1
extern void ini_cpudefs_mdep(void);
#endif

#if !defined(INCLUDE_NATIVE_REGSET_H)

#define REG_SP user.regs.gpr[1]
#define REG_BP user.regs.gpr[31]
#define REG_IP user.regs.nip

#else

#define REG_SP	REG_SET_GR(1)
#define REG_BP	REG_SET_GR(31)
#define REG_IP	pc

#endif

#if !defined(ENABLE_add_cpudefs_disassembler)
#define ENABLE_add_cpudefs_disassembler 1
extern void add_cpudefs_disassembler(Context * cpu_ctx);
#endif

#endif
