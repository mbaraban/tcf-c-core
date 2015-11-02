/*******************************************************************************
 * Copyright (c) 2010, 2015 Wind River Systems, Inc. and others.
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

/* Fake debug context API implementation. It used for testing symbol services. */

#include <tcf/config.h>

#include <sys/stat.h>
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>

#include <tcf/framework/mdep-fs.h>
#include <tcf/framework/context.h>
#include <tcf/framework/events.h>
#include <tcf/framework/myalloc.h>
#include <tcf/framework/exceptions.h>
#include <tcf/framework/trace.h>

#include <tcf/services/tcf_elf.h>
#include <tcf/services/elf-symbols.h>
#include <tcf/services/symbols.h>
#include <tcf/services/linenumbers.h>
#include <tcf/services/memorymap.h>
#include <tcf/services/dwarfframe.h>
#include <tcf/services/dwarfcache.h>
#include <tcf/services/stacktrace.h>
#include <tcf/services/expressions.h>
#include <tcf/services/dwarf.h>

#include <tcf/backend/backend.h>

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

#define MAX_REGS 2000

struct RegisterData {
    uint8_t data[MAX_REGS * 8];
    uint8_t mask[MAX_REGS * 8];
};

static Context * elf_ctx = NULL;
static MemoryMap mem_map;
static RegisterDefinition reg_defs[MAX_REGS];
static char reg_names[MAX_REGS][32];
static uint8_t reg_vals[MAX_REGS * 8];
static unsigned reg_size = 0;

static uint8_t frame_data[0x1000];
static ContextAddress frame_addr = 0x40000000u;

static const char * elf_file_name = NULL;
static ELF_File * elf_file = NULL;
static ContextAddress file_addr_offs = 0;
static int mem_region_pos = 0;
static int file_has_line_info = 0;
static unsigned line_info_cnt = 0;
static ContextAddress pc = 0;
static unsigned pass_cnt = 0;
static int test_posted = 0;
static struct timespec time_start;

static char ** files = NULL;
static unsigned files_max = 0;
static unsigned files_cnt = 0;

static int line_area_ok = 0;

#define AREA_BUF_SIZE 0x100
static CodeArea area_buf[AREA_BUF_SIZE];
static unsigned area_cnt = 0;


extern ObjectInfo * get_symbol_object(Symbol * sym);

static RegisterDefinition * get_reg_by_dwarf_id(unsigned id) {
    static RegisterDefinition ** map = NULL;
    static unsigned map_length = 0;

    if (map == NULL) {
        RegisterDefinition * r;
        RegisterDefinition * regs_index = get_reg_definitions(NULL);
        for (r = regs_index; r->name != NULL; r++) {
            if (r->dwarf_id >= (int)map_length) map_length = r->dwarf_id + 1;
        }
        map = (RegisterDefinition **)loc_alloc_zero(sizeof(RegisterDefinition *) * map_length);
        for (r = regs_index; r->name != NULL; r++) {
            if (r->dwarf_id >= 0) map[r->dwarf_id] = r;
        }
    }
    return id < map_length ? map[id] : NULL;
}

static RegisterDefinition * get_reg_by_eh_frame_id(unsigned id) {
    static RegisterDefinition ** map = NULL;
    static unsigned map_length = 0;

    if (map == NULL) {
        RegisterDefinition * r;
        RegisterDefinition * regs_index = get_reg_definitions(NULL);
        for (r = regs_index; r->name != NULL; r++) {
            if (r->eh_frame_id >= (int)map_length) map_length = r->eh_frame_id + 1;
        }
        map = (RegisterDefinition **)loc_alloc_zero(sizeof(RegisterDefinition *) * map_length);
        for (r = regs_index; r->name != NULL; r++) {
            if (r->eh_frame_id >= 0) map[r->eh_frame_id] = r;
        }
    }
    return id < map_length ? map[id] : NULL;
}

RegisterDefinition * get_reg_by_id(Context * ctx, unsigned id, RegisterIdScope * scope) {
    RegisterDefinition * def = NULL;
    switch (scope->id_type) {
    case REGNUM_DWARF: def = get_reg_by_dwarf_id(id); break;
    case REGNUM_EH_FRAME: def = get_reg_by_eh_frame_id(id); break;
    }
    if (def == NULL) set_errno(ERR_OTHER, "Invalid register ID");
    return def;
}

int read_reg_bytes(StackFrame * frame, RegisterDefinition * reg_def, unsigned offs, unsigned size, uint8_t * buf) {
    if (reg_def != NULL && frame != NULL) {
        if (frame->is_top_frame || frame->regs == NULL) {
            return context_read_reg(frame->ctx, reg_def, offs, size, buf);
        }
        if (frame->regs != NULL) {
            uint8_t * r_addr = (uint8_t *)&frame->regs->data + reg_def->offset;
            uint8_t * m_addr = (uint8_t *)&frame->regs->mask + reg_def->offset;
            size_t i;
            for (i = 0; i < size; i++) {
                if (m_addr[offs + i] != 0xff) {
                    return context_read_reg(frame->ctx, reg_def, offs, size, buf);
                }
            }
            if (offs + size > reg_def->size) {
                errno = ERR_INV_DATA_SIZE;
                return -1;
            }
            memcpy(buf, r_addr + offs, size);
            return 0;
        }
    }
    errno = ERR_INV_CONTEXT;
    return -1;
}

int write_reg_bytes(StackFrame * frame, RegisterDefinition * reg_def, unsigned offs, unsigned size, uint8_t * buf) {
    if (reg_def != NULL && frame != NULL) {
        if (frame->is_top_frame) {
            return context_write_reg(frame->ctx, reg_def, offs, size, buf);
        }
        if (frame->regs == NULL && context_has_state(frame->ctx)) {
            frame->regs = (RegisterData *)loc_alloc_zero(sizeof(RegisterData));
        }
        if (frame->regs != NULL) {
            uint8_t * r_addr = (uint8_t *)&frame->regs->data + reg_def->offset;
            uint8_t * m_addr = (uint8_t *)&frame->regs->mask + reg_def->offset;

            if (offs + size > reg_def->size) {
                errno = ERR_INV_DATA_SIZE;
                return -1;
            }
            memcpy(r_addr + offs, buf, size);
            memset(m_addr + offs, 0xff, size);
            return 0;
        }
    }
    errno = ERR_INV_CONTEXT;
    return -1;
}

RegisterDefinition * get_reg_definitions(Context * ctx) {
    return reg_defs;
}

RegisterDefinition * get_PC_definition(Context * ctx) {
    return reg_defs;
}

Context * id2ctx(const char * id) {
    if (id != NULL && strcmp(id, elf_ctx->id) == 0) return elf_ctx;
    return NULL;
}

unsigned context_word_size(Context * ctx) {
    return get_PC_definition(ctx)->size;
}

int context_has_state(Context * ctx) {
    return 1;
}

Context * context_get_group(Context * ctx, int group) {
    return ctx;
}

int context_read_reg(Context * ctx, RegisterDefinition * def, unsigned offs, unsigned size, void * buf) {
    if (ctx != elf_ctx) {
        errno = ERR_INV_CONTEXT;
        return -1;
    }
    memcpy(buf, reg_vals + def->offset + offs, size);
    return 0;
}

int context_write_reg(Context * ctx, RegisterDefinition * def, unsigned offs, unsigned size, void * buf) {
    if (ctx != elf_ctx) {
        errno = ERR_INV_CONTEXT;
        return -1;
    }
    memcpy(reg_vals + def->offset + offs, buf, size);
    return 0;
}

int context_read_mem(Context * ctx, ContextAddress address, void * buf, size_t size) {
    if (address >= frame_addr && address + size >= address && address + size <= frame_addr + sizeof(frame_data)) {
        memcpy(buf, frame_data + (address - frame_addr), size);
        return 0;
    }
    memset(buf, 0, size);
    return 0;
}

int context_write_mem(Context * ctx, ContextAddress address, void * buf, size_t size) {
    /* TODO: context_write_mem */
    errno = ERR_UNSUPPORTED;
    return -1;
}

int context_get_memory_map(Context * ctx, MemoryMap * map) {
    unsigned i;
    for (i = 0; i < mem_map.region_cnt; i++) {
        MemoryRegion * r = NULL;
        if (map->region_cnt >= map->region_max) {
            map->region_max += 8;
            map->regions = (MemoryRegion *)loc_realloc(map->regions, sizeof(MemoryRegion) * map->region_max);
        }
        r = map->regions + map->region_cnt++;
        *r = mem_map.regions[i];
        if (r->file_name) r->file_name = loc_strdup(r->file_name);
        if (r->sect_name) r->sect_name = loc_strdup(r->sect_name);
    }
    return 0;
}

int crawl_stack_frame(StackFrame * frame, StackFrame * down) {
    frame->fp = frame_addr;
    down->has_reg_data = 1;
    return 0;
}

static void print_symbol(Symbol * sym) {
    ObjectInfo * obj = get_symbol_object(sym);
    if (obj == NULL) {
        printf("Object  : NULL\n");
    }
    else {
        printf("Object  : 0x%" PRIX64 "\n", (uint64_t)obj->mID);
        printf("  Tag   : 0x%02x\n", obj->mTag);
        printf("  Flags : 0x%02x\n", obj->mFlags);
        if (obj->mName != NULL) {
            printf("  Name  : %s\n", obj->mName);
        }
        if (obj->mType != NULL) {
            printf("  Type  : 0x%" PRIX64 "\n", (uint64_t)obj->mType->mID);
        }
        if (obj->mDefinition != NULL) {
            printf("  Def   : 0x%" PRIX64 "\n", (uint64_t)obj->mDefinition->mID);
        }
    }
}

static void error(const char * func) {
    int err = errno;
    printf("File    : %s\n", elf_file_name);
    if (elf_open(elf_file_name)->debug_info_file_name) {
        printf("Symbols : %s\n", elf_open(elf_file_name)->debug_info_file_name);
    }
    printf("Address : 0x%" PRIX64 "\n", (uint64_t)pc);
    printf("Function: %s\n", func);
    printf("Error   : %s\n", errno_to_str(err));
    fflush(stdout);
    exit(1);
}

static void error_sym(const char * func, Symbol * sym) {
    int err = errno;
    print_symbol(sym);
    errno = err;
    error(func);
}

static void addr_to_line_callback(CodeArea * area, void * args) {
    if (area->start_address > pc || area->end_address <= pc) {
        errno = set_errno(ERR_OTHER, "Invalid line area address");
        error("address_to_line");
    }
    if (area->start_line > area->end_line) {
        errno = set_errno(ERR_OTHER, "Invalid line area end line number");
        error("address_to_line");
    }
    if (area->next_address != 0 &&
            area->next_address < area->end_address &&
            area->next_address >= area->start_address) {
        errno = set_errno(ERR_OTHER, "Invalid line area end address");
        error("address_to_line");
    }
    if (area->end_address - area->start_address >= 0x100000) {
        errno = set_errno(ERR_OTHER, "Invalid line area end address");
        error("address_to_line");
    }
    if (area_cnt < AREA_BUF_SIZE) area_buf[area_cnt++] = *area;
}

static void addr_to_line_callback_p1(CodeArea * area, void * args) {
    if (area_cnt < AREA_BUF_SIZE) area_buf[area_cnt++] = *area;
}

static void line_to_addr_callback(CodeArea * area, void * args) {
    CodeArea * org = (CodeArea *)args;
    if (area->start_line > org->start_line || (area->start_line == org->start_line && area->start_column > org->start_column) ||
        area->end_line < org->start_line || (area->end_line == org->start_line && area->end_column <= org->start_column)) {
        errno = set_errno(ERR_OTHER, "Invalid line area line numbers");
        error("line_to_address");
    }
    if (area->start_address > pc || area->end_address <= pc) return;
    if (org->start_address == area->start_address || org->end_address == area->end_address) {
        line_area_ok = 1;
    }
}

static void print_time(struct timespec time_start, int cnt) {
    struct timespec time_now;
    struct timespec time_diff;
    if (cnt == 0) return;
    clock_gettime(CLOCK_REALTIME, &time_now);
    time_diff.tv_sec = time_now.tv_sec - time_start.tv_sec;
    if (time_now.tv_nsec < time_start.tv_nsec) {
        time_diff.tv_sec--;
        time_diff.tv_nsec = time_now.tv_nsec + 1000000000 - time_start.tv_nsec;
    }
    else {
        time_diff.tv_nsec = time_now.tv_nsec - time_start.tv_nsec;
    }
    time_diff.tv_nsec /= cnt;
    time_diff.tv_nsec += (long)(((uint64_t)(time_diff.tv_sec % cnt) * 1000000000) / cnt);
    time_diff.tv_sec /= cnt;
    printf("search time: %ld.%06ld\n", (long)time_diff.tv_sec, time_diff.tv_nsec / 1000);
    fflush(stdout);
}

static int symcmp(Symbol * x, Symbol * y) {
    char id[256];
    strcpy(id, symbol2id(x));
    return strcmp(id, symbol2id(y));
}

static int errcmp(int err, const char * msg) {
    const char * txt = errno_to_str(err);
    size_t msg_len = strlen(msg);
    size_t txt_len = strlen(txt);
    unsigned i;
    for (i = 0; txt_len - i >= msg_len; i++) {
       if (strncmp(txt + i, msg, msg_len) == 0) return 0;
    }
    return 1;
}

static void test(void * args);
static void loc_var_func(void * args, Symbol * sym);

static int is_cpp_reference(Symbol * type) {
    int type_class = 0;
    if (type == NULL) return 0;
    if (get_symbol_type_class(type, &type_class) < 0) {
        error_sym("get_symbol_type_class", type);
    }
    if (type_class == TYPE_CLASS_POINTER) {
        Symbol * ptr = type;
        for (;;) {
            SYM_FLAGS ptr_flags = 0;
            Symbol * next = NULL;
            if (get_symbol_flags(ptr, &ptr_flags) < 0) {
                error_sym("get_symbol_flags", ptr);
            }
            if (ptr_flags & SYM_FLAG_REFERENCE) {
                return 1;
            }
            if (get_symbol_type(ptr, &next) < 0) {
                error_sym("get_symbol_type", ptr);
            }
            if (next == ptr) break;
            ptr = next;
        }
    }
    return 0;
}

static int is_indirect(Symbol * type) {
    if (type == NULL) return 0;
    for (;;) {
        SYM_FLAGS flags = 0;
        Symbol * next = NULL;
        if (get_symbol_flags(type, &flags) < 0) {
            error_sym("get_symbol_flags", type);
        }
        if (flags & SYM_FLAG_INDIRECT) {
            return 1;
        }
        if (get_symbol_type(type, &next) < 0) {
            error_sym("get_symbol_type", type);
        }
        if (next == type) break;
        type = next;
    }
    return 0;
}

static void test_enumeration_type(Symbol * type) {
    int i;
    int count = 0;
    Symbol ** children = NULL;
    Symbol * enum_type = type;
    ContextAddress enum_type_size = 0;
    char * type_name = NULL;
    Symbol * find_sym = NULL;
    int visible = 0;

    for (i = 0;; i++) {
        SYM_FLAGS enum_flags = 0;
        if (get_symbol_flags(enum_type, &enum_flags) < 0) {
            error_sym("get_symbol_flags", enum_type);
        }
        if ((enum_flags & SYM_FLAG_ENUM_TYPE) != 0) break;
        if ((enum_flags & SYM_FLAG_BOOL_TYPE) != 0) break;
        if (get_symbol_type(enum_type, &enum_type) < 0) {
            error_sym("get_symbol_type", enum_type);
        }
        if (i >= 1000) {
            errno = ERR_OTHER;
            error("Invalid original type for enumeration type class");
        }
    }
    if (get_symbol_size(enum_type, &enum_type_size) < 0) {
        error_sym("get_symbol_size", enum_type);
    }
    if (get_symbol_children(type, &children, &count) < 0) {
        error_sym("get_symbol_children", type);
    }
    if (get_symbol_name(type, &type_name) < 0) {
        error_sym("get_symbol_name", type);
    }
    if (type_name != NULL && find_symbol_by_name(elf_ctx, STACK_TOP_FRAME, 0, type_name, &find_sym) == 0) {
        visible = symcmp(type, find_sym) == 0;
    }
    for (i = 0; i < count; i++) {
        Symbol * child_type = NULL;
        void * value = NULL;
        size_t value_size = 0;
        int big_endian = 0;
        ContextAddress child_size = 0;
        char * name = NULL;
        if (get_symbol_value(children[i], &value, &value_size, &big_endian) < 0) {
            error_sym("get_symbol_value", children[i]);
        }
        if (get_symbol_type(children[i], &child_type) < 0) {
            error_sym("get_symbol_type", children[i]);
        }
        if (symcmp(enum_type, child_type) != 0) {
            errno = ERR_OTHER;
            error("Invalid type of enum element");
        }
        if (get_symbol_size(children[i], &child_size) < 0) {
            error_sym("get_symbol_size", children[i]);
        }
        if (enum_type_size != child_size) {
            errno = ERR_OTHER;
            error("Invalid size of enumeration constant");
        }
        if (value_size != child_size) {
            errno = ERR_OTHER;
            error("Invalid size of enumeration constant");
        }
        if (get_symbol_name(children[i], &name) < 0) {
            error_sym("get_symbol_name", children[i]);
        }
        if (visible && name != NULL && get_symbol_object(children[i]) != NULL) {
            if (find_symbol_by_name(elf_ctx, STACK_TOP_FRAME, 0, name, &find_sym) < 0) {
                error_sym("find_symbol_by_name", children[i]);
            }
#if 0
            /* TODO: better enum const search */
            if (symcmp(children[i], find_sym) != 0) {
                errno = ERR_OTHER;
                error_sym("find_symbol_by_name", children[i]);
            }
#endif
        }
    }
}

static void test_variant_part(Symbol * part) {
    int i;
    int count = 0;
    Symbol ** children = NULL;

    if (get_symbol_children(part, &children, &count) < 0) {
        error_sym("get_symbol_children", part);
    }
    for (i = 0; i < count; i++) {
        int member_class = 0;
        Symbol * member_container = NULL;
        LocationInfo * loc_info = NULL;
        if (get_symbol_class(children[i], &member_class) < 0) {
            error_sym("get_symbol_class", children[i]);
        }
        if (get_symbol_container(children[i], &member_container) < 0) {
            error_sym("get_symbol_container", children[i]);
        }
        if (get_location_info(children[i], &loc_info) < 0) {
            error_sym("get_location_info", children[i]);
        }
    }
}

static void test_composite_type(Symbol * type) {
    int i;
    int count = 0;
    char * type_name = NULL;
    Symbol * org_type = type;
    Symbol ** children = NULL;
    for (;;) {
        SYM_FLAGS flags = 0;
        if (get_symbol_flags(org_type, &flags) < 0) {
            error("get_symbol_flags");
        }
        if ((flags & SYM_FLAG_TYPEDEF) == 0) break;
        if (get_symbol_type(org_type, &org_type) < 0) {
            error("get_symbol_base_type");
        }
    }
    if (get_symbol_name(org_type, &type_name) < 0) {
        error("get_symbol_name");
    }
    if (get_symbol_children(type, &children, &count) < 0) {
        error_sym("get_symbol_children", type);
    }
    for (i = 0; i < count; i++) {
        int member_class = 0;
        Symbol * member_container = NULL;
        int container_class = 0;
        ContextAddress offs = 0;
        ContextAddress size = 0;
        ContextAddress length = 0;
        if (get_symbol_class(children[i], &member_class) < 0) {
            error_sym("get_symbol_class", children[i]);
        }
        if (get_symbol_container(children[i], &member_container) < 0) {
            error_sym("get_symbol_container", children[i]);
        }
        if (get_symbol_class(member_container, &container_class) < 0) {
            error_sym("get_symbol_class", member_container);
        }
        if (container_class != SYM_CLASS_TYPE) {
            errno = ERR_OTHER;
            error_sym("Invalid result of get_symbol_container()", children[i]);
        }
        if (type_name != NULL) {
            char * container_name = NULL;
            if (get_symbol_name(member_container, &container_name) < 0) {
                error_sym("get_symbol_name", member_container);
            }
            if (container_name == NULL || strcmp(container_name, type_name) != 0) {
                errno = ERR_OTHER;
                error_sym("Invalid result of get_symbol_container()", children[i]);
            }
        }
        if (member_class == SYM_CLASS_REFERENCE) {
            Symbol * member_type = NULL;
            int member_type_class  = 0;
            if (get_symbol_type(children[i], &member_type) < 0) {
                error_sym("get_symbol_type", children[i]);
            }
            if (get_symbol_type_class(children[i], &member_type_class) < 0) {
                error_sym("get_symbol_type_class", children[i]);
            }
            if (get_symbol_address(children[i], &offs) < 0) {
                if (get_symbol_offset(children[i], &offs) < 0) {
#if 0
                    int ok = 0;
                    int err = errno;
                    unsigned type_flags;
                    if (get_symbol_flags(children[i], &type_flags) < 0) {
                        error("get_symbol_flags");
                    }
                    if (type_flags & SYM_FLAG_EXTERNAL) ok = 1;
                    if (!ok) {
                        errno = err;
                        error("get_symbol_offset");
                    }
#endif
                }
                else if (!is_cpp_reference(member_type)) {
                    Value v;
                    uint64_t n = 0;
                    char * expr = (char *)tmp_alloc(512);
                    unsigned base = (rand() & 0xffff) << 4;
                    sprintf(expr, "&(((${%s} *)%u)->${%s})", tmp_strdup(symbol2id(type)), base, tmp_strdup(symbol2id(children[i])));
                    if (evaluate_expression(elf_ctx, STACK_TOP_FRAME, 0, expr, 0, &v) < 0) {
                        error("evaluate_expression");
                    }
                    if (value_to_unsigned(&v, &n) < 0) {
                        error("value_to_unsigned");
                    }
                    if (n != base + offs) {
                        errno = ERR_OTHER;
                        error("invalid result of evaluate_expression");
                    }
                }
            }
            if (get_symbol_size(children[i], &size) < 0) {
                error_sym("get_symbol_size", children[i]);
            }
            if (member_type_class == TYPE_CLASS_ARRAY && get_symbol_length(children[i], &length) < 0) {
                error_sym("get_symbol_length", children[i]);
            }
        }
        else if (member_class == SYM_CLASS_VALUE) {
            void * value = NULL;
            size_t value_size = 0;
            int big_endian = 0;
            if (get_symbol_value(children[i], &value, &value_size, &big_endian) < 0) {
                error_sym("get_symbol_value", children[i]);
            }
        }
        else if (member_class == SYM_CLASS_VARIANT_PART) {
            test_variant_part(children[i]);
        }
    }
}

static void test_this_pointer(Symbol * base_type) {
    int i;
    int count = 0;
    Symbol ** children = NULL;
    if (get_symbol_children(base_type, &children, &count) < 0) {
        error_sym("get_symbol_children", base_type);
    }
    for (i = 0; i < count; i++) {
        int member_class = 0;
        char * member_name = NULL;
        if (get_symbol_class(children[i], &member_class) < 0) {
            error_sym("get_symbol_class", children[i]);
        }
        if (get_symbol_name(children[i], &member_name) < 0) {
            error_sym("get_symbol_name", children[i]);
        }
        if (member_name != NULL) {
            Symbol * impl_this = NULL;
            if (find_symbol_by_name(elf_ctx, STACK_TOP_FRAME, pc, member_name, &impl_this) < 0) {
                error_sym("find_symbol_by_name", children[i]);
            }
            loc_var_func(NULL, impl_this);
        }
    }
}

static void test_implicit_pointer(Symbol * sym) {
    Trap trap;
    StackFrame * frame_info = NULL;
    LocationInfo * loc_info = NULL;
    const char * id = symbol2id(sym);
    LocationExpressionState * state = NULL;
    int type_class = 0;
    Symbol * type = NULL;
    int cpp_ref = 0; /* '1' if the symbol is C++ reference */
    Value v;

    if (get_symbol_type(sym, &type) < 0) {
        error_sym("get_symbol_type", sym);
    }
    if (type != NULL) {
        if (get_symbol_type_class(sym, &type_class) < 0) {
            error_sym("get_symbol_type_class", sym);
        }
        cpp_ref = is_cpp_reference(type);
    }

    if (get_location_info(sym, &loc_info) < 0) {
        error_sym("get_location_info", sym);
    }
    else if (get_frame_info(elf_ctx, STACK_TOP_FRAME, &frame_info) < 0) {
        error("get_frame_info");
    }
    assert(loc_info->value_cmds.cnt > 0);
    assert(loc_info->code_size == 0 || (loc_info->code_addr <= pc && loc_info->code_addr + loc_info->code_size > pc));
    if (set_trap(&trap)) {
        unsigned i;
        unsigned implicit_pointer = 0;
        state = evaluate_location_expression(elf_ctx, frame_info,
            loc_info->value_cmds.cmds, loc_info->value_cmds.cnt, NULL, 0);
        if (state->pieces_cnt == 0) {
            errno = set_errno(ERR_OTHER, "Expected pieces");
            error("evaluate_location_expression");
        }
        for (i = 0; i < state->pieces_cnt; i++) {
            implicit_pointer += state->pieces[i].implicit_pointer;
        }
        if (implicit_pointer == 0) {
            errno = set_errno(ERR_OTHER, "Expected implicit pointer");
            error("evaluate_location_expression");
        }
        clear_trap(&trap);
    }
    else {
        error("evaluate_location_expression");
    }

    if (state->pieces_cnt == 1 && !cpp_ref) {
        char * expr = (char *)tmp_alloc(strlen(id) + 16);
        sprintf(expr, "${%s}", id);
        if (evaluate_expression(elf_ctx, STACK_TOP_FRAME, pc, expr, 0, &v) < 0) {
            error_sym("evaluate_expression", sym);
        }
        if (v.loc->pieces->implicit_pointer != state->pieces->implicit_pointer) {
            errno = set_errno(ERR_OTHER, "Invalid implicit_pointer");
            error("evaluate_expression");
        }

        sprintf(expr, "*${%s}", id);
        if (evaluate_expression(elf_ctx, STACK_TOP_FRAME, pc, expr, 0, &v) < 0) {
            error_sym("evaluate_expression", sym);
        }
        if (v.loc && v.loc->pieces_cnt > 0) {
            if (v.loc->pieces->implicit_pointer != state->pieces->implicit_pointer - 1) {
                errno = set_errno(ERR_OTHER, "Invalid implicit_pointer");
                error("evaluate_expression");
            }
        }
        else {
            if (state->pieces->implicit_pointer != 1) {
                errno = set_errno(ERR_OTHER, "Invalid implicit_pointer");
                error("evaluate_expression");
            }
        }
    }
}

static void loc_var_func(void * args, Symbol * sym) {
    int frame = 0;
    Context * ctx = NULL;
    RegisterDefinition * reg = NULL;
    ContextAddress addr = 0;
    ContextAddress size = 0;
    int addr_ok = 0;
    int size_ok = 0;
    SYM_FLAGS flags = 0;
    int symbol_class = 0;
    int type_class = 0;
    Symbol * type = NULL;
    Symbol * index_type = NULL;
    Symbol * base_type = NULL;
    int cpp_ref = 0; /* '1' if the symbol is C++ reference */
    int indirect = 0;
    int ref_size_ok = 0;
    ContextAddress length = 0;
    int64_t lower_bound = 0;
    void * value = NULL;
    size_t value_size = 0;
    int value_big_endian = 0;
    char * name = NULL;
    char * type_name = NULL;
    StackFrame * frame_info = NULL;
    LocationInfo * loc_info = NULL;
    LocationExpressionState * loc_state = NULL;

    if (get_symbol_class(sym, &symbol_class) < 0) {
        error_sym("get_symbol_class", sym);
    }
    if (get_symbol_flags(sym, &flags) < 0) {
        error_sym("get_symbol_flags", sym);
    }
    if (get_symbol_name(sym, &name) < 0) {
        error_sym("get_symbol_name", sym);
    }
    if (name != NULL) {
        int found_next = 0;
        Symbol * find_sym = NULL;
        name = tmp_strdup(name);
        if (find_symbol_by_name(elf_ctx, STACK_TOP_FRAME, 0, name, &find_sym) < 0) {
            error("find_symbol_by_name");
        }
        for (;;) {
            Symbol * find_next = NULL;
            if (find_next_symbol(&find_next) < 0) {
                if (get_error_code(errno) != ERR_SYM_NOT_FOUND) {
                    error("find_next_symbol");
                }
                break;
            }
            else if (symcmp(find_sym, find_next) == 0) {
                errno = ERR_OTHER;
                error("Invalid result of find_next_symbol()");
            }
            else if (symcmp(sym, find_next) == 0) {
                found_next = 1;
            }
        }
        if (get_symbol_object(sym) != NULL && symcmp(sym, find_sym) != 0) {
            /* 'sym' is eclipsed in the current scope by a nested declaration */
            Symbol * sym_container = NULL;
            Symbol * find_container = NULL;
            if (!found_next) {
                find_symbol_by_name(elf_ctx, STACK_TOP_FRAME, 0, name, &find_sym);
                errno = ERR_OTHER;
                error_sym("Invalid result of find_next_symbol()", find_sym);
            }
            found_next = 0;
            if (get_symbol_container(sym, &sym_container) < 0) {
                error_sym("get_symbol_container", sym);
            }
            if (find_symbol_in_scope(elf_ctx, STACK_TOP_FRAME, 0, sym_container, name, &find_sym) < 0) {
                error("find_symbol_in_scope");
            }
            if (get_symbol_container(find_sym, &find_container) < 0) {
                error_sym("get_symbol_container", find_sym);
            }
            if (symcmp(sym_container, find_container) != 0) {
                errno = ERR_OTHER;
                error("Invalid result of find_symbol_in_scope()");
            }
            for (;;) {
                Symbol * find_next = NULL;
                if (find_next_symbol(&find_next) < 0) {
                    if (get_error_code(errno) != ERR_SYM_NOT_FOUND) {
                        error("find_next_symbol");
                    }
                    break;
                }
                else if (symcmp(find_sym, find_next) == 0) {
                    errno = ERR_OTHER;
                    error("Invalid result of find_next_symbol()");
                }
                else if (symcmp(sym, find_next) == 0) {
                    found_next = 1;
                }
                if (get_symbol_container(find_next, &find_container) < 0) {
                    error("get_symbol_container");
                }
                if (symcmp(sym_container, find_container) != 0) {
                    errno = ERR_OTHER;
                    error("Invalid result of find_next_symbol()");
                }
            }
            if (symcmp(sym, find_sym) != 0) {
                if (!found_next) {
                    errno = ERR_OTHER;
                    //error("Invalid result of find_next_symbol()");
                }
            }
        }
    }
    if (get_symbol_address(sym, &addr) < 0 &&
            (get_symbol_register(sym, &ctx, &frame, &reg) < 0 || reg == NULL) &&
            (get_symbol_value(sym, &value, &value_size, &value_big_endian) < 0 || value == NULL)) {
        int err = errno;
        ObjectInfo * obj = get_symbol_object(sym);
        if (errcmp(err, "No object location info found") == 0) return;
        if (obj != NULL && obj->mCompUnit->mLanguage == LANG_ADA95) {
            if (errcmp(err, "Object does not have location information") == 0) return;
        }
        if (symbol_class == SYM_CLASS_TYPE && obj != NULL && obj->mCompUnit->mLanguage == LANG_ADA95) {
            if (errcmp(err, "Invalid address of containing object") == 0) return;
        }
        if (errcmp(err, "Object is not available at this location") == 0) return;
        if (errcmp(err, "OP_fbreg: cannot read AT_frame_base") == 0) return;
        if (errcmp(err, "Division by zero in location") == 0) return;
        if (errcmp(err, "Cannot find loader debug") == 0) return;
        if (errcmp(err, "Cannot get TLS module ID") == 0) return;
        if (errcmp(err, "Cannot get address of ELF symbol: indirect symbol") == 0) return;
        if (errcmp(err, "Unsupported type in OP_GNU_const_type") == 0) return;
        if (errcmp(err, "Unsupported type in OP_GNU_convert") == 0) return;
        if (errcmp(err, "Invalid size of implicit value") == 0) return;
        if (errcmp(err, "Invalid implicit pointer") == 0) return;
        if (errcmp(err, "Cannot get symbol value: optimized away") == 0) return;
        if (errcmp(err, "Cannot get symbol value: implicit pointer") == 0) {
            test_implicit_pointer(sym);
            return;
        }
        if (symbol_class == SYM_CLASS_TYPE && errcmp(err, "Wrong object kind") == 0) return;
        errno = err;
        error_sym("get_symbol_value", sym);
    }
    else if (get_location_info(sym, &loc_info) < 0) {
        error_sym("get_location_info", sym);
    }
    else if (get_frame_info(elf_ctx, STACK_TOP_FRAME, &frame_info) < 0) {
        error("get_frame_info");
    }
    else {
        Trap trap;
        assert(loc_info->value_cmds.cnt > 0);
        assert(loc_info->code_size == 0 || (loc_info->code_addr <= pc && loc_info->code_addr + loc_info->code_size > pc));
        if (set_trap(&trap)) {
            loc_state = evaluate_location_expression(elf_ctx, frame_info,
                loc_info->value_cmds.cmds, loc_info->value_cmds.cnt, NULL, 0);
            if (loc_state->stk_pos == 1) {
                if (loc_state->stk[0] != addr) str_fmt_exception(ERR_OTHER,
                    "ID 0x%" PRIX64 ": invalid location expression result 0x%" PRIX64 " != 0x%" PRIX64,
                    get_symbol_object(sym)->mID, loc_state->stk[0], addr);
                addr_ok = 1;
            }
            clear_trap(&trap);
        }
        else {
            error("evaluate_location_expression");
        }
    }
    if (get_symbol_type(sym, &type) < 0) {
        error_sym("get_symbol_type", sym);
    }
    if (type != NULL) {
        if (get_symbol_name(type, &type_name) < 0) {
            error_sym("get_symbol_name", type);
        }
        if (type_name != NULL) type_name = tmp_strdup(type_name);
        if (get_symbol_type_class(sym, &type_class) < 0) {
            error_sym("get_symbol_type_class", sym);
        }
        cpp_ref = is_cpp_reference(type);
        indirect = is_indirect(type);
    }
    size_ok = 1;
    if (get_symbol_size(sym, &size) < 0) {
        int ok = 0;
        int err = errno;
        ObjectInfo * obj = get_symbol_object(sym);
        if (type != NULL) {
            unsigned type_flags;
            if (get_symbol_flags(type, &type_flags) < 0) {
                error_sym("get_symbol_flags", type);
            }
            if (name == NULL && type_name != NULL && strcmp(type_name, "exception") == 0 && (type_flags & SYM_FLAG_CLASS_TYPE)) {
                /* GCC does not tell size of std::exception class */
                ok = 1;
            }
        }
        if (!ok && errcmp(err, "Size not available: indirect symbol") == 0) ok = 1;
        if (!ok && errcmp(err, "Object is not available at this location") == 0) ok = 1;
        if (!ok && symbol_class == SYM_CLASS_COMP_UNIT) {
            /* Comp unit with code ranges */
            ok = 1;
        }
        if (!ok && obj->mTag == TAG_subprogram && (obj->mFlags & DOIF_declaration) != 0) {
            ok = 1;
        }
        if (!ok && symbol_class == SYM_CLASS_REFERENCE && addr_ok && type == NULL && name == NULL) {
            /* GCC C++ 4.1 produces entries like this */
            ok = 1;
        }
        if (!ok && obj != NULL && obj->mCompUnit->mLanguage == LANG_ADA95) {
            if (errcmp(err, "No object location info found in DWARF") == 0) ok = 1;
        }
        if (!ok && obj != NULL && obj->mCompUnit->mLanguage == LANG_ADA95) {
            if (errcmp(err, "Object does not have memory address") == 0) ok = 1;
        }
        if (!ok && obj != NULL && obj->mCompUnit->mLanguage == LANG_ADA95) {
            if (errcmp(err, "Cannot get object address: the object is located in a register") == 0) ok = 1;
        }
        if (!ok && obj != NULL && obj->mTag == TAG_dwarf_procedure) {
            ok = 1;
        }
        if (!ok) {
            errno = err;
            error_sym("get_symbol_size", sym);
        }
        size_ok = 0;
    }
    if (cpp_ref) {
        Symbol * base_type = NULL;
        ref_size_ok = 1;
        if (get_symbol_base_type(sym, &base_type) < 0) {
            error_sym("get_symbol_base_type", sym);
        }
        if (get_symbol_size(base_type, &size) < 0) {
            int ok = 0;
            int err = errno;
            ObjectInfo * obj = get_symbol_object(base_type);
            if (obj != NULL && obj->mCompUnit->mLanguage == LANG_ADA95) {
                if (!ok && errcmp(err, "Invalid reference in OP_call") == 0) ok = 1;
                if (!ok && errcmp(err, "Object is not available at this location") == 0) ok = 1;
            }
            if (!ok) {
                errno = err;
                error_sym("get_symbol_size", base_type);
            }
            ref_size_ok = 0;
        }
    }
    if (get_symbol_frame(sym, &ctx, &frame) < 0) {
        error_sym("get_symbol_frame", sym);
    }
    if (symbol_class != SYM_CLASS_COMP_UNIT && size_ok && (!cpp_ref || ref_size_ok)) {
        Value v;
        char expr[300];
        RegisterDefinition * reg = NULL;
        if (!cpp_ref && loc_state != NULL && loc_state->pieces_cnt == 1 && loc_state->pieces->implicit_pointer == 0 &&
            loc_state->pieces->reg != NULL && loc_state->pieces->reg->size == loc_state->pieces->size) reg = loc_state->pieces->reg;
        sprintf(expr, "${%s}", symbol2id(sym));
        if (evaluate_expression(elf_ctx, frame, pc, expr, 0, &v) < 0) {
            error_sym("evaluate_expression", sym);
        }
        if (indirect) {
            if (v.sym != NULL) {
                set_errno(ERR_OTHER, "Value.sym != NULL");
                error_sym("evaluate_expression", sym);
            }
        }
        else if (!cpp_ref) {
            if (v.sym == NULL) {
                set_errno(ERR_OTHER, "Value.sym = NULL");
                error_sym("evaluate_expression", sym);
            }
            if (symcmp(sym, v.sym) != 0) {
                set_errno(ERR_OTHER, "Invalid Value.sym");
                error_sym("evaluate_expression", sym);
            }
        }
        else {
            if (v.sym != NULL) {
                set_errno(ERR_OTHER, "Value.sym != NULL");
                error_sym("evaluate_expression", sym);
            }
        }
        if (reg != v.reg) {
            set_errno(ERR_OTHER, "Invalid Value.reg");
            error_sym("evaluate_expression", sym);
        }
        if (!v.remote && !cpp_ref) {
            unsigned n;
            int implicit_pointer = 0;
            if (v.loc != NULL) {
                for (n = 0; n < v.loc->pieces_cnt; n++) {
                    if (v.loc->pieces[n].implicit_pointer) {
                        implicit_pointer = 1;
                        break;
                    }
                }
            }
            if (!implicit_pointer) {
                if (evaluate_expression(elf_ctx, frame, pc, expr, 1, &v) < 0) {
                    error_sym("evaluate_expression", sym);
                }
                if (v.sym == NULL) {
                    set_errno(ERR_OTHER, "Value.sym = NULL");
                    error_sym("evaluate_expression", sym);
                }
                if (symcmp(sym, v.sym) != 0) {
                    set_errno(ERR_OTHER, "Invalid Value.sym");
                    error_sym("evaluate_expression", sym);
                }
                if (reg != v.reg) {
                    set_errno(ERR_OTHER, "Invalid Value.reg");
                    error_sym("evaluate_expression", sym);
                }
            }
        }
    }
    if (name != NULL) {
        Symbol * find_sym = NULL;
        if (find_symbol_by_name(elf_ctx, frame, pc, name, &find_sym) == 0 &&
                symcmp(sym, find_sym) == 0 &&
                symbol_class != SYM_CLASS_COMP_UNIT &&
                !cpp_ref) {
            Value v;
            char * expr = (char *)tmp_alloc(strlen(name) + 16);
            if (size_ok) {
                sprintf(expr, "$\"%s\"", name);
                if (evaluate_expression(elf_ctx, frame, pc, expr, 0, &v) < 0) {
                    error_sym("evaluate_expression", sym);
                }
            }
            if (addr_ok) {
                ContextAddress a0 = addr;
                ContextAddress a1 = 0;
                if (!elf_file->elf64) a0 &= 0xffffffffu;
                sprintf(expr, "&$\"%s\"", name);
                if (evaluate_expression(elf_ctx, frame, pc, expr, 0, &v) < 0) {
                    error_sym("evaluate_expression", sym);
                }
                if (v.size != (elf_file->elf64 ? 8 : 4)) {
                    errno = ERR_INV_ADDRESS;
                    error_sym("evaluate_expression", sym);
                }
                if (value_to_address(&v, &a1) < 0) {
                    error_sym("value_to_address", sym);
                }
                if (a0 != a1 && !indirect) {
                    errno = ERR_INV_ADDRESS;
                    error_sym("value_to_address", sym);
                }
            }
        }
    }
    if (type != NULL) {
        int type_sym_class = 0;
        int type_type_class = 0;
        Symbol * org_type = NULL;
        Symbol * container = NULL;
        int container_class = 0;
        int base_type_class = 0;
        ContextAddress type_length = 0;
        if (get_symbol_class(type, &type_sym_class) < 0) {
            error_sym("get_symbol_class", type);
        }
        if (type_sym_class != SYM_CLASS_TYPE) {
            errno = ERR_OTHER;
            error_sym("Invalid symbol class of a type", type);
        }
        if (get_symbol_type_class(type, &type_type_class) < 0) {
            error_sym("get_symbol_type_class", type);
        }
        if (type_class != type_type_class) {
            errno = ERR_OTHER;
            error("Invalid symbol type class");
        }
        if (get_symbol_flags(type, &flags) < 0) {
            error_sym("get_symbol_flags", type);
        }
        if (flags & SYM_FLAG_TYPEDEF) {
            if (get_symbol_object(type) == NULL) {
                errno = ERR_OTHER;
                error("Invalid DWARF object of typedef");
            }
            if (get_symbol_type(type, &org_type) < 0) {
                error_sym("get_symbol_type", type);
            }
            if (symcmp(type, org_type) == 0) {
                errno = ERR_OTHER;
                error("Invalid original type of typedef");
            }
#if 0
            if (type_name == NULL) {
                errno = ERR_OTHER;
                error("typedef must have a name");
            }
#endif
            if ((flags & SYM_FLAG_CONST_TYPE) || (flags & SYM_FLAG_VOLATILE_TYPE)) {
                errno = ERR_OTHER;
                error("Invalid flags of typedef");
            }
        }
        else if ((flags & SYM_FLAG_CONST_TYPE) || (flags & SYM_FLAG_VOLATILE_TYPE)) {
            if (get_symbol_type(type, &org_type) < 0) {
                error_sym("get_symbol_type", type);
            }
            if (symcmp(type, org_type) == 0) {
                errno = ERR_OTHER;
                error("Invalid original type of modified type");
            }
        }
        if (org_type != NULL) {
            int org_type_class = 0;
            if (get_symbol_type_class(org_type, &org_type_class) < 0) {
                error_sym("get_symbol_type_class", org_type);
            }
            if (type_class != org_type_class) {
                errno = ERR_OTHER;
                error("Invalid symbol type class");
            }
        }
        if (get_symbol_index_type(type, &index_type) < 0) {
            if (type_class == TYPE_CLASS_ARRAY) {
                error_sym("get_symbol_index_type", type);
            }
        }
        else if (org_type != NULL) {
            Symbol * org_index_type = NULL;
            if (get_symbol_index_type(org_type, &org_index_type) < 0) {
                error_sym("get_symbol_index_type", org_type);
            }
            if (symcmp(index_type, org_index_type) != 0) {
                errno = ERR_OTHER;
                error("Invalid index type of typedef");
            }
        }
        if (get_symbol_base_type(type, &base_type) < 0) {
            if (type_class == TYPE_CLASS_ARRAY || type_class == TYPE_CLASS_FUNCTION ||
                type_class == TYPE_CLASS_POINTER || type_class == TYPE_CLASS_MEMBER_PTR) {
                error_sym("get_symbol_base_type", type);
            }
        }
        else {
            char * base_type_name = NULL;
            if (get_symbol_name(base_type, &base_type_name) < 0) {
                error_sym("get_symbol_name", base_type);
            }
            if (base_type_name != NULL) base_type_name = tmp_strdup(base_type_name);
            if (get_symbol_type_class(base_type, &base_type_class) < 0) {
                error_sym("get_symbol_type_class", base_type);
            }
            if (org_type != NULL) {
                Symbol * org_base_type = NULL;
                if (get_symbol_base_type(org_type, &org_base_type) < 0) {
                    error_sym("get_symbol_base_type", org_type);
                }
                if (symcmp(base_type, org_base_type) != 0) {
                    errno = ERR_OTHER;
                    error("Invalid base type of typedef");
                }
            }
        }
        if (get_symbol_container(type, &container) < 0) {
            error_sym("get_symbol_container", type);
        }
        if (get_symbol_class(container, &container_class) < 0) {
            error_sym("get_symbol_class", type);
        }
        if (type_class == TYPE_CLASS_MEMBER_PTR) {
            if (org_type != NULL) {
                Symbol * org_container = NULL;
                if (get_symbol_container(org_type, &org_container) < 0) {
                    error_sym("get_symbol_container", org_type);
                }
                if (symcmp(container, org_container) != 0) {
                    errno = ERR_OTHER;
                    error("Invalid container of typedef");
                }
            }
        }
        else if (container_class == SYM_CLASS_COMP_UNIT && type_name != NULL) {
            /* This test is too slow, don't run it evry time */
            static unsigned cnt = 0;
            if (strcmp(type_name, "int") != 0 && cnt++ % 33 == 0) {
                Symbol * find_sym = NULL;
                unsigned find_cnt = 0;
                unsigned find_max = 16;
                Symbol ** find_syms = (Symbol **)tmp_alloc(sizeof(Symbol *) * find_max);
                unsigned i;
                if (find_symbol_by_name(elf_ctx, STACK_NO_FRAME, 0, type_name, &find_sym) < 0) {
                    error("find_symbol_by_name");
                }
                find_syms[find_cnt++] = find_sym;
                while (find_next_symbol(&find_sym) == 0) {
                    if (find_cnt >= find_max) {
                        find_max *= 2;
                        find_syms = (Symbol **)tmp_realloc(find_syms, sizeof(Symbol *) * find_max);
                    }
                    find_syms[find_cnt++] = find_sym;
                }
                for (i = 0; i < find_cnt; i++) {
                    int find_class = 0;
                    SYM_FLAGS find_flags = 0;
                    ContextAddress find_size = 0;
                    find_sym = find_syms[i];
                    if (get_symbol_class(find_sym, &find_class) < 0) {
                        error("get_symbol_class");
                    }
                    if (get_symbol_flags(find_sym, &find_flags) < 0) {
                        error("get_symbol_flags");
                    }
                    switch (find_class) {
                    case SYM_CLASS_TYPE:
                        if (get_symbol_size(find_sym, &find_size) == 0) {
                            Value v;
                            uint64_t n = 0;
                            char * expr = (char *)tmp_alloc(512);
                            const char * id = symbol2id(find_sym);
                            sprintf(expr, "sizeof(${%s})", id);
                            if (evaluate_expression(elf_ctx, STACK_NO_FRAME, 0, expr, 0, &v) < 0) {
                                error("evaluate_expression");
                            }
                            if (value_to_unsigned(&v, &n) < 0) {
                                error("value_to_unsigned");
                            }
                            if (n != find_size) {
                                errno = ERR_OTHER;
                                error("invalid result of evaluate_expression");
                            }
                        }
                        break;
                    }
                }
            }
        }
        if (get_symbol_length(sym, &length) < 0) {
            if (type_class == TYPE_CLASS_ARRAY) {
                int ok = 0;
                int err = errno;
                ObjectInfo * obj = get_symbol_object(sym);
                if (!ok && errcmp(err, "Object is not available at this location") == 0) ok = 1;
                if (!ok && obj != NULL && obj->mCompUnit->mLanguage == LANG_ADA95) {
                    if (errcmp(err, "No object location info found in DWARF") == 0) ok = 1;
                }
                if (!ok && obj != NULL && obj->mCompUnit->mLanguage == LANG_ADA95) {
                    if (errcmp(err, "Object does not have memory address") == 0) ok = 1;
                }
                if (!ok && obj != NULL && obj->mCompUnit->mLanguage == LANG_ADA95) {
                    if (errcmp(err, "Cannot get object address: the object is located in a register") == 0) ok = 1;
                }
                if (!ok) {
                    errno = err;
                    error_sym("get_symbol_length", sym);
                }
            }
        }
        else if (get_symbol_length(type, &type_length) < 0) {
            error_sym("get_symbol_length", type);
        }
        else if (length != type_length) {
            errno = ERR_OTHER;
            error("Invalid length of a type");
        }
        else if (org_type != NULL) {
            ContextAddress org_length = 0;
            if (get_symbol_length(org_type, &org_length) < 0) {
                error_sym("get_symbol_length", org_type);
            }
            if (length != org_length) {
                errno = ERR_OTHER;
                error("Invalid length of typedef");
            }
        }
        if (get_symbol_lower_bound(sym, &lower_bound) < 0) {
            if (type_class == TYPE_CLASS_ARRAY) {
                int ok = 0;
                int err = errno;
                ObjectInfo * obj = get_symbol_object(sym);
                if (obj != NULL && obj->mCompUnit->mLanguage == LANG_ADA95) {
                    if (!ok && errcmp(err, "Object is not available at this location") == 0) ok = 1;
                }
                if (obj != NULL && obj->mCompUnit->mLanguage == LANG_ADA95) {
                    if (!ok && errcmp(err, "Object does not have memory address") == 0) ok = 1;
                }
                if (!ok && obj != NULL && obj->mCompUnit->mLanguage == LANG_ADA95) {
                    if (errcmp(err, "Cannot get object address: the object is located in a register") == 0) ok = 1;
                }
                if (!ok) {
                    errno = err;
                    error_sym("get_symbol_lower_bound", sym);
                }
            }
        }
        else if (get_symbol_lower_bound(type, &lower_bound) < 0) {
            error_sym("get_symbol_lower_bound", type);
        }
        else if (org_type != NULL) {
            int64_t org_lower_bound = 0;
            if (get_symbol_lower_bound(org_type, &org_lower_bound) < 0) {
                error_sym("get_symbol_lower_bound", org_type);
            }
            if (lower_bound != org_lower_bound) {
                errno = ERR_OTHER;
                error("Invalid lower bound of typedef");
            }
        }
        if (type_class == TYPE_CLASS_ENUMERATION) {
            test_enumeration_type(type);
        }
        else if (type_class == TYPE_CLASS_COMPOSITE) {
            test_composite_type(type);
        }
        else if (type_class == TYPE_CLASS_POINTER) {
            if (base_type_class == TYPE_CLASS_COMPOSITE &&
                    (flags & SYM_FLAG_PARAMETER) && (flags & SYM_FLAG_ARTIFICIAL) &&
                    name != NULL && strcmp(name, "this") == 0) {
                test_composite_type(base_type);
                test_this_pointer(base_type);
            }
        }
    }
}

static int is_in_segment(U8_T lt_addr) {
    unsigned j;
    for (j = 0; j < elf_file->pheader_cnt; j++) {
        ELF_PHeader * p = elf_file->pheaders + j;
        if (p->type != PT_LOAD) continue;
        if (lt_addr >= p->address && lt_addr < p->address + p->mem_size) return 1;
    }
    return 0;
}

static void test_public_names(void) {
    DWARFCache * cache = get_dwarf_cache(get_dwarf_file(elf_file));
    unsigned n = 0;
    unsigned m = 0;
    time_t time_start = time(0);
    while (n < cache->mPubNames.mCnt) {
        ObjectInfo * obj = cache->mPubNames.mNext[n++].mObject;
        if (obj != NULL) {
            Symbol * sym1 = NULL;
            Symbol * sym2 = NULL;
            ContextAddress addr = 0;
            if (find_symbol_by_name(elf_ctx, STACK_NO_FRAME, 0, obj->mName, &sym1) < 0) {
                error("find_symbol_by_name");
            }
            if (obj->mCompUnit->mLanguage == LANG_C) {
                /* Note: this test fails for C++ because of name overloading */
                SYM_FLAGS flags = 0;
                if (get_symbol_flags(sym1, &flags) < 0) error("get_symbol_flags");
                if (obj->mTag == TAG_subprogram && (flags & SYM_FLAG_EXTERNAL) != 0 && get_symbol_address(sym1, &addr) == 0) {
                    /* Check weak symbol is not the first symbol */
                    while (find_next_symbol(&sym2) == 0) {
                        ContextAddress nxt_addr = 0;
                        if (get_symbol_object(sym2) == NULL && get_symbol_address(sym2, &nxt_addr) == 0) {
                            if (get_symbol_flags(sym2, &flags) < 0) error("get_symbol_flags");
                            if ((flags & SYM_FLAG_EXTERNAL) != 0 && addr != nxt_addr) {
                                set_errno(ERR_OTHER, "Invalid address - weak symbol?");
                                error("find_symbol_by_name");
                            }
                        }
                    }
                }
            }
            if (find_symbol_by_name(elf_ctx, STACK_TOP_FRAME, 0, obj->mName, &sym2) < 0) {
                error("find_symbol_by_name");
            }
            if (get_symbol_object(sym1) != get_symbol_object(sym2)) {
                /* Something else with same name found in the top frame.
                 * Cannot call loc_var_func() - it will fail. */
            }
            else {
                loc_var_func(NULL, sym2);
            }
        }
        if ((n % 10) == 0) {
            tmp_gc();
            if (time(0) - time_start >= 120) break;
        }
    }
    for (m = 1; m < elf_file->section_cnt; m++) {
        ELF_Section * tbl = elf_file->sections + m;
        if (tbl->sym_names_hash == NULL) continue;
        time_start = time(0);
        for (n = 0; n < tbl->sym_names_hash_size; n++) {
            Trap trap;
            if (set_trap(&trap)) {
                ELF_SymbolInfo sym_info;
                unpack_elf_symbol_info(tbl, n, &sym_info);
                if (sym_info.name && sym_info.section_index != SHN_UNDEF && sym_info.type != STT_FILE) {
                    Symbol * sym = NULL;
                    Symbol * sym1 = NULL;
                    Symbol * sym2 = NULL;
                    if (elf_tcf_symbol(elf_ctx, &sym_info, &sym) < 0) {
                        error("elf_tcf_symbol");
                    }
                    if (file_addr_offs) {
                        switch (sym_info.type) {
                        case STT_OBJECT:
                        case STT_FUNC:
                            if (sym_info.section != NULL) {
                                U8_T value = sym_info.value;
                                ContextAddress addr = 0;
                                ContextAddress lt = 0;
                                ELF_File * lt_file = NULL;
                                ELF_Section * lt_sec = NULL;
                                if (elf_file->type == ET_REL) {
                                    value += sym_info.section->addr;
                                }
                                if (is_in_segment(value)) {
                                    if (get_symbol_address(sym, &addr) < 0) {
                                        error("get_symbol_address");
                                    }
                                    if (addr != value + file_addr_offs) {
                                        set_errno(ERR_OTHER, "Invalid address - broken relocation logic?");
                                        error("get_symbol_address");
                                    }
                                    lt = elf_map_to_link_time_address(elf_ctx, addr, 0, &lt_file, &lt_sec);
                                    if (lt != value) {
                                        set_errno(ERR_OTHER, "Invalid address - broken relocation logic?");
                                        error("elf_map_to_link_time_address");
                                    }
                                }
                            }
                            break;
                        }
                    }
                    loc_var_func(NULL, sym);
                    if (find_symbol_by_name(elf_ctx, STACK_NO_FRAME, 0, sym_info.name, &sym1) < 0) {
                        error("find_symbol_by_name");
                    }
                    if (find_symbol_by_name(elf_ctx, STACK_TOP_FRAME, 0, sym_info.name, &sym2) < 0) {
                        error("find_symbol_by_name");
                    }
                    if (get_symbol_object(sym1) != get_symbol_object(sym2)) {
                        /* Something else with same name found in the top frame.
                         * Cannot call loc_var_func() - it will fail. */
                    }
                    else {
                        loc_var_func(NULL, sym2);
                    }
                }
                clear_trap(&trap);
            }
            else {
                error("unpack_elf_symbol_info");
            }
            if ((n % 10) == 0) {
                tmp_gc();
                if (time(0) - time_start >= 120) break;
            }
        }
    }
}

static void check_addr_ranges(void) {
    DWARFCache * cache = get_dwarf_cache(get_dwarf_file(elf_file));
    if (cache->mAddrRangesCnt > 1) {
        unsigned i;
        unsigned n = 0;
        for (i = 0; i < cache->mAddrRangesCnt - 1; i++) {
            UnitAddressRange * x = cache->mAddrRanges + i;
            UnitAddressRange * y = cache->mAddrRanges + i + 1;
            if (x->mSection == y->mSection &&
                    x->mAddr < y->mAddr + y->mSize &&
                    y->mAddr < x->mAddr + x->mSize) {
                if (n < 20) {
                    printf("Overlapping address ranges: %08x %08x, %08x %08x\n",
                        (unsigned)x->mAddr, (unsigned)x->mSize,
                        (unsigned)y->mAddr, (unsigned)y->mSize);
                }
                n++;
            }
        }
        if (n >= 20) printf("Overlapping address ranges: total %d ranges ...\n", n);
    }
}

static void next_pc(void) {
    Symbol * sym = NULL;
    ContextAddress lt_addr;
    ContextAddress rt_addr;
    ELF_File * lt_file;
    ELF_Section * lt_sec;
    ObjectInfo * func_object = NULL;
    char * func_name = NULL;
    struct timespec time_now;
    Trap trap;
    int test_cnt = 0;
    int loaded = mem_region_pos < 0;
    ContextAddress next_pc = pc + 1;
    UnitAddressRange * unit_range = NULL;
    ContextAddress rage_rt_addr = 0;

    for (;;) {
        if (mem_region_pos < 0) {
            mem_region_pos = 0;
            pc = mem_map.regions[mem_region_pos].addr;
        }
        else if (next_pc < mem_map.regions[mem_region_pos].addr + mem_map.regions[mem_region_pos].size) {
            pc = next_pc;
        }
        else if (mem_region_pos + 1 < (int)mem_map.region_cnt) {
            mem_region_pos++;
            pc = mem_map.regions[mem_region_pos].addr;
        }
        else {
            mem_region_pos++;
            pc = 0;
            print_time(time_start, test_cnt);
            return;
        }

        while ((mem_map.regions[mem_region_pos].flags & MM_FLAG_X) == 0 || mem_map.regions[mem_region_pos].size == 0) {
            if (mem_region_pos + 1 < (int)mem_map.region_cnt) {
                mem_region_pos++;
                pc = mem_map.regions[mem_region_pos].addr;
            }
            else {
                mem_region_pos++;
                pc = 0;
                print_time(time_start, test_cnt);
                return;
            }
        }

        set_regs_PC(elf_ctx, pc);
        send_context_changed_event(elf_ctx);

        func_name = NULL;
        func_object = NULL;
        if (find_symbol_by_addr(elf_ctx, STACK_NO_FRAME, pc, &sym) < 0) {
            if (get_error_code(errno) != ERR_SYM_NOT_FOUND) {
                error("find_symbol_by_addr");
            }
        }
        else {
            int i;
            ContextAddress func_addr = 0;
            ContextAddress func_size = 0;
            Symbol * func_type = NULL;
            int func_children_count = 0;
            Symbol ** func_children = NULL;
            SYM_FLAGS flags = 0;
            func_object = get_symbol_object(sym);
            if (get_symbol_name(sym, &func_name) < 0) {
                error_sym("get_symbol_name", sym);
            }
            if (func_name != NULL) func_name = tmp_strdup(func_name);
            if (get_symbol_address(sym, &func_addr) < 0) {
                error_sym("get_symbol_address", sym);
            }
            if (get_symbol_size(sym, &func_size) < 0) {
                error_sym("get_symbol_size", sym);
            }
            if (get_symbol_type(sym, &func_type) < 0) {
                error_sym("get_symbol_type", sym);
            }
            if (get_symbol_flags(sym, &flags) < 0) {
                error_sym("get_symbol_flags", sym);
            }
            if (func_type != NULL) {
                if (get_symbol_children(func_type, &func_children, &func_children_count) < 0) {
                    error_sym("get_symbol_children", func_type);
                }
                for (i = 0; i < func_children_count; i++) {
                    Symbol * arg_type = NULL;
                    int arg_class = 0;
                    ContextAddress arg_size = 0;
                    if (get_symbol_type(func_children[i], &arg_type) < 0) {
                        error_sym("get_symbol_type", func_children[i]);
                    }
                    if (get_symbol_class(func_children[i], &arg_class) < 0) {
                        error_sym("get_symbol_class", func_children[i]);
                    }
                    if (get_symbol_size(func_children[i], &arg_size) < 0) {
                        int error = errno;
                        ObjectInfo * obj = get_symbol_object(sym);
                        if (obj != NULL && obj->mCompUnit->mLanguage == LANG_ADA95 &&
                                errcmp(error, "Object location is relative to a stack frame") == 0) {
                            /* OK */
                        }
                        else {
                            errno = error;
                            error_sym("get_symbol_size", func_children[i]);
                        }
                    }
                }
            }
            if (pc < func_addr || (func_size > 0 && pc >= func_addr + func_size)) {
                errno = ERR_OTHER;
                error("invalid symbol address");
            }
            if (func_name != NULL) {
                Symbol * fnd_sym = NULL;
                if (find_symbol_by_name(elf_ctx, STACK_TOP_FRAME, 0, func_name, &fnd_sym) < 0) {
                    if (get_error_code(errno) != ERR_SYM_NOT_FOUND) {
                        error("find_symbol_by_name");
                    }
                }
                else {
                    Symbol * fnd_scp_sym = NULL;
                    char * fnd_name = NULL;
                    ContextAddress fnd_addr = 0;
                    if (find_symbol_in_scope(elf_ctx, STACK_TOP_FRAME, pc, NULL, func_name, &fnd_scp_sym) < 0) {
                        if (get_error_code(errno) != ERR_SYM_NOT_FOUND) {
                            error("find_symbol_in_scope");
                        }
                    }
                    if (get_symbol_name(fnd_sym, &fnd_name) < 0) {
                        error_sym("get_symbol_name", fnd_sym);
                    }
                    if (fnd_name == NULL) {
                        errno = ERR_OTHER;
                        error_sym("invalid symbol name", fnd_sym);
                    }
                    if (strcmp(fnd_name, func_name) != 0) {
                        errno = ERR_OTHER;
                        error_sym("strcmp(name_buf, name)", fnd_sym);
                    }
                    if (get_symbol_address(fnd_sym, &fnd_addr) == 0) {
                        Value v;
                        SYM_FLAGS flags = 0;
                        char * expr = (char *)tmp_alloc(strlen(func_name) + 16);
                        if (get_symbol_flags(fnd_sym, &flags) < 0) {
                            error_sym("get_symbol_flags", fnd_sym);
                        }
                        sprintf(expr, "$\"%s\"", func_name);
                        if (evaluate_expression(elf_ctx, STACK_TOP_FRAME, 0, expr, 0, &v) < 0) {
                            error_sym("evaluate_expression", fnd_sym);
                        }
                        if (flags & SYM_FLAG_EXTERNAL) {
                            if (find_symbol_by_name(elf_ctx, STACK_NO_FRAME, 0, func_name, &fnd_sym) < 0) {
                                error("find_symbol_by_name");
                            }
                        }
                    }
                }
            }
        }

        if (func_object != NULL) {
            Symbol * func_type = NULL;
            Symbol * ret_type1 = NULL;
            Symbol * ret_type2 = NULL;
            if (get_symbol_type(sym, &func_type) < 0) {
                error_sym("get_symbol_type", sym);
            }
            if (get_symbol_base_type(sym, &ret_type1) < 0) {
                error_sym("get_symbol_base_type", sym);
            }
            if (get_symbol_base_type(func_type, &ret_type2) < 0) {
                error_sym("get_symbol_base_type", func_type);
            }
            if (symcmp(ret_type1, ret_type2) != 0) {
                errno = ERR_OTHER;
                error_sym("symcmp(ret_type, ret_type2)", sym);
            }
        }

        unit_range = elf_find_unit(elf_ctx, pc, pc, &rage_rt_addr);
        if (unit_range == NULL && errno) error("elf_find_unit");
        if (unit_range != NULL) {
            ELF_Section * sec = unit_range->mUnit->mFile->sections + unit_range->mSection;
            ContextAddress addr = elf_map_to_run_time_address(elf_ctx, sec->file, sec, unit_range->mAddr);
            if (addr > pc || addr + unit_range->mSize <= pc) {
                set_errno(ERR_OTHER, "Invalid compile unit address");
                error("elf_find_unit");
            }
            if (func_object != NULL && unit_range->mUnit != func_object->mCompUnit) {
                set_errno(ERR_OTHER, "Invalid compile unit");
                error("elf_find_unit");
            }
        }
        else if (func_object != NULL) {
            set_errno(ERR_OTHER, "Compile unit not found");
            error("elf_find_unit");
        }

        if (find_symbol_by_name(elf_ctx, STACK_TOP_FRAME, 0, "@ non existing name @", &sym) < 0) {
            if (get_error_code(errno) != ERR_SYM_NOT_FOUND) {
                error("find_symbol_by_name");
            }
        }

        area_cnt = 0;
        line_area_ok = 0;
        if (address_to_line(elf_ctx, pc, pc + 1, addr_to_line_callback, NULL) < 0) {
            error("address_to_line");
        }
        next_pc = pc + 8;
        if (area_cnt > 0) {
            unsigned i;
            if (area_buf[0].end_address > pc) next_pc = area_buf[0].end_address + test_cnt % 6;
            if (next_pc > pc + 12) next_pc = pc + 12;
            if (area_cnt > 1) {
                printf("Overlapping line info ranges: %08x %08x, %08x %08x\n",
                    (unsigned)area_buf[0].start_address, (unsigned)area_buf[0].end_address,
                    (unsigned)area_buf[1].start_address, (unsigned)area_buf[1].end_address);
            }
            for (i = 0; i < area_cnt; i++) {
                CodeArea area = area_buf[i];
                char * elf_file_name = tmp_strdup(area.file);
                if (area.start_address > pc || area.end_address <= pc) {
                    errno = set_errno(ERR_OTHER, "Invalid line area address");
                    error("address_to_line");
                }
                if (line_to_address(elf_ctx, elf_file_name, area.start_line, area.start_column, line_to_addr_callback, &area) < 0) {
                    error("line_to_address");
                }
                if (!line_area_ok) {
                    errno = set_errno(ERR_OTHER, "Invalid line area address");
                    error("line_to_address");
                }
                line_info_cnt++;
            }
        }
        else {
            unsigned i;
            if (address_to_line(elf_ctx, pc >= 2 ? pc - 2: pc, pc + 3, addr_to_line_callback_p1, NULL) < 0) {
                error("address_to_line");
            }
            for (i = 0; i < area_cnt; i++) {
                CodeArea area = area_buf[i];
                if (pc >= area.start_address && pc < area.end_address) {
                    if (unit_range == NULL) {
                        printf("Incomplete or conflicting data in .debug_aranges section for %08x\n", (unsigned)pc);
                    }
                    else {
                        errno = set_errno(ERR_OTHER, "Line info not found");
                        error("address_to_line");
                    }
                }
            }
        }

        lt_file = NULL;
        lt_sec = NULL;
        lt_addr = elf_map_to_link_time_address(elf_ctx, pc, 0, &lt_file, &lt_sec);
        if (errno) error("elf_map_to_link_time_address");
        assert(lt_file != NULL);
        assert(lt_file == elf_file);
        assert(lt_sec == NULL || lt_sec->file == lt_file);
        assert(pc == elf_map_to_run_time_address(elf_ctx, lt_file, lt_sec, lt_addr));
        if (set_trap(&trap)) {
            get_dwarf_stack_frame_info(elf_ctx, lt_file, lt_sec, lt_addr);
            clear_trap(&trap);
        }
        else {
            error("get_dwarf_stack_frame_info 0");
        }

        lt_file = NULL;
        lt_sec = NULL;
        lt_addr = elf_map_to_link_time_address(elf_ctx, pc, 1, &lt_file, &lt_sec);
        if (errno) error("elf_map_to_link_time_address");
        assert(lt_file != NULL);
        assert(lt_file == get_dwarf_file(elf_file));
        assert(lt_sec == NULL || lt_sec->file == lt_file);
        rt_addr = elf_map_to_run_time_address(elf_ctx, lt_file, lt_sec, lt_addr);
        if (pc != rt_addr) {
            set_errno(ERR_OTHER, "Invalid address - broken relocation logic?");
            error("elf_map_to_run_time_address");
        }
        if (set_trap(&trap)) {
            get_dwarf_stack_frame_info(elf_ctx, lt_file, lt_sec, lt_addr);
            clear_trap(&trap);
        }
        else {
            error("get_dwarf_stack_frame_info 1");
        }

        if (enumerate_symbols(elf_ctx, STACK_TOP_FRAME, loc_var_func, NULL) < 0) {
            error("enumerate_symbols");
        }

        if (func_object != NULL) {
            if (set_trap(&trap)) {
                StackFrame * frame = NULL;
                if (get_frame_info(elf_ctx, STACK_TOP_FRAME, &frame) < 0) exception(errno);
                if (frame->fp != frame_addr) {
                    PropertyValue v;
                    uint64_t addr = 0;
                    memset(&v, 0, sizeof(v));
                    read_and_evaluate_dwarf_object_property(elf_ctx, STACK_TOP_FRAME, func_object, AT_frame_base, &v);
                    if (v.mPieceCnt == 1 && v.mPieces[0].reg != NULL && v.mPieces[0].bit_size == 0) {
                        if (read_reg_value(frame, v.mPieces[0].reg, &addr) < 0) exception(errno);
                    }
                    else {
                        addr = get_numeric_property_value(&v);
                    }
                    if (addr != frame->fp) {
                        /* AT_frame_base is not valid in prologue and epilogue.
                        str_exception(ERR_OTHER, "Invalid FP");
                        */
                    }
                }
                clear_trap(&trap);
            }
            else if (trap.error != ERR_SYM_NOT_FOUND) {
                error("AT_frame_base");
            }
        }

        if (func_object != NULL) {
            set_regs_PC(elf_ctx, 0);
            send_context_changed_event(elf_ctx);
            if (find_symbol_by_addr(elf_ctx, STACK_TOP_FRAME, pc, &sym) < 0) {
                error("find_symbol_by_addr");
            }
        }

        test_cnt++;
        tmp_gc();

        if (loaded) {
            struct timespec time_diff;
            clock_gettime(CLOCK_REALTIME, &time_now);
            time_diff.tv_sec = time_now.tv_sec - time_start.tv_sec;
            if (time_now.tv_nsec < time_start.tv_nsec) {
                time_diff.tv_sec--;
                time_diff.tv_nsec = time_now.tv_nsec + 1000000000 - time_start.tv_nsec;
            }
            else {
                time_diff.tv_nsec = time_now.tv_nsec - time_start.tv_nsec;
            }
            printf("load time: %ld.%06ld\n", (long)time_diff.tv_sec, time_diff.tv_nsec / 1000);
            fflush(stdout);
            time_start = time_now;
            loaded = 0;
            test_public_names();
            clock_gettime(CLOCK_REALTIME, &time_now);
            time_diff.tv_sec = time_now.tv_sec - time_start.tv_sec;
            if (time_now.tv_nsec < time_start.tv_nsec) {
                time_diff.tv_sec--;
                time_diff.tv_nsec = time_now.tv_nsec + 1000000000 - time_start.tv_nsec;
            }
            else {
                time_diff.tv_nsec = time_now.tv_nsec - time_start.tv_nsec;
            }
            printf("pub names time: %ld.%06ld\n", (long)time_diff.tv_sec, time_diff.tv_nsec / 1000);
            fflush(stdout);
            check_addr_ranges();
            time_start = time_now;
        }
        else if (test_cnt >= 10000) {
            print_time(time_start, test_cnt);
            clock_gettime(CLOCK_REALTIME, &time_start);
            return;
        }
    }
}

static void next_file(void) {
    unsigned j;
    ELF_File * f = NULL;
    struct stat st;

    if (pass_cnt == files_cnt) exit(0);
    elf_file_name = files[pass_cnt % files_cnt];

    printf("File: %s\n", elf_file_name);
    fflush(stdout);
    if (stat(elf_file_name, &st) < 0) {
        printf("Cannot stat ELF: %s\n", errno_to_str(errno));
        exit(1);
    }

    clock_gettime(CLOCK_REALTIME, &time_start);

    f = elf_open(elf_file_name);
    if (f == NULL) {
        printf("Cannot open ELF: %s\n", errno_to_str(errno));
        exit(1);
    }

    if (elf_ctx == NULL) {
        elf_ctx = create_context("test");
        elf_ctx->stopped = 1;
        elf_ctx->pending_intercept = 1;
        elf_ctx->mem = elf_ctx;
        elf_ctx->big_endian = f->big_endian;
        list_add_first(&elf_ctx->ctxl, &context_root);
        elf_ctx->ref_count++;
    }

    file_addr_offs = 0;
    context_clear_memory_map(&mem_map);
    for (j = 0; j < f->pheader_cnt; j++) {
        MemoryRegion * r = NULL;
        ELF_PHeader * p = f->pheaders + j;
        if (p->type != PT_LOAD) continue;
        file_addr_offs = 0x10000;
        if (p->file_size > 0) {
            if (mem_map.region_cnt >= mem_map.region_max) {
                mem_map.region_max += 8;
                mem_map.regions = (MemoryRegion *)loc_realloc(mem_map.regions, sizeof(MemoryRegion) * mem_map.region_max);
            }
            r = mem_map.regions + mem_map.region_cnt++;
            memset(r, 0, sizeof(MemoryRegion));
            r->addr = (ContextAddress)p->address + file_addr_offs; /* Relocated */
            r->file_name = loc_strdup(elf_file_name);
            r->file_offs = p->offset;
            r->size = (ContextAddress)p->file_size;
            r->flags = MM_FLAG_R | MM_FLAG_W;
            if (p->flags & PF_X) r->flags |= MM_FLAG_X;
            r->dev = st.st_dev;
            r->ino = st.st_ino;
        }
        if (p->file_size < p->mem_size) {
            if (mem_map.region_cnt >= mem_map.region_max) {
                mem_map.region_max += 8;
                mem_map.regions = (MemoryRegion *)loc_realloc(mem_map.regions, sizeof(MemoryRegion) * mem_map.region_max);
            }
            r = mem_map.regions + mem_map.region_cnt++;
            memset(r, 0, sizeof(MemoryRegion));
            r->bss = 1;
            r->addr = (ContextAddress)(p->address + p->file_size) + file_addr_offs; /* Relocated */
            r->file_name = loc_strdup(elf_file_name);
            r->file_offs = p->offset + p->file_size;
            r->size = (ContextAddress)(p->mem_size - p->file_size);
            r->flags = MM_FLAG_R | MM_FLAG_W;
            if (p->flags & PF_X) r->flags |= MM_FLAG_X;
            r->dev = st.st_dev;
            r->ino = st.st_ino;
        }
        if (r != NULL && (p->flags & PF_X) == 0 && (r->addr + r->size) % p->align != 0) {
            r->size = r->size + p->align - (r->addr + r->size) % p->align;
        }
    }
    if (mem_map.region_cnt == 0) {
        ContextAddress addr = 0x10000;
        for (j = 0; j < f->section_cnt; j++) {
            ELF_Section * sec = f->sections + j;
            if (sec->name == NULL) continue;
            if (sec->flags & SHF_ALLOC) {
                MemoryRegion * r = NULL;
                if (mem_map.region_cnt >= mem_map.region_max) {
                    mem_map.region_max += 8;
                    mem_map.regions = (MemoryRegion *)loc_realloc(mem_map.regions, sizeof(MemoryRegion) * mem_map.region_max);
                }
                r = mem_map.regions + mem_map.region_cnt++;
                memset(r, 0, sizeof(MemoryRegion));
                r->addr = addr;
                r->size = (ContextAddress)sec->size;
                r->file_offs = sec->offset;
                if (sec->type == SHT_NOBITS) {
                    r->bss = 1;
                }
                r->dev = st.st_dev;
                r->ino = st.st_ino;
                r->file_name = loc_strdup(elf_file_name);
                r->sect_name = loc_strdup(sec->name);
                r->flags = MM_FLAG_R;
                if (sec->flags & SHF_WRITE) r->flags |= MM_FLAG_W;
                if (sec->flags & SHF_EXECINSTR) r->flags |= MM_FLAG_X;
                if (strcmp(sec->name, ".text") == 0) r->flags |= MM_FLAG_X;
                if (strcmp(sec->name, ".init.text") == 0) r->flags |= MM_FLAG_X;
                if (strcmp(sec->name, ".exit.text") == 0) r->flags |= MM_FLAG_X;
                addr += r->size;
                addr = (addr + 0x10000) & ~(ContextAddress)0xffff;
            }
        }
    }
    if (mem_map.region_cnt == 0) {
        printf("File has no program headers.\n");
        exit(1);
    }
    memory_map_event_module_loaded(elf_ctx);
    mem_region_pos = -1;

    line_info_cnt = 0;
    file_has_line_info = 0;
    for (j = 0; j < f->section_cnt; j++) {
        ELF_Section * sec = f->sections + j;
        if (sec->name == NULL) continue;
        if (sec->size > 0 && (strcmp(sec->name, ".debug_line") == 0 || strcmp(sec->name, ".line") == 0)) {
            file_has_line_info = 1;
        }
    }

    reg_size = 0;
    memset(reg_defs, 0, sizeof(reg_defs));
    memset(reg_vals, 0, sizeof(reg_vals));
    for (j = 0; j < MAX_REGS - 1; j++) {
        RegisterDefinition * r = reg_defs + j;
        r->big_endian = f->big_endian;
        r->dwarf_id = (int16_t)(j == 0 ? MAX_REGS : j - 1);
        r->eh_frame_id = r->dwarf_id;
        r->name = reg_names[j];
        if (j == 0) {
            snprintf(reg_names[j], sizeof(reg_names[j]), "PC");
        }
        else {
            snprintf(reg_names[j], sizeof(reg_names[j]), "R%d", j - 1);
        }
        r->offset = reg_size;
        r->size = f->elf64 ? 8 : 4;
        if (j == 0) {
            r->role = "PC";
            r->no_write = 1;
        }
        reg_size += r->size;
    }

    pc = 0;
    elf_file = f;
    pass_cnt++;
}

static void test(void * args) {
    assert(test_posted);
    test_posted = 0;
    if (elf_file_name == NULL || mem_region_pos >= (int)mem_map.region_cnt) {
        if (file_has_line_info && line_info_cnt == 0) {
            set_errno(ERR_OTHER, "Line info not accessable");
            error("address_to_line");
        }
        next_file();
    }
    else {
        next_pc();
    }
    assert(test_posted == 0);
    test_posted = 1;
    post_event_with_delay(test, NULL, 1);
}

static void add_dir(const char * dir_name) {
    DIR * dir = opendir(dir_name);
    if (dir == NULL) {
        printf("Cannot open '%s' directory\n", dir_name);
        fflush(stdout);
        exit(1);
    }
    for (;;) {
        struct dirent * e = readdir(dir);
        char path[FILE_PATH_SIZE];
        struct stat st;
        if (e == NULL) break;
        if (strcmp(e->d_name, ".") == 0) continue;
        if (strcmp(e->d_name, "..") == 0) continue;
        if (strcmp(e->d_name + strlen(e->d_name) - 6, ".debug") == 0) continue;
        if (strcmp(e->d_name + strlen(e->d_name) - 4, ".txt") == 0) continue;
        snprintf(path, sizeof(path), "%s/%s", dir_name, e->d_name);
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                add_dir(path);
            }
            else {
                int fd = open(path, O_RDONLY | O_BINARY, 0);
                if (fd < 0) {
                    printf("File %s: %s\n", path, errno_to_str(errno));
                }
                else {
                    close(fd);
                    if (files_cnt >= files_max) {
                        files_max += 8;
                        files = (char **)loc_realloc(files, files_max * sizeof(char *));
                    }
                    files[files_cnt++] = loc_strdup(path);
                }
            }
        }
    }
    closedir(dir);
}

void init_contexts_sys_dep(void) {
    const char * dir_name = ".";
    add_dir(dir_name);
    test_posted = 1;
    post_event(test, NULL);
}
