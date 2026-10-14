#ifndef SOMA_LANG_H
#define SOMA_LANG_H

#include <stdint.h>
#include "lang_diag.h"

void lang_init(void);
int  lang_test_self(void);
struct soma_lint_report {
    uint32_t count;
    uint32_t hard;
    char     msg[8][96];
};
int  lang_lint(const char *src, uint32_t len, struct soma_lint_report *rep);
int  lang_compile(const char *src, uint32_t len, uint8_t *out_code, uint32_t max_code, uint32_t *out_len);
int  lang_compile_ex(const char *src, uint32_t len, uint8_t *out_code, uint32_t max_code, uint32_t *out_len, int interactive, int keep_syms);
int  cmd_lang_run(const char *args, char *out_buf, uint32_t max_out);
int  cmd_lang_vm(const char *args, char *out_buf, uint32_t max_out);
int  cmd_lang_coder(const char *args, char *out_buf, uint32_t max_out);

/* Telemetria de compilacion para bucle cerrado Coder */
const char *lang_get_last_error_msg(void);
uint32_t    lang_get_last_error_line(void);
const char *lang_get_last_error_token(void);
int         lang_get_diagnostic(struct soma_compile_diag *out);

#endif
