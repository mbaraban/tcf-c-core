/*******************************************************************************
 * Copyright (c) 2007, 2014 Wind River Systems, Inc. and others.
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
 *     Wind River Systems - initial API and implementation
 *******************************************************************************/

/*
 * This file contains "define" statements that control agent configuration.
 * SERVICE_* definitions control which service implementations are included into the agent.
 */

#ifndef D_config
#define D_config

#include <tcf/framework/mdep.h>

#if !defined(SERVICE_Locator)
#define SERVICE_Locator         0
#endif
#if !defined(SERVICE_Registers)
#define SERVICE_Registers       0
#endif
#if !defined(SERVICE_Memory)
#define SERVICE_Memory          0
#endif
#if !defined(SERVICE_LineNumbers)
#define SERVICE_LineNumbers     1
#endif
#if !defined(SERVICE_Symbols)
#define SERVICE_Symbols         1
#endif
#if !defined(SERVICE_Expressions)
#define SERVICE_Expressions     1
#endif
#if !defined(SERVICE_MemoryMap)
#define SERVICE_MemoryMap       1
#endif
#if !defined(SERVICE_StackTrace)
#define SERVICE_StackTrace      1
#endif

#define SERVICE_RunControl      0
#define SERVICE_Breakpoints     0
#define SERVICE_PathMap         0
#define SERVICE_Processes       0
#define SERVICE_Terminals       0
#define SERVICE_FileSystem      0
#define SERVICE_SysMonitor      0
#define SERVICE_Streams         0
#define SERVICE_DPrintf         0
#define SERVICE_ContextQuery    0
#define SERVICE_Disassembly     0
#define SERVICE_Profiler        0

#define ENABLE_ZeroCopy         0
#define ENABLE_Trace            1
#define ENABLE_Discovery        0
#define ENABLE_ContextMux       0
#define ENABLE_ContextProxy     1
#define ENABLE_SymbolsProxy     0
#define ENABLE_LineNumbersProxy 0
#define ENABLE_Symbols          1
#define ENABLE_LineNumbers      1
#define ENABLE_DebugContext     1
#define ENABLE_ELF              1
#define ENABLE_PE               0
#define ENABLE_SSL              0
#define ENABLE_Unix_Domain      0
#define ENABLE_AIO              0
#define ENABLE_RCBP_TEST        0
#define ENABLE_Splice           0
#define ENABLE_Plugins          0
#define ENABLE_Cmdline          0
#define ENABLE_STREAM_MACROS    0

#define ENABLE_ContextMemoryProperties          0
#define ENABLE_ContextExtraProperties           0
#define ENABLE_ContextStateProperties           0
#define ENABLE_ContextBreakpointCapabilities    0
#define ENABLE_ExtendedMemoryErrorReports       0
#define ENABLE_ExtendedBreakpointStatus         0
#define ENABLE_MemoryAccessModes                0
#define ENABLE_ExternalStackcrawl               0
#define ENABLE_SymbolsMux                       0
#define ENABLE_LineNumbersMux                   0
#define ENABLE_ContextISA                       0
#define ENABLE_ProfilerSST                      0
#define ENABLE_ContextIdHashTable               0

#endif /* D_config */
