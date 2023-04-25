#include <math.h>
#include "tests.h"

struct xv numobj(struct xv value, struct xv args, 
    void *udata)
{
    (void)value, (void) udata;
    double d = xv_double(xv_array_at(args, 0));
    if (d == -80808080) {
        return xv_new_error("OperatorError: bad news");
    }
    return xv_new_double(d);
}

struct xv i64(struct xv value, struct xv args, 
    void *udata)
{
    (void)value, (void) udata;
    char str[512];
    xv_string_copy(xv_array_at(args, 0), str, sizeof(str));
    return xv_new_int64(strtoll(str, NULL, 10));
}

struct xv u64(struct xv value, struct xv args, 
    void *udata)
{
    (void)value, (void) udata;
    char str[512];
    xv_string_copy(xv_array_at(args, 0), str, sizeof(str));
    return xv_new_uint64(strtoull(str, NULL, 10));
}

struct xv cust(struct xv value, struct xv args, 
    void *udata)
{
    (void)value, (void) udata;
    char str[512];
    xv_string_copy(xv_array_at(args, 0), str, sizeof(str));
    return xv_new_double((double)atoi(str));
}

struct xv myfn1(struct xv value, struct xv args, 
    void *udata)
{
    (void)value, (void) udata;
    if (xv_string_compare(xv_array_at(args, 0), "9999") == 0) {
        return xv_new_error("fantastic");
    }
    return value;
}

struct xv myfn2(struct xv value, struct xv args, 
    void *udata)
{
    (void)value, (void) udata;
    double sum = 0;
    for (size_t i = 0; i < xv_array_length(args); i++) {
        sum += xv_double(xv_array_at(args, i));
    }
    return xv_new_double(sum);
}

struct xv eref(struct xv this, struct xv ident, 
    void *udata)
{
    (void)udata;
    if (xv_is_global(this)) {
        // global
        if (xv_string_compare(ident, "numobj") == 0) {
            return xv_new_function(numobj);
        }
        if (xv_string_compare(ident, "i64") == 0) {
            return xv_new_function(i64);
        }
        if (xv_string_compare(ident, "u64") == 0) {
            return xv_new_function(u64);
        }
        if (xv_string_compare(ident, "custom_err") == 0) {
            return xv_new_error("ReferenceError: hiya");
        }
        if (xv_string_compare(ident, "cust") == 0) {
            return xv_new_function(cust);
        }
        if (xv_string_compare(ident, "howdy") == 0) {
            return xv_new_string("hiya");
        }
        if (xv_string_compare(ident, "user1") == 0) {
            return xv_new_object(NULL, 99);
        }
        if (xv_string_compare(ident, "json") == 0) {
            return xv_new_json("{"
                "\"name\": {\"first\": \"Janet\", \"last\": \"Anderson\"}, "
                "\"age\": 37,"
                "\"empty\": [],"
                "\"one\": [15],"
                "\"enc\": \"Big\\nBot\","
                "\"data\": [1,true,false,null,{\"a\":1}]"
            "}");
        }
        if (xv_string_compare(ident, "badj") == 0) {
            return xv_new_json("\"");
        }
        if (xv_string_compare(ident, "noj") == 0) {
            return xv_new_json("");
        }
        if (xv_string_compare(ident, "bigjson") == 0) {
            return xv_new_json("{\"a\":123456789012345678901234567890}");
        }

    } else {
        if (xv_string_compare(ident, "myfn1") == 0) {
            return xv_new_function(myfn1);
        }
        if (xv_string_compare(ident, "myfn2") == 0) {
            return xv_new_function(myfn2);
        }
        if (xv_object_tag(this) == 99) {
            if (xv_string_compare(ident, "name") == 0) {
                return xv_new_string("andy");
            }
            if (xv_string_compare(ident, "age") == 0) {
               return xv_new_double(51.0);
            }
            if (xv_string_compare(ident, "err") == 0) {
               return xv_new_error("oh no");
            }
        }
    }
    return xv_new_undefined();
}

static bool nocase = false;

#define eval(input, expect) { \
    int failed = 0; \
    int trys = 0; \
    while (1) { \
        struct xv_env env = { \
            .ref = eref, \
            .no_case = nocase, \
        }; \
        struct xv value = xv_eval((input), &env); \
        char *got = xv_string(value); \
        if (!got) { \
            failed++; \
            continue; \
        } \
        if (xv_is_oom(value)) { \
            if (got) xfree(got); \
            failed++; \
            continue; \
        } \
        if (strcmp(got, (expect)) != 0) { \
            fprintf(stderr, "line %d: expected '%s', got '%s\n", __LINE__, (expect), (got)); \
            exit(1); \
        } \
        if (got) xfree(got); \
        if (rand_alloc_fail && trys < 1000) { \
            trys++; \
            continue; \
        } \
        break; \
    } \
}

void test_xv_various(void) {
    eval(".1", "0.1");
    eval(".1e-1", "0.01");
    eval(".1e-1 + 5", "5.01");
    eval("0.1", "0.1");
    eval("1u64", "1");
    eval("1.0u64", "SyntaxError");
    eval("-1i64", "-1");
    eval("-1.0u64", "SyntaxError");
    eval("", "undefined");
    eval(" ", "undefined");
    eval("()", "SyntaxError");
    eval("\"\\\"", "SyntaxError");
    eval("1", "1");
    eval("-1", "-1");
    eval("- 1", "-1");
    eval(" - 1", "-1");
    eval(" - -1", "1");
    eval("- - 1", "1");
    eval("- - - -1", "1");
    eval("- - - -1 - 2", "-1");
    eval("+1", "1");
    eval("+ 1", "1");
    eval(" + 1", "1");
    eval(" + +1", "1");
    eval(" + +-1", "-1");
    eval(" + +-+ +- -1", "-1");
    eval("-+-+-+-1 - 2", "-1");
    eval("(", "SyntaxError");
    eval("(1", "SyntaxError");
    eval("(1)", "1");
    eval("( 1 )", "1");
    eval("--1", "SyntaxError");
    eval("1--", "SyntaxError");
    eval("1++", "SyntaxError");
    eval("++1", "SyntaxError");
    eval("-+1", "-1");
    eval("\"hello\"", "hello");
    eval("\"hel\\nlo\"", "hel\nlo");
    eval("\"hi\"+1", "hi1");
    eval("\"hi\"-1", "NaN");
    eval("1+1-0.5", "1.5");
    eval("2*4", "8");
    eval("(2*4", "SyntaxError");
    eval("\"2*4", "SyntaxError");
    eval("1 > 2", "false");
    eval("1 > 2 || 3 > 2", "true");
    eval("2 > 3", "false");
    eval("3 > 2 || (2 > 3 && 1 < 2)", "true");
    eval("(1 < 2 && 3 > 2) + 10", "11");
    eval("999 + 777 * (888 + (0.5 + 1.5)) * (0.5 + true)", "1038294");
    eval("999 + 777 * (888 / 0.456) / true", "1514104.2631578946");   // need ryu
    eval("999 + 777 * (888 / 0.456) / 0", "Infinity");
    eval("999 + 777 * (888 / 0.456) / 0", "Infinity");
    eval("1.0e1", "10");
    eval("1.0E1", "10");
    eval("1.0e+1", "10");
    eval("1.0E+1", "10");
    eval("1.0e-1", "0.1");
    eval("1.0E-1", "0.1");
    eval("-1.0E-1", "-0.1");
    eval(("\"he\\\"llo\""), ("he\"llo"));
    eval(("\"he\\\'llo\""), ("he\'llo"));
    eval(("\"he\\\"\\b\\fllo\""), ("he\"\b\fllo"));
    eval(("(\"hello\\\\\\t\\/\\r\\n\\t\\\\\\\"world\")"), ("hello\\\t/\r\n\t\\\"world"));
    eval("\"hello", "SyntaxError");
    eval("1 | 2", "3");
    eval("1 & 2", "0");
    eval("5 & 4", "4");
    eval("5 ^ 4", "1");
    eval("500 ^", "SyntaxError");
    eval("500 &", "SyntaxError");
    eval("500 |", "SyntaxError");
    eval("500 ^ 700", "840");
    eval("500u64 ^ 700u64", "840");
    eval("500i64 ^ 700i64", "840");
    eval("numobj(500) ^ numobj(700)", "840");
    eval("'500' ^ '700'", "840");
    eval("500 & 700", "180");
    eval("500u64 & 700u64", "180");
    eval("numobj(500) & numobj(700)", "180");
    eval("500i64 & 700i64", "180");
    eval("'500' & '700'", "180");
    eval("500 | 700", "1020");
    eval("500u64 | 700u64", "1020");
    eval("500i64 | 700i64", "1020");
    eval("numobj(500) | numobj(700)", "1020");
    eval("'500' | '700'", "1020");
    eval("500 | -700", "-524");
    eval("-500 & -700", "-1020");
    eval("500 ^ -700", "-848");
    eval("(%$#) | 500 | (%$#)", "SyntaxError");
    eval("(%$#) & -500 & (%$#)", "SyntaxError");
    eval("(%$#) ^ 500 ^ (%$#)", "SyntaxError");
    eval("(%$# | 500 | (%$#", "SyntaxError");
    eval("(%$# & -500 & (%$#", "SyntaxError");
    eval("(%$# ^ 500 ^ (%$#", "SyntaxError");
    eval("(400) | (500) ^ (%$#) & (%$#", "SyntaxError");
    eval("(%$#) & (-500 & (%$#", "SyntaxError");
    eval("(%$#) ^ (500 ^ (%$#", "SyntaxError");
    eval(("numobj(-80808080) & numobj(-80808080)"), ("OperatorError: bad news"));
    eval(("numobj(-80808080) | numobj(-80808080)"), ("OperatorError: bad news"));
    eval(("numobj(-80808080) ^ numobj(-80808080)"), ("OperatorError: bad news"));
    eval(("(1 && 2}"), ("SyntaxError"));
    eval(("1 != 2"), ("true"));
    eval(("1 ! 2"), ("SyntaxError"));
    eval(("1 >= 2"), ("false"));
    eval(("1 == 2"), ("false"));
    eval(("1 = 2"), ("SyntaxError"));
    eval(("1 == "), ("SyntaxError"));
    eval((" == 1"), ("SyntaxError"));
    eval(("\"Example emoji, KO: \\ud83d\\udd13, \\ud83c\\udfc3 OK: \\u2764\\ufe0f \""), ("Example emoji, KO: 🔓, 🏃 OK: ❤️ "));
    eval(("\"Example emoji, KO: \\u{d83d}\\udd13, \\ud83c\\udfc3 OK: \\u2764\\ufe0f \""), ("Example emoji, KO: 🔓, 🏃 OK: ❤️ "));
    eval(("\"Example emoji, KO: \\u{d83d}\\u{dd13}, \\ud83c\\udfc3 OK: \\u2764\\ufe0f \""), ("Example emoji, KO: 🔓, 🏃 OK: ❤️ "));
    eval(("\"Example emoji, KO: \\u{d83d}\\u{dd13}, \\u{d83c}\\udfc3 OK: \\u2764\\ufe0f \""), ("Example emoji, KO: 🔓, 🏃 OK: ❤️ "));
    eval(("\"Example emoji, KO: \\u{d83d}\\u{dd13}, \\u{d83c}\\u{dfc3} OK: \\u2764\\ufe0f \""), ("Example emoji, KO: 🔓, 🏃 OK: ❤️ "));
    eval(("\"Example emoji, KO: \\u{d83d}\\u{dd13}, \\u{d83c}\\u{dfc3} OK: \\u{2764}\\ufe0f \""), ("Example emoji, KO: 🔓, 🏃 OK: ❤️ "));
    eval(("\"Example emoji, KO: \\u{d83d}\\u{dd13}, \\u{d83c}\\u{dfc3} OK: \\u{2764}\\u{fe0f} \""), ("Example emoji, KO: 🔓, 🏃 OK: ❤️ "));
    eval(("\"KO: \\xffsd\""), ("KO: ÿsd"));
    eval(("\"KO: \\ud8\""), ("SyntaxError"));
    eval(("\"KO: \\zd8\""), ("KO: zd8"));
    eval(("\"\\1\\0\""), ("SyntaxError"));
    eval(("\"1\\0abc\""), ("1\0abc"));
    eval(("\"KO: \0\""), ("SyntaxError"));
    eval(("false == true"), ("false"));
    eval(("false + true"), ("1"));
    eval(("false - true"), ("-1"));
    eval(("NaN + 1"), ("NaN"));
    eval(("NaN * 1"), ("NaN"));
    eval(("0.24ab31 - 1"), ("SyntaxError"));
    eval(("0 + {1}"), ("SyntaxError"));
    eval(("0 + [1]"), ("01"));
    eval(("hello + 2"), ("ReferenceError: Can't find variable: 'hello'"));
    eval(("i64(\"-9223372036854775808\")"), ("-9223372036854775808"));
    eval(("-9223372036854775808i64"), ("-9223372036854775808"));
    eval(("i64(\"9223372036854775807\")"), ("9223372036854775807"));
    eval(("9223372036854775807i64"), ("9223372036854775807"));
    eval(("i64(\"-9223372036854775808\")"), ("-9223372036854775808"));
    eval(("u64(\"18446744073709551615\") - u64(\"18446744073709551614\")"), ("1"));
    eval(("18446744073709551615u64 - 18446744073709551614u64"), ("1"));
    eval(("u64(\"18446744073709551614\") + u64(\"1\")"), ("18446744073709551615"));
    eval(("i64(\"-9223372036854775808\") + i64(\"1\")"), ("-9223372036854775807"));
    eval(("i64(\"9223372036854775807\") - i64(\"1\")"), ("9223372036854775806"));
    eval(("i64(\"9223372036854775807\") - 1"), ("9223372036854776000"));
    eval(("u64(\"9223372036854775807\") - 1"), ("9223372036854776000"));
    eval(("u64(1) > 0"), ("true"));
    eval(("u64(1) >= 0"), ("true"));
    eval(("u64(0) >= 0"), ("true"));
    eval(("i64(0) >= 0"), ("true"));
    eval(("i64(0) >= 0"), ("true"));
    eval(("i64(-1) >= 0"), ("false"));
    eval(("i64(-1) >= i64(0)"), ("false"));
    eval(("u64(1) >= u64(0)"), ("true"));
    eval(("u64(1) > u64(0)"), ("true"));
    eval(("\"1\" >= \"2\" "), ("false"));
    eval(("\"2\" >= \"2\" "), ("true"));
    eval(("\"2\" >= \"10\" "), ("true"));
    eval(("\"1\" > \"2\" "), ("false"));
    eval(("\"2\" > \"2\" "), ("false"));
    eval(("\"2\" > \"10\" "), ("true"));
    eval(("i64(2) > i64(10)"), ("false"));
    eval(("i64(2) == i64(10)"), ("false"));
    eval(("i64(10) == i64(10)"), ("true"));
    eval(("u64(10) == u64(10)"), ("true"));
    eval(("u64(2) == u64(10)"), ("false"));
    eval(("\"2\" == \"2\""), ("true"));
    eval(("\"2\" == \"3\""), ("false"));
    eval(("\"2\" != \"2\""), ("false"));
    eval(("\"2\" != \"3\""), ("true"));
    eval(("i64(2) != i64(10)"), ("true"));
    eval(("i64(2) != i64(2)"), ("false"));
    eval(("u64(2) != u64(10)"), ("true"));
    eval(("u64(2) != u64(2)"), ("false"));
    eval(("true != false"), ("true"));
    eval(("true != true"), ("false"));
    eval(("true < false"), ("false"));
    eval(("false < true"), ("true"));
    eval(("true <= false"), ("false"));
    eval(("false <= true"), ("true"));
    eval(("\"2\" * \"4\""), ("8"));
    eval(("\"2\" + \"4\""), ("24"));
    eval(("i64(2) * i64(4)"), ("8"));
    eval(("u64(2) * u64(4)"), ("8"));
    eval(("i64(8) / i64(2)"), ("4"));
    eval(("u64(8) / u64(2)"), ("4"));
    eval(("2 <= 4"), ("true"));
    eval(("4 <= 2"), ("false"));
    eval(("i64(2) <= i64(4)"), ("true"));
    eval(("i64(4) <= i64(2)"), ("false"));
    eval(("u64(2) <= u64(4)"), ("true"));
    eval(("u64(4) <= u64(2)"), ("false"));
    eval(("\"2\" < \"2\""), ("false"));
    eval(("\"2\" < \"3\""), ("true"));
    eval(("\"10\" < \"2\""), ("true"));
    eval(("i64(2) < i64(2)"), ("false"));
    eval(("i64(2) < i64(3)"), ("true"));
    eval(("u64(2) < u64(2)"), ("false"));
    eval(("u64(2) < u64(3)"), ("true"));
    eval(("\"2\" <= \"1\""), ("false"));
    eval(("\"2\" <= \"2\""), ("true"));
    eval(("\"2\" <= \"3\""), ("true"));
    eval(("\"10\" <= \"2\""), ("true"));
    eval(("true && false"), ("false"));
    eval(("true || false"), ("true"));
    eval(("\"1\" || false"), ("true"));
    eval(("1 || false"), ("true"));
    eval(("0 || false"), ("false"));
    eval(("0 || false"), ("false"));
    eval(("100 + blank_err"), ("ReferenceError: Can't find variable: 'blank_err'"));
    eval(("100 + custom_err"), ("ReferenceError: hiya"));
    eval(("\"a \\u\\\"567\""), ("SyntaxError"));
    eval(("(hello) + (jello"), ("ReferenceError: Can't find variable: 'hello'"));
    eval(("(1) + (jello"), ("SyntaxError"));
    eval(("(1) && "), ("SyntaxError"));
    eval((" && (1)"), ("SyntaxError"));
    eval(("1 < (}2) < (1)"), ("SyntaxError"));
    eval(("1 + - 2"), ("-1"));
    eval(("1 +"), ("SyntaxError"));
    eval(("-1 + 2"), ("1"));
    eval(("/1"), ("SyntaxError"));
    eval(("10 % 2"), ("0"));
    eval(("10 % 3"), ("1"));
    eval(("i64(10) % i64(3)"), ("1"));
    eval(("u64(10) % u64(3)"), ("1"));
    eval(("\"10\" % \"3\""), ("1"));
    eval(("(1 || (2 > 5)) && (4 < 5 || 5 < 4)"), ("true"));
    eval(("true == !!true"), ("true"));
    eval(("true == !!true == !false"), ("true"));
    eval(("true == ! ! true == !false"), ("true"));
    eval(("true == ! ! true == ! ( 1 == 2 ) "), ("true"));
    eval(("cust(123)"), ("123"));
    eval(("cust(1) + cust(4)"), ("5"));
    eval(("cust(1) - cust(4)"), ("-3"));
    eval(("cust(2) * cust(4)"), ("8"));
    eval(("cust(2) / cust(4)"), ("0.5"));
    eval(("cust(10) % cust(3)"), ("1"));
    eval(("cust(10) < cust(3)"), ("false"));
    eval(("cust(10) <= cust(3)"), ("false"));
    eval(("cust(10) > cust(3)"), ("true"));
    eval(("cust(10) >= cust(3)"), ("true"));
    eval(("cust(10) == cust(3)"), ("false"));
    eval(("cust(10) != cust(3)"), ("true"));
    eval(("cust(10) && cust(0)"), ("false"));
    eval(("cust(10) || cust(3)"), ("true"));
    eval(("cust(10) || cust(3)"), ("true"));
    // eval(("-cust(999)"), ("OperatorError: not this time")); // custom operator
    // eval(("cust(-90909090) + cust(-90909090)"), ("undefined")); // custom operator
    // eval(("cust(-80808080) + cust(-80808080)"), ("OperatorError: bad news")); // custom operator
    eval(("0x1"), ("1"));
    eval(("0xZ"), ("SyntaxError"));
    eval(("Infinity"), ("Infinity"));
    eval(("-Infinity"), ("-Infinity"));
    eval(("0xFFFFFFFF"), ("4294967295"));
    eval(("0xFFFFFFFF+1"), ("4294967296"));
    eval(("0xFFFFFFFFFFFFFFFF"), ("18446744073709552000"));
    eval(("0xFFFFFFFFFFFFFFFF+1"), ("18446744073709552000"));
    eval(("true ? 1 : 2"), ("1"));
    eval(("false ? 1 : 2"), ("2"));
    eval(("false ? 1 : true ? 2 : 3"), ("2"));
    eval(("false ? 1 : false ? 2 : 3"), ("3"));
    eval(("5*2-10 ? 1 : (3*3-9 < 1 || 6+6-12 ? 8 : false) ? 2 : 3"), ("2"));
    eval(("(false ? 1 : 2"), ("SyntaxError"));
    eval(("(false) ? (0xTT) : (0xTT)"), ("SyntaxError"));
    eval(("(true) ? (0xTT) : (0xTT)"), ("SyntaxError"));
    eval(("(true) ? (0xTT) : (0xTT"), ("SyntaxError"));
    eval(("(true) ? (0xTT) 123"), ("SyntaxError"));
    eval(("(0xTT) ? (0xTT) : 123"), ("SyntaxError"));
    eval(("1e+10 > 0 ? \"big\" : \"small\""), ("big"));
    eval(("undefined"), ("undefined"));
    eval(("true ? () : ()"), ("SyntaxError"));
    eval(("undefined + 10"), ("NaN"));
    eval(("null"), ("null"));
    eval(("null + 10"), ("10"));
    eval(("undefined + undefined"), ("NaN"));
    eval(("null + null"), ("0"));
    eval(("null + undefined"), ("NaN"));
    eval(("!undefined"), ("true"));
    eval(("!!undefined"), ("false"));
    eval(("!null"), ("true"));
    eval(("!!null"), ("false"));
    eval(("null??1"), ("1"));
    eval(("null??0"), ("0"));
    eval(("undefined??1+1"), ("2"));
    eval(("undefined??0+1"), ("1"));
    eval(("false??1+1"), ("false"));
    eval(("true??1+1"), ("true"));
    eval(("false??1+1"), ("false"));
    eval(("true??1+1"), ("true"));
    eval(("(false??1)+1"), ("1"));
    eval(("(true??1)+1"), ("2"));
    eval(("(cust(1)??cust(2))+1"), ("2"));
    eval(("'hello \\'\\\"\\\"\\a\\xFF\\p world'"), ("hello '\"\"aÿp world"));
    eval(("\"\\u{A}\""), ("\n"));
    eval(("\'\\xFG'"), ("SyntaxError"));
    eval(("\"\\u{21}\""), ("!"));
    eval(("\"\\u{AFFF}\""), ("꿿"));
    eval(("\"\\u{1f516}\""), ("🔖"));
    eval(("\"\\v\""), ("\v"));
    eval(("\"\\0\""), ("\0"));
    eval(("\"\\u{YY}\""), ("SyntaxError"));
    eval(("\"\\u{FF\""), ("SyntaxError"));
    eval(("1,2,3,4"), ("4"));
    eval(("1=,2,3,4"), ("SyntaxError"));
    eval(("1(,2,3,4"), ("SyntaxError"));
    eval(("1,2,3,(4+)"), ("SyntaxError"));
    eval(("6<7 , 2>5 , 5"), ("5"));
    eval(("hello ?. world"), ("ReferenceError: Can't find variable: 'hello'"));
    eval(("this?.that(\"1\",\"2\")"), ("ReferenceError: Can't find variable: 'this'"));
    eval(("  != 100"), ("SyntaxError"));
    eval(("  >= 100"), ("SyntaxError"));
    eval((" (1) != (\"\\'1"), ("SyntaxError"));
    eval(("1 != 2 > 1 != 1"), ("true"));
    eval(("1 != 2 < 1 != 1"), ("false"));
    eval(("1 != 1 < 2 != 1"), ("true"));
    eval(("u64+\"hello\""), ("[Function]hello"));
    eval(("0.123123i64"), ("SyntaxError"));
    eval(("howdy.myfn1().myfn2(\"1\",2,\"3\") == 6"), ("true"));
    eval(("howdy.myfn1.there"), ("undefined"));
    eval(("howdy.myfn3.there"), ("TypeError: Cannot read properties of undefined (reading 'there')"));
    eval(("howdy.myfn3?.there"), ("undefined"));
    eval(("howdy.myfn1#e"), ("SyntaxError"));
    eval(("howdy.myfn1.#e"), ("SyntaxError"));
    eval(("#howdy.myfn1.#e"), ("SyntaxError"));
    eval(("howdy[\"do\"]"), ("undefined"));
    eval(("howdy[9i8203]"), ("SyntaxError"));
    eval(("howdy[\"did\"]"), ("undefined"));
    eval(("howdy.myfn1(9999)"), ("fantastic"));
    eval(("((0i64)%0i64)"), ("NaN"));
    eval(("((0i64)/0i64)"), ("NaN"));
    eval(("((0u64)%0u64)"), ("NaN"));
    eval(("((0u64)/0u64)"), ("NaN"));
    eval(("64"), ("64"));
    eval(("u64"), ("[Function]"));
    eval(("i64"), ("[Function]"));
    eval(("1 == \"1\""), ("true"));
    eval(("1 === \"1\""), ("false"));
    eval(("1 !== \"1\""), ("true"));
    eval(("\"1\" === \"1\""), ("true"));
    eval(("\"1\" === \"2\""), ("false"));
    eval(("\"1\" !== \"2\""), ("true"));
    eval(("false !== true"), ("true"));
    eval(("false !== ! true"), ("false"));
    // eval(("cust1 < cust2"), ("OperatorError: too bad 1"));
    // eval(("cust1 <= cust2"), ("OperatorError: too bad 1"));
    // eval(("cust1 == cust2"), ("OperatorError: too bad 1"));
    // eval(("cust1 > cust2"), ("OperatorError: too bad 2"));
    // eval(("cust1 >= cust2"), ("OperatorError: too bad 2"));
    // eval(("cust3 < cust2"), ("false"));
    // eval(("cust3 <= cust2"), ("OperatorError: too bad 2"));
    // eval(("cust2 > cust3"), ("false"));
    // eval(("cust2 >= cust3"), ("OperatorError: too bad 2"));
    // eval(("cust2 == cust3"), ("OperatorError: too bad 2"));
    // eval(("cust3 == cust2"), ("OperatorError: too bad 2"));
    // eval(("cust3 !== cust2"), ("OperatorError: too bad 2"));
    // eval(("cust3 != cust2"), ("OperatorError: too bad 2"));
    eval(("true.hello == undefined"), ("true"));
    eval(("true.hello == '11'"), ("false"));
    eval(("true.hello == null"), ("false"));
    eval(("null == null"), ("true"));
    eval(("[1,2,(3,4,'a','b'),3,1==2,3.5+4.5]"), ("1,2,b,3,false,8"));
    eval(("11*1"), ("11"));
    eval(("11*2"), ("22"));
    eval(("[11]*2"), ("22"));
    eval(("[11,22]*2"), ("NaN"));
    eval(("[]*2"), ("0"));
    eval(("[]+2"), ("2"));
    eval(("[]-2"), ("-2"));
    eval(("howdy()"), ("TypeError: howdy is not a function"));
    eval(("user1.name"), ("andy"));
    eval(("user1.age"), ("51"));
    eval(("json.name.first"), ("Janet"));
    eval(("json.name.last"), ("Anderson"));
    eval(("json.name"), ("{\"first\": \"Janet\", \"last\": \"Anderson\"}"));
    eval(("json.empty * 2"), ("0"));
    eval(("json.empty * 2"), ("0"));
    eval(("json.one * 2"), ("30"));
    eval(("json.data * 2"), ("NaN"));
    eval(("json.name * 2"), ("NaN"));
    eval(("user1 * 2"), ("NaN"));
    // "\"data\": [1,true,false,null,{\"a\":1}]"
    eval(("json.data[1] == true"), ("true"));
    eval(("json.data[2] == false"), ("true"));
    eval(("json.data[3] == null"), ("true"));
    eval(("json.data[0]"), ("1"));
    eval(("json.data.0"), ("SyntaxError"));
    eval(("json.data[-1]"), ("undefined"));
    eval(("(json.data[0]+4)*10"), ("50"));
    eval(("json.data[4].a"), ("1"));
    eval(("json.data[4].b"), ("undefined"));
    eval(("json.enc"), ("Big\nBot"));
    eval(("badj"), (""));
    eval(("noj"), ("ReferenceError: Can't find variable: 'noj'"));
    eval(("11i64 | 22i64"), ("31"));
    eval(("11i64 | 22"), ("31"));
    eval(("11i64 | '22'"), ("31"));
    eval(("11i64 | 22u64"), ("31"));
    eval(("11i64 | null"), ("11"));
    eval(("11i64 | undefined"), ("11"));
    eval(("10i64 | true"), ("11"));
    eval(("11u64 | 22u64"), ("31"));
    eval(("11u64 | 22"), ("31"));
    eval(("11u64 | '22'"), ("31"));
    eval(("11u64 | 22i64"), ("31"));
    eval(("11u64 | null"), ("11"));
    eval(("11u64 | undefined"), ("11"));
    eval(("10u64 | true"), ("11"));
    eval(("10u64 || 0"), ("true"));
    eval(("10u64 || 0u64"), ("true"));
    eval(("10u64 || 0i64"), ("true"));
    eval(("10i64 || 0i64"), ("true"));
    eval(("'1' || '0'"), ("true"));
    eval(("'1' ? '2' : '3'"), ("2"));
    eval(("[1] ? '2' : '3'"), ("2"));
    eval(("[] ? '2' : '3'"), ("2"));
    eval(("[0] ? '2' : '3'"), ("2"));
    eval(("user1"), ("[Object]"));
    eval(("'hello' + 'world' + '99999999999999999'"), ("helloworld99999999999999999"));
    eval(("8888888899999999999999999 + 8888888899999999999999999"), ("1.77777778e+25"));
    eval(("8888888899999999999999999 + '8888888899999999999999999'"), ("8.8888889e+248888888899999999999999999"));
    eval(("   'hello'   "), ("hello"));
    eval(("\t\n\r\v   'hello'   "), ("hello"));
    eval(("\t\n\r\v\1   'hello'   "), ("SyntaxError"));
    eval(("1 ? 2 ? 3 : 2 : 1"), ("3"));
    eval(("'11' < '1'"), ("false"));
    eval(("'11' < '11'"), ("false"))

    // case sensitivity toggling
    nocase = false;
    eval(("'hi' < 'HI'"), ("false"));
    eval(("'HI' < 'hi'"), ("true"));
    eval(("'HI' < 'HI'"), ("false"));
    eval(("'HI' < 'HII'"), ("true"));
    eval(("'HII' < 'HI'"), ("false"));
    nocase = true;
    eval(("'hi' < 'HI'"), ("false"));
    eval(("'HI' < 'hi'"), ("false"));
    eval(("'HI' < 'hii'"), ("true"));
    eval(("'hj' < 'HI'"), ("false"));
    eval(("'hi' < 'HJ'"), ("true"));
    nocase = false;

    eval(("'1' | (bad)"), ("ReferenceError: Can't find variable: 'bad'"));
    eval(("('\n') || '1'"), ("SyntaxError"));
    eval(("'1' | "), ("SyntaxError"));
    eval(("'1' | \t | 3"), ("SyntaxError"));
    eval(("'1' | (123) | (123 "), ("SyntaxError"));
    eval(("undefined.numobj"), ("TypeError: Cannot read properties of undefined (reading 'numobj')"));
    eval(("json.data[0+1,0+2]"), ("false"));
    eval(("json.data[0+1,0+]"), ("SyntaxError"));
    eval(("json.data[0"), ("SyntaxError"));
    eval(("json.data['123']"), ("undefined"));
    eval(("user1['e'+'rr']"), ("oh no"));
    eval(("user1(1"), ("SyntaxError"));
    eval(("numobj(1+'123',)"), ("SyntaxError"));
    eval(("123?"), ("SyntaxError"));
    eval(("json?.data[0]"), ("1"));
    eval(("json?.data[0]?"), ("SyntaxError"));
    eval(("json?.data[0]?."), ("SyntaxError"));
    eval((" & 1 & 1 "), ("SyntaxError"));
    eval((" | 1 | 1 "), ("SyntaxError"));
    eval(("1 + [2] + 3"), ("123"));
    eval(("1 * [2] * 3"), ("6"));
    eval(("1 * [{}] * 3"), ("SyntaxError"));

    // bad strings escape sequences
    eval(("'\\n'"), ("\n"));
    eval(("'"), ("SyntaxError"));
    eval(("'\\"), ("SyntaxError"));
    eval(("'\\\\"), ("SyntaxError"));
    eval(("'\\u"), ("SyntaxError"));
    eval(("'\\u'"), ("SyntaxError"));
    eval(("'\\u{"), ("SyntaxError"));
    eval(("'\\u{1"), ("SyntaxError"));
    eval(("'\\u{}"), ("SyntaxError"));
    eval(("'\\u{}'"), ("SyntaxError"));

    // weird codepoints
    eval(("'\\ufffd'"), ("�"));
    // The following input is an invalid pair.
    // Node and safari report differing results. 
    // But, this is the correct result afaik.
    eval(("'\\ud801\\ufffd'"), ("�"));  
    eval(("'\\ufffd'"), ("�"));
    eval(("'\\ud800'"), ("�"));
    eval(("'\\ud801'"), ("�"));


    // unsupported keywords
    eval(("new == true"), ("SyntaxError: Unsupported keyword 'new'"));
    eval(("typeof == true"), ("SyntaxError: Unsupported keyword 'typeof'"));
    eval(("void == true"), ("SyntaxError: Unsupported keyword 'void'"));
    eval(("await == true"), ("SyntaxError: Unsupported keyword 'await'"));
    eval(("function == true"), ("SyntaxError: Unsupported keyword 'function'"));
    eval(("in == true"), ("SyntaxError: Unsupported keyword 'in'"));
    eval(("instanceof == true"), ("SyntaxError: Unsupported keyword 'instanceof'"));
    eval(("yield == true"), ("SyntaxError: Unsupported keyword 'yield'"));

    eval(("'hello'?"), ("SyntaxError"));
    eval(("json?^data[0]"), ("SyntaxError"));

    eval(("howdy.myfn2(1,2,3) == 6"), ("true"));
    eval(("howdy.v1"), ("undefined"));
    eval(("howdy.v1.v2"), ("TypeError: Cannot read properties of undefined (reading 'v2')"));
    eval(("howdy.v1?.v2"), ("undefined"));
    eval(("howdy?<v2"), ("SyntaxError"));

    // eval(("howdy.myfn3"), ("false"));
    // eval(("howdy.myfn3?.(1,2,3) == 6"), ("true"));


    // exhaust the thread memory space
    while (1) {
        struct xv_memstats stats = xv_memstats();
        if (stats.heap_size > 0) break;
        eval(("'hello' + 'world'"), ("helloworld"));
    }
    eval(("bigjson + bigjson"), ("{\"a\":123456789012345678901234567890}{\"a\":123456789012345678901234567890}"));

    eval(("'100' / '2'"), ("50"));
    eval(("-'100' + 2"), ("-98"));
    eval(("-'100' + -'2'"), ("-102"));
    eval(("-'100' + -'\\42'"), ("SyntaxError"));
    eval(("-'\\4100' + -'\\42'"), ("SyntaxError"));
    // eval(("'" "\xFF" "\xFF" "\xFF" "100'"), ("���100"));
    // eval(("'\\u{fffd}100'"), ("�100"));
    // eval(("'\\u{0x10ffff}100'"), ("�100"));
    // eval(("'\\u{0x11ffff}100'"), ("�100"));


    struct xv result = xv_eval("bad == 1", NULL);
    assert(xv_is_error(result));
}

void test_xv_various_sysalloc(void) {
    test_xv_various();
}

void test_xv_various_chaos(void) {
    test_xv_various();
}

void test_xv_values(void) {
    assert(xv_is_undefined(xv_new_undefined()));
    assert(!xv_is_undefined(xv_new_null()));
    assert(xv_array_length(xv_new_array(NULL, 1)) == 1);
    assert(xv_array_length(xv_new_array(NULL, 0)) == 0);
    assert(xv_array_length(xv_new_undefined()) == 0);
    assert(xv_object_tag(xv_new_object(NULL, 99)) == 99);
    assert(xv_object_tag(xv_new_undefined()) == 0);
    assert(strcmp(xv_object(xv_new_object("hello", 99)), "hello") == 0);
    assert(xv_object(xv_new_undefined()) == NULL);
    assert(xv_bool(xv_new_boolean(true)) == true);
    assert(xv_bool(xv_new_boolean(false)) == false);
    assert(xv_bool(xv_new_undefined()) == false);
    assert(xv_bool(xv_new_double(0)) == false);
    assert(xv_bool(xv_new_double(1)) == true);
    assert(xv_string_compare(xv_new_string("hello"), "hello") == 0);
    assert(xv_string_compare(xv_new_string("hello"), "jello") < 0);
    assert(xv_string_compare(xv_new_string("jello"), "hello") > 0);
    assert(xv_string_compare(xv_new_string("jello"), NULL) > 0);
    assert(xv_string_compare(xv_new_string(NULL), "hello") < 0);
    assert(xv_string_compare(xv_new_string(NULL), NULL) == 0);
    assert(xv_string_compare(xv_new_json("{}"), "{}") == 0);
    assert(xv_string_compare(xv_new_json(NULL), "undefined") == 0);
    assert(xv_string_compare(xv_new_json("\"hello\""), "hello") == 0);
    assert(xv_string_equal(xv_new_string("hello"), "hello"));
    assert(!xv_string_equal(xv_new_string(NULL), "hello"));
    assert(xv_string_equal(xv_new_string(NULL), NULL));
    assert(!xv_string_equal(xv_new_string("hello"), NULL));
    assert(xv_string_equal(xv_new_json("{}"), "{}"));
    assert(xv_int64(xv_new_string("123")) == 123);
    assert(xv_int64(xv_new_string("")) == 0);
    assert(xv_int64(xv_new_string("123.123")) == 123);
    assert(xv_int64(xv_new_string("-123")) == -123);
    assert(xv_int64(xv_new_string("-123.123")) == -123);
    assert(xv_uint64(xv_new_string("123")) == 123);
    assert(xv_uint64(xv_new_string("")) == 0);
    assert(xv_uint64(xv_new_string("123.123")) == 123);
    assert(xv_double(xv_new_string("123")) == 123.0);
    assert(isnan(xv_double(xv_new_string(""))));
    assert(xv_double(xv_new_string("123.123")) == 123.123);
    assert(xv_double(xv_new_string("-123")) == -123.0);
    assert(xv_double(xv_new_string("+123")) == +123.0);
    assert(xv_double(xv_new_string("Infinity")) == INFINITY);
    assert(xv_double(xv_new_string("+Infinity")) == INFINITY);
    assert(xv_double(xv_new_string("-Infinity")) == -INFINITY);
    assert(isnan(xv_double(xv_new_string("NaN"))));


    assert(xv_string_compare(xv_new_double(123.1), "123.1") == 0);
    assert(xv_string_compare(xv_new_int64(-123), "-123") == 0);
    assert(xv_string_compare(xv_new_uint64(123), "123") == 0);
    assert(xv_int64(xv_new_uint64(UINT64_MAX)) == INT64_MAX);
    assert(xv_uint64(xv_new_int64(INT64_MIN)) == 0);
    assert(xv_uint64(xv_new_int64(100)) == 100);


    assert(xv_int64(xv_new_double(123.1)) == 123);
    assert(xv_int64(xv_new_double(123912039182039810293810293.1)) == INT64_MAX);
    assert(xv_int64(xv_new_double(-123912039182039810293810293.1)) == INT64_MIN);

    assert(xv_uint64(xv_new_double(123.1)) == 123);
    assert(xv_uint64(xv_new_double(123912039182039810293810293.1)) == UINT64_MAX);
    assert(xv_uint64(xv_new_double(-123912039182039810293810293.1)) == 0);

    assert(xv_uint64(xv_new_boolean(true)) == 1);
    assert(xv_int64(xv_new_boolean(true)) == 1);
    assert(xv_uint64(xv_new_boolean(false)) == 0);
    assert(xv_int64(xv_new_boolean(false)) == 0);

    assert(xv_type(xv_new_boolean(false)) == XV_BOOLEAN);
    assert(xv_type(xv_new_null()) == XV_OBJECT);
    assert(xv_type(xv_new_string("hello")) == XV_STRING);
    assert(xv_type(xv_new_double(123)) == XV_NUMBER);
    assert(xv_type(xv_new_undefined()) == XV_UNDEFINED);
    assert(xv_type(xv_new_function(numobj)) == XV_FUNCTION);
    char buf[256];
    xv_string_copy(xv_new_error("oh no"), buf, sizeof(buf));
    assert(strcmp(buf, "oh no") == 0);
    xv_string_copy(xv_new_error(""), buf, sizeof(buf));
    assert(strcmp(buf, "") == 0);

    xv_string_copy(xv_new_error("oh no"), buf, 1);
    assert(strcmp(buf, "") == 0);

    xv_string_copy(xv_new_error("oh no"), buf, 2);
    assert(strcmp(buf, "o") == 0);

    assert(xv_int64(xv_new_int64(-123)) == -123);
    assert(xv_uint64(xv_new_uint64(123)) == 123);
    assert(xv_int64(xv_new_string("-123")) == -123);
    assert(xv_uint64(xv_new_string("123")) == 123);
    assert(xv_int64(xv_new_error("-123")) == 0);
    assert(xv_uint64(xv_new_error("123")) == 0);
    assert(!xv_is_oom(xv_new_error("123")));

}

void test_xv_maxdepth(void) {
    char *expr = malloc(10000);
    assert(expr);
    int mdepth;

    mdepth = 100;
    expr[0] = '\0';
    strcat(expr, "1 + ");
    for (int i = 0; i < mdepth; i++) strcat(expr, "(");
    strcat(expr, "1");
    for (int i = 0; i < mdepth; i++) strcat(expr, ")");
    eval(expr, "2");

    
    mdepth = 101;
    expr[0] = '\0';
    strcat(expr, "1 + ");
    for (int i = 0; i < mdepth; i++) strcat(expr, "(");
    strcat(expr, "1");
    for (int i = 0; i < mdepth; i++) strcat(expr, ")");
    eval(expr, "MaxDepthError");

    



}

int main(int argc, char **argv) {
    do_test(test_xv_values);
    do_test(test_xv_various);
    do_sysalloc_test(test_xv_various_sysalloc);
    do_chaos_test(test_xv_various_chaos);
    do_test(test_xv_maxdepth);
    return 0;
}

