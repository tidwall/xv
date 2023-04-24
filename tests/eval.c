#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "../xv.h"

int main(int argc, char **argv) {
    if (argc == 1) {
        fprintf(stderr, "Usage:   %s expression\n", argv[0]);
        fprintf(stderr, "Example: %s '10 * 51 + 13'\n", argv[0]);
        exit(1);
    }

    struct xv value = xv_eval(argv[1], NULL);
    const char *result = xv_string(value);
    assert(result);
    printf("%s\n", result);
    xv_cleanup();
    return 0;
}
