// Copyright 2023 Joshua J Baker. All rights reserved.
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file.

#include <stdio.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include "ryu.h"
#include "json.h"
#include "xv.h"

#ifndef XV_THREAD_MEMORY_SIZE
#define XV_THREAD_MEMORY_SIZE 1024
#endif

#ifndef XV_MAXDEPTH
#define XV_MAXDEPTH 100
#endif

enum kind {
    UNDEF_KIND, NULL_KIND, ERR_KIND, FLOAT_KIND, INT_KIND, UINT_KIND, 
    STR_KIND, BOOL_KIND, FUNC_KIND, JSON_KIND, OBJECT_KIND, ARRAY_KIND,
};

enum flag {
    FLAG_CHAIN         = 1<<1, // undefined ident was chained
    FLAG_ESYNTAX       = 1<<2, // syntax error
    FLAG_EOOM          = 1<<3, // out of memory error
    FLAG_EUNDEFINED    = 1<<4, // undefined ident error
    FLAG_ENOTFUNC      = 1<<5, // not a function
    FLAG_EMSG          = 1<<6, // custom user message
    FLAG_GLOBAL        = 1<<7, // global variable (OBJECT_KIND)
    FLAG_EUNSUPKEYWORD = 1<<8, // unsupported keyword
};

struct value {
    enum kind kind:4;   // value kind
    enum flag flag:12;  // extra flags
    uint64_t len:48;    // length of str
    union {
        uint64_t u64;
        int64_t i64;
        double f64;
        bool t;
        const uint8_t *str;
        const void *obj;
        const struct value *arr;
        struct xv (*func)(struct xv value, 
            struct xv args, void *udata);
    };
};

// unreachable is just a hint that code is actually unreachable or otherwise
// not needed in product, but that we want to keep it in for verboseness.
#define unreachable(code) { code }

static void *(*_malloc)(size_t) = NULL;
static void (*_free)(void *) = NULL;

void xv_set_allocator(
    void *(*malloc)(size_t), 
    void *(*realloc)(void*, size_t),
    void (*free)(void*)) 
{
    (void)realloc;
    _malloc = malloc;
    _free = free;
}

struct alloc {
    struct alloc *next;
    uint8_t mem[];
};

static __thread char tmem[XV_THREAD_MEMORY_SIZE];
static __thread int tmemcount = 0;
static __thread size_t tmemused = 0;
static __thread size_t tnumallocs = 0;
static __thread size_t theapsize = 0;
static __thread struct alloc *tallocs = NULL;

static void *emalloc0(size_t sz) {
    return (_malloc?_malloc:malloc)(sz);
}

static void efree0(void *ptr) {
    (_free?_free:free)(ptr);
}

static void *emalloc(size_t sz) {
    if (sizeof(tmem)-tmemused >= sz) {
        if ((sz&7) != 0) sz += 8-(sz&7); // ensure 8-byte alignment
        void *mem = &tmem[tmemused];
        tmemused += sz;
        tmemcount++;
        return mem;
    } else {
        struct alloc *alloc = emalloc0(sizeof(struct alloc)+sz);
        if (!alloc) return NULL;
        alloc->next = tallocs;
        tallocs = alloc;
        tnumallocs++;
        theapsize += sizeof(struct alloc)+sz;
        return &alloc->mem[0];
    }
}

void xv_cleanup(void) {
    struct alloc *alloc = tallocs;
    while (alloc) {
        struct alloc *next = alloc->next;
        efree0(alloc);
        alloc = next;
    }
    tmemcount = 0;
    tmemused = 0;
    tnumallocs = 0;
    theapsize = 0;
    tallocs = NULL;
}

struct value to_value(struct xv value) {
    void *v = &value;
    return *(struct value*)v;
}

struct xv from_value(struct value value) {
    void *v = &value;
    return *(struct xv*)v;
}

struct array {
    struct value *items;
    size_t len;
    size_t cap;
};

static bool array_push_back(struct array *arr, struct value value) {
    if (arr->len == arr->cap) {
        size_t cap = arr->cap ? arr->cap*2 : 1;
        struct value *items = emalloc(cap*sizeof(struct value));
        if (!items) return false;
        memcpy(items, arr->items, arr->len*sizeof(struct value));
        arr->items = items;
        arr->cap = cap;
    }
    arr->items[arr->len++] = value;
    return true;
}

static struct value undefined(void) {
    return (struct value) { 0 };
}

static struct value err_syntax(void) {
    return (struct value) { 
        .kind = ERR_KIND,
        .flag = FLAG_ESYNTAX,
    };
}

static struct value err_notfunc(const uint8_t *ident, size_t ilen) {
    return (struct value) {
        .kind = ERR_KIND, 
        .flag = FLAG_ENOTFUNC,
        .len = ilen,
        .str = ident,
    };
}

static struct value err_oom(void) {
    return (struct value) { 
        .kind = ERR_KIND, 
        .flag = FLAG_EOOM 
    };
}

static struct value err_undefined(const uint8_t *ident, size_t ilen, bool chain)
{
    return (struct value) {
        .kind = ERR_KIND, 
        .flag = FLAG_EUNDEFINED|(chain?FLAG_CHAIN:0),
        .len = ilen,
        .str = ident,
    };
}

static struct value err_msg(const char *msg) {
    uint8_t *str = emalloc(strlen(msg)+1);
    if (!str) return err_oom();
    memcpy(str, msg, strlen(msg)+1);
    return (struct value) { 
        .kind = ERR_KIND, 
        .flag = FLAG_EMSG,
        .len = strlen(msg),
        .str = str,
    };
}

static struct value err_unsupported_keyword(const uint8_t *ident, size_t ilen) {
    return (struct value) { 
        .kind = ERR_KIND,
        .flag = FLAG_ESYNTAX|FLAG_EUNSUPKEYWORD,
        .len = ilen,
        .str = ident,
    };
}

static struct value make_float(double x) {
    return (struct value) { .kind = FLOAT_KIND, .f64 = x };
}

static struct value make_int(int64_t x) {
    return (struct value) { .kind = INT_KIND, .i64 = x };
}

static struct value make_uint(uint64_t x) {
    return (struct value) { .kind = UINT_KIND, .u64 = x };
}

static struct value make_bool(bool t) {
    return (struct value) { .kind = BOOL_KIND, .t = t };
}

static struct value make_undefined(void) {
    return (struct value) { .kind = UNDEF_KIND };
}

static struct value make_global(void) {
    return (struct value) { .kind = OBJECT_KIND, .flag = FLAG_GLOBAL };
}

static struct value make_null(void) {
    return (struct value) { .kind = NULL_KIND };
}
static struct value make_object(const void *ptr, uint32_t tag) {
    return (struct value) { 
        .len = tag,
        .kind = OBJECT_KIND, 
        .obj = ptr,
    };
}

static struct value make_string(const uint8_t *str, size_t len) {
    return (struct value) { 
        .kind = STR_KIND,
        .str = str,
        .len = len,
    };
}

static struct value make_func(struct xv (*func)(
            struct xv value, struct xv args, void *udata))
{
    return (struct value) { 
        .kind = FUNC_KIND,
        .func = func,
    };
}

static struct value make_array(struct value *values, size_t nvalues) {
    return (struct value) { 
        .kind = ARRAY_KIND,
        .len = nvalues,
        .arr = values,
    };
}


static bool isnum(struct value a) {
    switch (a.kind) {
    case FLOAT_KIND: case INT_KIND: case UINT_KIND: case BOOL_KIND:
    case NULL_KIND: case UNDEF_KIND:
        return true;
    default:
        return false;
    }
}

struct writer {
    char *dst;
    size_t n;
    size_t count;
};

static void write_nullterm(struct writer *wr) {
    if (wr->n > wr->count) wr->dst[wr->count] = '\0';
    else if (wr->n > 0) wr->dst[wr->n-1] = '\0';
}

static void write_char(struct writer *wr, char b) {
    if (wr->count < wr->n) wr->dst[wr->count] = b;
    wr->count++;
}

static void write_cstr(struct writer *wr, const char *s) {
    while (*s) write_char(wr, *(s++));
}

static void write_bytes(struct writer *wr, const uint8_t *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        write_char(wr, (char)s[i]);
    }
}

struct eval_context {
    const uint8_t *expr;                     // original expression
    size_t len;                              // 
    int steps;                               // all possible steps
    void (*iter)(struct value, void *udata); // iterator, if any
    void *iter_udata;                        // iterator udata, if any
    struct xv_env *env;                      // user context
};

static struct value make_json(const uint8_t *str, size_t len) {
    struct json json = json_parsen((char*)str, len);
    size_t rawlen;
    switch (json_type(json)) {
    case JSON_STRING:
        rawlen = json_raw_length(json);
        if (json_string_is_escaped(json)) {
            // must unescape string into a heap allocation
            uint8_t *mem = emalloc(rawlen+1);
            if (!mem) return err_oom();
            memset(mem, 0, rawlen+1);
            size_t n = json_string_copy(json, (char*)mem, rawlen+1);
            return make_string(mem, n);
        } else {
            // use the raw string
            const char *raw = json_raw(json);    
            if (rawlen <= 2) {
                return make_string(NULL, 0);
            }
            return make_string((uint8_t*)raw+1, rawlen-2);
        }
    case JSON_NUMBER:
        return make_float(json_double(json));
    case JSON_NULL:
        if (json_exists(json)) {
            return make_null();
        } else {
            return make_undefined();
        }
    case JSON_TRUE: 
        return make_bool(true);
    case JSON_FALSE: 
        return make_bool(false);
    default:
        // JSON_ARRAY, JSON_OBJECT
        return (struct value) { 
            .kind = JSON_KIND,
            .str = (uint8_t*)json_raw(json),
            .len = json_raw_length(json),
        };
    }
}

/////////////////////////////////
// JS native type conversions
/////////////////////////////////

static int64_t conv_ttoi(bool t);
static uint64_t conv_ttou(bool t);
static double conv_ttof(bool t);
static bool conv_ftot(double f);
static int64_t conv_ftoi(double f);
static uint64_t conv_ftou(double f);
static bool conv_itot(int64_t i);
static double conv_itof(int64_t i);
static uint64_t conv_itou(int64_t i);
static bool conv_utot(uint64_t u);
static double conv_utof(uint64_t u);
static int64_t conv_utoi(uint64_t u);
static bool conv_atot(const char *a, size_t alen);
static double conv_atof(const char *a, size_t alen);
static int64_t conv_atoi(const char *a, size_t alen);
static uint64_t conv_atou(const char *a, size_t alen);

static double to_f64(struct value a) {
    if (a.kind == FLOAT_KIND) return a.f64;
    switch (a.kind) {
    case UNDEF_KIND:
        return NAN;
    case NULL_KIND:
        return 0;
    case BOOL_KIND:
        return conv_ttof(a.t);
    case INT_KIND:
        return conv_itof(a.i64);
    case UINT_KIND:
        return conv_utof(a.u64);
    case STR_KIND:
        return conv_atof((char*)a.str, a.len);
    case ARRAY_KIND:
        if (a.len == 0) {
            return 0;
        }
        if (a.len == 1) {
            return to_f64(a.arr[0]);
        }
        return NAN;
    case JSON_KIND:
        {
            struct json json = json_parsen((char*)a.str, a.len);
            if (json_type(json) == JSON_ARRAY) {
                struct json val = json_first(json);
                if (!json_exists(val)) {
                    return 0;
                }
                if (!json_exists(json_next(val))) {
                    return to_f64(make_json((uint8_t*)json_raw(val), 
                        json_raw_length(val)));
                }
            }
            return NAN;
        }
    default:
        // everything else NaN
        return NAN;
    }
}

static int64_t to_i64(struct value a) {
    if (a.kind == INT_KIND) return a.i64;
    switch (a.kind) {
    case NULL_KIND:
        return 0;
    case BOOL_KIND:
        return conv_ttoi(a.t);
    case FLOAT_KIND:
        return conv_ftoi(a.f64);
    case UINT_KIND:
        return conv_utoi(a.u64);
    case STR_KIND:
        return conv_atoi((char*)a.str, a.len);
    default:
        // everything else to f64 then to u64
        return conv_ftoi(to_f64(a));
    }
}

static uint64_t to_u64(struct value a) {
    if (a.kind == UINT_KIND) return a.u64;
    switch (a.kind) {
    case BOOL_KIND:
        return conv_ttou(a.t);
    case FLOAT_KIND:
        return conv_ftou(a.f64);
    case INT_KIND:
        return conv_itou(a.i64);
    case STR_KIND:
        return conv_atou((char*)a.str, a.len);
    default:
        // everything else to f64 then to u64
        return conv_ftou(to_f64(a));
    }
}

static bool to_bool(struct value a) {
    if (a.kind == BOOL_KIND) return a.t;
    switch (a.kind) {
    case UNDEF_KIND:
        return false;
    case NULL_KIND:
        return false;
    case FLOAT_KIND:
        return conv_ftot(a.f64);
    case INT_KIND:
        return conv_itot(a.i64);
    case UINT_KIND:
        return conv_utot(a.u64);
    case STR_KIND:
        return conv_atot((char*)a.str, a.len);
    default:
        return true;
    }
}

static void write_error(struct writer *wr, struct value value);
static void write_double(struct writer *wr, double f);
static void write_int(struct writer *wr, int64_t i);
static void write_uint(struct writer *wr, uint64_t u);

static void write_value(struct writer *wr, struct value value) {
    switch (value.kind) {
    case UNDEF_KIND:
        write_cstr(wr, "undefined");
        break;
    case NULL_KIND:
        write_cstr(wr, "null");
        break;
    case ERR_KIND:
        write_error(wr, value);
        break;
    case FLOAT_KIND:
        write_double(wr, value.f64);
        break;
    case INT_KIND:
        write_int(wr, value.i64);
        break;
    case UINT_KIND:
        write_uint(wr, value.u64);
        break;
    case STR_KIND:
        write_bytes(wr, value.str, value.len);
        break;
    case BOOL_KIND:
        write_cstr(wr, value.t ? "true" : "false");
        break;
    case FUNC_KIND:
        write_cstr(wr, "[Function]");
        break;
    case JSON_KIND:
        write_bytes(wr, value.str, value.len);
        break;
    case OBJECT_KIND:
        write_cstr(wr, "[Object]");
        break;
    case ARRAY_KIND:
        for (size_t i = 0; i < value.len; i++) {
            if (i > 0) {
                write_char(wr, ',');
            }
            write_value(wr, value.arr[i]);
        }
        break;
    }
}

static const uint8_t *to_str(struct value a, size_t *len, 
    char buf[], size_t bufsize)
{
    if (a.kind == STR_KIND) {
        *len = a.len;
        return a.str;
    }
    struct writer wr = (struct writer){ .dst = buf, .n = bufsize };
    write_value(&wr, a);
    uint8_t *mem;
    if (wr.count >= bufsize) {
        mem = emalloc(wr.count+1);
        if (!mem) {
            *len = 0;
            return NULL;
        }
        wr = (struct writer){ .dst = (char*)mem, .n = wr.count+1 };
        write_value(&wr, a);
    } else {
        mem = (uint8_t*)buf;
    }
    *len = wr.count;
    return mem;
}

static bool is_err(struct value value) {
    return value.kind == ERR_KIND;
}

static bool has_suffix(const uint8_t *s, size_t len, const char *suffix) {
    size_t slen = strlen(suffix);
    return len >= slen && memcmp(s+len-slen, suffix, slen) == 0;
}

static bool isws(uint8_t c) {
    if (c > ' ') return false;
    switch (c) {
    case '\t': case '\n': case '\v': case '\f': case '\r': case ' ': 
        return true;
    default:
        return false;
    }
}

static const uint8_t *trim(const uint8_t *s, size_t len, size_t *out_len) {
    while (len > 0 && isws(s[0])) {
        s++;
        len--;
    }
    while (len > 0 && isws(s[len-1])) {
        len--;
    }
    *out_len = len;
    return s;
}

static uint64_t parse_uint(const uint8_t *s, size_t len, int base, bool *ok) {
    char *end = NULL;
    uint64_t x = strtoull((char*)s, &end, base);
    *ok = (size_t)((uint8_t*)end - s) == len;
    return x;
}

static int64_t parse_int(const uint8_t *s, size_t len, int base, bool *ok) {
    char *end = NULL;
    int64_t x = strtoll((char*)s, &end, base);
    *ok = (size_t)((uint8_t*)end - s) == len;
    return x;
}

static double parse_float(const uint8_t *s, size_t len, bool *ok) {
    char *end = NULL;
    double x = strtod((char*)s, &end);
    *ok = (size_t)((uint8_t*)end - s) == len;
    return x;
}

// Operator Precedence
// https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Operators/Operator_Precedence
enum step {
    STEP_COMMA         = 1<<1,   //  1: Comma / Sequence
    STEP_TERNS         = 1<<2,   //  3: Conditional (ternary) operator
    STEP_LOGICAL_OR    = 1<<3,   //  4: Logical OR (||) Nullish coalescing operator (??)
    STEP_LOGICAL_AND   = 1<<4,   //  5: Logical AND (&&)
    STEP_BITWISE_OR    = 1<<5,   //  6: Bitwise OR (|)
    STEP_BITWISE_XOR   = 1<<6,   //  7: Bitwise XOR (^)
    STEP_BITWISE_AND   = 1<<7,   //  8: Bitwise AND (&)
    STEP_EQUALITY      = 1<<8,   //  9: Equality (==) (!=)
    STEP_COMPS         = 1<<9,   // 10: Comparison (<) (<=) (>) (>=)
    STEP_SUMS          = 1<<10,  // 12: Summation (-) (+)
    STEP_FACTS         = 1<<11,  // 13: Factors (*) (/)
};

// all step tokens
// op_steps = {
// 	',': stepComma,                       // ','
// 	'?': stepTerns | stepLogicalOR,       // '?:' '??'
// 	':': stepTerns,                       // '?:'
// 	'|': stepLogicalOR | stepBitwiseOR,   // '||' '|'
// 	'&': stepLogicalAND | stepBitwiseAND, // '&&' '&'
// 	'^': stepBitwiseXOR,                  // '^'
// 	'=': stepComps | stepEquality,        // '==' '<=' '>='
// 	'!': stepEquality,                    // '!' '!='
// 	'<': stepComps,                       // '<' '<='
// 	'>': stepComps,                       // '>' '>='
// 	'+': stepSums,                        // '+'
// 	'-': stepSums,                        // '-'
// 	'*': stepFacts,                       // '*'
// 	'/': stepFacts,                       // '/'
// 	'%': stepFacts,                       // '%'
// }

static uint16_t op_steps[256] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  //
    0,0,0,0,0,0,0,0,0,0,0,0,                    //
    /*'!'*/ STEP_EQUALITY,                      // '!' '!='
    0,0,0,                                      //
    /*'%'*/ STEP_FACTS,                         // '%',
    /*'&'*/ STEP_LOGICAL_AND|STEP_BITWISE_AND,  // '&&' '&'
    0,0,0,                                      //
    /*'*'*/ STEP_FACTS,                         // '*',
    /*'+'*/ STEP_SUMS,                          // '+'
    /*','*/ STEP_COMMA,                         // ','
    /*'-'*/ STEP_SUMS,                          // '-'
    0,                                          //
    /*'/'*/ STEP_FACTS,                         // '/',
    0,0,0,0,0,0,0,0,0,0,                        //
    /*':'*/ STEP_TERNS,                         // '?:'
    0,                                          //
    /*'<'*/ STEP_COMPS,                         // '<' '<='
    /*'='*/ STEP_COMPS|STEP_EQUALITY,           // '==' '<=' '>='
    /*'>'*/ STEP_COMPS,                         // '<' '<=',
    /*'?'*/ STEP_TERNS|STEP_LOGICAL_OR,         // '?:' '??'
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  //
    0,0,0,0,0,0,0,0,0,                          //
    /*'^'*/ STEP_BITWISE_XOR,                   // '^'
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  //
    0,0,0,0,0,0,0,0,                            //
    /*'|'*/ STEP_LOGICAL_OR|STEP_BITWISE_OR,    // '||' '|'
};


static struct value eval_auto(int step, const uint8_t *expr, size_t len, 
    struct eval_context *ctx, int depth);

static const uint8_t *read_group(const uint8_t *data, size_t len, 
    size_t *len_out);

static struct value eval_expr(const uint8_t *expr, size_t len, 
    struct eval_context *ctx, int depth);


static struct value eval_comma(const uint8_t *expr, size_t len, 
    struct eval_context *ctx, int depth)
{
    size_t s = 0;
    struct value res = { 0 };
    void (*iter)(struct value, void *udata) = ctx->iter;
    void *iter_udata = ctx->iter_udata;
    size_t glen;
    const uint8_t *g;
    for (size_t i = 0; i < len; i++) {
        switch (expr[i]) {
        case ',':
            ctx->iter = NULL;       // disable the iter
            ctx->iter_udata = NULL; // disable the iter
            res = eval_auto(STEP_COMMA<<1, expr+s, i-s, ctx, depth);
            ctx->iter = iter;             // enable the iter
            ctx->iter_udata = iter_udata; // enable the iter
            if (is_err(res)) return res;
            if (ctx->iter) ctx->iter(res, ctx->iter_udata);
            s = i + 1;
            break;
        case '(': case '[': case '{': case '"': case '\'':
            g = read_group(expr+i, len-i, &glen);
            if (!g) return err_syntax();
            i = i + glen - 1;
            break;
        }
    }
    res = eval_auto(STEP_COMMA<<1, expr+s, len-s, ctx, depth); 
    if (is_err(res)) return res;
    if (ctx->iter) {
        ctx->iter(res, ctx->iter_udata);
    }
    return res;
}

static struct value eval_terns(const uint8_t *expr, size_t len, 
    struct eval_context *ctx, int depth)
{
    const uint8_t *cond = NULL;
    size_t condlen = 0;
    size_t s  = 0;
    size_t tdepth = 0;
    size_t glen;
    const uint8_t *g;
    for (size_t i = 0; i < len; i++) {
        switch (expr[i]) {
        case '?':
            if (i+1 < len && (expr[i+1] == '?' || expr[i+1] == '.')) {
                // '??' or '?.' operator
                i++;
                continue;
            }
            if (tdepth == 0) {
                cond = expr;
                condlen = i;
                s = i + 1;
            }
            tdepth++;
            break;
        case ':':
            tdepth--;
            if (tdepth == 0) {
                const uint8_t *left = expr+s;
                size_t leftlen = i-s;
                const uint8_t *right = expr+i+1;
                size_t rightlen = len-(i+1);
                struct value res = eval_expr(cond, condlen, ctx, depth);
                if (is_err(res)) return res;
                if (to_bool(res)) {
                    return eval_expr(left, leftlen, ctx, depth);
                }
                return eval_expr(right, rightlen, ctx, depth);
            }
            break;
        case '(': case '[': case '{': case '"': case '\'':
            g = read_group(expr+i, len-i, &glen);
            if (!g) return err_syntax();
            i = i + glen - 1;
            break;
        }
    }
    if (tdepth == 0) {
        return eval_auto(STEP_TERNS<<1, expr, len, ctx, depth);
    }
    return err_syntax();
}

static struct value vcoalesce(struct value a, struct value b) {
    switch (a.kind) {
    case UNDEF_KIND: case NULL_KIND:
        return b;
    default:
        return a;
    }
}

static struct value vor(struct value a, struct value b) {
    return make_bool(to_bool(a) || to_bool(b));
}

static struct value vand(struct value a, struct value b) {
    return make_bool(to_bool(a) && to_bool(b));
}

static struct value vband(struct value a, struct value b) {
    if (a.kind == b.kind) {
        switch (a.kind) {
        case INT_KIND:
            return make_int(a.i64 & b.i64);
        case UINT_KIND:
            return make_uint(a.u64 & b.u64);
        default:
            break;
        }
    }
    return make_float(conv_itof(to_i64(a) & to_i64(b)));
}

static struct value vbxor(struct value a, struct value b) {
    if (a.kind == b.kind) {
        switch (a.kind) {
        case INT_KIND:
            return make_int(a.i64 ^ b.i64);
        case UINT_KIND:
            return make_uint(a.u64 ^ b.u64);
        default:
            break;
        }
    }
    return make_float(conv_itof(to_i64(a) ^ to_i64(b)));
}

static struct value vbor(struct value a, struct value b) {
    if (a.kind == b.kind) {
        switch (a.kind) {
        case INT_KIND:
            return make_int(a.i64 | b.i64);
        case UINT_KIND:
            return make_uint(a.u64 | b.u64);
        default:
            break;
        }
    }
    return make_float(conv_itof(to_i64(a) | to_i64(b)));
}

static bool string_less(const uint8_t *a, size_t alen, 
    const uint8_t *b, size_t blen)
{
    size_t n = alen < blen ? alen : blen;
    for (size_t i = 0; i < n; i++) {
        if (a[i] < b[i]) return true;
        if (a[i] > b[i]) return false;
    }
    return alen < blen;
}

static bool string_less_insensitive(const uint8_t *a, size_t alen, 
    const uint8_t *b, size_t blen)
{
    size_t n = alen < blen ? alen : blen;
    for (size_t i = 0; i < n; i++) {
        uint8_t ach = (uint8_t)tolower((char)a[i]);
        uint8_t bch = (uint8_t)tolower((char)b[i]);
        if (ach < bch) return true;
        if (ach > bch) return false;
    }
    return alen < blen;
}

static struct value vlt(struct value a, struct value b, 
    struct eval_context *ctx)
{
    if (a.kind == b.kind) {
        bool less;
        switch (a.kind) {
        case FLOAT_KIND:
            return make_bool(a.f64 < b.f64);
        case INT_KIND:
            return make_bool(a.i64 < b.i64);
        case UINT_KIND:
            return make_bool(a.u64 < b.u64);
        case STR_KIND:
            if (ctx && ctx->env && ctx->env->no_case) {
                less = string_less_insensitive(a.str, a.len, b.str, b.len);
            } else {
                less = string_less(a.str, a.len, b.str, b.len);
            }
            return make_bool(less);
        default:
            break;
        }
    }
    return make_bool(to_f64(a) < to_f64(b));
}

static struct value vlte(struct value a, struct value b, 
    struct eval_context *ctx)
{
    struct value t = vlt(a, b, ctx);
    if (t.t) return t;
    t = vlt(b, a, ctx);
    return make_bool(!t.t);
}

static struct value vgt(struct value a, struct value b, 
    struct eval_context *ctx)
{
    return vlt(b, a, ctx);
}

static struct value vgte(struct value a, struct value b, 
    struct eval_context *ctx)
{
    struct value t = vgt(a, b, ctx);
    if (t.t) return t;
    t = vgt(b, a, ctx);
    return make_bool(!t.t);
}

static struct value veq(struct value a, struct value b, 
    struct eval_context *ctx)
{
    if (a.kind != b.kind) { // && a.kind != OBJ_KIND && b.kind != OBJ_KIND) {
        return make_bool(to_f64(a) == to_f64(b)); // MARK: float equality
    }
    struct value t = vlt(a, b, ctx);
    if (t.t) return make_bool(false);
    t = vlt(b, a, ctx);
    return make_bool(!t.t);
}

static struct value vneq(struct value a, struct value b, 
    struct eval_context *ctx)
{
    struct value t = veq(a, b, ctx);
    return make_bool(!t.t);
}

static struct value vseq(struct value a, struct value b, 
    struct eval_context *ctx)
{
    if (a.kind == b.kind) {
        return veq(a, b, ctx);
    }
    return make_bool(false);
}

static struct value vsneq(struct value a, struct value b, 
    struct eval_context *ctx)
{
    struct value t = vseq(a, b, ctx);
    return make_bool(!t.t);
}

static struct value logical_or(struct value left, uint8_t op, 
    const uint8_t *expr, size_t len, struct eval_context *ctx, int depth)
{
    expr = trim(expr, len, &len);
    if (len == 0) return err_syntax();
    struct value right = eval_auto(STEP_LOGICAL_OR<<1, expr, len, ctx, depth);
    if (is_err(right)) return right;
    switch (op) {
    case '|':
        return vor(left, right);
    case '?':
        return vcoalesce(left, right);
    default:
        return right;
    }
}

static struct value eval_logical_or(const uint8_t *expr, size_t len,
    struct eval_context *ctx, int depth)
{
    size_t s = 0;
    struct value left = { 0 };
    uint8_t op = 0;
    size_t glen;
    const uint8_t *g;
    for (size_t i = 0; i < len; i++) {
        switch (expr[i]) {
        case '?':
            if (i+1 < len && expr[i+1] == '.') {
                // '?.' operator
                i++;
                continue;
            }
            // fall through
        case '|':
            if (i+1 == len) return err_syntax();
            if (expr[i+1] != expr[i]) {
                // bitwise OR
                i++;
                continue;
            }
            left = logical_or(left, op, expr+s, i-s, ctx, depth);
            if (is_err(left)) return left;
            op = expr[i];
            i++;
            s = i + 1;
            break;
        case '(': case '[': case '{': case '"': case '\'':
            g = read_group(expr+i, len-i, &glen);
            if (!g) return err_syntax();
            i = i + glen - 1;
            break;
        }
    }
    return logical_or(left, op, expr+s, len-s, ctx, depth);

}

static struct value logical_and(struct value left, uint8_t op, 
    const uint8_t *expr, size_t len, struct eval_context *ctx, int depth)
{
    expr = trim(expr, len, &len);
    if (len == 0) return err_syntax();
    struct value right = eval_auto(STEP_LOGICAL_AND<<1, expr, len, ctx, depth);
    if (is_err(right)) return right;
    switch (op) {
    case '&':
        return vand(left, right);
    default:
        return right;
    }
}

static struct value eval_logical_and(const uint8_t *expr, size_t len,
    struct eval_context *ctx, int depth)
{
    size_t s = 0;
    struct value left = { 0 };
    uint8_t op = 0;
    size_t glen;
    const uint8_t *g;
    for (size_t i = 0; i < len; i++) {
        switch (expr[i]) {
        case '&':
            if (i+1 == len) return err_syntax();
            if (expr[i+1] != '&') {
                // bitwise AND
                i++;
                continue;
            }
            left = logical_and(left, op, expr+s, i-s, ctx, depth);
            if (is_err(left)) return left;
            op = expr[i];
            i++;
            s = i + 1;
            break;
        case '(': case '[': case '{': case '"': case '\'':
            g = read_group(expr+i, len-i, &glen);
            if (!g) return err_syntax();
            i = i + glen - 1;
            break;
        }
    }
    return logical_and(left, op, expr+s, len-s, ctx, depth);
}

static struct value bitwise_or(struct value left, uint8_t op, 
    const uint8_t *expr, size_t len, struct eval_context *ctx, int depth)
{
    expr = trim(expr, len, &len);
    if (len == 0) return err_syntax();
    struct value right = eval_auto(STEP_BITWISE_OR<<1, expr, len, ctx, depth);
    if (is_err(right)) return right;
    switch (op) {
    case '|':
        return vbor(left, right);
    default:
        return right;
    }
}

static struct value eval_bitwise_or(const uint8_t *expr, size_t len,
    struct eval_context *ctx, int depth)
{
    size_t s = 0;
    struct value left = { 0 };
    uint8_t op = 0;
    size_t glen;
    const uint8_t *g;
    for (size_t i = 0; i < len; i++) {
        switch (expr[i]) {
        case '|':
            left = bitwise_or(left, op, expr+s, i-s, ctx, depth);
            if (is_err(left)) return left;
            op = expr[i];
            s = i + 1;
            break;
        case '(': case '[': case '{': case '"': case '\'':
            g = read_group(expr+i, len-i, &glen);
            unreachable(
                // unreachable due to eval_logical_or already checking
                // this group.
                if (!g) return err_syntax();
            )
            i = i + glen - 1;
            break;
        }
    }
    return bitwise_or(left, op, expr+s, len-s, ctx, depth);
}

static struct value bitwise_xor(struct value left, uint8_t op, 
    const uint8_t *expr, size_t len, struct eval_context *ctx, int depth)
{
    expr = trim(expr, len, &len);
    if (len == 0) return err_syntax();
    struct value right = eval_auto(STEP_BITWISE_XOR<<1, expr, len, ctx, depth);
    if (is_err(right)) return right;
    switch (op) {
    case '^':
        return vbxor(left, right);
    default:
        return right;
    }
}

static struct value eval_bitwise_xor(const uint8_t *expr, size_t len,
    struct eval_context *ctx, int depth)
{
    size_t s = 0;
    struct value left = { 0 };
    uint8_t op = 0;
    size_t glen;
    const uint8_t *g;
    for (size_t i = 0; i < len; i++) {
        switch (expr[i]) {
        case '^':
            left = bitwise_xor(left, op, expr+s, i-s, ctx, depth);
            if (is_err(left)) return left;
            op = expr[i];
            s = i + 1;
            break;
        case '(': case '[': case '{': case '"': case '\'':
            g = read_group(expr+i, len-i, &glen);
            if (!g) return err_syntax();
            i = i + glen - 1;
            break;
        }
    }
    return bitwise_xor(left, op, expr+s, len-s, ctx, depth);
}

static struct value bitwise_and(struct value left, uint8_t op, 
    const uint8_t *expr, size_t len, struct eval_context *ctx, int depth)
{
    expr = trim(expr, len, &len);
    if (len == 0) return err_syntax();
    struct value right = eval_auto(STEP_BITWISE_AND<<1, expr, len, ctx, depth);
    if (is_err(right)) return right;
    switch (op) {
    case '&':
        return vband(left, right);
    default:
        return right;
    }
}

static struct value eval_bitwise_and(const uint8_t *expr, size_t len,
    struct eval_context *ctx, int depth)
{
    size_t s = 0;
    struct value left = { 0 };
    uint8_t op = 0;
    size_t glen;
    const uint8_t *g;
    for (size_t i = 0; i < len; i++) {
        switch (expr[i]) {
        case '&':
            left = bitwise_and(left, op, expr+s, i-s, ctx, depth);
            if (is_err(left)) return left;
            op = expr[i];
            s = i + 1;
            break;
        case '(': case '[': case '{': case '"': case '\'':
            g = read_group(expr+i, len-i, &glen);
            unreachable( 
                // unreachable due to eval_logical_or already checking
                // this group.
                if (!g) return err_syntax();
            );
            i = i + glen - 1;
            break;
        }
    }
    return bitwise_and(left, op, expr+s, len-s, ctx, depth);
}

static struct value equal(struct value left, uint8_t op, 
    const uint8_t *expr, size_t len, struct eval_context *ctx, int depth)
{
    bool neg = false;
    bool boolit = false;
    expr = trim(expr, len, &len);
    while(1) {
        if (len == 0) return err_syntax();
        if (expr[0] != '!') break;
        neg = !neg;
        boolit = true;
        expr++;
        len--;
        expr = trim(expr, len, &len);
    }
    // parse next expression
    struct value right = eval_auto(STEP_EQUALITY<<1, expr, len, ctx, depth);
    if (is_err(right)) return right;
    if (boolit) {
        if (right.kind != BOOL_KIND) {
            right = make_bool(to_bool(right));
        }
        if (neg) {
            right = make_bool(!right.t);
        }
    }
    switch (op) {
    case '=':
        return veq(left, right, ctx);
    case '!':
        return vneq(left, right, ctx);
    case '=' + 32:
        return vseq(left, right, ctx);
    case '!' + 32:
        return vsneq(left, right, ctx);
    default:
        return right;
    }
}

static struct value eval_equality(const uint8_t *expr, size_t len,
    struct eval_context *ctx, int depth)
{
    size_t s = 0;
    struct value left = { 0 };
    uint8_t op = 0;
    size_t glen;
    const uint8_t *g;
    uint8_t opch;
    size_t opsz;
    for (size_t i = 0; i < len; i++) {
        switch (expr[i]) {
        case '=': case '!':
            opch = expr[i];
            opsz = 1;
            switch (opch) {
            case '=':
                if (i > 0 && (expr[i-1] == '>' || expr[i-1] == '<')) {
                    continue;
                }
                if (i == len-1 || expr[i+1] != '=') {
                    return err_syntax();
                }
                opsz++;
                break;
            case '!':
                if (i == len-1 || expr[i+1] != '=') {
                    continue;
                }
                opsz++;
                break;
            }
            if (i+2 < len && expr[i+2] == '=') {
                // strict
                opch += 32;
                opsz++;
            }
            left = equal(left, op, expr+s, i-s, ctx, depth);
            if (is_err(left)) return left;
            op = opch;
            i = i + opsz - 1;
            s = i + 1;
            break;
        case '(': case '[': case '{': case '"': case '\'':
            g = read_group(expr+i, len-i, &glen);
            if (!g) return err_syntax();
            i = i + glen - 1;
            break;
        }
    }
    return equal(left, op, expr+s, len-s, ctx, depth);
}

static struct value comp(struct value left, uint8_t op, 
    const uint8_t *expr, size_t len, struct eval_context *ctx, int depth)
{
    expr = trim(expr, len, &len);
    if (len == 0) return err_syntax();
    // parse next expression
    struct value right = eval_auto(STEP_COMPS<<1, expr, len, ctx, depth);
    if (is_err(right)) return right;
    switch (op) {
    case '<':
        return vlt(left, right, ctx);
    case '<' + 32:
        return vlte(left, right, ctx);
    case '>':
        return vgt(left, right, ctx);
    case '>' + 32:
        return vgte(left, right, ctx);
    default:
        return right;
    }
}

static struct value eval_comps(const uint8_t *expr, size_t len,
    struct eval_context *ctx, int depth)
{
    size_t s = 0;
    struct value left = { 0 };
    uint8_t op = 0;
    size_t glen;
    const uint8_t *g;
    uint8_t opch;
    size_t opsz;
    for (size_t i = 0; i < len; i++) {
        switch (expr[i]) {
        case '<': case '>':
            opch = expr[i];
            opsz = 1;
            if (i < len-1 && expr[i+1] == '=') {
                opch += 32;
                opsz++;
            }
            left = comp(left, op, expr+s, i-s, ctx, depth);
            if (is_err(left)) return left;
            op = opch;
            i = i + opsz - 1;
            s = i + 1;
            break;
        case '(': case '[': case '{': case '"': case '\'':
            g = read_group(expr+i, len-i, &glen);
            if (!g) return err_syntax();
            i = i + glen - 1;
            break;
        }
    }
    return comp(left, op, expr+s, len-s, ctx, depth);
}

static struct value vmul(struct value a, struct value b) {
    if (a.kind == b.kind) {
        switch (a.kind) {
        case FLOAT_KIND:
            return make_float(a.f64 * b.f64);
        case INT_KIND:
            return make_int(a.i64 * b.i64);
        case UINT_KIND:
            return make_uint(a.u64 * b.u64);
        default:
            break;
        }
    }
    return make_float(to_f64(a) * to_f64(b));
}

static struct value vdiv(struct value a, struct value b) {
    if (a.kind == b.kind) {
        switch (a.kind) {
        case FLOAT_KIND:
            return make_float(a.f64 / b.f64);
        case INT_KIND:
            if (b.i64 == 0) {
                return make_float(NAN);
            }
            return make_int(a.i64 / b.i64);
        case UINT_KIND:
            if (b.u64 == 0) {
                return make_float(NAN);
            }
            return make_uint(a.u64 / b.u64);
        default:
            break;
        }
    }
    return make_float(to_f64(a) / to_f64(b));
}

static struct value vmod(struct value a, struct value b) {
    if (a.kind == b.kind) {
        switch (a.kind) {
        case INT_KIND:
            if (b.i64 == 0) {
                return make_float(NAN);
            }
            return make_int(a.i64 % b.i64);
        case UINT_KIND:
            if (b.u64 == 0) {
                return make_float(NAN);
            }
            return make_uint(a.u64 % b.u64);
        default:
            break;
        }
    }
    return make_float(fmod(to_f64(a), to_f64(b)));
}

static struct value string_concat(const uint8_t *astr, size_t alen,
    const uint8_t *bstr, size_t blen)
{
    uint8_t *str = emalloc(alen+blen+1);
    if (!str) return err_oom();
    memcpy(str, astr, alen);
    memcpy(str+alen, bstr, blen);
    str[alen+blen] = '\0';
    return make_string(str, alen+blen);
}

static struct value vadd(struct value a, struct value b) {
    if (a.kind == b.kind) {
        switch (a.kind) {
        case FLOAT_KIND:
            return make_float(a.f64 + b.f64);
        case INT_KIND:
            return make_int(a.i64 + b.i64);
        case UINT_KIND:
            return make_uint(a.u64 + b.u64);
        case STR_KIND:
            return string_concat(a.str, a.len, b.str, b.len);
        case BOOL_KIND: case UNDEF_KIND: case NULL_KIND:
            return make_float(to_f64(a) + to_f64(b));
        default:
            break;
        }
    } else if (isnum(a) && isnum(b)) {
        return make_float(to_f64(a) + to_f64(b));
    }
    char abuf[32];
    size_t alen;
    const uint8_t *astr = to_str(a, &alen, abuf, sizeof(abuf));
    if (!astr) return err_oom();
    char bbuf[32];
    size_t blen;
    const uint8_t *bstr = to_str(b, &blen, bbuf, sizeof(bbuf));
    if (!bstr) return err_oom();
    return string_concat(astr, alen, bstr, blen);
}

static struct value vsub(struct value a, struct value b) {
    if (a.kind == b.kind) {
        switch (a.kind) {
        case FLOAT_KIND:
            return make_float(a.f64 - b.f64);
        case INT_KIND:
            return make_int(a.i64 - b.i64);
        case UINT_KIND:
            return make_uint(a.u64 - b.u64);
        default:
            break;
        }
    }
    return make_float(to_f64(a) - to_f64(b));
}

static struct value sum(struct value left, uint8_t op, const uint8_t *expr, 
    size_t len, bool neg, struct eval_context *ctx, int depth)
{
    expr = trim(expr, len, &len);
    if (len == 0) {
        return err_syntax();
    }
    // parse factors of expression
    struct value right = eval_auto(STEP_SUMS<<1, expr, len, ctx, depth);
    if (is_err(right)) return right;
    if (neg) {
        // make right negative
        right = vmul(right, make_float(-1));
        if (is_err(right)) return right;
    }
    switch (op) {
    case '+':
        return vadd(left, right);
    case '-':
        return vsub(left, right);
    default:
        return right;
    }
}


static uint8_t closech(uint8_t open) {
    switch (open) {
    case '(':
        return ')';
    case '[':
        return ']';
    case '{':
        return '}';
    }
    return open;
}

static const uint8_t *squash(const uint8_t *data, size_t len, size_t *out_len) {
    // expects that the lead character is
    //   '[' or '{' or '(' or '"' or '\''
    // squash the value, ignoring all nested arrays and objects.
    size_t i = 0;
    int depth = 0;
    uint8_t qch;
    size_t s2;
    switch (data[0]) {
    case '"': case '\'':
        break;
    default:
        i = 1;
        depth = 1;
    }
    for (; i < len; i++) {
        if (data[i] < '"' || data[i] > '}') {
            continue;
        }
        switch (data[i]) {
        case '"': case '\'':
            qch = data[i];
            i++;
            s2 = i;
            for (; i < len; i++) {
                if (data[i] > '\\') {
                    continue;
                }
                if (data[i] == qch) {
                    // look for an escaped slash
                    if (data[i-1] == '\\') {
                        int n = 0;
                        if (i >= 2) {
                            for (size_t j = i - 2; j > s2-1; j--) {
                                if (data[j] != '\\') {
                                    break;
                                }
                                n++;
                            }
                        }
                        if (n%2 == 0) {
                            continue;
                        }
                    }
                    break;
                }
            }
            if (depth == 0) {
                if (i >= len) {
                    return NULL;
                }
                *out_len = i+1;
                return data;
            }
            break;
        case '{': case '[': case '(':
            depth++;
            break;
        case '}': case ']': case ')':
            depth--;
            if (depth == 0) {
                *out_len = i+1;
                return data;
            }
            break;
        }
    }
    return NULL;
}

static const uint8_t *read_group(const uint8_t *data, size_t len, 
    size_t *len_out)
{
    const uint8_t *g = squash(data, len, len_out);
    if (!g) return NULL;
    if (*len_out < 2 || g[*len_out-1] != closech(data[0])) return NULL;
    return g;
}


static size_t read_id_start(const uint8_t *expr, size_t len, bool *ok) {
    if (len == 0) {
        *ok = true;
        return 0;
    }
    if (expr[0] == '$' || expr[0] == '_' ||
        (expr[0] >= 'A' && expr[0] <= 'Z') ||
        (expr[0] >= 'a' && expr[0] <= 'z'))
    {
        *ok = true;
        return 1;
    }
    *ok = false;
    return 0;
}

static size_t read_id_continue(const uint8_t *expr, size_t len, bool *ok) {
    if (len == 0) {
        *ok = true;
        return 0;
    }
    if (expr[0] == '$' || expr[0] == '_' ||
        (expr[0] >= 'A' && expr[0] <= 'Z') ||
        (expr[0] >= 'a' && expr[0] <= 'z') ||
        (expr[0] >= '0' && expr[0] <= '9'))
    {
        *ok = true;
        return 1;
    }
    *ok = false;
    return 0;
}

static const uint8_t *read_ident(const uint8_t *expr, size_t len, 
    size_t *len_out)
{
    // Only ascii identifiers for now
    size_t i = 0;
    size_t z = 0;
    bool ok = false;
    z = read_id_start(expr, len, &ok);
    if (!ok || z == 0) {
        *len_out = 0;
        return NULL;
    }
    i += z;
    while (1) {
        z = read_id_continue(expr+i, len-i, &ok);
        if (!ok || z == 0) {
            *len_out = i;
            return expr;
        }
        i += z;
    }
}

// read_codepoint returns the codepoint from the the \uXXXX
static size_t read_codepoint(const uint8_t *expr, size_t len, uint8_t which, 
    uint32_t *cp)
{
    int x = 0;
    size_t n = 0;
    if (which == 'x') {
        x = strtol((char*)expr, NULL, 16);
        n = 2;
    } else {
        size_t s = 0;
        if (expr[0] == '{') {
            s = 1;
            n = len;
            for (size_t i = 0; i < len; i++) {
                if (expr[i] == '}') {
                    n = i + 1;
                    break;
                }
            }
        } else {
            n = 4;
        }
        x = strtol((char*)expr+s, NULL, 16);
    }
    *cp = x;
    return n;
}

static bool is_surrogate(uint32_t cp) {
    return cp > 55296 && cp < 57344;
}

static int decode_codepoint(uint32_t cp1, uint32_t cp2) {
    if (55296 <= cp1 && cp1 < 56320 && 56320 <= cp2 && cp2 < 57344) {
        return (((cp1-55296)<<10) | (cp2 - 56320)) + 65536;
    }
    return 65533;
}

static void write_codepoint(struct writer *wr, uint32_t r) {
    uint8_t p[4];
    size_t n;
    if (r <= 127) {
        p[0] = (uint8_t)r;
        n = 1;
    } else if (r <= 2047) {
        p[0] = 192 | ((uint8_t)(r>>6));
        p[1] = 128 | ((uint8_t)(r)&63);
        n = 2;
    } else if (r > 1114111 || (55296 <= r && r <= 57343)) {
        r = 65533;
        goto next; // fall through
    } else if (r <= 65535) {
    next:
        p[0] = 224 | ((uint8_t)(r>>12));
        p[1] = 128 | ((uint8_t)(r>>6)&63);
        p[2] = 128 | ((uint8_t)(r)&63);
        n = 3;
    } else {
        p[0] = 240 | ((uint8_t)(r>>18));
        p[1] = 128 | ((uint8_t)(r>>12)&63);
        p[2] = 128 | ((uint8_t)(r>>6)&63);
        p[3] = 128 | ((uint8_t)(r)&63);
        n = 4;
    }
    write_bytes(wr, p, n);
}

static const uint8_t *unescape_string(const uint8_t *expr, size_t len,
    size_t *slen, bool *oom)
{
    void *mem = emalloc(len+1);
    if (!mem) {
        *oom = true;
        *slen = 0;
        return NULL;
    }
    struct writer wr = { .dst = mem, .n = len+1 };
    uint32_t cp;
    size_t n;
    for (size_t i = 0; i < len; i++) {
        switch (expr[i]) {
        case '\\':
            i++;
            switch (expr[i]) {
            case '0': write_char(&wr, '\0'); break;
            case 'b': write_char(&wr, '\b'); break;
            case 'f': write_char(&wr, '\f'); break;
            case 'n': write_char(&wr, '\n'); break;
            case 'r': write_char(&wr, '\r'); break;
            case 't': write_char(&wr, '\t'); break;
            case 'v': write_char(&wr, '\v'); break;
            case 'u':
                i++;
                n = read_codepoint(expr+i, len-i, 'u', &cp);
                i += n;
                if (is_surrogate(cp)) {
                    // need another code
                    if (len-i >= 6 && expr[i] == '\\' && expr[i+1] == 'u') {
                        // we expect it to be correct so just consume it
                        i += 2;
                        uint32_t cp2;
                        n = read_codepoint(expr+i, len-i, 'u', &cp2);
                        i += n;
                        cp = decode_codepoint(cp, cp2);
                    }
                }
                // provide enough space to encode the largest utf8 possible
                write_codepoint(&wr, cp);
                i--; // backtrack index by one
                break;
            case 'x':
                i++;
                n = read_codepoint(expr+i, len-i, 'x', &cp);
                i += n;
                write_codepoint(&wr, cp);
                i--; // backtrack index by one
                break;
            default:
                write_char(&wr, (char)expr[i]);
            }
            break;
        default:
            write_char(&wr, (char)expr[i]);
        }
    }
    write_nullterm(&wr);
    *slen = wr.count;
    *oom = false;
    return mem;
}

// parse_string parses a Javascript encoded string.
static const uint8_t *parse_string(const uint8_t *expr, size_t len, 
    size_t *slen, size_t *rlen, bool *oom)
{
    bool esc = false;
    if (len < 2) goto fail;
    uint8_t qch = expr[0];
    for (size_t i = 1; i < len; i++) {
        if (expr[i] < ' ') goto fail;
        if (expr[i] == '\\') {
            esc = true;
            i++;
            if (i == len) goto fail;
            switch (expr[i]) {
            case 'u':
                if (i+1 < len && expr[i+1] == '{') {
                    i += 2;
                    bool end = false;
                    int cn = 0;
                    for (; i < len; i++) {
                        if (expr[i] == '}') {
                            end = true;
                            break;
                        }
                        if (!isxdigit(expr[i])) goto fail;
                        cn++;
                    }
                    if (cn == 0) goto fail;
                    if (!end) goto fail;
                } else {
                    for (size_t j = 0; j < 4; j++) {
                        i++;
                        if (i >= len || !isxdigit(expr[i])) goto fail;
                    }
                }
                break;
            case 'x':
                for (size_t j = 0; j < 2; j++) {
                    i++;
                    if (i >= len || !isxdigit(expr[i])) goto fail;
                }
                break;
            default:
                // LegacyOctalEscapeSequence and NonOctalDecimalEscapeSequence
                // are not allowed. Strict Mode Only. See
                // https://262.ecma-international.org/12.0/#sec-additional-syntax-string-literals
                if (expr[i] >= '1' && expr[i] <= '9') {
                    goto fail;
                }
            }
        } else if (expr[i] == qch) {
            const uint8_t *s = expr+1;
            *slen = i-1;
            if (esc) {
                s = unescape_string(s, *slen, slen, oom);
                if (!s) {
                    *slen = 0;
                    *rlen = 0;
                    return NULL;
                }
            }
            *oom = false;
            *rlen = i+1;
            return s;
        }
    }
fail:
    *oom = false;
    *slen = 0;
    *rlen = 0;
    return NULL;
}

// get_ref_value takes the value from an external reference. 
// It's possible that the ref value is on the heap, and if so we need to 
// steal it and place it in the allocs list.
static struct value get_ref_value(bool chain, struct value left, 
    const uint8_t *ident, size_t ilen, bool opt_chain, 
    struct eval_context *ctx)
{
    if (left.kind == JSON_KIND) {
        struct json json = json_parsen((char*)left.str, left.len);
        struct json key;
        struct json val;
        int64_t index;
        enum json_type type = json_type(json);
        if (type == JSON_OBJECT) {
            key = json_first(json);
            while (json_exists(key)) {
                val = json_next(key);
                if (json_string_comparen(key, (char*)ident, ilen) == 0) {
                    return make_json((uint8_t*)json_raw(val), 
                        json_raw_length(val));
                }
                key = json_next(val);
            }
        } else { // JSON_ARRAY
            index = conv_atoi((char*)ident, ilen);
            if (index >= 0) {
                val = json_first(json);
                while (json_exists(val)) {
                    if (index == 0) {
                        return make_json((uint8_t*)json_raw(val), 
                            json_raw_length(val));
                    }
                    index--;
                    val = json_next(val);
                }
            }
        }
        return make_undefined();
    }
    if (!ctx->env || !ctx->env->ref) {
        return err_undefined(ident, ilen, chain);
    }
    struct value val = to_value(ctx->env->ref(
        from_value(chain?left:make_global()),
        xv_new_stringn((char*)ident, ilen), ctx->env->udata));
    if (is_err(val)) return val;
    if (val.kind == UNDEF_KIND && left.kind == UNDEF_KIND) {
        val = err_undefined(ident, ilen, chain);
    }
    if (is_err(val)) {
        bool skip_err = false;
        if (opt_chain) {
            skip_err = true;
        }
        if (skip_err) {
            val = make_undefined();
        }
    }
    return val;
}

static struct value eval_foreach(const uint8_t *expr, size_t len, 
    struct xv_env *env, void (*iter)(struct value, void *udata), void *udata,
    int depth);

struct multi_iter_context {
    struct array *arr;
    bool oom;
};

static void multi_iter(struct value value, void *udata) {
    struct multi_iter_context *ictx = udata;
    if (!ictx->oom) {
        ictx->oom = !array_push_back(ictx->arr, value);
    }
}

static struct value multi_exprs_to_array(const uint8_t *expr, size_t len, 
    struct eval_context *ctx, int depth)
{
    struct array *arr = emalloc(sizeof(struct array));
    if (!arr) return err_oom();
    memset(arr, 0, sizeof(struct array));
    struct multi_iter_context ictx = { .arr = arr };
    struct value last = eval_foreach(expr, len, ctx->env, multi_iter, &ictx, 
        depth);
    if (is_err(last)) return last;
    if (ictx.oom) return err_oom();
    struct value v = make_array(arr->items, arr->len);
    return v;
}

static struct value eval_atom(const uint8_t *expr, size_t len,
    struct eval_context *ctx, int depth)
{
    expr = trim(expr, len, &len);
    if (len == 0) {
        return err_syntax();
    }
    struct value left = { 0 };
    bool left_ready = false;
    size_t glen;
    const uint8_t *g;
    size_t slen;
    size_t rlen;
    bool oom;
    const uint8_t *str;

    // first look for non-chainable atoms
    switch (expr[0]) {
    case '0':
        if (len > 1 && (expr[1] == 'x' || expr[1] == 'X')) {
            // hexadecimal
            bool ok = false;
            uint64_t x = parse_uint(expr+2, len-2, 16, &ok);
            if (!ok) {
                return err_syntax();
            }
            return make_float((double)x);
        }
        // fall through
    case '-': case'.': case '1': case '2': case '3': case '4': case '5': 
    case '6': case '7': case '8': case '9':
        if (len > 3 && has_suffix(expr, len, "64")) {
            if (expr[len-3] == 'u') {
                bool ok = false;
                uint64_t x = parse_uint(expr, len-3, 10, &ok);
                if (!ok) {
                    return err_syntax();
                }
                return make_uint(x);
            }
            if (expr[len-3] == 'i') {
                bool ok = false;
                int64_t x = parse_int(expr, len-3, 10, &ok);
                if (!ok) {
                    return err_syntax();
                }
                return make_int(x);
            }
        }
        bool ok = false;
        double x = parse_float(expr, len, &ok);
        if (!ok) {
            return err_syntax();
        }
        return make_float(x);
    case '"': case '\'':
        str = parse_string(expr, len, &slen, &rlen, &oom);
        if (!str) {
            return oom ? err_oom() : err_syntax();
        }
        left = make_string(str, slen);
        left_ready = true;
        expr = expr+rlen;
        len -= rlen;
        break;
    case '(': case '{': case '[':
        g = read_group(expr, len, &glen);
        if (!g) return err_syntax();
        if (g[0] == '(') {
            // paren groups can be evaluated and used as the leading value.
            left = eval_expr(g+1, len-2, ctx, depth);
            if (is_err(left)) return left;
            left_ready = true;
            expr += glen;
            len -= glen;
        } else if (g[0] == '[') {
            left = multi_exprs_to_array(g+1, glen-2, ctx, depth);
            if (is_err(left)) return left;
            left_ready = true;
            expr += glen;
            len -= glen;
        } else {
            // '{' is not currently allowed as a leading value
            // Perhaps in the future.
            return err_syntax();
        }
        break;
    }
    const uint8_t *left_ident = NULL;
    size_t left_ident_len = 0;
    if (!left_ready) {
        // probably a chainable identifier
        size_t ilen;
        const uint8_t *ident = read_ident(expr, len, &ilen);
        if (!ident) return err_syntax();

        // TODO: maybe use a tiny hashtable
        if (ilen == 4 && memcmp(ident, "true", 4) == 0) {
            left = make_bool(true);
        } else if (ilen == 5 && memcmp(ident, "false", 5) == 0) {
            left = make_bool(false);
        } else if (ilen == 4 && memcmp(ident, "null", 4) == 0) {
            left = make_null();
        } else if (ilen == 9 && memcmp(ident, "undefined", 9) == 0) {
            left = make_undefined();
        } else if (ilen == 3 && memcmp(ident, "NaN", 3) == 0) {
            left = make_float(NAN);
        } else if (ilen == 8 && memcmp(ident, "Infinity", 8) == 0) {
            left = make_float(INFINITY);
        } else if ((ilen == 2 && memcmp(ident, "in", 2) == 0) ||
                   (ilen == 3 && memcmp(ident, "new", 3) == 0) ||
                   (ilen == 4 && memcmp(ident, "void", 4) == 0) ||
                   (ilen == 5 && memcmp(ident, "await", 5) == 0) ||
                   (ilen == 5 && memcmp(ident, "yield", 5) == 0) ||
                   (ilen == 6 && memcmp(ident, "typeof", 6) == 0) ||
                   (ilen == 8 && memcmp(ident, "function", 8) == 0) ||
                   (ilen == 10 && memcmp(ident, "instanceof", 10) == 0))
        {
            // unsupported keyword
            return err_unsupported_keyword(ident, ilen);
        } else {
            left = get_ref_value(false, make_undefined(), ident, ilen, false, 
                ctx);
            if (is_err(left)) return left;
        }
        left_ready = true;
        expr = expr+ilen;
        len -= ilen;
        left_ident = ident;
        left_ident_len = ilen;
    }

    struct value left_left = { 0 };
    bool has_left_left = false;

    // read each chained component
    bool opt_chain = false;

    const uint8_t *ident;
    size_t ilen;
    struct value val;
    struct value last;
    char nbuf[32];
    while (1) {
        // There are more components to read
        expr = trim(expr, len, &len);
        if (len == 0) break;
        switch (expr[0]) {
        case '?':
            // Optional chaining
            unreachable(
                if (len == 1 || expr[1] != '.') {
                    // Unreachable due to the condition being checked in a 
                    // previous eval_logical_or step
                    return err_syntax(); 
                }
            )
            expr++;
            len--;
            opt_chain = true;
            // fall through
        case '.':
            // Member Access
            expr++;
            len--;
            expr = trim(expr, len, &len);
            ident = read_ident(expr, len, &ilen);
            if (!ident) return err_syntax();
            val = get_ref_value(true, left, ident, ilen, opt_chain, ctx);
            if (is_err(val)) return val;
            left_left = left;
            has_left_left = true;
            left = val;
            expr = expr+ilen;
            len -= ilen;
            left_ident = ident;
            left_ident_len = ilen;
            break;
        case '(':
            // Function call
            g = read_group(expr, len, &glen);
            if (!g) return err_syntax();
            if (left.kind != FUNC_KIND) {
                return err_notfunc(left_ident, left_ident_len);
            }
            (void)has_left_left;
            struct value args = multi_exprs_to_array(g+1, glen-2, ctx, depth);
            if (is_err(args)) return args;

            val = to_value(left.func(from_value(left_left), 
                from_value(args), 
                ctx->env?ctx->env->udata:NULL));
            if (is_err(val)) return val;
            left_left = left;
            has_left_left = true;
            left = val;
            expr += glen;
            len -= glen;
            break;
        case '[':
            // Computed Member Access
            g = read_group(expr, len, &glen);
            if (!g) return err_syntax();
            last = eval_expr(g+1, glen-2, ctx, depth);
            if (is_err(last)) return last;
            ident = to_str(last, &ilen, nbuf, sizeof(nbuf));
            val = get_ref_value(true, left, ident, ilen, opt_chain, ctx);
            if (is_err(val)) return val;
            left_left = left;
            has_left_left = true;
            left = val;
            expr += glen;
            len -= glen;
            break;
        default:
            return err_syntax();
        }
    }
    return left;
}

static struct value eval_sums(const uint8_t *expr, size_t len, 
    struct eval_context *ctx, int depth)
{
    size_t s = 0;
    struct value left = { 0 };
    uint8_t op = 0;
    bool fill = false;
    bool neg = false;
    size_t glen;
    const uint8_t *g;
    for (size_t i = 0; i < len; i++) {
        switch (expr[i]) {
        case '-': case '+':
            if (!fill) {
                if (i > 0 && expr[i-1] == expr[i]) {
                    // -- not allowed
                    return err_syntax();
                }
                if (expr[i] == '-') {
                    neg = !neg;
                }
                s = i + 1;
                continue;
            }
            if (i > 0 && (expr[i-1] == 'e' || expr[i-1] == 'E')) {
                // scientific notation
                continue;
            }
            if (neg) {
                if (s > 0 && s < len && expr[s-1] == '-' &&
                    expr[s] >= '0' && expr[s] <= '9')
                {
                    s--;
                    neg = false;
                }
            }
            left = sum(left, op, expr+s, i-s, neg, ctx, depth);
            if (is_err(left)) {
                return left;
            }
            op = expr[i];
            s = i + 1;
            fill = false;
            neg = false;
            break;
        case '(': case '[': case '{': case '"': case '\'':
            g = read_group(expr+i, len-i, &glen);
            if (!g) return err_syntax();
            i = i + glen - 1;
            fill = true;
            break;
        default:
            if (!fill && !isws(expr[i])) {
                fill = true;
            }
        }
    }
    if (neg) {
        if (s > 0 && s < len && expr[s-1] == '-' &&
            expr[s] >= '0' && expr[s] <= '9')
        {
            s--;
            neg = false;
        }
    }
    return sum(left, op, expr+s, len-s, neg, ctx, depth);
}

static struct value fact(struct value left, uint8_t op, const uint8_t *expr, 
    size_t len, struct eval_context *ctx, int depth)
{
    expr = trim(expr, len, &len);
    if (len == 0) return err_syntax();
    struct value right = eval_atom(expr, len, ctx, depth);
    if (is_err(right)) return right;
    switch (op) {
    case '*':
        return vmul(left, right);
    case '/':
        return vdiv(left, right);
    case '%':
        return vmod(left, right);
    default:
        return right;
    }
}

static struct value eval_facts(const uint8_t *expr, size_t len,
    struct eval_context *ctx, int depth)
{
    size_t s = 0;
    struct value left = { 0 };
    uint8_t op = 0;
    size_t glen;
    const uint8_t *g;
    for (size_t i = 0; i < len; i++) {
        switch (expr[i]) {
        case '*': case '/': case '%':
            left = fact(left, op, expr+s, i-s, ctx, depth);
            if (is_err(left)) return left;
            op = expr[i];
            s = i + 1;
            break;
        case '(': case '[': case '{': case '"': case '\'':
            g = read_group(expr+i, len-i, &glen);
            if (!g) return err_syntax();
            i = i + glen - 1;
            break;
        }
    }
    return fact(left, op, expr+s, len-s, ctx, depth);
}

static struct value eval_auto(int step, const uint8_t *expr, size_t len, 
    struct eval_context *ctx, int depth)
{
    if (depth-1 > XV_MAXDEPTH) {
        return err_msg("MaxDepthError");
    }
    switch (step) {
    case STEP_COMMA:
        if ((ctx->steps & STEP_COMMA) == STEP_COMMA) {
            return eval_comma(expr, len, ctx, depth);
        }
        // fall through
    case STEP_TERNS:
        if ((ctx->steps & STEP_TERNS) == STEP_TERNS) {
            return eval_terns(expr, len, ctx, depth);
        }
        // fall through
    case STEP_LOGICAL_OR:
        if ((ctx->steps & STEP_LOGICAL_OR) == STEP_LOGICAL_OR) {
            return eval_logical_or(expr, len, ctx, depth);
        }
        // fall through
    case STEP_LOGICAL_AND:
        if ((ctx->steps & STEP_LOGICAL_AND) == STEP_LOGICAL_AND) {
            return eval_logical_and(expr, len, ctx, depth);
        }
        // fall through
    case STEP_BITWISE_OR:
        if ((ctx->steps & STEP_BITWISE_OR) == STEP_BITWISE_OR) {
            return eval_bitwise_or(expr, len, ctx, depth);
        }
        // fall through
    case STEP_BITWISE_XOR:
        if ((ctx->steps & STEP_BITWISE_XOR) == STEP_BITWISE_XOR) {
            return eval_bitwise_xor(expr, len, ctx, depth);
        }
        // fall through
    case STEP_BITWISE_AND:
        if ((ctx->steps & STEP_BITWISE_AND) == STEP_BITWISE_AND) {
            return eval_bitwise_and(expr, len, ctx, depth);
        }
        // fall through
    case STEP_EQUALITY:
        if ((ctx->steps & STEP_EQUALITY) == STEP_EQUALITY) {
            return eval_equality(expr, len, ctx, depth);
        }
        // fall through
    case STEP_COMPS:
        if ((ctx->steps & STEP_COMPS) == STEP_COMPS) {
            return eval_comps(expr, len, ctx, depth);
        }
        // fall through
    case STEP_SUMS:
        if ((ctx->steps & STEP_SUMS) == STEP_SUMS) {
            return eval_sums(expr, len, ctx, depth);
        }
        // fall through
    case STEP_FACTS:
        if ((ctx->steps & STEP_FACTS) == STEP_FACTS) {
            return eval_facts(expr, len, ctx, depth);
        }
        // fall through
    default:
        return eval_atom(expr, len, ctx, depth);
    }
}

static struct value eval_expr(const uint8_t *expr, size_t len, 
    struct eval_context *ctx, int depth)
{
    // the only place where the depth is increased
    return eval_auto(STEP_COMMA, expr, len, ctx, depth+1);
}

static struct value eval_foreach(const uint8_t *expr, size_t len, 
    struct xv_env *env, void (*iter)(struct value, void *udata), void *udata,
    int depth)
{
    expr = trim(expr, len, &len);
    if (len == 0) return undefined();

    // Determine which steps are (possibly) needed by scanning every byte in
    // the input expression and looking for potential candidate characters.
    int steps = 0;
    for (size_t i = 0; i < len; i++) {
        steps |= (int)op_steps[expr[i]];
    }

    if (iter) {
        // require the comma step when using an iterator.
        steps |= STEP_COMMA;
    }

    struct eval_context ctx = {
        .expr = (uint8_t*)expr, 
        .len = len, 
        .steps = steps, 
        .iter = iter,
        .iter_udata = udata,
        .env = env,
    };
    return eval_expr(expr, len, &ctx, depth);
}

static struct value eval(const uint8_t *expr, size_t len, 
    struct xv_env *env, int depth)
{
    return eval_foreach(expr, len, env, NULL, NULL, depth);
}

struct xv xv_eval(const char *expr, struct xv_env *env) {
    return xv_evaln(expr, strlen(expr), env);
}

struct xv xv_evaln(const char *expr, size_t len, 
    struct xv_env *env)
{
    assert(sizeof(struct value) == sizeof(struct xv)); // static_assert?
    struct value value = eval((uint8_t*)expr, len, env, 0);
    struct xv fvalue;
    memcpy(&fvalue, &value, sizeof(struct xv));
    return fvalue;
}

static void write_error(struct writer *wr, struct value value) {
    if ((value.flag&FLAG_ENOTFUNC) == FLAG_ENOTFUNC) {
        write_cstr(wr, "TypeError: ");
        write_bytes(wr, value.str, value.len);
        write_cstr(wr, " is not a function");
    } else if ((value.flag&FLAG_ESYNTAX) == FLAG_ESYNTAX) {
        write_cstr(wr, "SyntaxError");
        if ((value.flag&FLAG_EUNSUPKEYWORD) == FLAG_EUNSUPKEYWORD) {
            write_cstr(wr, ": Unsupported keyword '");
            write_bytes(wr, value.str, value.len);
            write_cstr(wr, "'");
        }
    } else if ((value.flag&FLAG_EUNDEFINED) == FLAG_EUNDEFINED) {
        if ((value.flag&FLAG_CHAIN) == FLAG_CHAIN) {
            write_cstr(wr, "TypeError: Cannot read properties of undefined "
                "(reading '");
            write_bytes(wr, value.str, value.len);
            write_cstr(wr, "')");
        } else {
            write_cstr(wr, "ReferenceError: Can't find variable: '");
            write_bytes(wr, value.str, value.len);
            write_cstr(wr, "'");
        }
    } else if ((value.flag&FLAG_EOOM) == FLAG_EOOM) {
        write_cstr(wr, "MemoryError: Out of memory");
    } else { // if ((value.flag&FLAG_EMSG) == FLAG_EMSG) {
        if (value.len == 0) {
            write_cstr(wr, "");
        } else {
            write_bytes(wr, value.str, value.len);
        }
    }
}

size_t xv_string_copy(struct xv value, char *dst, size_t n) {
    struct value fvalue = to_value(value);
    struct writer wr = { .dst = dst, .n = n };    
    write_value(&wr, fvalue);
    write_nullterm(&wr);
    return wr.count;
}

double xv_double(struct xv value) {
    return to_f64(to_value(value));
}

int64_t xv_int64(struct xv value) {
    return to_i64(to_value(value));
}

uint64_t xv_uint64(struct xv value) {
    return to_u64(to_value(value));
}

bool xv_bool(struct xv value) {
    return to_bool(to_value(value));
}

enum xv_type xv_type(struct xv value) {
    switch (to_value(value).kind) {
    case UNDEF_KIND:
        return XV_UNDEFINED;
    case BOOL_KIND:
        return XV_BOOLEAN;
    case FLOAT_KIND: case INT_KIND: case UINT_KIND:
        return XV_NUMBER;
    case FUNC_KIND:
        return XV_FUNCTION;
    case STR_KIND:
        return XV_STRING;
    default:
        return XV_OBJECT;
    }
}

///////////////////////////////////////////
// Bool
///////////////////////////////////////////

// conv_ttoi converts bool to int64
static int64_t conv_ttoi(bool t) {
    return t ? 1 : 0;
}

// conv_ttou converts bool to uint64
static uint64_t conv_ttou(bool t) {
    return t ? 1 : 0;
}

// conv_ttof converts bool to double
static double conv_ttof(bool t) {
    return t ? 1 : 0;
}

///////////////////////////////////////////
// Float64
///////////////////////////////////////////

static const double UINT64_MAX_FLOAT = 18446744073709549568.0;
static const double INT64_MAX_FLOAT = 9223372036854774784.0;
static const double INT64_MIN_FLOAT = -9223372036854774784.0;

// Ftot converts float64 to bool
static bool conv_ftot(double f) {
    return f < 0 || f > 0;
}

// conv_ftoi converts float64 to int64
static int64_t conv_ftoi(double f) {
    if (isnan(f)) return 0;
    if (f < -9007199254740991.0 || f > 9007199254740991.0) {
        // The number is outside of the range for correct binary
        // representation of floating point as an integer value.
        // https://tc39.es/ecma262/#sec-number.min_safe_integer
        // https://tc39.es/ecma262/#sec-number.max_safe_integer
        if (f < 0) {
            f = ceil(f);
            if (f < INT64_MIN_FLOAT) return INT64_MIN;
        } else {
            f = floor(f);
            if (f > INT64_MAX_FLOAT) return INT64_MAX;
        }
    }
    return (int64_t)f;
}

// conv_ftou converts float64 to uint64
static uint64_t conv_ftou(double f) {
    if (isnan(f) || f < 0) return 0;
    if (f > 9007199254740991.0) {
        // Outside range: See conv_ftoi for description.
        f = floor(f);
        if (f > UINT64_MAX_FLOAT) return UINT64_MAX;
    }
    return (uint64_t)f;
}

static void write_double(struct writer *wr, double f) {
    size_t dstsz = wr->count < wr->n ? wr->n - wr->count : 0;
    wr->count += ryu_string(f, 'j', wr->dst?wr->dst+wr->count:NULL, dstsz);
}

///////////////////////////////////////////
// Int64
///////////////////////////////////////////

// conv_itot converts int64 to bool
static bool conv_itot(int64_t i) {
    return i != 0;
}

// conv_itof converts int64 to double
static double conv_itof(int64_t i) {
    return (double)i;
}

// conv_itou converts int64 to uint64
static uint64_t conv_itou(int64_t i) {
    if (i < 0) return 0;
    return (uint64_t)i;
}

static void write_int(struct writer *wr, int64_t i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRId64, i);
    write_cstr(wr, buf);
}


///////////////////////////////////////////
// Uint64
///////////////////////////////////////////

// conv_utot converts uint64 to bool
static bool conv_utot(uint64_t u) {
    return u != 0;
}

// Utof converts uint64 to double
static double conv_utof(uint64_t u) {
    return (double)u;
}

// conv_utoi converts uint64 to int64
static int64_t conv_utoi(uint64_t u) {
    if (u > INT64_MAX) return INT64_MAX;
    return (int64_t)u;
}

static void write_uint(struct writer *wr, uint64_t u) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRIu64, u);
    write_cstr(wr, buf);
}

///////////////////////////////////////////
// String
///////////////////////////////////////////

// conv_atot converts string to bool
// Always returns true unless string is empty.
static bool conv_atot(const char *a, size_t alen) {
    (void)a;
    return alen > 0;
}

static bool isnumch(char c) {
    return (c >= '0' && c <= '9') || c == '.';
}

// conv_atof converts string to double
// For infinte numbers use 'Infinity' or '-Infinity', not 'Inf' or '-Inf'.
// Returns NaN for invalid syntax
static double conv_atof(const char *a, size_t alen) {
    if (alen == 0) return NAN;
    if (!a[1] || isnumch(a[0]) || (a[0] == '-' && isnumch(a[1])) ||
        (a[0] == '+' && isnumch(a[1])))
    {
        char *end = NULL;
        double f = strtod((char*)a, &end);
        return (size_t)(end-a) == alen ? f : NAN;
    } 
    else if (alen == 8 && memcmp(a, "Infinity", 8) == 0) return INFINITY;
    else if (alen == 9 && memcmp(a, "+Infinity", 9) == 0) return INFINITY;
    else if (alen == 9 && memcmp(a, "-Infinity", 9) == 0) return -INFINITY;
    return NAN;
}

// conv_atoi converts string to int64
// Returns 0 for invalid syntax
static int64_t conv_atoi(const char *a, size_t alen) {
    if (alen == 0) return 0;
    char *end = NULL;
    int64_t i = strtoll(a, &end, 10);
    if ((size_t)(end-a) == alen) return i;
    return conv_ftoi(conv_atof(a, alen));
}

// conv_atou converts string to uint64
// Returns 0 for invalid syntax
static uint64_t conv_atou(const char *a, size_t alen) {
    if (alen == 0) return 0;
    char *end = NULL;
    uint64_t u = strtoull(a, &end, 10);
    if ((size_t)(end-a) == alen) return u;
    return conv_ftoi(conv_atof(a, alen));
}

////////////////////////////////////
// value construction 
////////////////////////////////////

struct xv xv_new_stringn(const char *s, size_t len) {
    return from_value(make_string((uint8_t*)s, len));
}

struct xv xv_new_string(const char *s) {
    return xv_new_stringn(s, s?strlen(s):0);
}

struct xv xv_new_double(double d) {
    return from_value(make_float(d));
}

struct xv xv_new_int64(int64_t i) {
    return from_value(make_int(i));
}

struct xv xv_new_uint64(uint64_t u) {
    return from_value(make_uint(u));
}

struct xv xv_new_boolean(bool t) {
    return from_value(make_bool(t));
}

struct xv xv_new_undefined(void) {
    return from_value(make_undefined());
}

struct xv xv_new_null(void) {
    return from_value(make_null());
}

struct xv xv_new_function(struct xv (*func)(
        struct xv value, const struct xv args, void *udata))
{
    return from_value(make_func(func));
}

struct xv xv_new_jsonn(const char *json, size_t len) {
    return from_value(make_json((uint8_t*)json, len));
}

struct xv xv_new_json(const char *json) {
    return xv_new_jsonn(json, json?strlen(json):0);
}

static int strcmpn(const char *a, size_t alen, const char *b, size_t blen) {
    size_t n = alen < blen ? alen : blen;
    int cmp = strncmp(a, b, n);
    if (cmp == 0) {
        cmp = alen < blen ? -1 : alen > blen ? 1 : 0;
    }
    return cmp;
}

static int nonstr_strcmpn(struct value value, const uint8_t *str, size_t len) {
    char dst[1024];
    xv_string_copy(from_value(value), dst, sizeof(dst));
    return strcmpn(dst, strlen(dst), (char*)str, len);
}

static int string_comparen(struct value value, const uint8_t *str, size_t len) {
    if (value.kind != STR_KIND) {
        return nonstr_strcmpn(value, str, len);
    }
    return strcmpn((char*)value.str, value.len, (char*)str, len);
}

int xv_string_compare(struct xv value, const char *str) {
    return xv_string_comparen(value, str, str?strlen(str):0);
}

int xv_string_comparen(struct xv value, const char *str, size_t len) {
    return string_comparen(to_value(value), (uint8_t*)str, len);
}

static int string_equaln(struct value value, const uint8_t *str, size_t len) {
    if (value.kind != STR_KIND) {
        return nonstr_strcmpn(value, str, len) == 0;
    }
    return value.len == len && memcmp(value.str, str, len) == 0;
}

int xv_string_equal(struct xv value, const char *str) {
    return xv_string_equaln(value, str, str?strlen(str):0);
}

int xv_string_equaln(struct xv value, const char *str, size_t len) {
    return string_equaln(to_value(value), (uint8_t*)str, len);
}

struct xv xv_new_object(const void *ptr, uint32_t tag) {
    return from_value(make_object(ptr, tag));
}

const void *xv_object(struct xv value) {
    struct value tvalue = to_value(value);
    if (tvalue.kind == OBJECT_KIND) {
        return tvalue.obj;
    }
    return NULL;
}

uint32_t xv_object_tag(struct xv value) {
    struct value tvalue = to_value(value);
    if (tvalue.kind == OBJECT_KIND) {
        return (uint32_t)tvalue.len;
    }
    return 0;
}

// array value
size_t xv_array_length(struct xv value) {
    struct value tvalue = to_value(value);
    if (tvalue.kind == ARRAY_KIND){
        return tvalue.len;
    }
    return 0;
}

struct xv xv_array_at(struct xv value, size_t index) {
    struct value tvalue = to_value(value);
    if (tvalue.kind == ARRAY_KIND){
        if (index < tvalue.len) {
            return from_value(tvalue.arr[index]);
        }
    }
    return xv_new_undefined();
}

struct xv xv_new_error(const char *msg) {
    return from_value(err_msg(msg));
}

bool xv_is_undefined(struct xv value) {
    return to_value(value).kind == UNDEF_KIND;
}

bool xv_is_global(struct xv value) {
    return (to_value(value).flag & FLAG_GLOBAL) == FLAG_GLOBAL;
}

struct xv xv_new_array(const struct xv *const*values, 
    size_t nvalues)
{
    return from_value(make_array((struct value*)values, nvalues));
}

struct xv_memstats xv_memstats(void) {
    return (struct xv_memstats) {
        .thread_total_size = sizeof(tmem),
        .thread_size = tmemused,
        .thread_allocs = tmemcount,
        .heap_allocs = tnumallocs,
        .heap_size = theapsize,
    };
}

bool xv_is_error(struct xv value) {
    return is_err(to_value(value));
}

bool xv_is_oom(struct xv value) {
    struct value fvalue = to_value(value);
    return (fvalue.flag&FLAG_EOOM) == FLAG_EOOM && fvalue.kind == ERR_KIND;
}

size_t xv_string_length(struct xv value) {
   struct value fvalue = to_value(value);
    if (fvalue.kind == STR_KIND) {
        return fvalue.len;
    }
    return xv_string_copy(value, NULL, 0);
}

char *xv_string(struct xv value) {
    size_t len = xv_string_length(value);
    char *str = emalloc0(len+1);
    if (!str) return NULL;
    struct value fvalue = to_value(value);
    if (fvalue.kind == STR_KIND) {
        memcpy(str, fvalue.str, len);
        str[len] = '\0';
    } else {
        xv_string_copy(value, str, len+1);
    }
    return str;
}
