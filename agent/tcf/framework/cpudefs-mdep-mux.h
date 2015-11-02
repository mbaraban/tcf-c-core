/*
 * Copyright (c) 2013 Wind River Systems, Inc.
 *
 * The right to copy, distribute, modify or otherwise make use
 * of this software may be licensed only pursuant to the terms
 * of an applicable Wind River license agreement.
 */

#include <tcf/config.h>

extern RegisterDefinition * sys_get_PC_definition(Context * ctx);

#if ENABLE_ContextMux
#define get_PC_definition  sys_get_PC_definition
#endif /* ENABLE_ContextMux */
