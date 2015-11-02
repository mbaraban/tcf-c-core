/*******************************************************************************
 * Copyright (c) 2012 Wind River Systems, Inc. and others.
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
 * This module contains the default implementation for target function
 * call support.  It is expected that this implementation wil be
 * overridden with ABI specific implementations.
 */

#include <tcf/framework/funccall.h>

#if ENABLE_Symbols && ENABLE_DebugContext

int get_function_call_location_expression(struct FunctionCallInfo * info) {
	errno = ERR_UNSUPPORTED;
	return -1;
}

#endif /* ENABLE_Symbols && ENABLE_DebugContext */
