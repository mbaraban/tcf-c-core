/*******************************************************************************
 * Copyright (c) 2014 Stanislav Yakovlev and others.
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

#include <tcf/config.h>

#include <stdio.h>
#include <tcf/services/symbols.h>
#include <machine/powerpc/tcf/disassembler-powerpc.h>

static char buf[128];
static size_t buf_pos = 0;
static Context * ctx = NULL;
static ContextAddress ctx_addr = 0;

#define bits_uint8(instr, bit, cnt) (uint8_t)((instr >> (32 - bit - cnt)) & ((1u << cnt) - 1))
#define bits_uint32(instr, bit, cnt) (uint32_t)((instr >> (32 - bit - cnt)) & ((1u << cnt) - 1))

static void add_char(char ch) {
    if (buf_pos >= sizeof(buf) - 1) return;
    buf[buf_pos++] = ch;
    if (ch == ' ') while (buf_pos < 8) buf[buf_pos++] = ch;
}

static void add_str(const char * s) {
    while (*s) add_char(*s++);
}

static void add_dec_uint8(uint8_t n) {
    char buf[32];

    snprintf(buf, sizeof(buf), "%u", (unsigned int)n);
    add_str(buf);
}

static void add_dec_int16(int16_t n) {
    char buf[32];

    snprintf(buf, sizeof(buf), "%d", (int)n);
    add_str(buf);
}

static void add_hex_uint16(uint16_t n) {
    char buf[32];

    snprintf(buf, sizeof(buf), "0x%.4x", (unsigned int)n);
    add_str(buf);
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

static void add_hex_uint64(uint64_t n) {
    char s[64];
    size_t i = 0;
    while (i < 16) {
        uint32_t d = n & 0xf;
        s[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        n = n >> 4;
    }
    while (i > 0) add_char(s[--i]);
}

static void add_addr(uint64_t addr) {
    add_hex_uint64(addr);
#if ENABLE_Symbols
    if (ctx != NULL) {
        Symbol * sym = NULL;
        char * name = NULL;
        ContextAddress sym_addr = 0;
        if (find_symbol_by_addr(ctx, STACK_NO_FRAME, (ContextAddress)addr, &sym) < 0) return;
        if (get_symbol_name(sym, &name) < 0 || name == NULL) return;
        if (get_symbol_address(sym, &sym_addr) < 0) return;
        if (sym_addr <= addr) {
            add_str(" ; ");
            add_str(name);
            if (sym_addr < addr) {
                add_str(" + 0x");
                add_hex_uint64(addr - sym_addr);
            }
        }
    }
#endif
}

static void add_trap_immediate(const char * mnemonic, uint8_t rX, uint8_t rA, uint16_t immediate) {
    /* mnemonic TO, rA, SI */
    add_str(mnemonic);
    add_str(" ");
    add_dec_uint8(rX);
    add_str(", r");
    add_dec_uint8(rA);
    add_str(", ");
    add_dec_int16((int16_t)immediate);
}

static void add_arithmetic_immediate(const char * mnemonic, uint8_t rX, uint8_t rA, uint16_t immediate) {
    /* mnemonic rX, rA, SI */
    add_str(mnemonic);
    add_str(" r");
    add_dec_uint8(rX);
    add_str(", r");
    add_dec_uint8(rA);
    add_str(", ");
    add_dec_int16((int16_t)immediate);
}

static void add_compare_logical_immediate(const char * mnemonic, uint8_t bf, uint8_t l, uint8_t rA, uint16_t immediate) {
    /* mnemonic BF, L, rA, UI */
    add_str(mnemonic);
    add_str(" cr");
    add_dec_uint8(bf);
    add_str(", ");
    add_dec_uint8(l);
    add_str(", r");
    add_dec_uint8(rA);
    add_str(", ");
    add_hex_uint16(immediate);
}

static void add_compare_immediate(const char * mnemonic, uint8_t bf, uint8_t l, uint8_t rA, uint16_t immediate) {
    /* mnemonic BF, L, rA, SI */
    add_str(mnemonic);
    add_str(" cr");
    add_dec_uint8(bf);
    add_str(", ");
    add_dec_uint8(l);
    add_str(", r");
    add_dec_uint8(rA);
    add_str(", ");
    add_dec_int16((int16_t)immediate);
}

static void add_logical_immediate(const char * mnemonic, uint8_t rX, uint8_t rA, uint16_t immediate) {
    /* mnemonic rA, rX, UI */
    add_str(mnemonic);
    add_str(" r");
    add_dec_uint8(rA);
    add_str(", r");
    add_dec_uint8(rX);
    add_str(", ");
    add_hex_uint16(immediate);
}

static void add_store_access_immediate(const char * mnemonic, uint8_t rX, uint8_t rA, uint16_t immediate) {
    /* mnemonic rX, D(rA) */
    add_str(mnemonic);
    add_str(" r");
    add_dec_uint8(rX);
    add_str(", ");
    add_dec_int16(immediate);
    add_str("(r");
    add_dec_uint8(rA);
    add_str(")");
}

static void add_xo_form(uint32_t instr, const char * mnemonic) {
    uint8_t rA = bits_uint8(instr, 11, 5);
    uint8_t rB = bits_uint8(instr, 16, 5);
    uint8_t rD = bits_uint8(instr, 6, 5);

    add_str(mnemonic);
    if (bits_uint8(instr, 21, 1)) add_char('o');
    if (bits_uint8(instr, 31, 1)) add_char('.');
    add_str(" r");
    add_dec_uint8(rD);
    add_str(", r");
    add_dec_uint8(rA);
    add_str(", r");
    add_dec_uint8(rB);
}

static void add_xo_form2(uint32_t instr, const char * mnemonic) {
    uint8_t rA = bits_uint8(instr, 11, 5);
    uint8_t rD = bits_uint8(instr, 6, 5);

    add_str(mnemonic);
    if (bits_uint8(instr, 21, 1)) add_char('o');
    if (bits_uint8(instr, 31, 1)) add_char('.');
    add_str(" r");
    add_dec_uint8(rD);
    add_str(", r");
    add_dec_uint8(rA);
}

static void add_op_3r(uint32_t instr, const char * mnemonic) {
    uint8_t rX = bits_uint8(instr, 6, 5);
    uint8_t rA = bits_uint8(instr, 11, 5);
    uint8_t rB = bits_uint8(instr, 16, 5);
    add_str(mnemonic);
    add_str(" r");
    add_dec_uint8(rX);
    add_str(", r");
    add_dec_uint8(rA);
    add_str(", r");
    add_dec_uint8(rB);
}

static void add_op_3r_rc(uint32_t instr, const char * mnemonic) {
    uint8_t rX = bits_uint8(instr, 6, 5);
    uint8_t rA = bits_uint8(instr, 11, 5);
    uint8_t rB = bits_uint8(instr, 16, 5);
    add_str(mnemonic);
    if (bits_uint8(instr, 31, 1)) add_char('.');
    add_str(" r");
    add_dec_uint8(rA);
    add_str(", r");
    add_dec_uint8(rX);
    add_str(", r");
    add_dec_uint8(rB);
}

static void add_op_2r_rc(uint32_t instr, const char * mnemonic) {
    uint8_t rX = bits_uint8(instr, 6, 5);
    uint8_t rA = bits_uint8(instr, 11, 5);
    add_str(mnemonic);
    if (bits_uint8(instr, 31, 1)) add_char('.');
    add_str(" r");
    add_dec_uint8(rA);
    add_str(", r");
    add_dec_uint8(rX);
}

static void add_op_31(uint32_t instr) {
    uint32_t xop = bits_uint32(instr, 21, 10);
    uint8_t rX = bits_uint8(instr, 6, 5);
    uint8_t rA = bits_uint8(instr, 11, 5);
    uint8_t rB = bits_uint8(instr, 16, 5);
    switch (xop) {
    case 0:
        add_str("cmp");
        add_str(" ");
        add_dec_uint8(bits_uint8(instr, 6, 3));
        add_str(", ");
        add_dec_uint8(bits_uint8(instr, 10, 1));
        add_str(", r");
        add_dec_uint8(rA);
        add_str(", r");
        add_dec_uint8(rB);
        break;
    case 4:
        add_str("tw");
        add_str(" ");
        add_dec_uint8(bits_uint8(instr, 6, 5));
        add_str(", r");
        add_dec_uint8(rA);
        add_str(", r");
        add_dec_uint8(rB);
        break;
    case 8:
        add_xo_form(instr, "subfc");
        break;
    case 9:
        add_xo_form(instr, "mulhdu");
        break;
    case 10:
        add_xo_form(instr, "addc");
        break;
    case 11:
        add_xo_form(instr, "mulhwu");
        break;
    case 19:
        if (bits_uint8(instr, 11, 1) == 0) {
            add_str("mfcr");
            add_str(" r");
            add_dec_uint8(bits_uint8(instr, 6, 5));
        }
        else {
            add_str("mfocrf");
            add_str(" r");
            add_dec_uint8(bits_uint8(instr, 6, 5));
            add_str(", ");
            add_dec_uint8(bits_uint32(instr, 12, 8));
        }
        break;
    case 20:
        add_op_3r(instr, "lwarx");
        break;
    case 21:
        add_op_3r(instr, "ldx");
        break;
    case 23:
        add_op_3r(instr, "lwzx");
        break;
    case 24:
        add_op_3r_rc(instr, "slw");
        break;
    case 26:
        add_op_2r_rc(instr, "cntlzw");
        break;
    case 27:
        add_op_3r_rc(instr, "sld");
        break;
    case 28:
        add_op_3r_rc(instr, "and");
        break;
    case 32:
        add_str("cmpl");
        add_str(" ");
        add_dec_uint8(bits_uint8(instr, 6, 3));
        add_str(", ");
        add_dec_uint8(bits_uint8(instr, 10, 1));
        add_str(", r");
        add_dec_uint8(rA);
        add_str(", r");
        add_dec_uint8(rB);
        break;
    case 40:
        add_xo_form(instr, "subf");
        break;
    case 55:
        add_op_3r(instr, "lwzux");
        break;
    case 73:
        add_xo_form(instr, "mulhd");
        break;
    case 75:
        add_xo_form(instr, "mulhw");
        break;
    case 83:
        add_str("mfmsr");
        add_str(" r");
        add_dec_uint8(rX);
        break;
    case 104:
        add_xo_form(instr, "neg");
        break;
    case 136:
        add_xo_form(instr, "subfe");
        break;
    case 138:
        add_xo_form(instr, "adde");
        break;
    case 146:
        add_str("mtmsr");
        add_str(" r");
        add_dec_uint8(rX);
        add_str(", ");
        add_dec_uint8(bits_uint8(instr, 15, 1));
        break;
    case 200:
        add_xo_form(instr, "subfze");
        break;
    case 202:
        add_xo_form(instr, "addze");
        break;
    case 232:
        add_xo_form2(instr, "subfme");
        break;
    case 233:
        add_xo_form(instr, "mulld");
        break;
    case 234:
        add_xo_form2(instr, "addme");
        break;
    case 235:
        add_xo_form(instr, "mullw");
        break;
    case 266:
        add_xo_form(instr, "add");
        break;
    case 339:
        {
            uint32_t spr = bits_uint8(instr, 11, 10);
            if (spr == 1) {
                add_str("mfxer");
                add_str(" r");
                add_dec_uint8(rX);
            }
            else if (spr == 8) {
                add_str("mflr");
                add_str(" r");
                add_dec_uint8(rX);
            }
            else if (spr == 9) {
                add_str("mfctr");
                add_str(" r");
                add_dec_uint8(rX);
            }
            else {
                add_str("mfspr");
                add_str(" r");
                add_dec_uint8(rX);
                add_str(", ");
                add_dec_uint32(spr);
            }
        }
        break;
    case 444:
        if (rX == rB) {
            add_str("mr");
            add_str(" r");
            add_dec_uint8(rA);
            add_str(", r");
            add_dec_uint8(rX);
        }
        else {
            add_str("or");
            add_str(" r");
            add_dec_uint8(rA);
            add_str(", r");
            add_dec_uint8(rX);
            add_str(", r");
            add_dec_uint8(rB);
        }
        break;
    case 457:
        add_xo_form(instr, "divdu");
        break;
    case 459:
        add_xo_form(instr, "divwu");
        break;
    case 467:
        {
            uint32_t spr = bits_uint8(instr, 11, 10);
            if (spr == 1) {
                add_str("mtxer");
                add_str(" r");
                add_dec_uint8(rX);
            }
            else if (spr == 8) {
                add_str("mtlr");
                add_str(" r");
                add_dec_uint8(rX);
            }
            else if (spr == 9) {
                add_str("mtctr");
                add_str(" r");
                add_dec_uint8(rX);
            }
            else {
                add_str("mtspr");
                add_str(" ");
                add_dec_uint32(spr);
                add_str(", ");
                add_dec_uint8(rX);
            }
        }
        break;
    case 489:
        add_xo_form(instr, "divd");
        break;
    case 491:
        add_xo_form(instr, "divw");
        break;
    case 792:
        add_op_3r_rc(instr, "sraw");
        break;
    case 794:
        add_op_3r_rc(instr, "srad");
        break;
    case 824:
        add_op_3r_rc(instr, "srawi");
        break;
    case 922:
        add_op_2r_rc(instr, "extsh");
        break;
    case 954:
        add_op_2r_rc(instr, "extsb");
        break;
    case 986:
        add_op_2r_rc(instr, "extsw");
        break;
    }
}

static void add_m_form(uint32_t instr, const char * mnemonic) {
    add_str(mnemonic);
    if (bits_uint8(instr, 31, 1)) add_char('.');
    add_str(" r");
    add_dec_uint8(bits_uint8(instr, 11, 5));
    add_str(", r");
    add_dec_uint8(bits_uint8(instr, 6, 5));
    add_str(", ");
    add_dec_uint8(bits_uint8(instr, 16, 5));
    add_str(", ");
    add_dec_uint8(bits_uint8(instr, 21, 5));
    add_str(", ");
    add_dec_uint8(bits_uint8(instr, 26, 5));
}

static void add_op_19(uint32_t instr) {
    uint32_t xop = bits_uint32(instr, 21, 10);
    switch (xop) {
    case 0:
        add_str("mcrf");
        add_str(" ");
        add_dec_uint8(bits_uint8(instr, 6, 3));
        add_str(", ");
        add_dec_uint8(bits_uint8(instr, 11, 3));
        return;
    case 18:
        add_str("rfid");
        return;
    case 150:
        add_str("isync");
        return;
    case 274:
        add_str("hrfid");
        return;
    }

    switch (xop) {
    case 16:
        add_str("bclr");
        break;
    case 528:
        add_str("bcctr");
        break;
    }

    if (buf_pos > 0) {
        if (bits_uint8(instr, 31, 1)) add_char('l');
        add_str(" ");
        add_dec_uint8(bits_uint8(instr, 6, 5));
        add_str(", ");
        add_dec_uint8(bits_uint8(instr, 11, 5));
        add_str(", ");
        add_dec_uint8(bits_uint8(instr, 19, 2));
        return;
    }

    switch (xop) {
    case 33:
        add_str("crnor");
        break;
    case 129:
        add_str("crandc");
        break;
    case 193:
        add_str("crxor");
        break;
    case 225:
        add_str("crnand");
        break;
    case 257:
        add_str("crand");
        break;
    case 289:
        add_str("creqv");
        break;
    case 417:
        add_str("crorc");
        break;
    case 449:
        add_str("cror");
        break;
    }

    if (buf_pos > 0) {
        add_str(" ");
        add_dec_uint8(bits_uint8(instr, 6, 5));
        add_str(", ");
        add_dec_uint8(bits_uint8(instr, 11, 5));
        add_str(", ");
        add_dec_uint8(bits_uint8(instr, 16, 5));
        return;
    }
}

static void disassemble_opcode(uint32_t instr) {
    uint8_t opcode = (instr & 0xfc000000) >> 26; /* bits 0-5 */
    /* D-Form */
    uint8_t rX = (uint8_t)((instr & 0x03e00000) >> 21); /* bits 6-10  */
    uint8_t rA = (uint8_t)((instr & 0x001f0000) >> 16); /* bits 11-15 */
    uint16_t immediate =  instr & 0xffff;        /* bits 16-31 */
    /* Compare and compare logical D-Form */
    uint8_t bf = rX >> 2;
    uint8_t zero = rX & 0x2;
    uint8_t l = rX & 0x1;

    switch (opcode) {
        /* 0 */
        /* 1 */
        case 2:
            add_trap_immediate("tdi", rX, rA, immediate);
            break;
        case 3:
            add_trap_immediate("twi", rX, rA, immediate);
            break;
        /* 4 */
        /* 5 */
        /* 6 */
        case 7:
            add_arithmetic_immediate("mulli", rX, rA, immediate);
            break;
        case 8:
            add_arithmetic_immediate("subfic", rX, rA, immediate);
            break;
        /* 9 */
        case 10:
            if (zero == 0) {
                add_compare_logical_immediate("cmpli", bf, l, rA, immediate);
            }
            break;
        case 11:
            if (zero == 0) {
                add_compare_immediate("cmpi", bf, l, rA, immediate);
            }
            break;
        case 12:
            add_arithmetic_immediate("addic", rX, rA, immediate);
            break;
        case 13:
            add_arithmetic_immediate("addic.", rX, rA, immediate);
            break;
        case 14:
            add_arithmetic_immediate("addi", rX, rA, immediate);
            break;
        case 15:
            add_arithmetic_immediate("addis", rX, rA, immediate);
            break;
        case 16:
            {
                uint64_t addr = bits_uint32(instr, 16, 14) << 2;
                if (addr & (1 << 15)) addr |= ~(((uint64_t)1 << 16) - 1);
                add_str("bc");
                if (bits_uint8(instr, 31, 1)) add_char('l');
                if (bits_uint8(instr, 30, 1)) {
                    add_char('a');
                }
                else {
                    addr = ctx_addr + addr;
                }
                add_str(" ");
                add_dec_uint8(bits_uint8(instr, 6, 5));
                add_str(", ");
                add_dec_uint8(bits_uint8(instr, 11, 5));
                add_str(", ");
                add_addr(addr);
            }
            break;
        /* 17 */
        case 18:
            {
                uint64_t addr = bits_uint32(instr, 6, 24) << 2;
                if (addr & (1 << 25)) addr |= ~(((uint64_t)1 << 26) - 1);
                add_str("b");
                if (bits_uint8(instr, 31, 1)) add_char('l');
                if (bits_uint8(instr, 30, 1)) {
                    add_char('a');
                }
                else {
                    addr = ctx_addr + addr;
                }
                add_str(" ");
                add_addr(addr);
            }
            break;
        case 19:
            add_op_19(instr);
            break;
        case 20:
            add_m_form(instr, "rlwimi");
            break;
        case 21:
            add_m_form(instr, "rlwinm");
            break;
        /* 23 */
        case 23:
            add_m_form(instr, "rlwnm");
            break;
        case 24:
            add_logical_immediate("ori", rX, rA, immediate);
            break;
        case 25:
            add_logical_immediate("oris", rX, rA, immediate);
            break;
        case 26:
            add_logical_immediate("xori", rX, rA, immediate);
            break;
        case 27:
            add_logical_immediate("xoris", rX, rA, immediate);
            break;
        case 28:
            add_logical_immediate("andi.", rX, rA, immediate);
            break;
        case 29:
            add_logical_immediate("andis.", rX, rA, immediate);
            break;
        /* 30 */
        case 31:
            add_op_31(instr);
            break;
        case 32:
            add_store_access_immediate("lwz", rX, rA, immediate);
            break;
        case 33:
            if (rA != 0 && rA != rX) {
                add_store_access_immediate("lwzu", rX, rA, immediate);
            }
            break;
        case 34:
            add_store_access_immediate("lbz", rX, rA, immediate);
            break;
        case 35:
            if (rA != 0 && rA != rX) {
                add_store_access_immediate("lbzu", rX, rA, immediate);
            }
            break;
        case 36:
            add_store_access_immediate("stw", rX, rA, immediate);
            break;
        case 37:
            if (rA != 0) {
                add_store_access_immediate("stwu", rX, rA, immediate);
            }
            break;
        case 38:
            add_store_access_immediate("stb", rX, rA, immediate);
            break;
        case 39:
            if (rA != 0) {
                add_store_access_immediate("stbu", rX, rA, immediate);
            }
            break;
        case 40:
            add_store_access_immediate("lhz", rX, rA, immediate);
            break;
        case 41:
            if (rA != 0 && rA != rX) {
                add_store_access_immediate("lhzu", rX, rA, immediate);
            }
            break;
        case 42:
            add_store_access_immediate("lha", rX, rA, immediate);
            break;
        case 43:
            if (rA != 0 && rA != rX) {
                add_store_access_immediate("lhau", rX, rA, immediate);
            }
            break;
        case 44:
            add_store_access_immediate("sth", rX, rA, immediate);
            break;
        case 45:
            if (rA != 0) {
                add_store_access_immediate("sthu", rX, rA, immediate);
            }
            break;
        case 46:
            if (rA < rX) {
                add_store_access_immediate("lmw", rX, rA, immediate);
            }
            break;
        case 47:
            add_store_access_immediate("stmw", rX, rA, immediate);
            break;
        case 48:
            add_store_access_immediate("lfs", rX, rA, immediate);
            break;
        case 49:
            if (rA != 0) {
                add_store_access_immediate("lfsu", rX, rA, immediate);
            }
            break;
        case 50:
            add_store_access_immediate("lfd", rX, rA, immediate);
            break;
        case 51:
            if (rA != 0) {
                add_store_access_immediate("lfdu", rX, rA, immediate);
            }
            break;
        case 52:
            add_store_access_immediate("stfs", rX, rA, immediate);
            break;
        case 53:
            if (rA != 0) {
                add_store_access_immediate("stfsu", rX, rA, immediate);
            }
            break;
        case 54:
            add_store_access_immediate("stfd", rX, rA, immediate);
            break;
        case 55:
            if (rA != 0) {
                add_store_access_immediate("stfdu", rX, rA, immediate);
            }
            break;
        /* 56 */
        /* 57 */
        case 58:
            {
                const char * mnemonic[] = {
                    "ld", "ldu", "lwa"
                };
                uint8_t ds_type = immediate & 0x3;
                uint16_t ds_imm = immediate & ~0x3;

                if (ds_type == 1 && (rA == 0 || rA == rX)) {
                    break;
                }
                if (ds_type < 3) {
                    add_store_access_immediate(mnemonic[ds_type], rX, rA, ds_imm);
                }
                break;
            }
        /* 59 - 59 */
        /* 60 */
        /* 61 */
        case 62:
            {
                const char * mnemonic[] = {
                    "std", "stdu"
                };
                uint8_t ds_type = immediate & 0x3;
                uint16_t ds_imm = immediate & ~0x3;

                if (ds_type == 1 && rA == 0) {
                    break;
                }
                if (ds_type < 2) {
                    add_store_access_immediate(mnemonic[ds_type], rX, rA, ds_imm);
                }
                break;
            }
        /* 63 - 63 */
    }
}

DisassemblyResult * disassemble_powerpc(uint8_t * code,
        ContextAddress addr, ContextAddress size, DisassemblerParams * params) {
    static DisassemblyResult dr;
    uint32_t instr;

    if (size < 4) return NULL;
    memset(&dr, 0, sizeof(dr));
    dr.size = 4;
    buf_pos = 0;
    ctx = params->ctx;
    ctx_addr = addr;

    instr = code[0];
    instr <<= 8;
    instr |= code[1];
    instr <<= 8;
    instr |= code[2];
    instr <<= 8;
    instr |= code[3];

    disassemble_opcode(instr);

    if (buf_pos == 0) {
        snprintf(buf, sizeof(buf), ".word 0x%08x ; opcode %d,%d",
            instr, bits_uint8(instr, 0, 6), bits_uint32(instr, 21, 10));
    }
    else {
        buf[buf_pos] = 0;
    }

    dr.text = buf;
    return &dr;
}
