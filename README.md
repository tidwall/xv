# xv

An expression evaluator for C.

## Features

- **Friendly.** Provides Javascript [syntax](#syntax). 
- **Fast.** Evaluates expressions using an optimized [recursive descent parser](https://en.wikipedia.org/wiki/Recursive_descent_parser).
- **Extensible.** Allows for [adding custom](#customize) functions and variables. Includes [JSON](#json) support.
- **Stateless.** [No variable assignments](#stateless) or statements.
- **Safe.** [Thread-safe](#memory-and-safety). No allocations for most expressions. 100% [test](#tests) coverage.

## Install

Clone this respository and then use [pkg.sh](https://github.com/tidwall/pkg.sh)
to import dependencies from the [.package](.package) file. 

```
$ git clone https://github.com/tidwall/xv
$ cd xv/
$ pkg.sh import
```

## Example

```C
#include "xv.h"

struct xv value = xv_eval("1 + 2 * (10 * 20)", NULL);
printf("%d\n", xv_int64(value));
// Output: 401
```

There's also an example command line tool in the `tests` directory that can be built
using:

```sh
$ cc -o eval tests/eval.c *.c
$ ./eval '2 * (15 + 13) - 4'
# Output: 52
```

## Syntax

This implemetation uses a subset of Javascript that includes some of the most
common operators, such as:  
`+` `-` `*` `/` `%` `!` `<` `<=` `>` `>=` `==` `===` `!=` `!===` `?:` `??` `,` `[]` `()` `&` `|` `^`.

By design, xv expressions should work in most Javascript consoles, like Node or the Chrome developer console. 

For example, the following expression evaluates to the same result in xv and Node.

```js
(1 + 2 == 3 ? 'yes' : 'no') + ' this works'
```

### Stateless

XV is not intended to be a complete programming language.
It's mainly designed to work with simple expressions and read-only user data.
Mutating data and creating variables are not encouraged.

Thus the `;` and `=` operators are intentionally omitted.

## Customize

The runtime environment can be customized by adding new functions or variables.

For example:

```C
#include <stdio.h>
#include "xv.h"

struct xv get_ref(struct xv this, struct xv ident, void *udata) {
    if (xv_is_global(this)) {
        if (xv_string_compare(ident, "lastName") == 0) {
            return xv_new_string("Anderson");
        }
        if (xv_string_compare(ident, "age") == 0) {
            return xv_new_int64(51);
        }
    }
    return xv_new_undefined();
}

int main() {
    struct xv_env env = { .ref = get_ref };
    struct xv result = xv_eval("lastName == 'Anderson' && age > 45", &env);
    printf("%s\n", xv_bool(result) ? "YES" : "NO");
    return 0;
};

// Output: YES
```

### JSON

There's also built-in support for reading data from JSON documents. 

```C
#include <stdio.h>
#include "xv.h"

struct xv get_ref(struct xv this, struct xv ident, void *udata) {
    if (xv_is_global(this)) {
        if (xv_string_compare(ident, "json") == 0) {
            return xv_new_json(udata);
        }
    }
    return xv_new_undefined();
}

int main() {
    char *json = "{\"lastName\": \"Anderson\", \"age\": 51}";
    struct xv_env env = { .ref = get_ref, .udata = json };
    struct xv result = xv_eval("json.lastName == 'Anderson' && json.age > 45", &env);
    printf("%s\n", xv_bool(result) ? "YES" : "NO");
    return 0;
};

// Output: YES
```

### Custom functions

Here's an example of adding a function to the runtime enviroment.

```C
#include <stdio.h>
#include <math.h>
#include "xv.h"

#define EARTH 6371e3
#define RADIANS (M_PI / 180)
#define DEGREES (180 / M_PI)

// geo_distance is the haversine formula for calculating the distance of two
// points on earth.
struct xv geo_distance(struct xv this, struct xv args, void *udata) {
    double lat1 = xv_double(xv_array_at(args, 0));
    double lon1 = xv_double(xv_array_at(args, 1));
    double lat2 = xv_double(xv_array_at(args, 2));
    double lon2 = xv_double(xv_array_at(args, 3));
    double φ1 = lat1 * RADIANS;
    double λ1 = lon1 * RADIANS;
    double φ2 = lat2 * RADIANS;
    double λ2 = lon2 * RADIANS;
    double Δφ = φ2 - φ1;
    double Δλ = λ2 - λ1;
    double sΔφ2 = sin(Δφ / 2);
    double sΔλ2 = sin(Δλ / 2);
    double haver = sΔφ2*sΔφ2 + cos(φ1)*cos(φ2)*sΔλ2*sΔλ2;
    double meters = EARTH * 2 * asin(sqrt(haver));
    return xv_new_double(meters);
}

struct xv get_ref(struct xv this, struct xv ident, void *udata) {
    if (xv_is_global(this)) {
        if (xv_string_compare(ident, "geo_distance") == 0) {
            return xv_new_function(geo_distance);
        }
    }
    return xv_new_undefined();
}

int main() {
    struct xv_env env = { .ref = get_ref };
    // Distance from Paris, France to Tempe, Arizona
    struct xv result = xv_eval("geo_distance(48.8647, 2.3490, 33.4255, -111.9412)", &env);
    printf("%.0f km\n", xv_double(result)/1000);
    return 0;
};

// Output: 8796 km
```

## Memory and safety

The xv library is designed to be thread-safe. It uses thread local variables
for managing runtime state and includes a fast internal bump allocator for 
expressions that require allocating memory.

### xv_cleanup

There are some cases where the `xv_eval` or `xv_new_*` functions will need to
allocate memory, usually when concatenating strings.
Therefore it's always a good idea to call `xv_cleanup` when you are done. 
This will reset the thread-local environment and free any allocated memory.

For example:

```C
struct xv value = xv_eval("'hello' + ' ' + 'world'", NULL);
char *str = xv_string(value);
xv_cleanup(); 

printf("%s\n", str);  // prints "hello world"
free(str);

// Avoid using the returned 'value' variable after calling
// xv_cleanup, otherwise you risk causing undefined behavior.
```

### Tests

This project includes a test suite can be run from the command line with:

```sh
$ tests/run.sh
```

If [clang](https://clang.llvm.org) is installed then various sanitizers are used such as [AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html) and [UndefinedBehaviorSanitizer](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html).  
And clang will also check code coverage, which this library should always be at 100%.
