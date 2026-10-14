#ifndef SOMA_LANG_DIAG_H
#define SOMA_LANG_DIAG_H

#include <stdint.h>

struct soma_compile_diag {
    uint32_t line;
    uint32_t col;
    char     token[48];
    char     msg[128];
    char     snippet[96];
    int      has_error;
};

#endif
