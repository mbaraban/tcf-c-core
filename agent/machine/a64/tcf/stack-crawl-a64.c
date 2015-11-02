/*******************************************************************************
 * Copyright (c) 2014 Xilinx, Inc. and others.
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
 *     Xilinx - initial API and implementation
 *******************************************************************************/

/*
 * This module implements stack crawl for ARM AArch64.
 */

#include <tcf/config.h>

#if ENABLE_DebugContext

#include <assert.h>
#include <tcf/framework/errors.h>
#include <tcf/framework/cpudefs.h>
#include <tcf/framework/context.h>
#include <tcf/framework/trace.h>
#include <tcf/services/stacktrace.h>
#include <machine/a64/tcf/stack-crawl-a64.h>

#define MEM_HASH_SIZE       61
#define BRANCH_LIST_SIZE    12

#define REG_VAL_ADDR         1
#define REG_VAL_STACK        2
#define REG_VAL_OTHER        3

typedef struct {
    uint64_t v;
    unsigned o;
} RegData;

typedef struct {
    uint64_t v[MEM_HASH_SIZE]; /* Value */
    uint64_t a[MEM_HASH_SIZE]; /* Address */
    uint8_t  used[MEM_HASH_SIZE];
    uint8_t  tracked[MEM_HASH_SIZE];
} MemData;

typedef struct {
    uint64_t addr;
    RegData reg_data[32];
    RegData cpsr_data;
    RegData pc_data;
    MemData mem_data;
} BranchData;

static Context * stk_ctx = NULL;
static StackFrame * stk_frame = NULL;
static RegData reg_data[32];
static RegData cpsr_data;
static RegData pc_data;
static MemData mem_data;
static unsigned mem_cache_idx = 0;
static int trace_return = 0;
static int trace_branch = 0;

static unsigned branch_pos = 0;
static unsigned branch_cnt = 0;
static BranchData branch_data[BRANCH_LIST_SIZE];

static uint32_t instr;

typedef struct {
    uint64_t addr;
    uint32_t size;
    uint8_t data[64];
} MemCache;

#define MEM_CACHE_SIZE       8
static MemCache mem_cache[MEM_CACHE_SIZE];

static int read_byte(uint64_t addr, uint8_t * bt) {
    unsigned i = 0;
    MemCache * c = NULL;

    if (addr == 0) {
        errno = ERR_INV_ADDRESS;
        return -1;
    }
    for (i = 0; i < MEM_CACHE_SIZE; i++) {
        c = mem_cache + mem_cache_idx;
        if (c->addr <= addr && (c->addr + c->size < c->addr || c->addr + c->size > addr)) {
            *bt = c->data[addr - c->addr];
            return 0;
        }
        mem_cache_idx = (mem_cache_idx + 1) % MEM_CACHE_SIZE;
    }
    mem_cache_idx = (mem_cache_idx + 1) % MEM_CACHE_SIZE;
    c = mem_cache + mem_cache_idx;
    c->addr = addr;
    c->size = sizeof(c->data);
    if (context_read_mem(stk_ctx, (ContextAddress)addr, c->data, c->size) < 0) {
        int error = errno;
        MemoryErrorInfo info;
        if (context_get_mem_error_info(&info) < 0 || info.size_valid == 0) {
            c->size = 0;
            errno = error;
            return -1;
        }
        c->size = info.size_valid;
    }
    *bt = c->data[0];
    return 0;
}

static int read_u16(uint64_t addr, uint16_t * w) {
    unsigned i;
    uint16_t n = 0;
    for (i = 0; i < 2; i++) {
        uint8_t bt = 0;
        if (read_byte(addr + i, &bt) < 0) return -1;
        n |= (uint32_t)bt << (i * 8);
    }
    *w = n;
    return 0;
}

static int read_u32(uint64_t addr, uint32_t * w) {
    unsigned i;
    uint32_t n = 0;
    for (i = 0; i < 4; i++) {
        uint8_t bt = 0;
        if (read_byte(addr + i, &bt) < 0) return -1;
        n |= (uint32_t)bt << (i * 8);
    }
    *w = n;
    return 0;
}

static int read_u64(uint64_t addr, uint64_t * w) {
    unsigned i;
    uint64_t n = 0;
    for (i = 0; i < 8; i++) {
        uint8_t bt = 0;
        if (read_byte(addr + i, &bt) < 0) return -1;
        n |= (uint64_t)bt << (i * 8);
    }
    *w = n;
    return 0;
}

static int mem_hash_index(const uint64_t addr) {
    int v = (int)(addr % MEM_HASH_SIZE);
    int s = v;

    do {
        /* Check if the element is occupied */
        if (mem_data.used[s]) {
            /* Check if it is occupied with the sought data */
            if (mem_data.a[s] == addr)  return s;
        }
        else {
            /* Item is free, this is where the item should be stored */
            return s;
        }

        /* Search the next entry */
        s++;
        if (s >= MEM_HASH_SIZE) s = 0;
    }
    while(s != v);

    /* Search failed, hash is full and the address not stored */
    errno = ERR_OTHER;
    return -1;
}

static int mem_hash_read(uint64_t addr, uint64_t * data, int * tracked) {
    int i = mem_hash_index(addr);

    if (i >= 0 && mem_data.used[i] && mem_data.a[i] == addr) {
        *data    = mem_data.v[i];
        *tracked = mem_data.tracked[i];
        return 0;
    }

    /* Address not found in the hash */
    errno = ERR_OTHER;
    return -1;
}

static int mem_hash_write(uint64_t addr, uint64_t value, int valid) {
    int i = mem_hash_index(addr);

    if (i < 0) {
        set_errno(ERR_OTHER, "Memory hash overflow");
        return -1;
    }

    /* Store the item */
    mem_data.used[i] = 1;
    mem_data.a[i] = addr;
    mem_data.v[i] = valid ? value : 0;
    mem_data.tracked[i] = (uint8_t)valid;
    return 0;
}

static int load_reg(uint64_t addr, int r) {
    int tracked = 0;

    /* Check if the value can be found in the hash */
    if (mem_hash_read(addr, &reg_data[r].v, &tracked) == 0) {
        reg_data[r].o = tracked ? REG_VAL_OTHER : 0;
    }
    else {
        /* Not in the hash, so read from real memory */
        reg_data[r].o = 0;
        if (read_u64(addr, &reg_data[r].v) < 0) return -1;
        reg_data[r].o = REG_VAL_OTHER;
    }
    return 0;
}

static int load_reg_lazy(uint64_t addr, int r) {
    reg_data[r].o = REG_VAL_ADDR;
    reg_data[r].v = addr;
    return 0;
}

static int chk_loaded(int r) {
    if (reg_data[r].o != REG_VAL_ADDR && reg_data[r].o != REG_VAL_STACK) return 0;
    return load_reg(reg_data[r].v, r);
}

static int store_reg(uint64_t addr, int r) {
    unsigned i;
    if (chk_loaded(r) < 0) return -1;
    assert(reg_data[r].o != REG_VAL_ADDR);
    assert(reg_data[r].o != REG_VAL_STACK);
    for (i = 0; i < 32; i++) {
        if (reg_data[i].o != REG_VAL_ADDR && reg_data[i].o != REG_VAL_STACK) continue;
        if (reg_data[i].v >= addr + 8) continue;
        if (reg_data[i].v + 8 <= addr) continue;
        if (load_reg(reg_data[i].v, i) < 0) return -1;
    }
    return mem_hash_write(addr, reg_data[r].v, reg_data[r].o != 0);
}

static int store_invalid(uint64_t addr) {
    unsigned i;
    for (i = 0; i < 32; i++) {
        if (reg_data[i].o != REG_VAL_ADDR && reg_data[i].o != REG_VAL_STACK) continue;
        if (reg_data[i].v >= addr + 8) continue;
        if (reg_data[i].v + 8 <= addr) continue;
        if (load_reg(reg_data[i].v, i) < 0) return -1;
    }
    return mem_hash_write(addr, 0, 0);
}

static void add_branch(uint64_t addr) {
    if (branch_cnt < BRANCH_LIST_SIZE) {
        int add = 1;
        unsigned i = 0;
        for (i = 0; i < branch_cnt; i++) {
            BranchData * b = branch_data + i;
            if (b->addr == addr) {
                add = 0;
                break;
            }
        }
        if (add) {
            BranchData * b = branch_data + branch_cnt++;
            b->addr = addr;
            b->mem_data = mem_data;
            b->cpsr_data = cpsr_data;
            memcpy(b->reg_data, reg_data, sizeof(reg_data));
            b->pc_data.o = REG_VAL_OTHER;
            b->pc_data.v = addr;
        }
    }
}

static int search_reg_value(StackFrame * frame, RegisterDefinition * def, uint64_t * v) {
    for (;;) {
        int n;
        if (read_reg_value(frame, def, v) == 0) return 0;
        if (frame->is_top_frame) break;
        n = get_next_frame(frame->ctx, get_info_frame(frame->ctx, frame));
        if (get_frame_info(frame->ctx, n, &frame) < 0) break;
    }
    errno = ERR_OTHER;
    return -1;
}

static void set_reg(uint32_t r, int sf, uint64_t v) {
    if (!sf) {
        if (r == 31) return;
        /* 32-bit value is zero-extended */
        /* See: Write to general-purpose register from either a 32-bit and 64-bit value */
        v = v & (uint64_t)0xffffffffu;
    }
    reg_data[r].v = v;
    reg_data[r].o = REG_VAL_OTHER;
}

static uint64_t decode_bit_mask(int sf, int n, uint32_t imms, uint32_t immr, int immediate, uint64_t * tmask_res) {
    unsigned len = 6;
    unsigned levels = 0;
    unsigned s, r, diff, esize, d;
    uint64_t welem, telem, wmask, tmask, w_ror;
    unsigned i;

    if (!n) {
        len--;
        while (len > 0 && (imms & (1u << len)) != 0) len--;
        if (len < 1) {
            /* Reserved value */
            return 0;
        }
    }
    levels = (1u << len) - 1;
    if (immediate && (imms & levels) == levels) {
        /* Reserved value */
        return 0;
    }
    s = imms & levels;
    r = immr & levels;
    diff = s - r;
    esize = 1u << len;
    d = diff & levels;
    welem = ((uint64_t)1 << (s + 1)) - 1;
    telem = ((uint64_t)1 << (d + 1)) - 1;
    w_ror = 0;
    for (i = 0; i < esize; i++) {
        if (welem & ((uint64_t)1 << i)) {
            w_ror |= (uint64_t)1 << ((esize + i - r) % esize);
        }
    }
    wmask = 0;
    tmask = 0;
    for (i = 0; i * esize < 64; i++) {
        wmask |= w_ror << i * esize;
        tmask |= telem << i * esize;
    }
    if (!sf) {
        wmask &= 0xffffffff;
        tmask &= 0xffffffff;
    }
    if (tmask_res) *tmask_res = tmask;
    return wmask;
}

static int data_processing_immediate(void) {
    if ((instr & 0x1f000000) == 0x10000000) {
        /* PC-rel. addressing */
        uint64_t base = pc_data.v;
        uint32_t rd = instr & 0x1f;
        uint64_t imm = ((instr >> 29) & 0x3);
        imm |= ((instr >> 5) & 0x7ffff) << 2;
        if (imm & (1u << 20)) imm |= ~((uint64_t)(1u << 20) - 1);
        if (instr & (1u << 31)) {
            imm = imm << 12;
            base &= ~((uint64_t)0xfff);
        }
        set_reg(rd, 1, base + imm);
        return 0;
    }

    if ((instr & 0x1f000000) == 0x11000000) {
        /* Add/subtract (immediate) */
        int sf = (instr & (1u << 31)) != 0;
        uint32_t imm = (instr >> 10) & 0xfff;
        uint32_t op = (instr >> 29) & 3;
        uint32_t rt = instr & 0x1f;
        uint32_t rn = (instr >> 5) & 0x1f;
        uint64_t v = 0;
        switch ((instr >> 22) & 3) {
        case 1: imm = imm << 12; break;
        }
        if ((op == 1 || op == 3) && rt == 31) {
            /* CMN or CMP */
            return 0;
        }
        chk_loaded(rn);
        if (reg_data[rn].o == REG_VAL_OTHER) {
            switch (op) {
            case 0:
            case 1:
                v = reg_data[rn].v + imm;
                break;
            case 2:
            case 3:
                v = reg_data[rn].v - imm;
                break;
            }
            set_reg(rt, sf, v);
        }
        else {
            reg_data[rt].o = 0;
        }
        return 0;
    }

    if ((instr & 0x1f800000) == 0x12000000) {
        /* Logical (immediate) */
        int sf = (instr & (1u << 31)) != 0;
        int n = (instr & (1 << 22)) != 0;
        uint32_t opc = (instr >> 29) & 3;
        uint32_t immr = (instr >> 16) & 0x3f;
        uint32_t imms = (instr >> 10) & 0x3f;
        uint32_t rd = instr & 0x1f;
        uint32_t rn = (instr >> 5) & 0x1f;
        uint64_t v = decode_bit_mask(sf, n, imms, immr, 1, NULL);
        if (rd == 31 && opc == 3) {
             /* TST */
        }
        else if (rn == 31) {
            /* MOV */
            switch (opc) {
            case 0: v = 0; break;
            case 3: v = 0; break;
            }
            set_reg(rd, sf, v);
        }
        else {
            chk_loaded(rn);
            if (reg_data[rn].o == REG_VAL_OTHER) {
                switch (opc) {
                case 0: v = reg_data[rn].v & v; break;
                case 1: v = reg_data[rn].v | v; break;
                case 2: v = reg_data[rn].v ^ v; break;
                case 3: v = reg_data[rn].v & v; break;
                }
                set_reg(rd, sf, v);
            }
            else {
                reg_data[rd].o = 0;
            }
        }
        return 0;
    }

    if ((instr & 0x1f800000) == 0x12800000) {
        /* Move wide (immediate) */
        int sf = (instr & (1u << 31)) != 0;
        uint32_t op = (instr >> 29) & 3;
        uint32_t hw = (instr >> 21) & 3;
        uint64_t imm = (instr >> 5) & 0xffff;
        uint32_t rd = instr & 0x1f;
        uint64_t v = 0;

        if (op == 3) {
            chk_loaded(rd);
            if (reg_data[rd].o != REG_VAL_OTHER) return 0;
            v = reg_data[rd].v;
            v &= ~((uint64_t)0xffff << (hw * 16));
        }
        v |= imm << (hw * 16);
        if (op == 0) v = ~v;
        set_reg(rd, sf, v);
        return 0;
    }

    return 0;
}

static int branch_exception_system(void) {
    if ((instr & 0x7c000000) == 0x14000000) {
        /* Unconditional branch (immediate) */
        int32_t imm = instr & 0x3ffffff;
        if (instr & (1u << 31)) {
            /* bl */
            set_reg(30, 1, pc_data.v + 4);
            return 0;
        }
        if (imm & 0x02000000) imm |= 0xfc000000;
        add_branch(pc_data.v + ((int64_t)imm << 2));
        trace_branch = 1;
        return 0;
    }

    if ((instr & 0xfe000000) == 0x54000000) {
        /* Conditional branch (immediate) */
        int32_t imm = (instr >> 5) & 0x7ffff;
        uint32_t cond = instr & 0xf;
        if (imm & 0x00040000) imm |= 0xfffc0000;
        add_branch(pc_data.v + ((int64_t)imm << 2));
        if (cond == 0xe) trace_branch = 1;
        return 0;
    }

    if ((instr & 0xfe000000) == 0xd6000000) {
        /* Unconditional branch (register) */
        uint32_t opc = (instr >> 21) & 0xf;
        uint32_t op2 = (instr >> 16) & 0x1f;
        uint32_t op3 = (instr >> 10) & 0x3f;
        uint32_t op4 = (instr >>  0) & 0x1f;
        uint32_t rn = (instr >> 5) & 0x1f;
        if (op2 == 31 && op3 == 0 && op4 == 0) {
            switch (opc) {
            case 0: /* br */
                if (chk_loaded(rn) < 0) return -1;
                if (reg_data[rn].o) add_branch(reg_data[rn].v);
                trace_branch = 1;
                break;
            case 1: /* blr */
                set_reg(30, 1, pc_data.v + 4);
                break;
            case 2: /* ret */
                if (chk_loaded(rn) < 0) return -1;
                pc_data = reg_data[rn];
                trace_return = 1;
                break;
            }
        }
        return 0;
    }

    return 0;
}

static int loads_and_stores(void) {
    if ((instr & 0x3b000000) == 0x18000000) {
        /* Load register (literal) */
        uint32_t opc = (instr >> 30) & 3;
        int V = (instr & (1 << 26)) != 0;
        uint32_t imm = (instr >> 5) & 0x7ffff;
        uint32_t rt = (instr >> 0) & 0x1f;
        uint64_t addr = 0;

        if (opc == 3) {
            /* prfm */
            return 0;
        }
        if (imm & 0x40000) {
            addr = pc_data.v - (0x80000 - imm) * 4;
        }
        else {
            addr = pc_data.v + imm * 4;
        }
        if (V) {
            /* Floating Point */
        }
        else {
            reg_data[rt].o = 0;
            if (opc == 1) { /* 64-bit */
                load_reg_lazy(addr, rt);
            }
            else if (opc == 0) {
                uint32_t v = 0;
                if (read_u32(addr, &v) == 0) set_reg(rt, 0, v);
            }
        }
        return 0;
    }

    if ((instr & 0x3a800000) == 0x28800000) {
        /* Load/store register pair (post-indexed) - bit 24 = 0 */
        /* Load/store register pair (pre-indexed) - bit 24 = 1 */
        uint32_t opc = (instr >> 30) & 3;
        int V = (instr & (1 << 26)) != 0;
        int L = (instr & (1 << 22)) != 0;
        int px = (instr & (1 << 24)) != 0;
        uint64_t imm = (instr >> 15) & 0x7f;
        uint32_t rn = (instr >> 5) & 0x1f;
        uint32_t rt1 = (instr >> 0) & 0x1f;
        uint32_t rt2 = (instr >> 10) & 0x1f;
        uint32_t shift = 0;

        if (V) {
            /* Floating Point */
            switch (opc) {
            case 0: shift = 2; break;
            case 1: shift = 3; break;
            case 2: shift = 4; break;
            case 3: return 0;
            }
        }
        else {
            shift = opc >= 2 ? 3 : 2;
        }
        if (imm & 0x40) imm |= ~(uint64_t)0x3f;
        if (chk_loaded(rn) < 0) reg_data[rn].o = 0;
        if (px && reg_data[rn].o) {
            assert(reg_data[rn].o == REG_VAL_OTHER);
            reg_data[rn].v += imm << shift;
        }
        if (!V) {
            if (L) {
                uint64_t addr = reg_data[rn].v;
                reg_data[rt1].o = 0;
                reg_data[rt2].o = 0;
                if (reg_data[rn].o) {
                    if (opc >= 2) { /* 64-bit */
                        load_reg_lazy(addr, rt1);
                        addr += (uint64_t)1 << shift;
                        load_reg_lazy(addr, rt2);
                    }
                    else if (opc != 1) {
                        uint32_t v = 0;
                        if (read_u32(addr, &v) == 0) set_reg(rt1, 0, v);
                        addr += (uint64_t)1 << shift;
                        if (read_u32(addr, &v) == 0) set_reg(rt2, 0, v);
                    }
                }
            }
            else if (reg_data[rn].o) {
                uint64_t addr = reg_data[rn].v;
                if (opc >= 2) { /* 64-bit */
                    store_reg(addr, rt1);
                    addr += (uint64_t)1 << shift;
                    store_reg(addr, rt2);
                }
            }
        }
        if (!px && reg_data[rn].o) {
            assert(reg_data[rn].o == REG_VAL_OTHER);
            reg_data[rn].v += imm << shift;
        }
        return 0;
    }

    {
        uint32_t size = (instr >> 30) & 3;
        uint32_t opc = (instr >> 22) & 3;
        int V = (instr & (1 << 26)) != 0;
        uint32_t rn = (instr >> 5) & 0x1f;
        uint32_t rt = instr & 0x1f;
        int shift = 0;
        int prf = 0;

        if (V) {
            if (opc == 0) {
                switch (size) {
                case 1: shift = 1; break;
                case 2: shift = 2; break;
                case 3: shift = 3; break;
                }
            }
            else {
                switch (size) {
                case 0: shift = 4; break;
                default: shift = -1; break;
                }
            }
        }
        else if (size == 3 && opc == 2) {
            shift = 3;
            prf = 1;
        }
        else {
            shift = size;
        }

        if (shift >= 0) {
            int instr_ok = 0;
            uint64_t addr = 0;
            int addr_ok = 0;

            if ((instr & 0x3b200c00) == 0x38000000) {
                /* Load/store register (unscaled immediate) */
                int64_t imm = (instr >> 12) & 0x1ff;
                if (imm & 0x100) imm |= ~(uint64_t)0xff;
                chk_loaded(rn);
                addr_ok = reg_data[rn].o == REG_VAL_OTHER;
                addr = reg_data[rn].v + imm;
                instr_ok = 1;
            }
            else if ((instr & 0x3b200c00) == 0x38000400) {
                /* Load/store register (immediate post-indexed) */
                int64_t imm = (instr >> 12) & 0x1ff;
                if (imm & 0x100) imm |= ~(uint64_t)0xff;
                chk_loaded(rn);
                addr_ok = reg_data[rn].o == REG_VAL_OTHER;
                addr = reg_data[rn].v;
                if (addr_ok) reg_data[rn].v += imm;
                instr_ok = 1;
            }
            else if ((instr & 0x3b200c00) == 0x38000800) {
                /* Load/store register (unprivileged) */
                int64_t imm = (instr >> 12) & 0x1ff;
                if (imm & 0x100) imm |= ~(uint64_t)0xff;
                chk_loaded(rn);
                addr_ok = reg_data[rn].o == REG_VAL_OTHER;
                addr = reg_data[rn].v + imm;
                instr_ok = 1;
            }
            else if ((instr & 0x3b200c00) == 0x38000c00) {
                /* Load/store register (immediate pre-indexed) */
                int64_t imm = (instr >> 12) & 0x1ff;
                if (imm & 0x100) imm |= ~(uint64_t)0xff;
                chk_loaded(rn);
                addr_ok = reg_data[rn].o == REG_VAL_OTHER;
                addr = reg_data[rn].v + imm;
                if (addr_ok) reg_data[rn].v = addr;
                instr_ok = 1;
            }
            else if ((instr & 0x3b200c00) == 0x38200800) {
                /* Load/store register (register offset) */
                /*
                uint32_t option = (instr >> 13) & 7;
                uint32_t rm = (instr >> 16) & 0x1f;
                int s = (instr & (1 << 12)) != 0;
                */
            }
            else if ((instr & 0x3b000000) == 0x39000000) {
                /* Load/store register (unsigned immediate) */
                uint32_t imm = (instr >> 10) & 0xfff;
                chk_loaded(rn);
                addr_ok = reg_data[rn].o == REG_VAL_OTHER;
                addr = reg_data[rn].v + (imm << shift);
                instr_ok = 1;
            }

            if (instr_ok) {
                if (!prf && !V) {
                    if (addr_ok) {
                        if (opc == 0) { /* store */
                            switch (shift) {
                            case 3:
                                store_reg(addr, rt);
                                break;
                            }
                        }
                        else {
                            uint8_t v8 = 0;
                            uint16_t v16 = 0;
                            uint32_t v32 = 0;
                            switch (shift) {
                            case 0:
                                if (read_byte(addr, &v8) == 0) set_reg(rt, 0, v8);
                                break;
                            case 1:
                                if (read_u16(addr, &v16) == 0) set_reg(rt, 0, v16);
                                break;
                            case 2:
                                if (read_u32(addr, &v32) == 0) set_reg(rt, 0, v32);
                                break;
                            case 3:
                                load_reg_lazy(addr, rt);
                                break;
                            }
                        }
                    }
                    else if (opc != 0) {
                        reg_data[rt].o = 0;
                    }
                }
                return 0;
            }
        }
    }

    return 0;
}

static int data_processing_register(void) {
    if ((instr & 0x1f000000) == 0x0a000000) {
        /* Logical (shifted register) */
        int sf = (instr & (1 << 31)) != 0;
        uint32_t opc = (instr >> 29) & 3;
        uint32_t shift = (instr >> 22) & 3;
        int n = (instr & (1 << 21)) != 0;
        uint32_t imm = (instr >> 10) & 0x3f;
        uint32_t rm = (instr >> 16) & 0x1f;
        uint32_t rn = (instr >> 5) & 0x1f;
        uint32_t rd = instr & 0x1f;
        uint64_t v = 0;

        if (rd == 31) return 0;

        if (rm != 31) {
            chk_loaded(rm);
            if (reg_data[rm].o != REG_VAL_OTHER) {
                reg_data[rd].o = 0;
                return 0;
            }
            v = reg_data[rm].v;
        }
        switch (shift) {
        case 0: /* LSL */
            v = v << imm;
            break;
        case 1: /* LSR */
            v = v >> imm;
            break;
        case 2: /* ASR */
            if (!sf) {
                int sign = (v & (1 << 31)) != 0;
                v = (v & 0xffffffff) >> imm;
                if (sign) v |= 0xffffffff << (32 - imm);
            }
            else {
                int sign = (v & ((uint64_t)1 << 63)) != 0;
                v = v >> imm;
                if (sign) v |= 0xffffffffffffffff << (64 - imm);
            }
            break;
        case 3: /* ROR */
            if (!sf) {
                v = ((v & 0xffffffff) >> imm) | (v << (32 - imm));
            }
            else {
                v = (v >> imm) | (v << (64 - imm));
            }
            break;
        }
        if (n) v = ~v;
        if (rn == 31) {
            switch (opc) {
            case 0: v = 0; break;
            case 3: v = 0; break;
            }
            set_reg(rd, sf, v);
        }
        else {
            chk_loaded(rn);
            if (reg_data[rn].o == REG_VAL_OTHER) {
                switch (opc) {
                case 0: v = reg_data[rn].v & v; break;
                case 1: v = reg_data[rn].v | v; break;
                case 2: v = reg_data[rn].v ^ v; break;
                case 3: v = reg_data[rn].v & v; break;
                }
                set_reg(rd, sf, v);
            }
            else {
                reg_data[rd].o = 0;
            }
        }
        return 0;
    }

    if ((instr & 0x1f200000) == 0x0b000000) {
        /* Add/subtract (shifted register) */
        int sf = (instr & (1 << 31)) != 0;
        int n = (instr & (1 << 30)) != 0;
        uint32_t imm = (instr >> 10) & 0x3f;
        uint32_t shift = (instr >> 22) & 3;
        uint32_t rm = (instr >> 16) & 0x1f;
        uint32_t rn = (instr >> 5) & 0x1f;
        uint32_t rd = instr & 0x1f;
        uint64_t v = 0;

        if (rd == 31) return 0;

        if (rm != 31) {
            chk_loaded(rm);
            if (reg_data[rm].o != REG_VAL_OTHER) {
                reg_data[rd].o = 0;
                return 0;
            }
            v = reg_data[rm].v;
        }
        switch (shift) {
        case 0: /* LSL */
            v = v << imm;
            break;
        case 1: /* LSR */
            v = v >> imm;
            break;
        case 2: /* ASR */
            if (!sf) {
                int sign = (v & (1 << 31)) != 0;
                v = (v & 0xffffffff) >> imm;
                if (sign) v |= 0xffffffff << (32 - imm);
            }
            else {
                int sign = (v & ((uint64_t)1 << 63)) != 0;
                v = v >> imm;
                if (sign) v |= 0xffffffffffffffff << (64 - imm);
            }
            break;
        default:
            return 0;
        }
        if (n) v = ~v + 1;

        if (rn == 31) {
            set_reg(rd, sf, v);
        }
        else {
            chk_loaded(rn);
            if (reg_data[rn].o == REG_VAL_OTHER) {
                set_reg(rd, sf, reg_data[rn].v + v);
            }
            else {
                reg_data[rd].o = 0;
            }
        }
        return 0;
    }

    return 0;
}

static int data_processing_simd_and_fp(void) {
    return 0;
}

static int trace_a64(void) {

    assert(pc_data.o != REG_VAL_ADDR);
    assert(pc_data.o != REG_VAL_STACK);

    /* Check PC alignment */
    if (pc_data.v & 0x3) {
        set_errno(ERR_OTHER, "PC misalignment");
        return -1;
    }

    /* Read the instruction */
    if (read_u32(pc_data.v, &instr) < 0) return -1;

    if ((instr & 0x1c000000) == 0x10000000) {
        if (data_processing_immediate() < 0) return -1;
    }
    else if ((instr & 0x1c000000) == 0x14000000) {
        if (branch_exception_system() < 0) return -1;
    }
    else if ((instr & 0x0a000000) == 0x08000000) {
        if (loads_and_stores() < 0) return -1;
    }
    else if ((instr & 0x0e000000) == 0x0a000000) {
        if (data_processing_register() < 0) return -1;
    }
    else if ((instr & 0x0e000000) == 0x0e000000) {
        if (data_processing_simd_and_fp() < 0) return -1;
    }
    else {
        unsigned i;
        /* Unknown/undecoded. May alter some register, so invalidate file */
        for (i = 0; i < 30; i++) reg_data[i].o = 0;
        trace(LOG_STACK, "Stack crawl: unknown instruction %08x", instr);
    }

    if (!trace_return && !trace_branch) {
        /* Next PC */
        pc_data.v += 4;
    }
    return 0;
}

static int trace_instructions(void) {
    unsigned i;
    RegData org_sp = reg_data[31];
    RegData org_lr = reg_data[30];
    RegData org_pc = pc_data;
    for (;;) {
        unsigned t = 0;
        BranchData * b = NULL;
        if (chk_loaded(31) < 0) return -1;
        if (chk_loaded(30) < 0) return -1;
        trace(LOG_STACK, "Stack crawl: pc 0x%"PRIX64", sp 0x%"PRIX64,
            pc_data.o ? pc_data.v : (uint64_t)0,
            reg_data[31].o ? reg_data[31].v : (uint64_t)0);
        for (t = 0; t < 200; t++) {
            int error = 0;
            trace_return = 0;
            trace_branch = 0;
            if (!pc_data.o) {
                error = set_errno(ERR_OTHER, "PC value not available");
            }
            else if (!pc_data.v) {
                error = set_errno(ERR_OTHER, "PC == 0");
            }
            else if (trace_a64() < 0) {
                error = errno;
            }
            if (!error && trace_return) {
                if (chk_loaded(31) < 0 || !reg_data[31].o) {
                    error = set_errno(ERR_OTHER, "Stack crawl: invalid SP value");
                }
            }
            if (error) {
                trace(LOG_STACK, "Stack crawl: %s", errno_to_str(error));
                break;
            }
            if (trace_return) return 0;
            if (trace_branch) break;
        }
        if (branch_pos >= branch_cnt) break;
        b = branch_data + branch_pos++;
        mem_data = b->mem_data;
        cpsr_data = b->cpsr_data;
        pc_data = b->pc_data;
        memcpy(reg_data, b->reg_data, sizeof(reg_data));
    }
    trace(LOG_STACK, "Stack crawl: Function epilogue not found");
    for (i = 0; i < 32; i++) reg_data[i].o = 0;
    cpsr_data.o = 0;
    pc_data.o = 0;
    if (org_sp.v != 0 && org_lr.v != 0 && org_pc.v != org_lr.v) {
        reg_data[31] = org_sp;
        pc_data = org_lr;
    }
    return 0;
}

int crawl_stack_frame_a64(StackFrame * frame, StackFrame * down) {
    RegisterDefinition * def = NULL;
    unsigned i;

    stk_ctx = frame->ctx;
    stk_frame = frame;
    memset(&mem_data, 0, sizeof(mem_data));
    memset(&reg_data, 0, sizeof(reg_data));
    memset(&cpsr_data, 0, sizeof(cpsr_data));
    memset(&pc_data, 0, sizeof(pc_data));
    branch_pos = 0;
    branch_cnt = 0;

    for (i = 0; i < MEM_CACHE_SIZE; i++) mem_cache[i].size = 0;

    for (def = get_reg_definitions(stk_ctx); def->name; def++) {
        if (def->dwarf_id >= 0 && def->dwarf_id <= 31) {
            if (read_reg_value(frame, def, &reg_data[def->dwarf_id].v) < 0) continue;
            reg_data[def->dwarf_id].o = REG_VAL_OTHER;
        }
        else if (strcmp(def->name, "cpsr") == 0) {
            if (read_reg_value(frame, def, &cpsr_data.v) < 0) continue;
            cpsr_data.o = REG_VAL_OTHER;
        }
        else if (strcmp(def->name, "pc") == 0) {
            if (read_reg_value(frame, def, &pc_data.v) < 0) continue;
            pc_data.o = REG_VAL_OTHER;
        }
    }

    if (trace_instructions() < 0) return -1;

    for (def = get_reg_definitions(stk_ctx); def->name; def++) {
        if (def->dwarf_id >= 0 && def->dwarf_id <= 31) {
            int r = def->dwarf_id;
            if (chk_loaded(r) < 0) continue;
            if (!reg_data[r].o) continue;
            if (r == 31) frame->fp = (ContextAddress)reg_data[r].v;
            if (write_reg_value(down, def, reg_data[r].v) < 0) return -1;
        }
        else if (strcmp(def->name, "cpsr") == 0) {
            if (!cpsr_data.o) continue;
            if (write_reg_value(down, def, cpsr_data.v) < 0) return -1;
        }
        else if (strcmp(def->name, "pc") == 0) {
            if (!pc_data.o) continue;
            if (write_reg_value(down, def, pc_data.v) < 0) return -1;
        }
    }

    stk_ctx = NULL;
    return 0;
}

#endif
