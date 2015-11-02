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

#include <tcf/config.h>

#if SERVICE_Disassembly

#include <assert.h>
#include <stdio.h>
#include <tcf/framework/context.h>
#include <tcf/services/symbols.h>
#include <machine/a64/tcf/disassembler-a64.h>

static char buf[128];
static size_t buf_pos = 0;
static DisassemblerParams * params = NULL;
static uint64_t instr_addr = 0;
static uint32_t instr = 0;

static const char * cond_names[] = {
    "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
    "hi", "ls", "ge", "lt", "gt", "le", "", "nv"
};

static void add_char(char ch) {
    if (buf_pos >= sizeof(buf) - 1) return;
    buf[buf_pos++] = ch;
    if (ch == ' ') while (buf_pos < 8) buf[buf_pos++] = ch;
}

static void add_str(const char * s) {
    while (*s) add_char(*s++);
}

static void add_dec_uint32(uint32_t n) {
    char s[32];
    size_t i = 0;
    do {
        s[i++] = (char)('0' + n % 10);
        n = n / 10;
    }
    while (n != 0);
    while (i > 0) add_char(s[--i]);
}

static void add_dec_uint64(uint64_t n) {
    char s[64];
    size_t i = 0;
    do {
        s[i++] = (char)('0' + (int)(n % 10));
        n = n / 10;
    }
    while (n != 0);
    while (i > 0) add_char(s[--i]);
}

static void add_hex_uint32(uint32_t n) {
    char s[32];
    size_t i = 0;
    while (i < 8) {
        uint32_t d = n & 0xf;
        if (i > 0 && n == 0) break;
        s[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        n = n >> 4;
    }
    while (i > 0) add_char(s[--i]);
}

static void add_hex_uint64(uint64_t n) {
    char s[64];
    size_t i = 0;
    while (i < 16) {
        uint32_t d = n & 0xf;
        if (i > 0 && n == 0) break;
        s[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        n = n >> 4;
    }
    while (i > 0) add_char(s[--i]);
}

static void add_flt_uint32(uint32_t n) {
    char buf[32];
    union {
        uint32_t n;
        float f;
    } u;
    u.n = n;
    snprintf(buf, sizeof(buf), "%g", u.f);
    add_str(buf);
}

static void add_flt_uint64(uint64_t n) {
    char buf[32];
    union {
        uint64_t n;
        double d;
    } u;
    u.n = n;
    snprintf(buf, sizeof(buf), "%g", u.d);
    add_str(buf);
}

static void add_reg_name(uint32_t n, int sf, int sp) {
    if (n == 31) {
        if (sp) add_str(sf ? "sp" : "wsp");
        else add_str(sf ? "xzr" : "wzr");
        return;
    }
    add_char(sf ? 'x' : 'w');
    add_dec_uint32(n);
}

static void add_prfm_name(uint32_t n) {
    switch (n) {
    case 0: add_str("pldl1keep"); break;
    case 1: add_str("pldl1strm"); break;
    case 2: add_str("pldl2keep"); break;
    case 3: add_str("pldl2strm"); break;
    case 4: add_str("pldl3keep"); break;
    case 5: add_str("pldl3strm"); break;
    case 8: add_str("plil1keep"); break;
    case 9: add_str("plil1strm"); break;
    case 10: add_str("plil2keep"); break;
    case 11: add_str("plil2strm"); break;
    case 12: add_str("plil3keep"); break;
    case 13: add_str("plil3strm"); break;
    case 16: add_str("pstl1keep"); break;
    case 17: add_str("pstl1strm"); break;
    case 18: add_str("pstl2keep"); break;
    case 19: add_str("pstl2strm"); break;
    case 20: add_str("pstl3keep"); break;
    case 21: add_str("pstl3strm"); break;
    default: add_char('#'); add_dec_uint32(instr & 0x1f); break;
    }
}

static void add_addr(uint64_t addr) {
    while (buf_pos < 16) add_char(' ');
    add_str("; addr=0x");
    add_hex_uint64(addr);
#if ENABLE_Symbols
    if (params->ctx != NULL) {
        Symbol * sym = NULL;
        char * name = NULL;
        ContextAddress sym_addr = 0;
        if (find_symbol_by_addr(params->ctx, STACK_NO_FRAME, (ContextAddress)addr, &sym) < 0) return;
        if (get_symbol_name(sym, &name) < 0 || name == NULL) return;
        if (get_symbol_address(sym, &sym_addr) < 0) return;
        if (sym_addr <= addr) {
            add_str(": ");
            add_str(name);
            if (sym_addr < addr) {
                add_str(" + 0x");
                add_hex_uint64(addr - (uint64_t)sym_addr);
            }
        }
    }
#endif
}

static void add_sys_reg_info(uint32_t reg) {
    uint32_t op0 = (reg >> 14) & 3;
    uint32_t op1 = (reg >> 11) & 7;
    uint32_t CRn = (reg >> 7) & 0xf;
    uint32_t CRm = (reg >> 3) & 0xf;
    uint32_t op2 = (reg >> 0) & 7;
    while (buf_pos < 20) add_char(' ');
    add_str("; op0:op1:CRn:CRm:op2 = ");
    add_dec_uint32(op0);
    add_char(':');
    add_dec_uint32(op1);
    add_char(':');
    add_dec_uint32(CRn);
    add_char(':');
    add_dec_uint32(CRm);
    add_char(':');
    add_dec_uint32(op2);
}

static void add_data_barrier_option(void) {
    uint32_t imm = (instr >> 8) & 0xf;
    switch (imm) {
    case  1: add_str("oshld"); break;
    case  2: add_str("oshst"); break;
    case  3: add_str("osh"); break;
    case  5: add_str("nshld"); break;
    case  6: add_str("nshst"); break;
    case  7: add_str("nsh"); break;
    case  9: add_str("ishld"); break;
    case 10: add_str("ishst"); break;
    case 11: add_str("ish"); break;
    case 13: add_str("ld"); break;
    case 14: add_str("st"); break;
    case 15: add_str("sy"); break;
    default:
        add_char('#');
        add_dec_uint32(imm);
        break;
    }
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

static int bfx_preferred(int sf, int uns, uint32_t imms, uint32_t immr) {
    if (imms < immr) return 0;
    if (imms == (sf ? 0x3fu : 0x1fu)) return 0;
    if (immr == 0) {
        if (!sf && (imms == 7 || imms == 15)) return 0;
        if (sf && !uns && (imms == 7 || imms == 15 || imms == 31)) return 0;
    }
    return 1;
}

static void data_processing_immediate(void) {
    if ((instr & 0x1f000000) == 0x10000000) {
        /* PC-rel. addressing */
        uint64_t base = instr_addr;
        uint64_t imm = 0;
        add_str(instr & (1u << 31) ? "adrp" : "adr");
        add_char(' ');
        add_reg_name(instr & 0x1f, 1, 1);
        add_str(", ");
        imm |= ((instr >> 29) & 0x3);
        imm |= ((instr >> 5) & 0x7ffff) << 2;
        if (imm & (1u << 20)) imm |= ~((uint64_t)(1u << 20) - 1);
        if (instr & (1u << 31)) {
            imm = imm << 12;
            base &= ~((uint64_t)0xfff);
        }
        if (imm & ((uint64_t)1u << 63)) {
            add_char('-');
            add_dec_uint64(~imm + 1);
        }
        else {
            add_char('+');
            add_dec_uint64(imm);
        }
        add_addr(base + imm);
        return;
    }

    if ((instr & 0x1f000000) == 0x11000000) {
        /* Add/subtract (immediate) */
        int sf = (instr & (1u << 31)) != 0;
        uint32_t imm = (instr >> 10) & 0xfff;
        uint32_t op = (instr >> 29) & 3;
        uint32_t rt = instr & 0x1f;
        uint32_t rn = (instr >> 5) & 0x1f;
        if (op == 0 && imm == 0 && rt != rn) {
            add_str("mov ");
            add_reg_name(rt, sf, 1);
            add_str(", ");
            add_reg_name(rn, sf, 1);
            return;
        }
        if ((op == 1 || op == 3) && rt == 31) {
            add_str(op == 1 ? "cmn " : "cmp ");
            add_reg_name((instr >> 5) & 0x1f, sf, 1);
            add_str(", #0x");
            add_hex_uint32(imm);
            switch ((instr >> 22) & 3) {
            case 1: add_str(", lsl #12"); break;
            }
            return;
        }
        switch (op) {
        case 0: add_str("add"); break;
        case 1: add_str("adds"); break;
        case 2: add_str("sub"); break;
        case 3: add_str("subs"); break;
        }
        add_char(' ');
        add_reg_name(rt, sf, 1);
        add_str(", ");
        add_reg_name(rn, sf, 1);
        add_str(", #0x");
        add_hex_uint32(imm);
        switch ((instr >> 22) & 3) {
        case 1: add_str(", lsl #12"); break;
        }
        return;
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
        int no_rd = 0;
        int no_rn = 0;
        if (rd == 31 && opc == 3) {
            add_str("tst");
            no_rd = 1;
        }
        else if (rn == 31 && opc == 1) {
            add_str("mov");
            no_rn = 1;
        }
        else {
            switch (opc) {
            case 0: add_str("and"); break;
            case 1: add_str("orr"); break;
            case 2: add_str("eor"); break;
            case 3: add_str("ands"); break;
            }
        }
        add_char(' ');
        if (!no_rd) {
            add_reg_name(rd, sf, 0);
            add_str(", ");
        }
        if (!no_rn) {
            add_reg_name(rn, sf, 0);
            add_str(", ");
        }
        add_str("#0x");
        add_hex_uint64(decode_bit_mask(sf, n, imms, immr, 1, NULL));
        return;
    }

    if ((instr & 0x1f800000) == 0x12800000) {
        /* Move wide (immediate) */
        int sf = (instr & (1u << 31)) != 0;
        uint32_t op = (instr >> 29) & 3;
        uint32_t hw = (instr >> 21) & 3;
        uint64_t imm = (instr >> 5) & 0xffff;
        if ((op == 0 || op == 2) && (imm > 0 || hw == 0)) {
            add_str("mov");
            imm = imm << (hw * 16);
            if (op == 0) imm = ~imm;
            if (!sf) imm &= 0xffffffff;
            hw = 0;
        }
        else {
            switch (op) {
            case 0: add_str("movn"); break;
            case 1: return;
            case 2: add_str("movz"); break;
            case 3: add_str("movk"); break;
            }
        }
        add_char(' ');
        add_reg_name(instr & 0x1f, sf, 1);
        add_str(", #0x");
        add_hex_uint64(imm);
        switch (hw) {
        case 1: add_str(", lsl #16"); break;
        case 2: add_str(", lsl #32"); break;
        case 3: add_str(", lsl #48"); break;
        }
        return;
    }

    if ((instr & 0x1f800000) == 0x13000000) {
        /* Bitfield */
        int sf = (instr & (1u << 31)) != 0;
        uint32_t opc = (instr >> 29) & 3;
        uint32_t imms = (instr >> 10) & 0x3f;
        uint32_t immr = (instr >> 16) & 0x3f;
        if (opc == 0) {
            if (imms == (sf ? 0x3fu : 0x1fu)) {
                add_str("asr ");
                add_reg_name(instr & 0x1f, sf, 0);
                add_str(", ");
                add_reg_name((instr >> 5) & 0x1f, sf, 0);
                add_str(", #");
                add_dec_uint32(immr);
                return;
            }
        }
        else if (opc == 1) {
            if (imms < immr) {
                add_str("bfi");
                immr = (~immr + 1) & (sf ? 0x3f : 0x1f);
                imms++;
            }
            else {
                add_str("bfxil");
                imms = imms - immr + 1;
            }
        }
        else if (opc == 2) {
            if (imms == (sf ? 0x3fu : 0x1fu)) {
                add_str("lsr ");
                add_reg_name(instr & 0x1f, sf, 0);
                add_str(", ");
                add_reg_name((instr >> 5) & 0x1f, sf, 0);
                add_str(", #");
                add_dec_uint32(immr);
                return;
            }
            if (imms != (sf ? 0x3fu : 0x1fu) && imms + 1 == immr) {
                add_str("lsl ");
                add_reg_name(instr & 0x1f, sf, 0);
                add_str(", ");
                add_reg_name((instr >> 5) & 0x1f, sf, 0);
                add_str(", #");
                add_dec_uint32((sf ? 0x3fu : 0x1fu) - imms);
                return;
            }
        }
        if (opc == 0 || opc == 2) {
            if (imms < immr) {
                add_str(opc ? "ubfiz" : "sbfiz");
                immr = (~immr + 1) & (sf ? 0x3f : 0x1f);
                imms = imms + 1;
            }
            else if (bfx_preferred(sf, opc != 0, imms, immr)) {
                add_str(opc ? "ubfx " : "sbfx ");
                imms = imms - immr + 1;
            }
            else if (immr == 0 && imms == 7) {
                add_str(opc ? "uxtb " : "sxtb ");
                add_reg_name(instr & 0x1f, sf, 0);
                add_str(", ");
                add_reg_name((instr >> 5) & 0x1f, 0, 0);
                return;
            }
            else if (immr == 0 && imms == 15) {
                add_str(opc ? "uxth " : "sxth ");
                add_reg_name(instr & 0x1f, sf, 0);
                add_str(", ");
                add_reg_name((instr >> 5) & 0x1f, 0, 0);
                return;
            }
            else if (immr == 0 && imms == 31) {
                add_str(opc ? "uxtw " : "sxtw ");
                add_reg_name(instr & 0x1f, sf, 0);
                add_str(", ");
                add_reg_name((instr >> 5) & 0x1f, 0, 0);
                return;
            }
        }
        if (buf_pos == 0) {
            switch (opc) {
            case 0: add_str("sbfm"); break;
            case 1: add_str("bfm"); break;
            case 2: add_str("ubfm"); break;
            case 3: return;
            }
        }
        add_char(' ');
        add_reg_name(instr & 0x1f, sf, 0);
        add_str(", ");
        add_reg_name((instr >> 5) & 0x1f, sf, 0);
        add_str(", #");
        add_dec_uint32(immr);
        add_str(", #");
        add_dec_uint32(imms);
        return;
    }

    if ((instr & 0x1f800000) == 0x13800000) {
        /* Extract */
        int sf = (instr & (1u << 31)) != 0;
        switch ((instr >> 29) & 3) {
        case 0: add_str("extr"); break;
        case 1: return;
        case 2: return;
        case 3: return;
        }
        add_char(' ');
        add_reg_name(instr & 0x1f, sf, 0);
        add_str(", ");
        add_reg_name((instr >> 5) & 0x1f, sf, 0);
        add_str(", ");
        add_reg_name((instr >> 16) & 0x1f, sf, 0);
        add_str(", #0x");
        add_hex_uint32((instr >> 10) & 0x3f);
        return;
    }
}

static void branch_exception_system(void) {
    if ((instr & 0x7c000000) == 0x14000000) {
        /* Unconditional branch (immediate) */
        int32_t imm = instr & 0x3ffffff;
        add_str(instr & (1u << 31) ? "bl" : "b");
        add_char(' ');
        if (imm & 0x02000000) {
            imm |= 0xfc000000;
            add_char('-');
            add_dec_uint32(~imm + 1);
        }
        else {
            add_char('+');
            add_dec_uint32(imm);
        }
        add_addr(instr_addr + ((int64_t)imm << 2));
        return;
    }

    if ((instr & 0x7e000000) == 0x34000000) {
        /* Compare & branch (immediate) */
        int sf = (instr & (1u << 31)) != 0;
        int32_t imm = (instr >> 5) & 0x7ffff;
        add_str(instr & (1u << 24) ? "cbnz" : "cbz");
        add_char(' ');
        add_reg_name(instr & 0x1f, sf, 1);
        add_str(", ");
        if (imm & 0x00040000) {
            imm |= 0xfffc0000;
            add_char('-');
            add_dec_uint32(~imm + 1);
        }
        else {
            add_char('+');
            add_dec_uint32(imm);
        }
        add_addr(instr_addr + ((int64_t)imm << 2));
        return;
    }

    if ((instr & 0x7e000000) == 0x36000000) {
        /* Test & branch (immediate) */
        int sf = (instr & (1u << 31)) != 0;
        int32_t imm = (instr >> 5) & 0x3fff;
        add_str(instr & (1u << 24) ? "tbnz" : "tbz");
        add_char(' ');
        add_reg_name(instr & 0x1f, sf, 1);
        add_str(", #");
        add_dec_uint32(((instr >> 19) & 0x1f) | (((instr >> 31) & 0x1) << 5));
        add_str(", ");
        if (imm & 0x00002000) {
            imm |= 0xffffe000;
            add_char('-');
            add_dec_uint32(~imm + 1);
        }
        else {
            add_char('+');
            add_dec_uint32(imm);
        }
        add_addr(instr_addr + ((int64_t)imm << 2));
        return;
    }

    if ((instr & 0xfe000000) == 0x54000000) {
        /* Conditional branch (immediate) */
        int32_t imm = (instr >> 5) & 0x7ffff;
        add_str("b.");
        add_str(cond_names[instr & 0xf]);
        add_char(' ');
        if (imm & 0x00040000) {
            imm |= 0xfffc0000;
            add_char('-');
            add_dec_uint32(~imm + 1);
        }
        else {
            add_char('+');
            add_dec_uint32(imm);
        }
        add_addr(instr_addr + ((int64_t)imm << 2));
        return;
    }

    if ((instr & 0xff000000) == 0xd4000000) {
        /* Exception generation */
        uint32_t opc = (instr >> 21) & 0x7;
        uint32_t op2 = (instr >> 2) & 0x7;
        uint32_t ll = instr & 0x3;
        uint32_t imm = (instr >> 5) & 0xffff;
        if (opc == 0 && op2 == 0 && ll == 1) add_str("svc");
        else if (opc == 0 && op2 == 0 && ll == 2) add_str("hvc");
        else if (opc == 0 && op2 == 0 && ll == 3) add_str("smc");
        else if (opc == 1 && op2 == 0 && ll == 0) add_str("brk");
        else if (opc == 2 && op2 == 0 && ll == 0) add_str("hlt");
        else if (opc == 5 && op2 == 0 && ll == 1) add_str("dcps1");
        else if (opc == 5 && op2 == 0 && ll == 2) add_str("dcps2");
        else if (opc == 5 && op2 == 0 && ll == 3) add_str("dcps3");
        else return;
        if (imm != 0 || opc != 5) {
            add_str(" #0x");
            add_hex_uint32(imm);
        }
        return;
    }

    if ((instr & 0xffc00000) == 0xd5000000) {
        /* System */
        int l = (instr & (1u << 21)) != 0;
        uint32_t op0 = (instr >> 19) & 0x3;
        uint32_t op1 = (instr >> 16) & 0x7;
        uint32_t crn = (instr >> 12) & 0xf;
        uint32_t op2 = (instr >> 5) & 0x7;
        uint32_t rt = instr & 0x1f;
        if (l == 0 && op0 == 0 && crn == 4 && rt == 31) {
            /* MSR (immediate) */
            const char * psf = NULL;
            if (op1 == 0 && op2 == 5) psf = "spsel";
            else if (op1 == 3 && op2 == 6) psf = "daifset";
            else if (op1 == 3 && op2 == 7) psf = "daifclr";
            if (psf != NULL) {
                add_str("msr ");
                add_str(psf);
                add_str(", #");
                add_dec_uint32((instr >> 8) & 0xf);
            }
        }
        else if (l == 0 && op0 == 0 && op1 == 3 && crn == 2 && rt == 31) {
            /* HINT */
            switch (op2) {
            case 1: add_str("yield"); break;
            case 2: add_str("wfe"); break;
            case 3: add_str("wfi"); break;
            case 4: add_str("sev"); break;
            case 5: add_str("sevl"); break;
            default: add_str("nop"); break;
            }
        }
        else if (l == 0 && op0 == 0 && op1 == 3 && crn == 3 && op2 == 2 && rt == 31) {
            /* CLREX */
            uint32_t imm = (instr >> 8) & 0xf;
            add_str("clrex");
            if (imm != 15) {
                add_str(", #");
                add_dec_uint32(imm);
            }
        }
        else if (l == 0 && op0 == 0 && op1 == 3 && crn == 3 && op2 == 4 && rt == 31) {
            /* DSB */
            add_str("dsb ");
            add_data_barrier_option();
        }
        else if (l == 0 && op0 == 0 && op1 == 3 && crn == 3 && op2 == 5 && rt == 31) {
            /* DMB */
            add_str("dmb ");
            add_data_barrier_option();
        }
        else if (l == 0 && op0 == 0 && op1 == 3 && crn == 3 && op2 == 6 && rt == 31) {
            /* ISB */
            uint32_t imm = (instr >> 8) & 0xf;
            add_str("isb ");
            switch (imm) {
            case 15: add_str("sy"); break;
            default:
                add_char('#');
                add_dec_uint32(imm);
                break;
            }
        }
        else if (l == 0 && op0 == 1) {
            /* SYS */
            uint32_t crm = (instr >> 8) & 0xf;
            add_str("sys #");
            add_dec_uint32(op1);
            add_str(", c");
            add_dec_uint32(crn);
            add_str(", c");
            add_dec_uint32(crm);
            add_str(", #");
            add_dec_uint32(op2);
            if (rt != 31) {
                add_str(", ");
                add_reg_name(rt, 1, 1);
            }
        }
        else if (l == 0 && op0 >= 2) {
            /* MSR (register) */
            uint32_t reg = (instr >> 5) & 0xffff;
            add_str("msr ");
            add_dec_uint32(reg);
            add_str(", ");
            add_reg_name(rt, 1, 1);
            add_sys_reg_info(reg);
        }
        else if (l == 1 && op0 == 1) {
            /* SYSL */
            uint32_t crm = (instr >> 8) & 0xf;
            add_str("sys ");
            add_reg_name(rt, 1, 1);
            add_str(", #");
            add_dec_uint32(op1);
            add_str(", c");
            add_dec_uint32(crn);
            add_str(", c");
            add_dec_uint32(crm);
            add_str(", #");
            add_dec_uint32(op2);
        }
        else if (l == 1 && op0 >= 2) {
            /* MRS */
            uint32_t reg = (instr >> 5) & 0xffff;
            add_str("mrs ");
            add_reg_name(rt, 1, 1);
            add_str(", ");
            add_dec_uint32(reg);
            add_sys_reg_info(reg);
        }
        return;
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
            case 0: add_str("br"); break;
            case 1: add_str("blr"); break;
            case 2: add_str("ret"); break;
            }
            if (buf_pos > 0) {
                if (opc == 2 && rn == 30) return;
                add_char(' ');
                add_reg_name(rn, 1, 1);
            }
            else if (rn == 31) {
                switch (opc) {
                case 4: add_str("eret"); break;
                case 5: add_str("drps"); break;
                }
            }
        }
        return;
    }
}

static void loads_and_stores(void) {
    if ((instr & 0x3f000000) == 0x08000000) {
        /* Load/store exclusive */
        int sz64 = 0;
        int L = (instr & (1 << 22)) != 0;
        unsigned op = ((instr >> 15) & 1) + ((instr >> 20) & 2) + ((instr >> 21) & 4);
        add_str(L ? "ld" : "st");
        switch (op) {
        case 0: add_str("xr"); break;
        case 1: add_str(L ? "axr" : "lxr"); break;
        case 2: add_str("xp"); break;
        case 3: add_str(L ? "axp" : "lxp"); break;
        case 5: add_str(L ? "ar" : "lr"); break;
        }
        switch (instr >> 30) {
        case 0: add_char('b'); break;
        case 1: add_char('h'); break;
        case 3: sz64 = 1; break;
        }
        add_char(' ');
        if (!L && op != 5) {
            add_reg_name((instr >> 16) & 0x1f, 0, 1);
            add_str(", ");
        }
        add_reg_name(instr & 0x1f, sz64, 1);
        if (op == 2 || op == 3) {
            add_str(", ");
            add_reg_name((instr >> 10) & 0x1f, sz64, 1);
        }
        add_str(", [");
        add_reg_name((instr >> 5) & 0x1f, 1, 1);
        add_str("]");
        return;
    }

    if ((instr & 0x3b000000) == 0x18000000) {
        /* Load register (literal) */
        uint32_t opc = (instr >> 30) & 3;
        int V = (instr & (1 << 26)) != 0;
        uint32_t imm = (instr >> 5) & 0x7ffff;
        switch (opc) {
        case 0:
        case 1:
            add_str("ldr");
            break;
        case 2:
            add_str(V ? "ldr" : "ldrsw");
            break;
        case 3:
            add_str("prfm");
            break;
        }
        add_char(' ');
        if (V) {
            switch (opc) {
            case 0: add_char('s'); break;
            case 1: add_char('d'); break;
            case 2: add_char('q'); break;
            case 3: buf_pos = 0; return;
            }
            add_dec_uint32(instr & 0x1f);
        }
        else if (opc == 3) {
            add_prfm_name(instr & 0x1f);
        }
        else {
            add_reg_name(instr & 0x1f, opc == 1, 1);
        }
        add_str(", ");
        if (imm & 0x40000) {
            add_char('-');
            add_dec_uint32(0x80000 - imm);
            add_addr(instr_addr - (0x80000 - imm) * 4);
        }
        else {
            add_dec_uint32(imm);
            add_addr(instr_addr + imm * 4);
        }
        return;
    }

    if ((instr & 0x3a800000) == 0x28000000) {
        /* Load/store no-allocate pair (offset) */
        /* Load/store register pair (offset) */
        uint32_t opc = (instr >> 30) & 3;
        int V = (instr & (1 << 26)) != 0;
        int L = (instr & (1 << 22)) != 0;
        int N = (instr & (1 << 24)) == 0;
        uint32_t imm = (instr >> 15) & 0x7f;
        uint32_t shift = 0;

        add_str(L ? "ld" : "st");
        if (N) add_char('n');
        add_char('p');
        if (opc == 1 && L && !V) add_str("sw");
        add_char(' ');
        if (V) {
            char ch = 0;
            switch (opc) {
            case 0: ch = 's'; shift = 2; break;
            case 1: ch = 'd'; shift = 3; break;
            case 2: ch = 'q'; shift = 4; break;
            case 3: buf_pos = 0; return;
            }
            add_char(ch);
            add_dec_uint32(instr & 0x1f);
            add_str(", ");
            add_char(ch);
            add_dec_uint32((instr >> 10) & 0x1f);
        }
        else {
            add_reg_name(instr & 0x1f, opc >= 2, 0);
            add_str(", ");
            add_reg_name((instr >> 10) & 0x1f, opc >= 2, 0);
            shift = opc >= 2 ? 3 : 2;
        }
        add_str(", [");
        add_reg_name((instr >> 5) & 0x1f, 1, 1);
        if (imm != 0) {
            add_str(", #");
            if (imm & 0x40) {
                add_char('-');
                add_dec_uint32((0x80 - imm) << shift);
            }
            else {
                add_dec_uint32(imm << shift);
            }
        }
        add_char(']');
        return;
    }

    if ((instr & 0x3b800000) == 0x28800000) {
        /* Load/store register pair (post-indexed) */
        uint32_t opc = (instr >> 30) & 3;
        int V = (instr & (1 << 26)) != 0;
        int L = (instr & (1 << 22)) != 0;
        uint32_t imm = (instr >> 15) & 0x7f;
        uint32_t shift = 0;

        add_str(L ? "ldp" : "stp");
        if (opc == 1 && L && !V) add_str("sw");
        add_char(' ');
        if (V) {
            char ch = 0;
            switch (opc) {
            case 0: ch = 's'; shift = 2; break;
            case 1: ch = 'd'; shift = 3; break;
            case 2: ch = 'q'; shift = 4; break;
            case 3: buf_pos = 0; return;
            }
            add_char(ch);
            add_dec_uint32(instr & 0x1f);
            add_str(", ");
            add_char(ch);
            add_dec_uint32((instr >> 10) & 0x1f);
        }
        else {
            add_reg_name(instr & 0x1f, opc >= 2, 0);
            add_str(", ");
            add_reg_name((instr >> 10) & 0x1f, opc >= 2, 0);
            shift = opc >= 2 ? 3 : 2;
        }
        add_str(", [");
        add_reg_name((instr >> 5) & 0x1f, 1, 1);
        add_str("], #");
        if (imm & 0x40) {
            add_char('-');
            add_dec_uint32((0x80 - imm) << shift);
        }
        else {
            add_dec_uint32(imm << shift);
        }
        return;
    }

    if ((instr & 0x3b800000) == 0x29800000) {
        /* Load/store register pair (pre-indexed) */
        uint32_t opc = (instr >> 30) & 3;
        int V = (instr & (1 << 26)) != 0;
        int L = (instr & (1 << 22)) != 0;
        uint32_t imm = (instr >> 15) & 0x7f;
        uint32_t shift = 0;

        add_str(L ? "ldp" : "stp");
        if (opc == 1 && L && !V) add_str("sw");
        add_char(' ');
        if (V) {
            char ch = 0;
            switch (opc) {
            case 0: ch = 's'; shift = 2; break;
            case 1: ch = 'd'; shift = 3; break;
            case 2: ch = 'q'; shift = 4; break;
            case 3: buf_pos = 0; return;
            }
            add_char(ch);
            add_dec_uint32(instr & 0x1f);
            add_str(", ");
            add_char(ch);
            add_dec_uint32((instr >> 10) & 0x1f);
        }
        else {
            add_reg_name(instr & 0x1f, opc >= 2, 0);
            add_str(", ");
            add_reg_name((instr >> 10) & 0x1f, opc >= 2, 0);
            shift = opc >= 2 ? 3 : 2;
        }
        add_str(", [");
        add_reg_name((instr >> 5) & 0x1f, 1, 1);
        add_str(", #");
        if (imm & 0x40) {
            add_char('-');
            add_dec_uint32((0x80 - imm) << shift);
        }
        else {
            add_dec_uint32(imm << shift);
        }
        add_str("]!");
        return;
    }

    {
        char nm = 0;
        uint32_t size = (instr >> 30) & 3;
        uint32_t opc = (instr >> 22) & 3;
        int V = (instr & (1 << 26)) != 0;
        int shift = 0;

        /* if ((instr & 0x3b200c00) == 0x38000000) nm = 'u'; */
        if ((instr & 0x3b200c00) == 0x38000800) nm = 't';

        if (V) {
            add_str(opc == 0 || opc == 2 ? "st" : "ld");
            if (nm) add_char(nm);
            add_char('r');
        }
        else if (size == 3 && opc == 2) {
            add_str("prf");
            if (nm) add_char(nm);
            add_char('m');
        }
        else {
            add_str(opc == 0 ? "st" : "ld");
            if (nm) add_char(nm);
            add_char('r');
        }
        if (!V) {
            if (size == 0) {
                if (opc >= 2) add_char('s');
                add_char('b');
            }
            else if (size == 1) {
                if (opc >= 2) add_char('s');
                add_char('h');
            }
        }
        if (size == 2 && !V && opc == 2) add_str("sw");
        add_char(' ');

        if (V) {
            if (opc == 0) {
                switch (size) {
                case 0: add_char('b'); break;
                case 1: add_char('h'); shift = 1; break;
                case 2: add_char('s'); shift = 2; break;
                case 3: add_char('d'); shift = 3; break;
                }
            }
            else {
                switch (size) {
                case 0: add_char('q'); shift = 4; break;
                default: shift = -1; break;
                }
            }
            add_dec_uint32(instr & 0x1f);
        }
        else if (size == 3 && opc == 2) {
            add_prfm_name(instr & 0x1f);
            shift = 3;
        }
        else {
            add_reg_name(instr & 0x1f, size == 3 || opc == 2, 0);
            shift = size;
        }

        if (shift >= 0) {
            if ((instr & 0x3b200c00) == 0x38000000) {
                /* Load/store register (unscaled immediate) */
                uint32_t imm = (instr >> 12) & 0x1ff;

                add_str(", [");
                add_reg_name((instr >> 5) & 0x1f, 1, 1);
                if (imm != 0) {
                    add_str(", #");
                    if (imm & 0x100) {
                        add_char('-');
                        add_dec_uint32(0x200 - imm);
                    }
                    else {
                        add_dec_uint32(imm);
                    }
                }
                add_char(']');
                return;
            }

            if ((instr & 0x3b200c00) == 0x38000400) {
                /* Load/store register (immediate post-indexed) */
                uint32_t imm = (instr >> 12) & 0x1ff;

                add_str(", [");
                add_reg_name((instr >> 5) & 0x1f, 1, 1);
                add_str("], #");
                if (imm & 0x100) {
                    add_char('-');
                    add_dec_uint32(0x200 - imm);
                }
                else {
                    add_dec_uint32(imm);
                }
                return;
            }

            if ((instr & 0x3b200c00) == 0x38000800) {
                /* Load/store register (unprivileged) */
                uint32_t imm = (instr >> 12) & 0x1ff;

                add_str(", [");
                add_reg_name((instr >> 5) & 0x1f, 1, 1);
                if (imm != 0) {
                    add_str(", #");
                    if (imm & 0x100) {
                        add_char('-');
                        add_dec_uint32(0x200 - imm);
                    }
                    else {
                        add_dec_uint32(imm);
                    }
                }
                add_char(']');
                return;
            }

            if ((instr & 0x3b200c00) == 0x38000c00) {
                /* Load/store register (immediate pre-indexed) */
                uint32_t imm = (instr >> 12) & 0x1ff;

                add_str(", [");
                add_reg_name((instr >> 5) & 0x1f, 1, 1);
                if (imm != 0) {
                    add_str(", #");
                    if (imm & 0x100) {
                        add_char('-');
                        add_dec_uint32(0x200 - imm);
                    }
                    else {
                        add_dec_uint32(imm);
                    }
                }
                add_str("]!");
                return;
            }

            if ((instr & 0x3b200c00) == 0x38200800) {
                /* Load/store register (register offset) */
                uint32_t option = (instr >> 13) & 7;
                uint32_t rm = (instr >> 16) & 0x1f;
                int s = (instr & (1 << 12)) != 0;
                add_str(", [");
                add_reg_name((instr >> 5) & 0x1f, 1, 1);
                add_str(", ");
                switch (option) {
                case 2:
                case 6:
                    add_reg_name(rm, 0, 1);
                    break;
                case 3:
                case 7:
                    add_reg_name(rm, 1, 1);
                    break;
                default:
                    buf_pos = 0;
                    return;
                }
                if (s || option != 3) {
                    add_str(", ");
                    switch (option) {
                    case 2: add_str("uxtw"); break;
                    case 3: add_str("lsl"); break;
                    case 6: add_str("sxtw"); break;
                    case 7: add_str("sxtx"); break;
                    default: buf_pos = 0; return;
                    }
                    if (s) {
                        add_str(" #");
                        add_dec_uint32(shift);
                    }
                }
                add_char(']');
                return;
            }

            if ((instr & 0x3b000000) == 0x39000000) {
                /* Load/store register (unsigned immediate) */
                uint32_t imm = (instr >> 10) & 0xfff;

                add_str(", [");
                add_reg_name((instr >> 5) & 0x1f, 1, 1);
                if (imm != 0) {
                    add_str(", #");
                    add_dec_uint32(imm << shift);
                }
                add_char(']');
                return;
            }
        }

        buf_pos = 0;
    }

    if ((instr & 0xbfbf0000) == 0x0c000000) {
        /* AdvSIMD load/store multiple structures */
        return;
    }

    if ((instr & 0xbfa00000) == 0x0c800000) {
        /* AdvSIMD load/store multiple structures (post-indexed) */
        return;
    }

    if ((instr & 0xbf9f0000) == 0x0d000000) {
        /* AdvSIMD load/store single structure */
        return;
    }

    if ((instr & 0xbf800000) == 0x0d800000) {
        /* AdvSIMD load/store single structure (post-indexed) */
        return;
    }
}

static void data_processing_register(void) {
    if ((instr & 0x1f000000) == 0x0a000000) {
        /* Logical (shifted register) */
        int sf = (instr & (1 << 31)) != 0;
        uint32_t opc = (instr >> 29) & 3;
        uint32_t shift = (instr >> 22) & 3;
        int n = (instr & (1 << 21)) != 0;
        uint32_t imm = (instr >> 10) & 0x3f;
        uint32_t rn = (instr >> 5) & 0x1f;
        uint32_t rd = instr & 0x1f;
        int no_rd = 0;

        if (opc == 1 && shift == 0 && imm == 0 && rn == 31) {
            add_str(n ? "mvn " : "mov ");
            add_reg_name(instr & 0x1f, sf, 0);
            add_str(", ");
            add_reg_name((instr >> 16) & 0x1f, sf, 0);
            return;
        }
        if (rd == 31 && opc == 3 && !n) {
            add_str("tst");
            no_rd = 1;
        }
        else {
            switch (opc) {
            case 0: add_str(n ? "bic" : "and"); break;
            case 1: add_str(n ? "orn" : "orr"); break;
            case 2: add_str(n ? "eon" : "eor"); break;
            case 3: add_str(n ? "bics" : "ands"); break;
            }
        }
        add_char(' ');
        if (!no_rd) {
            add_reg_name(rd, sf, 0);
            add_str(", ");
        }
        add_reg_name(rn, sf, 0);
        add_str(", ");
        add_reg_name((instr >> 16) & 0x1f, sf, 0);
        if (shift != 0 || imm != 0) {
            add_str(", ");
            switch (shift) {
            case 0: add_str("lsl"); break;
            case 1: add_str("lsr"); break;
            case 2: add_str("asr"); break;
            case 3: add_str("ror"); break;
            }
            add_str(" #");
            add_dec_uint32(imm);
        }
        return;
    }

    if ((instr & 0x1f200000) == 0x0b000000) {
        /* Add/subtract (shifted register) */
        int sf = (instr & (1 << 31)) != 0;
        uint32_t imm = (instr >> 10) & 0x3f;
        uint32_t shift = (instr >> 22) & 3;
        int no_rd = 0;
        int no_rn = 0;
        if ((instr & 0x6000001f) == 0x6000001f) {
            add_str("cmp");
            no_rd = 1;
        }
        else if ((instr & 0x6000001f) == 0x2000001f) {
            add_str("cmn");
            no_rd = 1;
        }
        else if ((instr & 0x600003e0) == 0x400003e0) {
            add_str("neg");
            no_rn = 1;
        }
        else {
            add_str(instr & (1 << 30) ? "sub" : "add");
            if (instr & (1 << 29)) add_char('s');
        }
        add_char(' ');
        if (!no_rd) {
            add_reg_name(instr & 0x1f, sf, 0);
            add_str(", ");
        }
        if (!no_rn) {
            add_reg_name((instr >> 5) & 0x1f, sf, 0);
            add_str(", ");
        }
        add_reg_name((instr >> 16) & 0x1f, sf, 0);
        if (imm != 0) {
            add_str(", ");
            switch (shift) {
            case 0: add_str("lsl"); break;
            case 1: add_str("lsr"); break;
            case 2: add_str("asr"); break;
            default: buf_pos = 0; return;
            }
            add_str(" #");
            add_dec_uint32(imm);
        }
        return;
    }

    if ((instr & 0x1f200000) == 0x0b200000) {
        /* Add/subtract (extended register) */
        int sf = (instr & (1 << 31)) != 0;
        uint32_t imm = (instr >> 10) & 7;
        uint32_t option = (instr >> 13) & 7;
        uint32_t rn = (instr >> 5) & 0x1f;
        uint32_t rd = instr & 0x1f;
        int s = instr & (1 << 29);
        int no_rd = 0;

        if ((instr & 0x6000001f) == 0x6000001f) {
            add_str("cmp");
            no_rd = 1;
        }
        else {
            add_str(instr & (1 << 30) ? "sub" : "add");
            if (s) add_char('s');
        }
        add_char(' ');
        if (!no_rd) {
            add_reg_name(rd, sf, !s);
            add_str(", ");
        }
        add_reg_name(rn, sf, 1);
        add_str(", ");
        add_reg_name((instr >> 16) & 0x1f, sf && (option == 3 || option == 7), 1);
        if (imm == 0 && !sf && option == 2) {
            /* Nothing */
        }
        else if (imm == 0 && sf && option == 3) {
            /* Nothing */
        }
        else {
            add_str(", ");
            switch (option) {
            case 0: add_str("uxtb"); break;
            case 1: add_str("uxth"); break;
            case 2: add_str("uxtw"); break;
            case 3: add_str("uxtx"); break;
            case 4: add_str("sxtb"); break;
            case 5: add_str("sxth"); break;
            case 6: add_str("sxtw"); break;
            case 7: add_str("sxtx"); break;
            }
            if (imm != 0) {
                add_str(" #");
                add_dec_uint32(imm);
            }
        }
        return;
    }

    if ((instr & 0x1fe00000) == 0x1a000000) {
        /* Add/subtract (with carry) */
        int sf = (instr & (1 << 31)) != 0;
        uint32_t opcode2 = (instr >> 10) & 0x3f;
        if (opcode2 == 0) {
            add_str(instr & (1 << 30) ? "sbc" : "adc");
            if (instr & (1 << 29)) add_char('s');
            add_char(' ');
            add_reg_name(instr & 0x1f, sf, 0);
            add_str(", ");
            add_reg_name((instr >> 5) & 0x1f, sf, 0);
            add_str(", ");
            add_reg_name((instr >> 16) & 0x1f, sf, 0);
        }
        return;
    }

    if ((instr & 0x1fe00800) == 0x1a400000) {
        /* Conditional compare (register) */
        int sf = (instr & (1 << 31)) != 0;
        int op = (instr & (1 << 30)) != 0;
        int s = (instr & (1 << 29)) != 0;
        int o2 = (instr & (1 << 10)) != 0;
        int o3 = (instr & (1 << 4)) != 0;
        if (s && !o2 && !o3) {
            uint32_t cond = (instr >> 12) & 0xf;
            add_str(op ? "ccmp" : "ccmn");
            add_char(' ');
            add_reg_name((instr >> 5) & 0x1f, sf, 0);
            add_str(", ");
            add_reg_name((instr >> 16) & 0x1f, sf, 0);
            add_str(", #");
            add_dec_uint32(instr & 0xf);
            add_str(", ");
            add_str(cond_names[cond]);
        }
        return;
    }

    if ((instr & 0x1fe00800) == 0x1a400800) {
        /* Conditional compare (immediate) */
        int sf = (instr & (1 << 31)) != 0;
        int op = (instr & (1 << 30)) != 0;
        int s = (instr & (1 << 29)) != 0;
        int o2 = (instr & (1 << 10)) != 0;
        int o3 = (instr & (1 << 4)) != 0;
        if (s && !o2 && !o3) {
            uint32_t cond = (instr >> 12) & 0xf;
            add_str(op ? "ccmp" : "ccmn");
            add_char(' ');
            add_reg_name((instr >> 5) & 0x1f, sf, 0);
            add_str(", #");
            add_dec_uint32((instr >> 16) & 0x1f);
            add_str(", #");
            add_dec_uint32(instr & 0xf);
            add_str(", ");
            add_str(cond_names[cond]);
        }
        return;
    }

    if ((instr & 0x1fe00000) == 0x1a800000) {
        /* Conditional select */
        int sf = (instr & (1 << 31)) != 0;
        int op = (instr & (1 << 30)) != 0;
        int s = (instr & (1 << 29)) != 0;
        uint32_t op2 = (instr >> 10) & 3;
        if (!s) {
            uint32_t cond = (instr >> 12) & 0xf;
            uint32_t rn = (instr >> 5) & 0x1f;
            uint32_t rm = (instr >> 16) & 0x1f;
            if (rn == rm && cond < 14 && op2 == 1) {
                if (op) {
                    add_str("cneg ");
                    add_reg_name(instr & 0x1f, sf, 0);
                    add_str(", ");
                    add_reg_name(rn, sf, 0);
                }
                else if (rn == 31) {
                    add_str("cset ");
                    add_reg_name(instr & 0x1f, sf, 0);
                }
                else {
                    add_str("cinc ");
                    add_reg_name(instr & 0x1f, sf, 0);
                    add_str(", ");
                    add_reg_name(rn, sf, 0);
                }
                add_str(", ");
                add_str(cond_names[cond ^ 1]);
                return;
            }
            if (rn == rm && cond < 14 && op2 == 0 && op) {
                if (rn == 31) {
                    add_str("csetm ");
                    add_reg_name(instr & 0x1f, sf, 0);
                }
                else {
                    add_str("cinv ");
                    add_reg_name(instr & 0x1f, sf, 0);
                    add_str(", ");
                    add_reg_name(rn, sf, 0);
                }
                add_str(", ");
                add_str(cond_names[cond ^ 1]);
                return;
            }
            switch (op2) {
            case 0: add_str(op ? "csinv" : "csel"); break;
            case 1: add_str(op ? "csneg" : "csinc"); break;
            default: buf_pos = 0; return;
            }
            add_char(' ');
            add_reg_name(instr & 0x1f, sf, 0);
            add_str(", ");
            add_reg_name(rn, sf, 0);
            add_str(", ");
            add_reg_name(rm, sf, 0);
            add_str(", ");
            add_str(cond_names[cond]);
        }
        return;
    }

    if ((instr & 0x1f000000) == 0x1b000000) {
        /* Data-processing (3 source) */
        int sf = (instr & (1 << 31)) != 0;
        uint32_t op54 = (instr >> 29) & 3;
        uint32_t op31 = (instr >> 21) & 7;
        int o0 = (instr & (1 << 15)) != 0;
        if (op54 == 0) {
            uint32_t ra = (instr >> 10) & 0x1f;
            int no_ra = 0;
            int no_sf = 0;
            if (op31 == 0) {
                if (!o0 && ra == 31) {
                    add_str("mul");
                    no_ra = 1;
                }
                else {
                    add_str(o0 ? "msub" : "madd");
                }
            }
            else if (sf) {
                if (op31 == 1 && !o0 && ra == 31) {
                    add_str("smull");
                    no_sf = 1;
                    no_ra = 1;
                }
                else {
                    switch (op31) {
                    case 1: add_str(o0 ? "smsubl" : "smaddl"); no_sf = 1; break;
                    case 2: add_str(o0 ? "" : "smulh"); break;
                    case 5: add_str(o0 ? "umsubl" : "umaddl"); no_sf = 1; break;
                    case 6: add_str(o0 ? "" : "umulh"); no_ra = 1; break;
                    }
                }
            }
            if (buf_pos > 0) {
                add_char(' ');
                add_reg_name(instr & 0x1f, sf, 0);
                add_str(", ");
                add_reg_name((instr >> 5) & 0x1f, !no_sf && sf, 0);
                add_str(", ");
                add_reg_name((instr >> 16) & 0x1f, !no_sf && sf, 0);
                if (!no_ra) {
                    add_str(", ");
                    add_reg_name(ra, sf, 0);
                }
                return;
            }
        }
        return;
    }

    if ((instr & 0x5fe00000) == 0x1ac00000) {
        /* Data-processing (2 source) */
        int sf = (instr & (1 << 31)) != 0;
        int s = (instr & (1 << 29)) != 0;
        uint32_t opcode = (instr >> 10) & 0x3f;

        if (!s) {
            int no_sf = 0;
            switch (opcode) {
            case 2: add_str("udiv"); break;
            case 3: add_str("sdiv"); break;
            case 8: add_str("lsl"); break;
            case 9: add_str("lsr"); break;
            case 10: add_str("asr"); break;
            case 11: add_str("ror"); break;
            case 16: add_str(sf ? "" : "crc32b"); no_sf = 1; break;
            case 17: add_str(sf ? "" : "crc32h"); no_sf = 1; break;
            case 18: add_str(sf ? "" : "crc32w"); no_sf = 1; break;
            case 19: add_str(sf ? "crc32x" : ""); no_sf = 1; break;
            case 20: add_str(sf ? "" : "crc32cb"); no_sf = 1; break;
            case 21: add_str(sf ? "" : "crc32ch"); no_sf = 1; break;
            case 22: add_str(sf ? "" : "crc32cw"); no_sf = 1; break;
            case 23: add_str(sf ? "crc32cx" : ""); no_sf = 1; break;
            }

            if (buf_pos > 0) {
                add_char(' ');
                add_reg_name(instr & 0x1f, !no_sf && sf, 0);
                add_str(", ");
                add_reg_name((instr >> 5) & 0x1f, !no_sf && sf, 0);
                add_str(", ");
                add_reg_name((instr >> 16) & 0x1f, sf, 0);
            }
        }

        return;
    }

    if ((instr & 0x5fe00000) == 0x5ac00000) {
        /* Data-processing (1 source) */
        int sf = (instr & (1 << 31)) != 0;
        int s = (instr & (1 << 29)) != 0;
        uint32_t opcode = (instr >> 10) & 0x3f;
        uint32_t opcode2 = (instr >> 16) & 0x1f;

        if (!s && opcode2 == 0) {
            switch (opcode) {
            case 0: add_str("rbit"); break;
            case 1: add_str("rev16"); break;
            case 2: add_str(sf ? "rev32" : "rev"); break;
            case 3: add_str(sf ? "rev" : ""); break;
            case 4: add_str("clz"); break;
            case 5: add_str("cls"); break;
            }
            if (buf_pos > 0) {
                add_char(' ');
                add_reg_name(instr & 0x1f, sf, 0);
                add_str(", ");
                add_reg_name((instr >> 5) & 0x1f, sf, 0);
            }
        }

        return;
    }
}

static void data_processing_simd_and_fp(void) {
}

DisassemblyResult * disassemble_a64(uint8_t * code,
        ContextAddress addr, ContextAddress size,
        DisassemblerParams * disass_params) {
    unsigned i;
    static DisassemblyResult dr;

    if (size < 4) return NULL;
    memset(&dr, 0, sizeof(dr));
    dr.size = 4;
    buf_pos = 0;
    instr = 0;
    instr_addr = addr;
    for (i = 0; i < 4; i++) instr |= (uint32_t)*code++ << (i * 8);
    params = disass_params;

    if ((instr & 0x1c000000) == 0x10000000) data_processing_immediate();
    else if ((instr & 0x1c000000) == 0x14000000) branch_exception_system();
    else if ((instr & 0x0a000000) == 0x08000000) loads_and_stores();
    else if ((instr & 0x0e000000) == 0x0a000000) data_processing_register();
    else if ((instr & 0x0e000000) == 0x0e000000) data_processing_simd_and_fp();

    dr.text = buf;
    if (buf_pos == 0) {
        snprintf(buf, sizeof(buf), ".word 0x%08x", instr);
    }
    else {
        buf[buf_pos] = 0;
    }
    return &dr;
}

#endif /* SERVICE_Disassembly */
