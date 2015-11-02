/*******************************************************************************
 * Copyright (c) 2013, 2015 Wind River Systems, Inc. and others.
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

#include <tcf/config.h>
#if ENABLE_ContextMux
#include <tcf/framework/context.h>
#include <tcf/framework/context-dispatcher.h>
#include <assert.h>

typedef struct ContextExtensionMux {
    ContextIf * ctx_iface; /* context interface */
} ContextExtensionMux;

#define EXT(ctx) ((ContextExtensionMux *)((char *)(ctx) + context_extension_offset))

#if ENABLE_ExtendedMemoryErrorReports
/* Last memory access error info */
static MemoryErrorInfo mem_err_info;
static int get_err_info_errno;
#endif

static size_t context_extension_offset = 0;

/* Set memory error info structure. Each underlying context (TOS, physical context) handles its
 * own memory error info structure; we use the context dispatcher to provide a unique source for
 * memory error information (we get the error from underlying context and set the global memory
 * error).
 */

#if ENABLE_ExtendedMemoryErrorReports
int set_dispatcher_mem_error_info(MemoryErrorInfo * info) {
    if (info->error == 0) {
        memset(&mem_err_info, 0, sizeof(mem_err_info));
        return 0;
    }
    /* Errno must already have been set by caller */
    assert(errno == info->error);
    get_err_info_errno = 0;
    mem_err_info = *info;
    return -1;
}

static int set_mem_error_info_unsupported(size_t size) {
    get_err_info_errno = 0;
    memset(&mem_err_info, 0, sizeof(mem_err_info));
    mem_err_info.error = errno = ERR_UNSUPPORTED;
    mem_err_info.size_error = size;
    return -1;
}

static int set_mem_error_info(ContextExtensionMux * ext, int rc) {
    int error = errno;
    get_err_info_errno = 0;
    memset(&mem_err_info, 0, sizeof(mem_err_info));
    if (rc < 0) {
        if (ext->ctx_iface->context_get_mem_error_info != NULL &&
                ext->ctx_iface->context_get_mem_error_info(&mem_err_info) < 0) {
            get_err_info_errno = errno;
        }
    }
    errno = error;
    return rc;
}
#else
#  define set_mem_error_info(ext, rc) rc
#  define set_mem_error_info_unsupported(size) -1
#endif

const char * context_suspend_reason(Context * ctx) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_suspend_reason == NULL) return NULL;
    return ext->ctx_iface->context_suspend_reason(ctx);
}

int context_has_state(Context * ctx) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_has_state == NULL) return 0;
    return ext->ctx_iface->context_has_state(ctx);
}

int context_stop(Context * ctx) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_stop == NULL) {
        errno = ERR_UNSUPPORTED;
        return -1;
    }
    return ext->ctx_iface->context_stop(ctx);
}

int context_continue(Context * ctx) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_continue == NULL) {
        return context_resume(ctx, RM_RESUME, 0, 0);
    }
    return ext->ctx_iface->context_continue(ctx);
}

int context_resume(Context * ctx, int mode, ContextAddress range_start, ContextAddress range_end) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_resume == NULL) {
        errno = ERR_UNSUPPORTED;
        return -1;
    }
    return ext->ctx_iface->context_resume(ctx, mode, range_start, range_end);
}

int context_can_resume(Context * ctx, int mode) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_can_resume == NULL) return 0;
    return ext->ctx_iface->context_can_resume(ctx, mode);
}

int context_single_step(Context * ctx) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_single_step == NULL) {
        return context_resume(ctx, RM_STEP_INTO, 0, 0);
    }
    return ext->ctx_iface->context_single_step(ctx);
}

int context_write_mem(Context * ctx, ContextAddress address, void * buf, size_t size) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_write_mem == NULL) return set_mem_error_info_unsupported(size);
    return set_mem_error_info(ext, ext->ctx_iface->context_write_mem(ctx, address, buf, size));
}

int context_read_mem(Context * ctx, ContextAddress address, void * buf, size_t size) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_read_mem == NULL) return set_mem_error_info_unsupported(size);
    return set_mem_error_info(ext, ext->ctx_iface->context_read_mem(ctx, address, buf, size));
}

#if ENABLE_MemoryAccessModes
int context_write_mem_ext(Context * ctx, MemoryAccessMode * mode, ContextAddress address, void * buf, size_t size) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_write_mem_ext == NULL) return set_mem_error_info_unsupported(size);
    return set_mem_error_info(ext, ext->ctx_iface->context_write_mem_ext(ctx, mode, address, buf, size));
}

int context_read_mem_ext(Context * ctx, MemoryAccessMode * mode, ContextAddress address, void * buf, size_t size) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_read_mem_ext == NULL) return set_mem_error_info_unsupported(size);
    return set_mem_error_info(ext, ext->ctx_iface->context_read_mem_ext(ctx, mode, address, buf, size));
}
#endif

unsigned context_word_size(Context * ctx) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_word_size == NULL) return 0;
    return ext->ctx_iface->context_word_size(ctx);
}

int context_read_reg(Context * ctx, RegisterDefinition * def, unsigned offs, unsigned size, void * buf) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_read_reg == NULL) {
        errno = ERR_UNSUPPORTED;
        return -1;
    }
    return ext->ctx_iface->context_read_reg(ctx, def, offs, size, buf);
}

int context_write_reg(Context * ctx, RegisterDefinition * def, unsigned offs, unsigned size, void * buf) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_write_reg == NULL) {
        errno = ERR_UNSUPPORTED;
        return -1;
    }
    return ext->ctx_iface->context_write_reg(ctx, def, offs, size, buf);
}

Context * context_get_group(Context * ctx, int group) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_get_group == NULL) {
        if (group == CONTEXT_GROUP_BREAKPOINT) return NULL;
        return ctx;
    }
    return ext->ctx_iface->context_get_group(ctx, group);
}

int context_get_canonical_addr(Context * ctx, ContextAddress addr, Context ** canonical_ctx,
        ContextAddress * canonical_addr, ContextAddress * block_addr, ContextAddress * block_size) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_get_canonical_addr == NULL) {
        errno = ERR_UNSUPPORTED;
        return -1;
    }
    return ext->ctx_iface->context_get_canonical_addr(ctx, addr, canonical_ctx, canonical_addr,
            block_addr, block_size);
}

int context_get_memory_map(Context * ctx, MemoryMap * map) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_get_memory_map == NULL) return 0;
    return ext->ctx_iface->context_get_memory_map(ctx, map);
}

int context_unplant_breakpoint(ContextBreakpoint * bp) {
    ContextExtensionMux * ext = EXT(bp->ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_unplant_breakpoint == NULL) {
        errno = ERR_UNSUPPORTED;
        return -1;
    }
    return ext->ctx_iface->context_unplant_breakpoint(bp);
}

int context_plant_breakpoint(ContextBreakpoint * bp) {
    ContextExtensionMux * ext = EXT(bp->ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_plant_breakpoint == NULL) {
        errno = ERR_UNSUPPORTED;
        return -1;
    }
    return ext->ctx_iface->context_plant_breakpoint(bp);
}

int context_get_supported_bp_access_types(Context * ctx) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_get_supported_bp_access_types == NULL) return 0;
    return ext->ctx_iface->context_get_supported_bp_access_types(ctx);
}

#if ENABLE_ContextStateProperties
int context_get_state_properties(Context * ctx, const char *** names, const char *** values, int * cnt) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_get_state_properties == NULL) {
        errno = ERR_UNSUPPORTED;
        return -1;
    }
    return (ext->ctx_iface->context_get_state_properties(ctx, names, values, cnt));
}
#endif

#if ENABLE_ExtendedBreakpointStatus
int context_get_breakpoint_status(ContextBreakpoint * bp, const char *** names, const char *** values, int * cnt) {
    ContextExtensionMux * ext = EXT(bp->ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_get_breakpoint_status == NULL) {
        errno = ERR_UNSUPPORTED;
        return -1;
    }
    return ext->ctx_iface->context_get_breakpoint_status(bp, names, values, cnt);
}
#endif

#if ENABLE_ContextBreakpointCapabilities
int context_get_breakpoint_capabilities(Context * ctx, const char *** names, const char *** values, int * cnt) {
    ContextExtensionMux * ext = ctx ? EXT(ctx) : NULL;
    if (ext == NULL || ext->ctx_iface == NULL || ext->ctx_iface->context_get_breakpoint_capabilities == NULL) {
        errno = ERR_UNSUPPORTED;
        return -1;
    }
    return ext->ctx_iface->context_get_breakpoint_capabilities(ctx, names, values, cnt);
}
#endif

#if ENABLE_ContextMemoryProperties
int context_get_memory_properties(Context * ctx, const char *** names, const char *** values, int * cnt)
{
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_get_memory_properties == NULL) {
        *cnt = 0;
        return 0;
    }
    return ext->ctx_iface->context_get_memory_properties(ctx, names, values, cnt);
}
#endif

#if ENABLE_ContextExtraProperties
int context_get_extra_properties(Context * ctx, const char *** names, const char *** values, int * cnt)
{
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_get_extra_properties == NULL) {
        *cnt = 0;
        return 0;
    }
    return ext->ctx_iface->context_get_extra_properties(ctx, names, values, cnt);
}
#endif

#if ENABLE_ContextISA
int context_get_isa(Context * ctx, ContextAddress addr, ContextISA * isa) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->context_get_isa == NULL) {
        errno = ERR_UNSUPPORTED;
        return -1;
    }
    return ext->ctx_iface->context_get_isa(ctx, addr, isa);
}
#endif

#if ENABLE_ExtendedMemoryErrorReports
int context_get_mem_error_info(MemoryErrorInfo * info) {
    if (get_err_info_errno != 0) {
        errno = get_err_info_errno;
        return -1;
    }
    if (mem_err_info.error == 0) {
        set_errno(ERR_OTHER, "Extended memory error info not available");
        return -1;
    }
    *info = mem_err_info;
    return 0;
}
#endif

RegisterDefinition * get_reg_definitions(Context * ctx) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->cpudefs_if.get_reg_definitions == NULL) return NULL;
    return ext->ctx_iface->cpudefs_if.get_reg_definitions(ctx);
}

RegisterDefinition * get_PC_definition(Context * ctx) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->cpudefs_if.get_PC_definition == NULL) return NULL;
    return ext->ctx_iface->cpudefs_if.get_PC_definition(ctx);
}

RegisterDefinition * get_reg_by_id(Context * ctx, unsigned id, RegisterIdScope * scope) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->cpudefs_if.get_reg_by_id == NULL) return NULL;
    return ext->ctx_iface->cpudefs_if.get_reg_by_id(ctx, id, scope);
}

int read_reg_bytes(StackFrame * frame, RegisterDefinition * reg_def, unsigned offs, unsigned size, uint8_t * buf) {
    ContextExtensionMux * ext = EXT(frame->ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->cpudefs_if.read_reg_bytes == NULL) {
        errno = ERR_UNSUPPORTED;
        return -1;
    }
    return ext->ctx_iface->cpudefs_if.read_reg_bytes(frame, reg_def, offs, size, buf);
}

int write_reg_bytes(StackFrame * frame, RegisterDefinition * reg_def, unsigned offs, unsigned size, uint8_t * buf) {
    ContextExtensionMux * ext = EXT(frame->ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->cpudefs_if.write_reg_bytes == NULL) {
        errno = ERR_UNSUPPORTED;
        return -1;
    }
    return ext->ctx_iface->cpudefs_if.write_reg_bytes(frame, reg_def, offs, size, buf);
}

uint8_t * get_break_instruction(Context * ctx, size_t * size) {
    ContextExtensionMux * ext = EXT(ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->cpudefs_if.get_break_instruction == NULL) return NULL;
    return ext->ctx_iface->cpudefs_if.get_break_instruction(ctx, size);
}

#if ENABLE_StackCrawlMux
int crawl_stack_frame(StackFrame * frame, StackFrame * down) {
    ContextExtensionMux * ext = EXT(frame->ctx);
    if (ext->ctx_iface == NULL || ext->ctx_iface->cpudefs_if.crawl_stack_frame == NULL) return 0;
    return ext->ctx_iface->cpudefs_if.crawl_stack_frame(frame, down);
}
#endif

ContextIf * context_get_interface(Context * ctx) {
    return EXT(ctx)->ctx_iface;
}

int context_set_interface(Context * ctx, ContextIf * ctx_iface) {
    EXT(ctx)->ctx_iface = ctx_iface;
    return 0;
}

void ini_context_dispatcher(void) {
    context_extension_offset = context_extension(sizeof(struct ContextExtensionMux));
}
#endif /* ENABLE_ContextMux */
