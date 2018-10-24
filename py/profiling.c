
/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) SatoshiLabs
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "py/runtime.h"

#if MICROPY_PY_SYS_PROFILING

#include "py/profiling.h"
#include "py/objstr.h"
#include "py/emitglue.h"
#include "py/runtime.h"
#include "py/bc0.h"

#include <stdio.h>
#include <string.h>

typedef struct _mp_bytecode_prelude_t {
    uint n_state;
    uint n_exc_stack;
    uint scope_flags;
    uint n_pos_args;
    uint n_kwonly_args;
    uint n_def_pos_args;
    const char* source_file;
    qstr qstr_source_file;
    const char* block_name;
    qstr qstr_block_name;
    const byte* code_info;
    const byte* line_info;
    const byte* locals;
    const byte* bytecode;
} mp_bytecode_prelude_t;

typedef struct _mp_dis_instruction {
    mp_obj_t opcode;
    mp_obj_t opname;
    mp_obj_t arg;
    mp_obj_t argval;
    mp_obj_t cache;
} mp_dis_instruction;

#define DECODE_UINT { \
    unum = 0; \
    do { \
        unum = (unum << 7) + (*ip & 0x7f); \
    } while ((*ip++ & 0x80) != 0); \
}
#define DECODE_ULABEL do { unum = (ip[0] | (ip[1] << 8)); ip += 2; } while (0)
#define DECODE_SLABEL do { unum = (ip[0] | (ip[1] << 8)) - 0x8000; ip += 2; } while (0)

#define DECODE_QSTR \
    qst = ip[0] | ip[1] << 8; \
    ip += 2;
#define DECODE_PTR \
    DECODE_UINT; \
    ptr = (const byte*)const_table[unum]
#define DECODE_OBJ \
    DECODE_UINT; \
    obj = (mp_obj_t)const_table[unum]

STATIC const byte *opcode_decode(const byte *ip, const mp_uint_t *const_table, mp_dis_instruction *instruction) {
    mp_uint_t unum;
    const byte* ptr;
    mp_obj_t obj;
    qstr qst;

    instruction->opname = mp_const_none;
    instruction->arg = mp_const_none;
    instruction->argval = mp_const_none;
    instruction->cache = mp_const_none;

    switch (*ip++) {
        case MP_BC_LOAD_CONST_FALSE:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_LOAD_CONST_FALSE);
            instruction->opname = MP_ROM_QSTR(MP_QSTR_LOAD_CONST_FALSE);
            break;

        case MP_BC_LOAD_CONST_NONE:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_LOAD_CONST_NONE);
            break;

        case MP_BC_LOAD_CONST_TRUE:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_LOAD_CONST_TRUE);
            break;

        case MP_BC_LOAD_CONST_SMALL_INT: {
            mp_int_t num = 0;
            if ((ip[0] & 0x40) != 0) {
                // Number is negative
                num--;
            }
            do {
                num = (num << 7) | (*ip & 0x7f);
            } while ((*ip++ & 0x80) != 0);
            instruction->opname = MP_ROM_QSTR(MP_QSTR_LOAD_CONST_SMALL_INT);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(num);
            break;
        }

        case MP_BC_LOAD_CONST_STRING:
            DECODE_QSTR;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_LOAD_CONST_STRING);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(qst);
            instruction->argval = MP_OBJ_NEW_QSTR(qst);
            break;

        case MP_BC_LOAD_CONST_OBJ:
            DECODE_OBJ;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_LOAD_CONST_OBJ);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            instruction->argval = obj;
            break;

        case MP_BC_LOAD_NULL:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_LOAD_NULL);
            break;

        case MP_BC_LOAD_FAST_N:
            DECODE_UINT;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_LOAD_FAST_N);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_LOAD_DEREF:
            DECODE_UINT;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_LOAD_DEREF);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_LOAD_NAME:
            DECODE_QSTR;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_LOAD_NAME);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(qst);
            instruction->argval = MP_OBJ_NEW_QSTR(qst);
            if (MICROPY_OPT_CACHE_MAP_LOOKUP_IN_BYTECODE) {
                instruction->cache = MP_OBJ_NEW_SMALL_INT(*ip++);
            }
            break;

        case MP_BC_LOAD_GLOBAL:
            DECODE_QSTR;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_LOAD_GLOBAL);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(qst);
            instruction->argval = MP_OBJ_NEW_QSTR(qst);
            if (MICROPY_OPT_CACHE_MAP_LOOKUP_IN_BYTECODE) {
                instruction->cache = MP_OBJ_NEW_SMALL_INT(*ip++);
            }
            break;

        case MP_BC_LOAD_ATTR:
            DECODE_QSTR;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_LOAD_ATTR);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(qst);
            instruction->argval = MP_OBJ_NEW_QSTR(qst);
            if (MICROPY_OPT_CACHE_MAP_LOOKUP_IN_BYTECODE) {
                instruction->cache = MP_OBJ_NEW_SMALL_INT(*ip++);
            }
            break;

        case MP_BC_LOAD_METHOD:
            DECODE_QSTR;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_LOAD_METHOD);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(qst);
            instruction->argval = MP_OBJ_NEW_QSTR(qst);
            break;

        case MP_BC_LOAD_SUPER_METHOD:
            DECODE_QSTR;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_LOAD_SUPER_METHOD);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(qst);
            instruction->argval = MP_OBJ_NEW_QSTR(qst);
            break;

        case MP_BC_LOAD_BUILD_CLASS:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_LOAD_BUILD_CLASS);
            break;

        case MP_BC_LOAD_SUBSCR:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_LOAD_SUBSCR);
            break;

        case MP_BC_STORE_FAST_N:
            DECODE_UINT;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_STORE_FAST_N);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_STORE_DEREF:
            DECODE_UINT;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_STORE_DEREF);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_STORE_NAME:
            DECODE_QSTR;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_STORE_NAME);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(qst);
            instruction->argval = MP_OBJ_NEW_QSTR(qst);
            break;

        case MP_BC_STORE_GLOBAL:
            DECODE_QSTR;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_STORE_GLOBAL);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(qst);
            instruction->argval = MP_OBJ_NEW_QSTR(qst);
            break;

        case MP_BC_STORE_ATTR:
            DECODE_QSTR;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_STORE_ATTR);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(qst);
            instruction->argval = MP_OBJ_NEW_QSTR(qst);
            if (MICROPY_OPT_CACHE_MAP_LOOKUP_IN_BYTECODE) {
                instruction->cache = MP_OBJ_NEW_SMALL_INT(*ip++);
            }
            break;

        case MP_BC_STORE_SUBSCR:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_STORE_SUBSCR);
            break;

        case MP_BC_DELETE_FAST:
            DECODE_UINT;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_DELETE_FAST);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_DELETE_DEREF:
            DECODE_UINT;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_DELETE_DEREF);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_DELETE_NAME:
            DECODE_QSTR;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_DELETE_NAME);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(qst);
            instruction->argval = MP_OBJ_NEW_QSTR(qst);
            break;

        case MP_BC_DELETE_GLOBAL:
            DECODE_QSTR;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_DELETE_GLOBAL);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(qst);
            instruction->argval = MP_OBJ_NEW_QSTR(qst);
            break;

        case MP_BC_DUP_TOP:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_DUP_TOP);
            break;

        case MP_BC_DUP_TOP_TWO:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_DUP_TOP_TWO);
            break;

        case MP_BC_POP_TOP:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_POP_TOP);
            break;

        case MP_BC_ROT_TWO:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_ROT_TWO);
            break;

        case MP_BC_ROT_THREE:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_ROT_THREE);
            break;

        case MP_BC_JUMP:
            DECODE_SLABEL;
            // printf("JUMP " UINT_FMT, (mp_uint_t)(ip + unum - code_start));
            instruction->opname = MP_ROM_QSTR(MP_QSTR_JUMP);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_POP_JUMP_IF_TRUE:
            DECODE_SLABEL;
            // printf("POP_JUMP_IF_TRUE " UINT_FMT, (mp_uint_t)(ip + unum - code_start));
            instruction->opname = MP_ROM_QSTR(MP_QSTR_POP_JUMP_IF_TRUE);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_POP_JUMP_IF_FALSE:
            DECODE_SLABEL;
            // printf("POP_JUMP_IF_FALSE " UINT_FMT, (mp_uint_t)(ip + unum - code_start));
            instruction->opname = MP_ROM_QSTR(MP_QSTR_POP_JUMP_IF_FALSE);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_JUMP_IF_TRUE_OR_POP:
            DECODE_SLABEL;
            // printf("JUMP_IF_TRUE_OR_POP " UINT_FMT, (mp_uint_t)(ip + unum - code_start));
            instruction->opname = MP_ROM_QSTR(MP_QSTR_JUMP_IF_TRUE_OR_POP);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_JUMP_IF_FALSE_OR_POP:
            DECODE_SLABEL;
            // printf("JUMP_IF_FALSE_OR_POP " UINT_FMT, (mp_uint_t)(ip + unum - code_start));
            instruction->opname = MP_ROM_QSTR(MP_QSTR_JUMP_IF_FALSE_OR_POP);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_SETUP_WITH:
            DECODE_ULABEL; // loop-like labels are always forward
            // printf("SETUP_WITH " UINT_FMT, (mp_uint_t)(ip + unum - code_start));
            instruction->opname = MP_ROM_QSTR(MP_QSTR_SETUP_WITH);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_WITH_CLEANUP:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_WITH_CLEANUP);
            break;

        case MP_BC_UNWIND_JUMP:
            DECODE_SLABEL;
            // printf("UNWIND_JUMP " UINT_FMT " %d", (mp_uint_t)(ip + unum - code_start), *ip);
            instruction->opname = MP_ROM_QSTR(MP_QSTR_UNWIND_JUMP);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_SETUP_EXCEPT:
            DECODE_ULABEL; // except labels are always forward
            // printf("SETUP_EXCEPT " UINT_FMT, (mp_uint_t)(ip + unum - code_start));
            instruction->opname = MP_ROM_QSTR(MP_QSTR_SETUP_EXCEPT);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_SETUP_FINALLY:
            DECODE_ULABEL; // except labels are always forward
            // printf("SETUP_FINALLY " UINT_FMT, (mp_uint_t)(ip + unum - code_start));
            instruction->opname = MP_ROM_QSTR(MP_QSTR_SETUP_FINALLY);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_END_FINALLY:
            // if TOS is an exception, reraises the exception (3 values on TOS)
            // if TOS is an integer, does something else
            // if TOS is None, just pops it and continues
            // else error
            instruction->opname = MP_ROM_QSTR(MP_QSTR_END_FINALLY);
            break;

        case MP_BC_GET_ITER:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_GET_ITER);
            break;

        case MP_BC_GET_ITER_STACK:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_GET_ITER_STACK);
            break;

        case MP_BC_FOR_ITER:
            DECODE_ULABEL; // the jump offset if iteration finishes; for labels are always forward
            // printf("FOR_ITER " UINT_FMT, (mp_uint_t)(ip + unum - code_start));
            instruction->opname = MP_ROM_QSTR(MP_QSTR_FOR_ITER);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_POP_BLOCK:
            // pops block and restores the stack
            instruction->opname = MP_ROM_QSTR(MP_QSTR_POP_BLOCK);
            break;

        case MP_BC_POP_EXCEPT:
            // pops block, checks it's an exception block, and restores the stack, saving the 3 exception values to local threadstate
            instruction->opname = MP_ROM_QSTR(MP_QSTR_POP_EXCEPT);
            break;

        case MP_BC_BUILD_TUPLE:
            DECODE_UINT;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_BUILD_TUPLE);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_BUILD_LIST:
            DECODE_UINT;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_BUILD_LIST);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_BUILD_MAP:
            DECODE_UINT;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_BUILD_MAP);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_STORE_MAP:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_STORE_MAP);
            break;

        case MP_BC_BUILD_SET:
            DECODE_UINT;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_BUILD_SET);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

#if MICROPY_PY_BUILTINS_SLICE
        case MP_BC_BUILD_SLICE:
            DECODE_UINT;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_BUILD_SLICE);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;
#endif

        case MP_BC_STORE_COMP:
            DECODE_UINT;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_STORE_COMP);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_UNPACK_SEQUENCE:
            DECODE_UINT;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_UNPACK_SEQUENCE);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_UNPACK_EX:
            DECODE_UINT;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_UNPACK_EX);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_MAKE_FUNCTION:
            DECODE_PTR;
            // printf("MAKE_FUNCTION %p", (void*)(uintptr_t)unum);
            instruction->opname = MP_ROM_QSTR(MP_QSTR_MAKE_FUNCTION);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            instruction->argval = mp_obj_new_int_from_ull((uint64_t)ptr);
            break;

        case MP_BC_MAKE_FUNCTION_DEFARGS:
            DECODE_PTR;
            // printf("MAKE_FUNCTION_DEFARGS %p", (void*)(uintptr_t)unum);
            instruction->opname = MP_ROM_QSTR(MP_QSTR_MAKE_FUNCTION_DEFARGS);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            instruction->argval = mp_obj_new_int_from_ull((uint64_t)ptr);
            break;

        case MP_BC_MAKE_CLOSURE: {
            DECODE_PTR;
            mp_uint_t n_closed_over = *ip++;
            // printf("MAKE_CLOSURE %p " UINT_FMT, (void*)(uintptr_t)unum, n_closed_over);
            instruction->opname = MP_ROM_QSTR(MP_QSTR_MAKE_CLOSURE);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            instruction->argval = mp_obj_new_int_from_ull((uint64_t)ptr);
            instruction->cache = MP_OBJ_NEW_SMALL_INT(n_closed_over);
            break;
        }

        case MP_BC_MAKE_CLOSURE_DEFARGS: {
            DECODE_PTR;
            mp_uint_t n_closed_over = *ip++;
            // printf("MAKE_CLOSURE_DEFARGS %p " UINT_FMT, (void*)(uintptr_t)unum, n_closed_over);
            instruction->opname = MP_ROM_QSTR(MP_QSTR_MAKE_CLOSURE_DEFARGS);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            instruction->argval = mp_obj_new_int_from_ull((uint64_t)ptr);
            instruction->cache = MP_OBJ_NEW_SMALL_INT(n_closed_over);
            break;
        }

        case MP_BC_CALL_FUNCTION:
            DECODE_UINT;
            // printf("CALL_FUNCTION n=" UINT_FMT " nkw=" UINT_FMT, unum & 0xff, (unum >> 8) & 0xff);
            instruction->opname = MP_ROM_QSTR(MP_QSTR_CALL_FUNCTION);
            // instruction->arg = MP_OBJ_NEW_SMALL_INT(unum & 0xff);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum & 0xff);
            instruction->cache = MP_OBJ_NEW_SMALL_INT((unum >> 8) & 0xff);
            break;

        case MP_BC_CALL_FUNCTION_VAR_KW:
            DECODE_UINT;
            // printf("CALL_FUNCTION_VAR_KW n=" UINT_FMT " nkw=" UINT_FMT, unum & 0xff, (unum >> 8) & 0xff);
            instruction->opname = MP_ROM_QSTR(MP_QSTR_CALL_FUNCTION_VAR_KW);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum & 0xff);
            instruction->cache = MP_OBJ_NEW_SMALL_INT((unum >> 8) & 0xff);
            break;

        case MP_BC_CALL_METHOD:
            DECODE_UINT;
            // printf("CALL_METHOD n=" UINT_FMT " nkw=" UINT_FMT, unum & 0xff, (unum >> 8) & 0xff);
            instruction->opname = MP_ROM_QSTR(MP_QSTR_CALL_METHOD);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum & 0xff);
            instruction->cache = MP_OBJ_NEW_SMALL_INT((unum >> 8) & 0xff);
            break;

        case MP_BC_CALL_METHOD_VAR_KW:
            DECODE_UINT;
            // printf("CALL_METHOD_VAR_KW n=" UINT_FMT " nkw=" UINT_FMT, unum & 0xff, (unum >> 8) & 0xff);
            instruction->opname = MP_ROM_QSTR(MP_QSTR_CALL_METHOD_VAR_KW);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum & 0xff);
            instruction->cache = MP_OBJ_NEW_SMALL_INT((unum >> 8) & 0xff);
            break;

        case MP_BC_RETURN_VALUE:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_RETURN_VALUE);
            break;

        case MP_BC_RAISE_VARARGS:
            unum = *ip++;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_RAISE_VARARGS);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(unum);
            break;

        case MP_BC_YIELD_VALUE:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_YIELD_VALUE);
            break;

        case MP_BC_YIELD_FROM:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_YIELD_FROM);
            break;

        case MP_BC_IMPORT_NAME:
            DECODE_QSTR;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_IMPORT_NAME);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(qst);
            instruction->argval = MP_OBJ_NEW_QSTR(qst);
            break;

        case MP_BC_IMPORT_FROM:
            DECODE_QSTR;
            instruction->opname = MP_ROM_QSTR(MP_QSTR_IMPORT_FROM);
            instruction->arg = MP_OBJ_NEW_SMALL_INT(qst);
            instruction->argval = MP_OBJ_NEW_QSTR(qst);
            break;

        case MP_BC_IMPORT_STAR:
            instruction->opname = MP_ROM_QSTR(MP_QSTR_IMPORT_STAR);
            break;

        default:
            if (ip[-1] < MP_BC_LOAD_CONST_SMALL_INT_MULTI + 64) {
                // printf("LOAD_CONST_SMALL_INT " INT_FMT, (mp_int_t)ip[-1] - MP_BC_LOAD_CONST_SMALL_INT_MULTI - 16);
                instruction->opname = MP_ROM_QSTR(MP_QSTR_LOAD_CONST_SMALL_INT);
                instruction->arg = MP_OBJ_NEW_SMALL_INT((mp_int_t)ip[-1] - MP_BC_LOAD_CONST_SMALL_INT_MULTI - 16);
            } else if (ip[-1] < MP_BC_LOAD_FAST_MULTI + 16) {
                // printf("LOAD_FAST " UINT_FMT, (mp_uint_t)ip[-1] - MP_BC_LOAD_FAST_MULTI);
                instruction->opname = MP_ROM_QSTR(MP_QSTR_LOAD_FAST);
                instruction->arg = MP_OBJ_NEW_SMALL_INT((mp_uint_t)ip[-1] - MP_BC_LOAD_FAST_MULTI);
            } else if (ip[-1] < MP_BC_STORE_FAST_MULTI + 16) {
                // printf("STORE_FAST " UINT_FMT, (mp_uint_t)ip[-1] - MP_BC_STORE_FAST_MULTI);
                instruction->opname = MP_ROM_QSTR(MP_QSTR_STORE_FAST);
                instruction->arg = MP_OBJ_NEW_SMALL_INT((mp_uint_t)ip[-1] - MP_BC_STORE_FAST_MULTI);
            } else if (ip[-1] < MP_BC_UNARY_OP_MULTI + MP_UNARY_OP_NUM_BYTECODE) {
                // printf("UNARY_OP " UINT_FMT, (mp_uint_t)ip[-1] - MP_BC_UNARY_OP_MULTI);
                instruction->opname = MP_ROM_QSTR(MP_QSTR_UNARY_OP);
                instruction->arg = MP_OBJ_NEW_SMALL_INT((mp_uint_t)ip[-1] - MP_BC_UNARY_OP_MULTI);
            } else if (ip[-1] < MP_BC_BINARY_OP_MULTI + MP_BINARY_OP_NUM_BYTECODE) {
                mp_uint_t op = ip[-1] - MP_BC_BINARY_OP_MULTI;
                // printf("BINARY_OP " UINT_FMT " %s", op, MP_OBJ_NEW_QSTR(mp_binary_op_method_name[op]));
                instruction->opname = MP_ROM_QSTR(MP_QSTR_BINARY_OP);
                instruction->arg = MP_OBJ_NEW_SMALL_INT(op);
            } else {
                // printf("code %p, byte code 0x%02x not implemented\n", ip-1, ip[-1]);
                // assert(0);
                return ip;
            }
            break;
    }

    return ip;
}

STATIC uint get_line(const byte *line_info, size_t bc) {
    const byte *ip = line_info;
    size_t source_line = 1;
    size_t c;

    while ((c = *ip)) {
        size_t b, l;
        if ((c & 0x80) == 0) {
            // 0b0LLBBBBB encoding
            b = c & 0x1f;
            l = c >> 5;
            ip += 1;
        } else {
            // 0b1LLLBBBB 0bLLLLLLLL encoding (l's LSB in second byte)
            b = c & 0xf;
            l = ((c << 4) & 0x700) | ip[1];
            ip += 2;
        }
        if (bc >= b) {
            bc -= b;
            source_line += l;
        } else {
            // found source line corresponding to bytecode offset
            break;
        }
    }

    return source_line;
}

STATIC void extract_prelude(const byte *bytecode, mp_bytecode_prelude_t *prelude) {
    const byte *ip = bytecode;

    prelude->n_state = mp_decode_uint(&ip); // ip++
    prelude->n_exc_stack = mp_decode_uint(&ip); // ip++
    prelude->scope_flags = *ip++;
    prelude->n_pos_args = *ip++;
    prelude->n_kwonly_args = *ip++;
    prelude->n_def_pos_args = *ip++;

    prelude->code_info = ip;
    size_t code_info_size = mp_decode_uint(&ip); // ip++

    #if MICROPY_PERSISTENT_CODE
    qstr block_name = ip[0] | (ip[1] << 8);
    qstr source_file = ip[2] | (ip[3] << 8);
    ip += 4;
    #else
    qstr block_name = mp_decode_uint(&ip); // ip++
    qstr source_file = mp_decode_uint(&ip); // ip++
    #endif
    prelude->qstr_block_name = block_name;
    prelude->block_name = qstr_str(block_name);
    prelude->qstr_source_file = source_file;
    prelude->source_file = qstr_str(source_file);

    prelude->line_info = ip;

    prelude->locals = prelude->code_info + code_info_size;

    ip = prelude->locals;
    while (*ip++ != 255);
    prelude->bytecode = ip;
}

STATIC mp_obj_dict_t* prof_fun_bytecode_parse_level(const mp_raw_code_t *rc, char *path, mp_obj_dict_t *bytecode_tree, mp_obj_dict_t *rc_map) {

    mp_bytecode_prelude_t _prelude, *prelude = &_prelude;
    extract_prelude(rc->data.u_byte.bytecode, prelude);

    char current_path[4024];

    int __attribute__((unused)) mem_ok = snprintf(current_path, MP_ARRAY_SIZE(current_path), "%s/%s", path, prelude->block_name);
    assert(mem_ok >= 0 && mem_ok < MP_ARRAY_SIZE(current_path));

    // How many instructions to parse? (needed for the tuple allocation)
    uint instr_count = 0;
    const byte *ip = prelude->bytecode;
    while (ip - rc->data.u_byte.bytecode < rc->data.u_byte.bc_len) {
        instr_count++;

        size_t opcode_size = 0;
        mp_opcode_format(ip, &opcode_size);

#if 0 // find opcode size mistakes
        mp_dis_instruction _instruction, *instruction = &_instruction;
        const byte *next_ip = opcode_decode(ip, rc->data.u_byte.const_table, instruction);

        if (opcode_size != next_ip - ip) {
            mp_printf(&mp_plat_print, "size miss %d != %d %s\n", opcode_size, next_ip - ip, mp_obj_str_get_str(instruction->opname));
            assert(opcode_size == next_ip - ip);
        }
#endif

        ip += opcode_size;
    }

    mp_obj_t bytecode = mp_obj_new_tuple(instr_count, NULL);
    mp_obj_tuple_t *bytecode_ptr = MP_OBJ_TO_PTR(bytecode);
    mp_obj_t *bytecode_ptr_items = bytecode_ptr->items;

    ip = prelude->bytecode;
    while (ip - rc->data.u_byte.bytecode < rc->data.u_byte.bc_len) {
        mp_obj_t instr_info = mp_obj_new_tuple(7, NULL);
        mp_obj_tuple_t *instr_info_ptr = MP_OBJ_TO_PTR(instr_info);
        instr_info_ptr->items[0] = MP_OBJ_NEW_SMALL_INT(ip - prelude->bytecode);
        instr_info_ptr->items[1] = MP_OBJ_NEW_SMALL_INT(get_line(prelude->line_info, ip - prelude->locals));
        mp_dis_instruction _instruction, *instruction = &_instruction;
        const byte *next_ip = opcode_decode(ip, rc->data.u_byte.const_table, instruction);
        size_t opcode_size = next_ip - ip;

        // Since the tuple is immutable removing the const qualifier here
        // shuldn't matter right?
        instr_info_ptr->items[2] = (mp_obj_t)instruction->opname;
        instr_info_ptr->items[3] = (mp_obj_t)instruction->arg;
        instr_info_ptr->items[4] = (mp_obj_t)instruction->argval;
        instr_info_ptr->items[5] = (mp_obj_t)instruction->cache;
        instr_info_ptr->items[6] = mp_obj_new_bytearray(opcode_size, (void*)ip);
        *bytecode_ptr_items++ = instr_info;

        switch(*ip) {
        case MP_BC_MAKE_FUNCTION:
        case MP_BC_MAKE_FUNCTION_DEFARGS:
        case MP_BC_MAKE_CLOSURE:
        case MP_BC_MAKE_CLOSURE_DEFARGS:
            do {
                ip++;
                mp_uint_t unum;
                const byte* ptr;
                const mp_uint_t *const_table = rc->data.u_byte.const_table;

                DECODE_PTR;
                mp_raw_code_t *sub_rc = (mp_raw_code_t*)ptr;
                prof_fun_bytecode_parse_level(sub_rc, current_path, bytecode_tree, rc_map);
            } while(0);
            break;
        }

        ip = next_ip;
    }

#if 0 // export bytecode constants
    mp_printf(&mp_plat_print, "const %s 0/0:\n", current_path);
    for(int i =0; i < prelude->n_pos_args + prelude->n_kwonly_args + rc->data.u_byte.n_obj; i++) {
        mp_printf(&mp_plat_print, "const %s %d/%d ", current_path, i+1, prelude->n_pos_args + prelude->n_kwonly_args + rc->data.u_byte.n_obj);
        mp_obj_print_helper(&mp_plat_print, (mp_obj_t)rc->data.u_byte.const_table[i], PRINT_STR);
        mp_printf(&mp_plat_print, "\n");
    }
#endif

    mp_obj_t path_obj = mp_obj_new_str(current_path, strlen(current_path));
    mp_obj_t bytecode_tree_obj = MP_OBJ_FROM_PTR(bytecode_tree);
    mp_obj_dict_store(
        bytecode_tree_obj,
        path_obj,
        bytecode
    );

    if (rc_map) {
        mp_obj_t rc_map_obj = MP_OBJ_FROM_PTR(rc_map);
        mp_obj_dict_store(
            rc_map_obj,
            mp_obj_new_int_from_ull((uintptr_t)rc->data.u_byte.bytecode),
            path_obj
        );
    }

    return bytecode_tree;
}

STATIC mp_obj_t dict_soft_relookup(mp_obj_dict_t *dict, mp_obj_t *keys) {
    if (!keys || keys[0] == NULL) {
        return MP_OBJ_NULL;
    }

    mp_obj_t key = keys[0];
    mp_map_elem_t* elem = mp_map_lookup(&dict->map, key, MP_MAP_LOOKUP);
    if (!elem) {
        return MP_OBJ_NULL;
    }

    if (keys[1] == NULL) {
        return elem->value;
    }

    return dict_soft_relookup(MP_OBJ_TO_PTR(elem->value), keys+1);
}

mp_obj_t prof_fun_bytecode_parse(mp_obj_t module_fun) {

    mp_obj_t module_profiling = mp_obj_new_dict(0);
    mp_obj_t prof_bytecode = mp_obj_new_dict(0);
    mp_obj_t prof_rc_map = mp_obj_new_dict(0);
    mp_obj_dict_store(
        module_profiling,
        MP_ROM_QSTR(MP_QSTR_bytecode),
        prof_bytecode
    );
    mp_obj_dict_store(
        module_profiling,
        MP_ROM_QSTR(MP_QSTR_rc_map),
        prof_rc_map
    );
    mp_obj_dict_store(
        module_profiling,
        MP_ROM_QSTR(MP_QSTR_module_fun),
        module_fun
    );

    mp_obj_fun_bc_t *fun = MP_OBJ_TO_PTR(module_fun);

    prof_fun_bytecode_parse_level(fun->rc, "", MP_OBJ_TO_PTR(prof_bytecode), MP_OBJ_TO_PTR(prof_rc_map));

    return module_profiling;
}


//
// exported functions
//

void prof_module_parse_store(mp_obj_t module_fun) {
    mp_store_global(MP_QSTR___profiling__, prof_fun_bytecode_parse(module_fun));
}

mp_obj_t prof_module_bytecode(mp_obj_t module) {
    mp_obj_t bytecode = dict_soft_relookup(
        ((mp_obj_module_t*)MP_OBJ_TO_PTR(module))->globals,
        (mp_obj_t[]){
            MP_ROM_QSTR(MP_QSTR___profiling__),
            MP_ROM_QSTR(MP_QSTR_bytecode),
            NULL
        }
    );

    if (!bytecode) {
        return mp_const_none;
    }

    return bytecode;
}

mp_obj_t prof_instr_tick(mp_code_state_t *code_state, bool isException) {

    const byte *ip = code_state->ip;

    mp_obj_t frame = mp_const_none;
    mp_obj_t event = mp_const_none;
    mp_obj_t arg = mp_const_none;

    mp_bytecode_prelude_t _prelude, *prelude = &_prelude;
    extract_prelude(code_state->fun_bc->bytecode, prelude);

    //
    // CODE
    //
    static const qstr code_fields[] = {
        MP_QSTR_co_filename,
        MP_QSTR_co_name,
        MP_QSTR_co_codepath,
    };
    mp_obj_tuple_t *code_attr = mp_obj_new_attrtuple(
        code_fields,
        MP_ARRAY_SIZE(code_fields),
        NULL
    );
    code_attr->items[0] = MP_OBJ_NEW_QSTR(prelude->qstr_source_file);
    code_attr->items[1] = MP_OBJ_NEW_QSTR(prelude->qstr_block_name);

    mp_obj_t rc_map = NULL;
    mp_obj_t codepath = mp_const_none;

    rc_map = dict_soft_relookup(mp_globals_get(), (mp_obj_t[]){
        MP_ROM_QSTR(MP_QSTR___profiling__),
        MP_ROM_QSTR(MP_QSTR_rc_map),
        NULL}
    );

    if (rc_map) {
        codepath = mp_obj_dict_get(rc_map, mp_obj_new_int_from_ull((uintptr_t)code_state->fun_bc->bytecode));
    }
    code_attr->items[2] = codepath;
    mp_obj_t code = MP_OBJ_FROM_PTR(code_attr);

    //
    // FRAME
    //
    static const qstr frame_fields[] = {
        MP_QSTR_f_code,
        MP_QSTR_f_lineno,
        MP_QSTR_f_instroffset,
        MP_QSTR_f_modulename,
        MP_QSTR_f_back
    };

    code_state->frame = frame = mp_obj_new_attrtuple(
        frame_fields,
        MP_ARRAY_SIZE(frame_fields),
        NULL
    );
    mp_obj_tuple_t *frame_attr = MP_OBJ_TO_PTR(frame);
    frame_attr->items[0] = code;
    frame_attr->items[1] = MP_OBJ_NEW_SMALL_INT(
        get_line(prelude->line_info, ip - prelude->locals)
    );
    frame_attr->items[2] = MP_OBJ_NEW_SMALL_INT(
        ip - prelude->bytecode
    );
    frame_attr->items[3] = mp_map_lookup(
        &mp_globals_get()->map,
        MP_ROM_QSTR(MP_QSTR___name__),
        MP_MAP_LOOKUP
    )->value;
    if (code_state == code_state->prev) {
        code_state->prev = NULL;
    }
    frame_attr->items[4] = code_state->prev ? code_state->prev->frame : mp_const_false;

    switch (*ip) {
        case MP_BC_RETURN_VALUE:
            event = MP_ROM_QSTR(MP_QSTR_return);
            break;
        case MP_BC_CALL_FUNCTION:
        case MP_BC_CALL_FUNCTION_VAR_KW:
        case MP_BC_CALL_METHOD:
        case MP_BC_CALL_METHOD_VAR_KW:
            event = MP_ROM_QSTR(MP_QSTR_call);
            break;
        default:
            event = MP_ROM_QSTR(MP_QSTR_line);
    }

    if (MP_STATE_VM(cur_exception) && isException) {
        event = MP_ROM_QSTR(MP_QSTR_exception);
        mp_obj_t e[] = {mp_const_false, MP_STATE_VM(cur_exception), mp_const_false};
        arg = mp_obj_new_tuple(3, e);
    }

    mp_obj_t args[3];
    args[0] = frame;
    args[1] = event;
    args[2] = arg;

    MP_STATE_THREAD(prof_instr_tick_callback_is_executing) = true;
    mp_obj_t top = mp_call_function_n_kw(MP_STATE_THREAD(prof_instr_tick_callback), 3, 0, args);
    MP_STATE_THREAD(prof_instr_tick_callback_is_executing) = false;
    return top;
}

#endif // MICROPY_PY_SYS_PROFILING
