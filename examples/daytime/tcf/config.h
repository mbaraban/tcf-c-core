/*******************************************************************************
 * Copyright (c) 2008, 2011 Wind River Systems, Inc. and others.
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
 *
 * This is example agent configuration. It includes only few standard services,
 * and one example service: Day Time.
 */

#ifndef D_config
#define D_config

#include <tcf/framework/mdep.h>

#if defined(WIN32) || defined(__CYGWIN__)
#  define TARGET_UNIX           0
#elif defined(_WRS_KERNEL)
#  define TARGET_UNIX           0
#else
#  define TARGET_UNIX           1
#endif

#if !defined(SERVICE_Locator)
#  define SERVICE_Locator       1
#endif

#if !defined(SERVICE_FileSystem)
#  define SERVICE_FileSystem    1
#endif

#if !defined(SERVICE_SysMonitor)
#  define SERVICE_SysMonitor    TARGET_UNIX
#endif

#if !defined(SERVICE_Processes)
#  define SERVICE_Processes     0
#endif

#if !defined(SERVICE_Terminals)
#  define SERVICE_Terminals     0
#endif

#if !defined(SERVICE_ContextQuery)
#  define SERVICE_ContextQuery  0
#endif

#if !defined(SERVICE_RunControl)
#  define SERVICE_RunControl    0
#endif

#if !defined(SERVICE_Breakpoints)
#  define SERVICE_Breakpoints   0
#endif

#if !defined(SERVICE_Memory)
#  define SERVICE_Memory        0
#endif

#if !defined(SERVICE_MemoryMap)
#  define SERVICE_MemoryMap     0
#endif

#if !defined(SERVICE_Registers)
#  define SERVICE_Registers     0
#endif

#if !defined(SERVICE_StackTrace)
#  define SERVICE_StackTrace    0
#endif

#if !defined(SERVICE_Symbols)
#  define SERVICE_Symbols       0
#endif

#if !defined(SERVICE_Expressions)
#  define SERVICE_Expressions   0
#endif

#if !defined(SERVICE_PathMap)
#  define SERVICE_PathMap       0
#endif

#if !defined(SERVICE_DPrintf)
#  define SERVICE_DPrintf       0
#endif

#if !defined(SERVICE_LineNumbers)
#  define SERVICE_LineNumbers   0
#endif

#if !defined(SERVICE_Streams)
#  define SERVICE_Streams       0
#endif

#if !defined(SERVICE_Disassembly)
#  define SERVICE_Disassembly   0
#endif

#if !defined(SERVICE_Profiler)
#  define SERVICE_Profiler      0
#endif

#if !defined(ENABLE_Trace)
#  define ENABLE_Trace          1
#endif

#if !defined(ENABLE_Discovery)
#  define ENABLE_Discovery      1
#endif

#if !defined(ENABLE_Cmdline)
#  define ENABLE_Cmdline        0
#endif

#if !defined(ENABLE_RCBP_TEST)
#  define ENABLE_RCBP_TEST      0
#endif

#if !defined(ENABLE_SSL)
#  define ENABLE_SSL            0
#endif

#if !defined(ENABLE_Unix_Domain)
#  define ENABLE_Unix_Domain    0
#endif

#if !defined(ENABLE_Plugins)
#  if TARGET_UNIX && defined(PATH_Plugins)
#    define ENABLE_Plugins      1
#  else
#    define ENABLE_Plugins      0
#  endif
#endif

#if !defined(ENABLE_STREAM_MACROS)
#  define ENABLE_STREAM_MACROS  0
#endif

#if !defined(ENABLE_AIO)
#  define ENABLE_AIO            0
#endif

#if !defined(ENABLE_DebugContext)
#  define ENABLE_DebugContext   0
#endif

#if !defined(ENABLE_ContextProxy)
#  define ENABLE_ContextProxy   0
#endif

#if !defined(ENABLE_ContextMux)
#  define ENABLE_ContextMux     0
#endif

#if !defined(ENABLE_ZeroCopy)
#  define ENABLE_ZeroCopy       1
#endif

#if !defined(ENABLE_Splice)
#  define ENABLE_Splice         0
#endif

#if !defined(ENABLE_Symbols)
#  define ENABLE_Symbols        0
#endif

#if !defined(ENABLE_LineNumbers)
#  define ENABLE_LineNumbers    0
#endif

#if !defined(ENABLE_SymbolsProxy)
#  define ENABLE_SymbolsProxy   0
#endif

#if !defined(ENABLE_LineNumbersProxy)
#  define ENABLE_LineNumbersProxy 0
#endif

#if !defined(ENABLE_SymbolsMux)
#  define ENABLE_SymbolsMux     0
#endif

#if !defined(ENABLE_ELF)
#  define ENABLE_ELF            0
#endif

#if !defined(ENABLE_ProfilerSST)
#  define ENABLE_ProfilerSST    0
#endif

#endif /* D_config */
