#ifndef XV_H
#define XV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// struct xv is an expression value.
//
// This is typically created as a result from the xv_eval function, but can
// also be created using the xv_new_*() functions.
struct xv { uint64_t priv[2]; };

// enum xv_type is returned from the xv_type() function.
enum xv_type { 
    XV_UNDEFINED, XV_NULL, XV_STRING, XV_NUMBER, 
    XV_BOOLEAN, XV_FUNCTION, XV_OBJECT,
};

// struct xv_env is a custom environment that is provided to xv_eval.
struct xv_env {
    // no_case tells xv_eval to perform case-insensitive comparisons.
    bool no_case;
    // udata is custom user data.
    void *udata;
    // ref is a callback that returns a reference value for unknown
    // identifiers, properties, and functions.
    struct xv (*ref)(struct xv this, struct xv ident, void *udata);
};

// xv_eval evaluate an expression and returns the resulting value.
//
// If the xv_eval resulted in an error, such as a syntax error or if the 
// system ran out of memory, then the reuslt can be checked with xv_is_err().
struct xv xv_eval(const char *expr, struct xv_env *env);
struct xv xv_evaln(const char *expr, size_t len, struct xv_env *env);

// xv_cleanup resets the environment and frees any allocated memory
// that may have occured during an xv_eval.
//
// This should be called when you are totally done with the value that resulted
// from the xv_eval call. Thus each xv_eval should have one xv_cleanup.
//
// DO NOT use the resulting 'struct xv' value after calling xv_cleanup, 
// otherwise you risk undefined behavior.
//
// Regarding thread-safety.
// The xv_eval & xv_cleanup are thread-safe functions that use thread-local
// variables and allocations.
void xv_cleanup(void);

// xv_string_copy copies a value as a string representation into the
// provided C string buffer.
//
// Returns the number of characters, not including the null-terminator, needed
// to store the string representation into the C string buffer.
// If the returned length is greater than nbytes-1, then only a parital copy
// occurred, for example:
//    
//    char buf[64];
//    size_t len = xv_string_copy(value, str, sizeof(str));
//    if (len > sizeof(str)-1) {
//        // ... copy did not complete ...
//    }
//
size_t xv_string_copy(struct xv value, char *dst, size_t n);

// xv_string returns the string representation of the value.
// 
// This operation will allocate new memory and it's the caller's responsibility
// to free the string at a later point.
//
// Alternatively the xv_string_copy may be used to avoid allocations.
// 
// Returns NULL if system is out of memory.
char *xv_string(struct xv value);

// xv_string_length returns the length of the string representation of the 
// value.
size_t xv_string_length(struct xv value);

// xv_double returns the double representation of the value.
double xv_double(struct xv value);

// xv_int64 returns the int64_t representation of the value.
int64_t xv_int64(struct xv value);

// xv_uint64 returns the uint64_t representation of the value.
uint64_t xv_uint64(struct xv value);

// xv_object returns the user defined object, or NULL if none.
const void *xv_object(struct xv value);

// xv_object_tag returns the user defined tag for value, or zero if none.
uint32_t xv_object_tag(struct xv value);

// xv_bool returns the C bool representation of the value.
bool xv_bool(struct xv value);

// xv_type return the primitive type of the value.
enum xv_type xv_type(struct xv value);

// xv_is_undefined returns true if the value is undefined'
bool xv_is_undefined(struct xv value);

// xv_is_global return true if the value is the global variable.
//
// This is mainly used in the ref callback. When the 'this' value is global
// then the 'ident' should be a global variable, otherwise the 'ident' should
// be a property of 'this'.
bool xv_is_global(struct xv value);

// xv_is_error return true if the value is an error.
bool xv_is_error(struct xv value);

// xv_is_oom returns true if the value is an error because xv_eval failed due
// to the system being out of memory.
bool xv_is_oom(struct xv value);

// xv_array_length returns the number of items in an array value, or zero if
// the value is not an array or there are no items.
size_t xv_array_length(struct xv value);

// xv_array_at returns the item at index for an array value, or undefined if
// the value is not an array or the index is out of range.
struct xv xv_array_at(struct xv value, size_t index);

// xv_string_compare compares the value to a C string.
//
// This function performs a binary comparison of the characters. It compares
// each character, one-by-one, of both strings. If they are equal to each
// other, it continues until the characters differ or until the end of the
// value or a terminating null-character in the C string is reached.
// Returns < 0, 0, > 0 for less-than, equal-to, greater-than.
int xv_string_compare(struct xv value, const char *str);
int xv_string_comparen(struct xv value, const char *str, size_t len);

// value construction
struct xv xv_new_string(const char *str);
struct xv xv_new_stringn(const char *str, size_t len);
struct xv xv_new_object(const void *ptr, uint32_t tag);
struct xv xv_new_json(const char *json);
struct xv xv_new_jsonn(const char *json, size_t len);
struct xv xv_new_double(double d);
struct xv xv_new_int64(int64_t i);
struct xv xv_new_uint64(uint64_t u);
struct xv xv_new_boolean(bool t);
struct xv xv_new_undefined(void);
struct xv xv_new_null(void);
struct xv xv_new_error(const char *msg);
struct xv xv_new_array(const struct xv *const *values, size_t nvalues);
struct xv xv_new_function(struct xv (*func)(
    struct xv this, const struct xv args, void *udata));

// struct xv_memstats is returned by xv_memstats
struct xv_memstats {
    size_t thread_total_size; // total size of the thread-local memory space
    size_t thread_size;       // number of used thread-local memory space bytes
    size_t thread_allocs;     // number of thread-local memory space allocations
    size_t heap_allocs;       // number of thread-local heap allocations
    size_t heap_size;         // number of used thread-local heap bytes
};

// xv_memstats returns various memory statistics due to an xv_eval call.
//
// These stats are reset by calling xv_cleanup.
struct xv_memstats xv_memstats(void);

// xv_set_allocator allows for configuring a custom allocator for
// all xv library operations. This function, if needed, should be called
// only once at program start up and prior to calling any xv_*() functions.
void xv_set_allocator(
    void *(*malloc)(size_t),
    void *(*realloc)(void*, size_t),
    void (*free)(void*));

#endif // XV_H
