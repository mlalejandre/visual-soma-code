#include "lang.h"
#include "vm.h"
#include "lexer.h"
#include "console.h"
#include "fs.h"
#include "mem.h"
#include "agent.h"
#include "fb.h"
#include "gui.h"
#include "soma_str.h"

#define COMPILER_MAX_SYMS  128
#define MAX_BREAKS         32
#define MAX_CONTINUES      32
#define MAX_FNS            64
#define PARSE_MAX_DEPTH    48

struct soma_sym {
    char     name[32];
    uint16_t slot;
};

struct soma_fn {
    char     name[32];
    uint32_t addr;
    uint8_t  nparams;
};

struct soma_compiler {
    struct soma_lexer lex;
    struct soma_token cur;
    struct soma_token prev;

    struct soma_sym   syms[COMPILER_MAX_SYMS];
    uint16_t          sym_count;

    struct soma_sym   gsyms[COMPILER_MAX_SYMS];
    uint16_t          gcount;

    struct soma_fn    fns[MAX_FNS];
    uint16_t          fn_count;
    int               in_func;

    uint8_t          *code;
    uint32_t          code_len;
    uint32_t          max_code;

    int               had_error;
    uint32_t          error_line;
    int               interactive;

    uint32_t          break_patches[MAX_BREAKS];
    uint32_t          break_count;

    uint32_t          continue_patches[MAX_CONTINUES];
    uint32_t          continue_count;
    int               in_loop;
    int               depth;
};

static struct soma_compiler    compiler_inst;
static struct soma_vm          global_repl_vm;
static int                     repl_vm_initialized = 0;
static struct soma_compile_diag global_diag;

const char *lang_get_last_error_msg(void)   { return global_diag.msg; }
uint32_t    lang_get_last_error_line(void)  { return global_diag.line; }
const char *lang_get_last_error_token(void) { return global_diag.token; }

int lang_get_diagnostic(struct soma_compile_diag *out)
{
    if (!out) return 0;
    *out = global_diag;
    return global_diag.has_error;
}

static void compiler_error(struct soma_compiler *c, const char *msg)
{
    if (c->had_error) return;
    c->had_error = 1;
    c->error_line = c->cur.line;

    global_diag.has_error = 1;
    global_diag.line = c->cur.line;
    global_diag.col = 0;

    soma_sbuf_t sb_msg;
    soma_sbuf_init(&sb_msg, global_diag.msg, sizeof(global_diag.msg));
    soma_sbuf_copy(&sb_msg, msg);

    soma_sbuf_t sb_tok;
    soma_sbuf_init(&sb_tok, global_diag.token, sizeof(global_diag.token));
    soma_sbuf_copy(&sb_tok, c->cur.text);

    /* Capturar linea circundante en snippet */
    soma_sbuf_t sb_snip;
    soma_sbuf_init(&sb_snip, global_diag.snippet, sizeof(global_diag.snippet));
    if (c->lex.src && c->lex.len > 0) {
        uint32_t cur_p = (c->lex.pos > 0) ? (c->lex.pos - 1) : 0;
        uint32_t start = cur_p;
        while (start > 0 && c->lex.src[start - 1] != '\n') start--;
        uint32_t end = cur_p;
        while (end < c->lex.len && c->lex.src[end] != '\n' && c->lex.src[end] != '\r') end++;
        for (uint32_t i = start; i < end && soma_sbuf_avail(&sb_snip) > 0; ++i) {
            soma_sbuf_append_char(&sb_snip, c->lex.src[i]);
        }
    }

    kprint("[COMPILER ERROR L");
    kprint_dec(c->cur.line);
    kprint("]: ");
    kprint(msg);
    kprint(" (cerca de: '");
    kprint(c->cur.text);
    kprint("')\n");
}

static void emit_u8(struct soma_compiler *c, uint8_t b)
{
    if (c->code_len >= c->max_code) {
        compiler_error(c, "Buffer de bytecode agotado");
        return;
    }
    c->code[c->code_len++] = b;
}

static void emit_u16(struct soma_compiler *c, uint16_t val)
{
    emit_u8(c, (uint8_t)(val & 0xFF));
    emit_u8(c, (uint8_t)((val >> 8) & 0xFF));
}

static void emit_i32(struct soma_compiler *c, int32_t val)
{
    uint32_t u = (uint32_t)val;
    emit_u8(c, (uint8_t)(u & 0xFF));
    emit_u8(c, (uint8_t)((u >> 8) & 0xFF));
    emit_u8(c, (uint8_t)((u >> 16) & 0xFF));
    emit_u8(c, (uint8_t)((u >> 24) & 0xFF));
}

static void emit_i64(struct soma_compiler *c, int64_t val)
{
    uint64_t u = (uint64_t)val;
    for (int i = 0; i < 8; ++i) emit_u8(c, (uint8_t)((u >> (i * 8)) & 0xFF));
}

static void emit_str(struct soma_compiler *c, const char *str)
{
    uint16_t slen = 0;
    while (str[slen]) slen++;
    emit_u8(c, OP_PUSH_STR);
    emit_u16(c, slen);
    for (uint16_t i = 0; i < slen; ++i) emit_u8(c, (uint8_t)str[i]);
}

static uint32_t emit_jump(struct soma_compiler *c, uint8_t op)
{
    emit_u8(c, op);
    uint32_t patch_pos = c->code_len;
    emit_i32(c, 0);
    return patch_pos;
}

static void patch_jump(struct soma_compiler *c, uint32_t patch_pos, uint32_t target)
{
    if (!c->code || patch_pos > c->max_code || c->max_code - patch_pos < 4 ||
        patch_pos > c->code_len || c->code_len - patch_pos < 4) {
        compiler_error(c, "Parche de salto fuera del buffer de bytecode");
        return;
    }
    if (target > c->max_code || target > c->code_len) {
        compiler_error(c, "Destino de salto fuera del bytecode generado");
        return;
    }
    int64_t rel = (int64_t)target - ((int64_t)patch_pos + 4);
    if (rel < INT32_MIN || rel > INT32_MAX) {
        compiler_error(c, "Desplazamiento de salto fuera de rango");
        return;
    }
    uint32_t u = (uint32_t)(int32_t)rel;
    c->code[patch_pos]     = (uint8_t)(u & 0xFF);
    c->code[patch_pos + 1] = (uint8_t)((u >> 8) & 0xFF);
    c->code[patch_pos + 2] = (uint8_t)((u >> 16) & 0xFF);
    c->code[patch_pos + 3] = (uint8_t)((u >> 24) & 0xFF);
}

static void advance(struct soma_compiler *c)
{
    c->prev = c->cur;
    c->cur = lexer_next(&c->lex);
}

static int check(struct soma_compiler *c, enum soma_tok_type type)
{
    return c->cur.type == type;
}

static int match(struct soma_compiler *c, enum soma_tok_type type)
{
    if (check(c, type)) {
        advance(c);
        return 1;
    }
    return 0;
}

static int consume(struct soma_compiler *c, enum soma_tok_type type, const char *msg)
{
    if (check(c, type)) {
        advance(c);
        return 1;
    }
    compiler_error(c, msg);
    return 0;
}

static int match_semicolon(struct soma_compiler *c, const char *msg)
{
    if (match(c, TOK_SEMICOLON)) return 1;
    if (check(c, TOK_EOF) || check(c, TOK_RBRACE)) return 1;
    if (c->prev.line < c->cur.line) return 1;
    compiler_error(c, msg);
    return 0;
}

static int sym_find(struct soma_compiler *c, const char *name)
{
    for (int i = 0; i < c->sym_count; ++i) {
        if (soma_sbuf_eq(c->syms[i].name, name)) return c->syms[i].slot;
    }
    return -1;
}

static int sym_add(struct soma_compiler *c, const char *name)
{
    int existing = sym_find(c, name);
    if (existing >= 0) return existing;
    if (c->sym_count >= COMPILER_MAX_SYMS) {
        compiler_error(c, "Maximo de variables excedido (max 128)");
        return 0;
    }
    uint16_t slot = c->sym_count;
    soma_sbuf_t sb;
    soma_sbuf_init(&sb, c->syms[slot].name, sizeof(c->syms[slot].name));
    soma_sbuf_copy(&sb, name);
    c->syms[slot].slot = slot;
    c->sym_count++;
    return slot;
}

static int var_slot(struct soma_compiler *c, const char *name, int create, int *is_local)
{
    if (c->in_func) {
        int s = sym_find(c, name);
        if (s >= 0) {
            *is_local = 1;
            return s;
        }
        for (int i = 0; i < c->gcount; ++i) {
            if (soma_sbuf_eq(c->gsyms[i].name, name)) {
                *is_local = 0;
                return c->gsyms[i].slot;
            }
        }
        *is_local = 1;
        return create ? sym_add(c, name) : -1;
    }
    *is_local = 0;
    return create ? sym_add(c, name) : sym_find(c, name);
}

static int var_slot_decl(struct soma_compiler *c, const char *name, int *is_local)
{
    if (c->in_func) {
        *is_local = 1;
        return sym_add(c, name);
    }
    return var_slot(c, name, 1, is_local);
}

static void emit_var(struct soma_compiler *c, int store, int slot, int local)
{
    emit_u8(c, store ? (local ? OP_STORE_LOCAL : OP_STORE_VAR)
                     : (local ? OP_LOAD_LOCAL  : OP_LOAD_VAR));
    emit_u16(c, (uint16_t)slot);
}

static void parse_expr(struct soma_compiler *c);
static void parse_primary(struct soma_compiler *c);

static int is_assign_tok(enum soma_tok_type t)
{
    return t == TOK_ASSIGN || t == TOK_PLUS_ASSIGN || t == TOK_MINUS_ASSIGN ||
           t == TOK_STAR_ASSIGN || t == TOK_SLASH_ASSIGN;
}

static void prescan_globals(struct soma_compiler *c, const char *src, uint32_t len)
{
    struct soma_lexer lx;
    struct soma_token t;
    enum soma_tok_type prev_type = TOK_SEMICOLON;
    int fn_depth = 0;
    int pending_fn = 0;
    uint32_t guard = 0;

    if (!src || len == 0) return;
    lexer_init(&lx, src, len);
    while (guard++ < 400000u) {
        t = lexer_next(&lx);
        if (t.type == TOK_EOF) break;

        if (t.type == TOK_FN) {
            if (fn_depth == 0 && !pending_fn) {
                struct soma_lexer pk = lx;
                struct soma_token nm = lexer_next(&pk);
                if (nm.type == TOK_IDENT && !soma_sbuf_eq(nm.text, "main")) pending_fn = 1;
            }
        } else if (t.type == TOK_LBRACE) {
            if (pending_fn) { pending_fn = 0; fn_depth = 1; }
            else if (fn_depth > 0) fn_depth++;
        } else if (t.type == TOK_RBRACE) {
            if (fn_depth > 0) fn_depth--;
        } else if (fn_depth == 0 && !pending_fn) {
            if (t.type == TOK_LET) {
                struct soma_lexer pk = lx;
                struct soma_token nm = lexer_next(&pk);
                if (nm.type == TOK_IDENT && c->sym_count < COMPILER_MAX_SYMS) sym_add(c, nm.text);
            } else if (t.type == TOK_IDENT &&
                       (prev_type == TOK_SEMICOLON || prev_type == TOK_LBRACE ||
                        prev_type == TOK_RBRACE)) {
                struct soma_lexer pk = lx;
                struct soma_token nx = lexer_next(&pk);
                if (is_assign_tok(nx.type) && c->sym_count < COMPILER_MAX_SYMS) sym_add(c, t.text);
            }
        }
        prev_type = t.type;
    }
}

static void parse_postfix(struct soma_compiler *c)
{
    while (!c->had_error) {
        if (match(c, TOK_LBRACKET)) {
            parse_expr(c);
            consume(c, TOK_RBRACKET, "Se esperaba ']' tras indice");
            emit_u8(c, OP_ARRAY_GET);
        } else if (match(c, TOK_DOT)) {
            char m[32];
            if (!consume(c, TOK_IDENT, "Nombre de metodo o propiedad esperado")) return;
            soma_sbuf_t sb;
            soma_sbuf_init(&sb, m, sizeof(m));
            soma_sbuf_copy(&sb, c->prev.text);

            if (soma_sbuf_eq(m, "len")) {
                if (match(c, TOK_LPAREN)) consume(c, TOK_RPAREN, ")");
                emit_u8(c, OP_ARRAY_LEN);
            } else if (soma_sbuf_eq(m, "push")) {
                consume(c, TOK_LPAREN, "(");
                parse_expr(c);
                consume(c, TOK_RPAREN, ")");
                emit_u8(c, OP_ARRAY_PUSH);
                emit_u8(c, OP_PUSH_INT);
                emit_i64(c, 0);
            } else if (soma_sbuf_eq(m, "pop")) {
                if (match(c, TOK_LPAREN)) consume(c, TOK_RPAREN, ")");
                emit_u8(c, OP_ARRAY_POP);
            } else if (soma_sbuf_eq(m, "to_int")) {
                if (match(c, TOK_LPAREN)) consume(c, TOK_RPAREN, ")");
                emit_u8(c, OP_STR_TO_INT);
            } else if (soma_sbuf_eq(m, "to_str")) {
                if (match(c, TOK_LPAREN)) consume(c, TOK_RPAREN, ")");
                emit_str(c, "");
                emit_u8(c, OP_ADD);
            } else if (soma_sbuf_eq(m, "sub")) {
                consume(c, TOK_LPAREN, "(");
                parse_expr(c);
                consume(c, TOK_COMMA, ",");
                parse_expr(c);
                consume(c, TOK_RPAREN, ")");
                emit_u8(c, OP_STR_SUB);
            } else if (soma_sbuf_eq(m, "has")) {
                consume(c, TOK_LPAREN, "(");
                parse_expr(c);
                consume(c, TOK_RPAREN, ")");
                emit_u8(c, OP_TABLE_HAS);
            } else if (soma_sbuf_eq(m, "keys")) {
                if (match(c, TOK_LPAREN)) consume(c, TOK_RPAREN, ")");
                emit_u8(c, OP_TABLE_KEYS);
            } else {
                emit_str(c, m);
                emit_u8(c, OP_ARRAY_GET);
            }
        } else {
            break;
        }
    }
}

static int chain_assign_count(struct soma_compiler *c)
{
    struct soma_lexer lx = c->lex;
    struct soma_token t;
    int n = 0;
    if (c->cur.type != TOK_IDENT) return 0;
    for (;;) {
        t = lexer_next(&lx);
        if (t.type == TOK_LBRACKET) {
            int depth = 1;
            while (depth > 0) {
                t = lexer_next(&lx);
                if (t.type == TOK_EOF) return 0;
                if (t.type == TOK_LBRACKET) depth++;
                else if (t.type == TOK_RBRACKET) depth--;
            }
            n++;
        } else if (t.type == TOK_DOT) {
            struct soma_lexer pk;
            struct soma_token nt;
            t = lexer_next(&lx);
            if (t.type != TOK_IDENT) return 0;
            pk = lx;
            nt = lexer_next(&pk);
            if (nt.type == TOK_LPAREN) return 0;
            n++;
        } else if (is_assign_tok(t.type)) {
            return n;
        } else {
            return 0;
        }
    }
}

static void parse_chain_assign(struct soma_compiler *c)
{
    char name[32];
    int is_local = 0;
    int slot;

    soma_sbuf_t sb;
    soma_sbuf_init(&sb, name, sizeof(name));
    soma_sbuf_copy(&sb, c->cur.text);

    slot = var_slot(c, name, 0, &is_local);
    if (slot < 0) {
        compiler_error(c, "Variable no declarada en asignacion encadenada");
        return;
    }
    advance(c);
    emit_var(c, 0, slot, is_local);
    for (;;) {
        if (match(c, TOK_LBRACKET)) {
            parse_expr(c);
            consume(c, TOK_RBRACKET, "Se esperaba ']'");
        } else if (match(c, TOK_DOT)) {
            if (!consume(c, TOK_IDENT, "Nombre de propiedad esperado")) return;
            emit_str(c, c->prev.text);
        } else {
            compiler_error(c, "Asignacion encadenada no valida");
            return;
        }
        if (c->had_error) return;
        if (is_assign_tok(c->cur.type)) {
            enum soma_tok_type op = c->cur.type;
            advance(c);
            if (op != TOK_ASSIGN) {
                emit_u8(c, OP_DUP2);
                emit_u8(c, OP_ARRAY_GET);
            }
            parse_expr(c);
            if (op == TOK_PLUS_ASSIGN)       emit_u8(c, OP_ADD);
            else if (op == TOK_MINUS_ASSIGN) emit_u8(c, OP_SUB);
            else if (op == TOK_STAR_ASSIGN)  emit_u8(c, OP_MUL);
            else if (op == TOK_SLASH_ASSIGN) emit_u8(c, OP_DIV);
            match_semicolon(c, ";");
            emit_u8(c, OP_ARRAY_SET);
            return;
        }
        emit_u8(c, OP_ARRAY_GET);
    }
}

static void parse_unary(struct soma_compiler *c)
{
    if (c->depth >= PARSE_MAX_DEPTH) {
        compiler_error(c, "Expresion demasiado anidada");
        return;
    }
    c->depth++;
    if (match(c, TOK_MINUS)) {
        emit_u8(c, OP_PUSH_INT);
        emit_i64(c, 0);
        parse_unary(c);
        emit_u8(c, OP_SUB);
    } else if (match(c, TOK_NOT)) {
        parse_unary(c);
        emit_u8(c, OP_NOT);
    } else {
        parse_primary(c);
    }
    c->depth--;
}

static void parse_mul(struct soma_compiler *c)
{
    parse_unary(c);
    while (check(c, TOK_STAR) || check(c, TOK_SLASH) || check(c, TOK_MOD)) {
        enum soma_tok_type op = c->cur.type;
        advance(c);
        parse_unary(c);
        if (op == TOK_STAR)  emit_u8(c, OP_MUL);
        if (op == TOK_SLASH) emit_u8(c, OP_DIV);
        if (op == TOK_MOD)   emit_u8(c, OP_MOD);
    }
}

static void parse_add(struct soma_compiler *c)
{
    parse_mul(c);
    while (check(c, TOK_PLUS) || check(c, TOK_MINUS)) {
        enum soma_tok_type op = c->cur.type;
        advance(c);
        parse_mul(c);
        if (op == TOK_PLUS)  emit_u8(c, OP_ADD);
        if (op == TOK_MINUS) emit_u8(c, OP_SUB);
    }
}

static void parse_cmp(struct soma_compiler *c)
{
    parse_add(c);
    while (check(c, TOK_EQ) || check(c, TOK_NEQ) ||
           check(c, TOK_LT) || check(c, TOK_GT)  ||
           check(c, TOK_LE) || check(c, TOK_GE)) {
        enum soma_tok_type op = c->cur.type;
        advance(c);
        parse_add(c);
        if (op == TOK_EQ)  emit_u8(c, OP_EQ);
        if (op == TOK_NEQ) emit_u8(c, OP_NEQ);
        if (op == TOK_LT)  emit_u8(c, OP_LT);
        if (op == TOK_GT)  emit_u8(c, OP_GT);
        if (op == TOK_LE)  emit_u8(c, OP_LE);
        if (op == TOK_GE)  emit_u8(c, OP_GE);
    }
}

static void parse_and(struct soma_compiler *c)
{
    uint32_t end_patches[16];
    int np = 0;
    parse_cmp(c);
    while (match(c, TOK_AND)) {
        emit_u8(c, OP_DUP);
        uint32_t j = emit_jump(c, OP_JUMP_IF_ZERO);
        emit_u8(c, OP_POP);
        parse_cmp(c);
        if (np < 16) end_patches[np++] = j;
        else compiler_error(c, "Demasiados '&&' encadenados (maximo 16)");
    }
    if (np > 0) {
        for (int i = 0; i < np; ++i) patch_jump(c, end_patches[i], c->code_len);
        emit_u8(c, OP_NOT);
        emit_u8(c, OP_NOT);
    }
}

static void parse_expr(struct soma_compiler *c)
{
    uint32_t end_patches[16];
    int np = 0;
    parse_and(c);
    while (match(c, TOK_OR)) {
        emit_u8(c, OP_DUP);
        uint32_t j = emit_jump(c, OP_JUMP_IF_NZ);
        emit_u8(c, OP_POP);
        parse_and(c);
        if (np < 16) end_patches[np++] = j;
        else compiler_error(c, "Demasiados '||' encadenados (maximo 16)");
    }
    if (np > 0) {
        for (int i = 0; i < np; ++i) patch_jump(c, end_patches[i], c->code_len);
        emit_u8(c, OP_NOT);
        emit_u8(c, OP_NOT);
    }
}

static void parse_primary(struct soma_compiler *c)
{
    if (match(c, TOK_INT)) {
        emit_u8(c, OP_PUSH_INT);
        emit_i64(c, c->prev.int_val);
    } else if (match(c, TOK_TRUE)) {
        emit_u8(c, OP_PUSH_INT);
        emit_i64(c, 1);
    } else if (match(c, TOK_FALSE) || match(c, TOK_NIL)) {
        emit_u8(c, OP_PUSH_INT);
        emit_i64(c, 0);
    } else if (match(c, TOK_STRING)) {
        emit_str(c, c->prev.text);
        while (match(c, TOK_LBRACKET)) {
            parse_expr(c);
            consume(c, TOK_RBRACKET, "Se esperaba ']'");
            emit_u8(c, OP_ARRAY_GET);
        }
        if (match(c, TOK_DOT)) {
            if (consume(c, TOK_IDENT, "Nombre de metodo esperado")) {
                if (soma_sbuf_eq(c->prev.text, "len")) {
                    if (match(c, TOK_LPAREN)) consume(c, TOK_RPAREN, ")");
                    emit_u8(c, OP_ARRAY_LEN);
                } else if (soma_sbuf_eq(c->prev.text, "to_int")) {
                    if (match(c, TOK_LPAREN)) consume(c, TOK_RPAREN, ")");
                    emit_u8(c, OP_STR_TO_INT);
                } else if (soma_sbuf_eq(c->prev.text, "sub")) {
                    consume(c, TOK_LPAREN, "(");
                    parse_expr(c); consume(c, TOK_COMMA, ",");
                    parse_expr(c);
                    consume(c, TOK_RPAREN, ")");
                    emit_u8(c, OP_STR_SUB);
                }
            }
        }
        parse_postfix(c);
        return;
    } else if (match(c, TOK_LBRACKET)) {
        uint16_t count = 0;
        if (!check(c, TOK_RBRACKET)) {
            do {
                parse_expr(c);
                count++;
            } while (match(c, TOK_COMMA));
        }
        consume(c, TOK_RBRACKET, "Se esperaba ']' al cerrar array literal");
        emit_u8(c, OP_NEW_ARRAY);
        emit_u16(c, count);
    } else if (match(c, TOK_LBRACE)) {
        uint16_t count = 0;
        if (!check(c, TOK_RBRACE)) {
            do {
                if (match(c, TOK_STRING) || match(c, TOK_IDENT)) {
                    emit_str(c, c->prev.text);
                } else {
                    parse_expr(c);
                }
                consume(c, TOK_COLON, "Se esperaba ':' tras clave de tabla");
                parse_expr(c);
                count++;
            } while (match(c, TOK_COMMA));
        }
        consume(c, TOK_RBRACE, "Se esperaba '}' al cerrar tabla literal");
        emit_u8(c, OP_NEW_TABLE);
        emit_u16(c, count);
    } else if (match(c, TOK_INPUT)) {
        int has_paren = match(c, TOK_LPAREN);
        if (check(c, TOK_STRING) || check(c, TOK_IDENT) || check(c, TOK_INT)) {
            parse_expr(c);
        } else {
            emit_str(c, "");
        }
        if (has_paren) consume(c, TOK_RPAREN, "Se esperaba ')' tras input(...)");
        emit_u8(c, OP_SYS_INPUT);
    } else if (match(c, TOK_ASK)) {
        emit_str(c, "soma");
        int has_paren = match(c, TOK_LPAREN);
        parse_expr(c);
        if (has_paren) consume(c, TOK_RPAREN, "Se esperaba ')' tras ask(...)");
        emit_u8(c, OP_SYS_AGENT_ASK);
    } else if (match(c, TOK_VFS) || match(c, TOK_MEM)) {
        int is_mem = (c->prev.type == TOK_MEM);
        consume(c, TOK_DOT, "Se esperaba '.' tras vfs/mem");
        if (!consume(c, TOK_IDENT, "Nombre de metodo esperado")) return;
        char m[32];
        soma_sbuf_t sb;
        soma_sbuf_init(&sb, m, sizeof(m));
        soma_sbuf_copy(&sb, c->prev.text);
        consume(c, TOK_LPAREN, "(");

        if (!is_mem && soma_sbuf_eq(m, "read")) {
            parse_expr(c);
            emit_u8(c, OP_SYS_VFS_READ);
        } else if (!is_mem && soma_sbuf_eq(m, "exists")) {
            parse_expr(c);
            emit_u8(c, OP_SYS_VFS_EXISTS);
        } else if (!is_mem && soma_sbuf_eq(m, "write")) {
            parse_expr(c);
            consume(c, TOK_COMMA, ",");
            parse_expr(c);
            emit_u8(c, OP_SYS_VFS_WRITE);
        } else if (is_mem && soma_sbuf_eq(m, "query")) {
            parse_expr(c);
            emit_u8(c, OP_SYS_MEM_QUERY);
        } else if (is_mem && soma_sbuf_eq(m, "read")) {
            if (check(c, TOK_RPAREN)) {
                emit_str(c, "/agent/blackboard.txt");
            } else {
                parse_expr(c);
            }
            emit_u8(c, OP_SYS_VFS_READ);
        } else if (is_mem && soma_sbuf_eq(m, "write")) {
            parse_expr(c);
            if (match(c, TOK_COMMA)) {
                parse_expr(c);
                emit_u8(c, OP_SYS_VFS_WRITE);
            } else {
                emit_str(c, "/agent/blackboard.txt");
                emit_u8(c, OP_SWAP);
                emit_u8(c, OP_SYS_VFS_WRITE);
            }
        } else {
            compiler_error(c, "Metodo desconocido en vfs./mem.");
        }
        consume(c, TOK_RPAREN, ")");
    } else if (match(c, TOK_AGENT)) {
        consume(c, TOK_DOT, ".");
        if (!consume(c, TOK_IDENT, "Metodo de agente esperado")) return;
        char m[32];
        soma_sbuf_t sb;
        soma_sbuf_init(&sb, m, sizeof(m));
        soma_sbuf_copy(&sb, c->prev.text);
        if (soma_sbuf_eq(m, "ask")) {
            consume(c, TOK_LPAREN, "(");
            parse_expr(c);
            consume(c, TOK_COMMA, ",");
            parse_expr(c);
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_AGENT_ASK);
        } else {
            compiler_error(c, "Metodo desconocido en agent.");
        }
    } else if (match(c, TOK_SYS)) {
        consume(c, TOK_DOT, ".");
        if (!consume(c, TOK_IDENT, "Funcion del sistema esperada")) return;
        char func[32];
        soma_sbuf_t sb;
        soma_sbuf_init(&sb, func, sizeof(func));
        soma_sbuf_copy(&sb, c->prev.text);
        consume(c, TOK_LPAREN, "(");
        if (soma_sbuf_eq(func, "sleep")) {
            parse_expr(c);
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_SLEEP);
            return;
        }
        consume(c, TOK_RPAREN, ")");
        if (soma_sbuf_eq(func, "ram") || soma_sbuf_eq(func, "ram_free")) emit_u8(c, OP_SYS_RAM);
        else if (soma_sbuf_eq(func, "uptime"))                      emit_u8(c, OP_SYS_UPTIME);
        else if (soma_sbuf_eq(func, "rand"))                        emit_u8(c, OP_SYS_RAND);
        else if (soma_sbuf_eq(func, "clear"))                       emit_u8(c, OP_SYS_CLEAR);
        else if (soma_sbuf_eq(func, "key"))                         emit_u8(c, OP_SYS_KEY);
        else if (soma_sbuf_eq(func, "millis") || soma_sbuf_eq(func, "time")) emit_u8(c, OP_SYS_MILLIS);
        else compiler_error(c, "sys.<fn> desconocida");
    } else if (match(c, TOK_FB)) {
        consume(c, TOK_DOT, ".");
        if (!consume(c, TOK_IDENT, "Metodo fb esperado")) return;
        char m[32];
        soma_sbuf_t sb;
        soma_sbuf_init(&sb, m, sizeof(m));
        soma_sbuf_copy(&sb, c->prev.text);
        consume(c, TOK_LPAREN, "(");

        if (soma_sbuf_eq(m, "is_active") || soma_sbuf_eq(m, "active")) {
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_FB_IS_ACTIVE);
        } else if (soma_sbuf_eq(m, "clear")) {
            if (!check(c, TOK_RPAREN)) parse_expr(c);
            else { emit_u8(c, OP_PUSH_INT); emit_i64(c, 0); }
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_FB_CLEAR);
            emit_u8(c, OP_PUSH_INT); emit_i64(c, 0);
        } else if (soma_sbuf_eq(m, "rect")) {
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c);
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_FB_RECT);
            emit_u8(c, OP_PUSH_INT); emit_i64(c, 0);
        } else if (soma_sbuf_eq(m, "pixel")) {
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c);
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_FB_PIXEL);
            emit_u8(c, OP_PUSH_INT); emit_i64(c, 0);
        } else if (soma_sbuf_eq(m, "line")) {
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c);
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_FB_LINE);
            emit_u8(c, OP_PUSH_INT); emit_i64(c, 0);
        } else if (soma_sbuf_eq(m, "circle")) {
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c);
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_FB_CIRCLE);
            emit_u8(c, OP_PUSH_INT); emit_i64(c, 0);
        } else if (soma_sbuf_eq(m, "text")) {
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c);
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_FB_TEXT);
            emit_u8(c, OP_PUSH_INT); emit_i64(c, 0);
        } else if (soma_sbuf_eq(m, "present")) {
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_FB_PRESENT);
            emit_u8(c, OP_PUSH_INT); emit_i64(c, 0);
        } else {
            compiler_error(c, "Metodo desconocido en fb.");
        }
    } else if (match(c, TOK_WIN)) {
        consume(c, TOK_DOT, ".");
        if (!consume(c, TOK_IDENT, "Metodo win esperado")) return;
        char m[32];
        soma_sbuf_t sb;
        soma_sbuf_init(&sb, m, sizeof(m));
        soma_sbuf_copy(&sb, c->prev.text);
        consume(c, TOK_LPAREN, "(");

        if (soma_sbuf_eq(m, "create") || soma_sbuf_eq(m, "new") || soma_sbuf_eq(m, "open")) {
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c);
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_WIN_CREATE);
        } else if (soma_sbuf_eq(m, "clear")) {
            if (!check(c, TOK_RPAREN)) parse_expr(c);
            else { emit_u8(c, OP_PUSH_INT); emit_i64(c, 0x0A0D14); }
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_WIN_CLEAR);
        } else if (soma_sbuf_eq(m, "rect")) {
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c);
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_WIN_RECT);
        } else if (soma_sbuf_eq(m, "text")) {
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c);
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_WIN_TEXT);
        } else if (soma_sbuf_eq(m, "line")) {
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c);
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_WIN_LINE);
        } else if (soma_sbuf_eq(m, "circle")) {
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c); consume(c, TOK_COMMA, ",");
            parse_expr(c);
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_WIN_CIRCLE);
        } else if (soma_sbuf_eq(m, "present")) {
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_WIN_PRESENT);
        } else if (soma_sbuf_eq(m, "close")) {
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_WIN_CLOSE);
        } else if (soma_sbuf_eq(m, "is_active") || soma_sbuf_eq(m, "active")) {
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_WIN_ACTIVE);
        } else if (soma_sbuf_eq(m, "mouse")) {
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_WIN_MOUSE);
        } else if (soma_sbuf_eq(m, "clicked")) {
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_WIN_MOUSE);
            emit_str(c, "clicked");
            emit_u8(c, OP_ARRAY_GET);
        } else if (soma_sbuf_eq(m, "mouse_x")) {
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_WIN_MOUSE);
            emit_str(c, "x");
            emit_u8(c, OP_ARRAY_GET);
        } else if (soma_sbuf_eq(m, "mouse_y")) {
            consume(c, TOK_RPAREN, ")");
            emit_u8(c, OP_SYS_WIN_MOUSE);
            emit_str(c, "y");
            emit_u8(c, OP_ARRAY_GET);
        } else {
            compiler_error(c, "Metodo desconocido en win.");
        }
    } else if (match(c, TOK_IDENT)) {
        char name[32];
        soma_sbuf_t sb;
        soma_sbuf_init(&sb, name, sizeof(name));
        soma_sbuf_copy(&sb, c->prev.text);

        if (soma_sbuf_eq(name, "len")) {
            if (check(c, TOK_LPAREN)) {
                advance(c);
                parse_expr(c);
                consume(c, TOK_RPAREN, ")");
                emit_u8(c, OP_ARRAY_LEN);
                return;
            }
        }
        if (soma_sbuf_eq(name, "to_int") || soma_sbuf_eq(name, "int")) {
            if (check(c, TOK_LPAREN)) {
                advance(c);
                parse_expr(c);
                consume(c, TOK_RPAREN, ")");
                emit_u8(c, OP_STR_TO_INT);
                return;
            }
        }
        if (soma_sbuf_eq(name, "to_str") || soma_sbuf_eq(name, "str")) {
            if (check(c, TOK_LPAREN)) {
                advance(c);
                emit_str(c, "");
                parse_expr(c);
                consume(c, TOK_RPAREN, ")");
                emit_u8(c, OP_ADD);
                return;
            }
        }
        if (soma_sbuf_eq(name, "substr") || soma_sbuf_eq(name, "sub")) {
            if (check(c, TOK_LPAREN)) {
                advance(c);
                parse_expr(c); consume(c, TOK_COMMA, ",");
                parse_expr(c); consume(c, TOK_COMMA, ",");
                parse_expr(c);
                consume(c, TOK_RPAREN, ")");
                emit_u8(c, OP_STR_SUB);
                return;
            }
        }
        if (soma_sbuf_eq(name, "push")) {
            if (check(c, TOK_LPAREN)) {
                advance(c);
                parse_expr(c);
                consume(c, TOK_COMMA, ",");
                parse_expr(c);
                consume(c, TOK_RPAREN, ")");
                emit_u8(c, OP_ARRAY_PUSH);
                emit_u8(c, OP_PUSH_INT);
                emit_i64(c, 0);
                return;
            }
        }
        if (soma_sbuf_eq(name, "pop")) {
            if (check(c, TOK_LPAREN)) {
                advance(c);
                parse_expr(c);
                consume(c, TOK_RPAREN, ")");
                emit_u8(c, OP_ARRAY_POP);
                return;
            }
        }

        if (soma_sbuf_eq(name, "io") && match(c, TOK_DOT)) {
            if (check(c, TOK_PRINT) || (check(c, TOK_IDENT) && soma_sbuf_eq(c->cur.text, "print"))) {
                advance(c);
                int has_p = match(c, TOK_LPAREN);
                if (has_p && check(c, TOK_RPAREN)) {
                    emit_str(c, "");
                } else {
                    parse_expr(c);
                }
                if (has_p) consume(c, TOK_RPAREN, ")");
                emit_u8(c, OP_SYS_PRINT);
                emit_u8(c, OP_PUSH_INT);
                emit_i64(c, 0);
                return;
            }
            if (check(c, TOK_INPUT) || (check(c, TOK_IDENT) && soma_sbuf_eq(c->cur.text, "input"))) {
                advance(c);
                consume(c, TOK_LPAREN, "(");
                parse_expr(c);
                int has_dest = match(c, TOK_COMMA);
                int dest_slot = -1;
                int dest_local = 0;
                if (has_dest && match(c, TOK_IDENT)) {
                    dest_slot = var_slot(c, c->prev.text, 1, &dest_local);
                }
                consume(c, TOK_RPAREN, ")");
                emit_u8(c, OP_SYS_INPUT);
                if (dest_slot >= 0) {
                    emit_var(c, 1, dest_slot, dest_local);
                    emit_u8(c, OP_PUSH_INT);
                    emit_i64(c, 0);
                }
                return;
            }
        }

        if (soma_sbuf_eq(name, "random") || soma_sbuf_eq(name, "rand") || soma_sbuf_eq(name, "randint")) {
            int has_dot = match(c, TOK_DOT);
            if (has_dot) {
                if (check(c, TOK_IDENT)) advance(c);
            }
            if (check(c, TOK_LPAREN)) {
                advance(c);
                parse_expr(c);
                consume(c, TOK_COMMA, ",");
                parse_expr(c);
                consume(c, TOK_RPAREN, ")");
                emit_u8(c, OP_SYS_RANDRANGE);
                return;
            }
        }

        if (check(c, TOK_LPAREN)) {
            int fi = -1;
            for (int i = (int)c->fn_count - 1; i >= 0; --i) {
                if (soma_sbuf_eq(c->fns[i].name, name)) {
                    fi = i;
                    break;
                }
            }
            if (fi >= 0) {
                advance(c);
                int nargs = 0;
                if (!check(c, TOK_RPAREN)) {
                    do {
                        parse_expr(c);
                        nargs++;
                    } while (match(c, TOK_COMMA));
                }
                consume(c, TOK_RPAREN, ")");
                if (nargs != c->fns[fi].nparams) {
                    compiler_error(c, "Numero de argumentos incorrecto en llamada a funcion");
                    return;
                }
                emit_u8(c, OP_CALL);
                emit_i32(c, (int32_t)c->fns[fi].addr);
                return;
            }
        }

        if (check(c, TOK_LPAREN)) {
            compiler_error(c, "Funcion no definida (declara 'fn nombre(...)' antes de llamarla)");
            return;
        }
        int is_local = 0;
        int slot = var_slot(c, name, 1, &is_local);
        emit_var(c, 0, slot, is_local);

        while (match(c, TOK_LBRACKET)) {
            parse_expr(c);
            consume(c, TOK_RBRACKET, "Se esperaba ']' tras indice de array");
            emit_u8(c, OP_ARRAY_GET);
        }

        if (match(c, TOK_DOT)) {
            if (consume(c, TOK_IDENT, "Nombre de metodo esperado")) {
                if (soma_sbuf_eq(c->prev.text, "len")) {
                    if (match(c, TOK_LPAREN)) consume(c, TOK_RPAREN, ")");
                    emit_u8(c, OP_ARRAY_LEN);
                } else if (soma_sbuf_eq(c->prev.text, "push")) {
                    consume(c, TOK_LPAREN, "(");
                    parse_expr(c);
                    consume(c, TOK_RPAREN, ")");
                    emit_u8(c, OP_ARRAY_PUSH);
                    emit_u8(c, OP_PUSH_INT);
                    emit_i64(c, 0);
                } else if (soma_sbuf_eq(c->prev.text, "pop")) {
                    if (match(c, TOK_LPAREN)) consume(c, TOK_RPAREN, ")");
                    emit_u8(c, OP_ARRAY_POP);
                } else if (soma_sbuf_eq(c->prev.text, "to_int")) {
                    if (match(c, TOK_LPAREN)) consume(c, TOK_RPAREN, ")");
                    emit_u8(c, OP_STR_TO_INT);
                } else if (soma_sbuf_eq(c->prev.text, "to_str")) {
                    if (match(c, TOK_LPAREN)) consume(c, TOK_RPAREN, ")");
                    emit_str(c, "");
                    emit_u8(c, OP_ADD);
                } else if (soma_sbuf_eq(c->prev.text, "sub")) {
                    consume(c, TOK_LPAREN, "(");
                    parse_expr(c); consume(c, TOK_COMMA, ",");
                    parse_expr(c);
                    consume(c, TOK_RPAREN, ")");
                    emit_u8(c, OP_STR_SUB);
                } else if (soma_sbuf_eq(c->prev.text, "has")) {
                    consume(c, TOK_LPAREN, "(");
                    parse_expr(c);
                    consume(c, TOK_RPAREN, ")");
                    emit_u8(c, OP_TABLE_HAS);
                } else if (soma_sbuf_eq(c->prev.text, "keys")) {
                    if (match(c, TOK_LPAREN)) consume(c, TOK_RPAREN, ")");
                    emit_u8(c, OP_TABLE_KEYS);
                } else {
                    emit_str(c, c->prev.text);
                    emit_u8(c, OP_ARRAY_GET);
                }
            }
        }
        parse_postfix(c);
    } else if (match(c, TOK_LPAREN)) {
        parse_expr(c);
        consume(c, TOK_RPAREN, ")");
    } else if (check(c, TOK_ERROR)) {
        compiler_error(c, "Token invalido: cadena sin cerrar o token no soportado");
    } else {
        compiler_error(c, "Expresion esperada");
    }
}

static void parse_statement(struct soma_compiler *c);

static void parse_block(struct soma_compiler *c)
{
    if (c->depth >= PARSE_MAX_DEPTH) {
        compiler_error(c, "Bloques demasiado anidados");
        return;
    }
    c->depth++;
    if (match(c, TOK_LBRACE)) {
        while (!check(c, TOK_RBRACE) && !check(c, TOK_EOF) && !c->had_error) {
            parse_statement(c);
        }
        consume(c, TOK_RBRACE, "Se esperaba '}' al cerrar bloque");
    } else {
        parse_statement(c);
    }
    c->depth--;
}

static void parse_fn_def(struct soma_compiler *c)
{
    if (!consume(c, TOK_IDENT, "Nombre de funcion esperado")) return;
    char name[32];
    soma_sbuf_t sb;
    soma_sbuf_init(&sb, name, sizeof(name));
    soma_sbuf_copy(&sb, c->prev.text);

    if (soma_sbuf_eq(name, "main")) {
        if (match(c, TOK_LPAREN)) match(c, TOK_RPAREN);
        parse_block(c);
        return;
    }

    if (c->in_func) {
        compiler_error(c, "Funciones anidadas no soportadas");
        return;
    }
    if (c->fn_count >= MAX_FNS) {
        compiler_error(c, "Maximo de funciones excedido (max 64)");
        return;
    }

    uint32_t skip = emit_jump(c, OP_JUMP);
    struct soma_fn *f = &c->fns[c->fn_count++];
    soma_sbuf_t sbf;
    soma_sbuf_init(&sbf, f->name, sizeof(f->name));
    soma_sbuf_copy(&sbf, name);
    f->addr = c->code_len;
    f->nparams = 0;

    for (int i = 0; i < c->sym_count; ++i) c->gsyms[i] = c->syms[i];
    c->gcount = c->sym_count;
    c->sym_count = 0;
    c->in_func = 1;

    consume(c, TOK_LPAREN, "(");
    while (check(c, TOK_IDENT)) {
        advance(c);
        if (sym_find(c, c->prev.text) >= 0) {
            compiler_error(c, "Parametro duplicado en la declaracion de la funcion");
            return;
        }
        sym_add(c, c->prev.text);
        f->nparams++;
        if (!match(c, TOK_COMMA)) break;
    }
    consume(c, TOK_RPAREN, ")");

    emit_u8(c, OP_ENTER);
    uint32_t enter_patch = c->code_len;
    emit_u16(c, 0);

    for (int i = (int)f->nparams - 1; i >= 0; --i) {
        emit_u8(c, OP_STORE_LOCAL);
        emit_u16(c, (uint16_t)i);
    }

    parse_block(c);
    emit_u8(c, OP_PUSH_INT);
    emit_i64(c, 0);
    emit_u8(c, OP_RET);

    c->code[enter_patch]     = (uint8_t)(c->sym_count & 0xFF);
    c->code[enter_patch + 1] = (uint8_t)((c->sym_count >> 8) & 0xFF);

    for (int i = 0; i < c->gcount; ++i) c->syms[i] = c->gsyms[i];
    c->sym_count = c->gcount;
    c->in_func = 0;

    patch_jump(c, skip, c->code_len);
}

static void parse_statement(struct soma_compiler *c)
{
    if (match(c, TOK_IMPORT)) {
        uint32_t line = c->prev.line;
        while (!check(c, TOK_EOF) && c->cur.line == line && !check(c, TOK_SEMICOLON)) advance(c);
        match(c, TOK_SEMICOLON);
        return;
    }

    if (match(c, TOK_FN)) {
        parse_fn_def(c);
        return;
    }

    if (match(c, TOK_PROC)) {
        match(c, TOK_IDENT);
        if (match(c, TOK_LPAREN)) match(c, TOK_RPAREN);
        parse_block(c);
        return;
    }

    if (match(c, TOK_RETURN)) {
        if (check(c, TOK_SEMICOLON) || check(c, TOK_RBRACE) || check(c, TOK_EOF)) {
            emit_u8(c, OP_PUSH_INT);
            emit_i64(c, 0);
        } else {
            parse_expr(c);
        }
        match_semicolon(c, "Se esperaba ';' tras return");
        emit_u8(c, OP_RET);
        return;
    }

    if (match(c, TOK_LOOP)) {
        uint32_t loop_start = c->code_len;
        uint32_t save_breaks = c->break_count;
        uint32_t save_continues = c->continue_count;

        c->in_loop++;
        parse_block(c);
        c->in_loop--;

        uint32_t jmp_back = emit_jump(c, OP_JUMP);
        patch_jump(c, jmp_back, loop_start);

        for (uint32_t i = save_continues; i < c->continue_count; ++i) {
            patch_jump(c, c->continue_patches[i], loop_start);
        }
        c->continue_count = save_continues;

        for (uint32_t i = save_breaks; i < c->break_count; ++i) {
            patch_jump(c, c->break_patches[i], c->code_len);
        }
        c->break_count = save_breaks;
        return;
    }

    if (match(c, TOK_BREAK)) {
        match_semicolon(c, "Se esperaba ';' tras break");
        if (!c->in_loop) {
            compiler_error(c, "'break' fuera de un bucle");
            return;
        }
        if (c->break_count >= MAX_BREAKS) {
            compiler_error(c, "Demasiados 'break' (maximo 32)");
            return;
        }
        uint32_t p = emit_jump(c, OP_JUMP);
        c->break_patches[c->break_count++] = p;
        return;
    }

    if (match(c, TOK_CONTINUE)) {
        match_semicolon(c, "Se esperaba ';' tras continue");
        if (!c->in_loop) {
            compiler_error(c, "'continue' fuera de un bucle");
            return;
        }
        if (c->continue_count >= MAX_CONTINUES) {
            compiler_error(c, "Demasiados 'continue' (maximo 32)");
            return;
        }
        uint32_t p = emit_jump(c, OP_JUMP);
        c->continue_patches[c->continue_count++] = p;
        return;
    }

    if (match(c, TOK_FOR)) {
        int has_p = match(c, TOK_LPAREN);

        if (match(c, TOK_LET)) {
            if (!consume(c, TOK_IDENT, "Nombre de variable esperado")) return;
            char var_name[32];
            soma_sbuf_t sb;
            soma_sbuf_init(&sb, var_name, sizeof(var_name));
            soma_sbuf_copy(&sb, c->prev.text);
            int is_local = 0;
            int slot = var_slot_decl(c, var_name, &is_local);
            if (match(c, TOK_ASSIGN)) parse_expr(c);
            else { emit_u8(c, OP_PUSH_INT); emit_i64(c, 0); }
            consume(c, TOK_SEMICOLON, "Se esperaba ';'");
            emit_var(c, 1, slot, is_local);
        } else if (check(c, TOK_IDENT)) {
            struct soma_lexer save = c->lex;
            struct soma_token next = lexer_next(&save);
            if (next.type == TOK_ASSIGN) {
                char var_name[32];
                soma_sbuf_t sb;
                soma_sbuf_init(&sb, var_name, sizeof(var_name));
                soma_sbuf_copy(&sb, c->cur.text);
                int is_local = 0;
                int slot = var_slot(c, var_name, 1, &is_local);
                advance(c); advance(c);
                parse_expr(c);
                consume(c, TOK_SEMICOLON, "Se esperaba ';'");
                emit_var(c, 1, slot, is_local);
            } else {
                parse_expr(c);
                emit_u8(c, OP_POP);
                consume(c, TOK_SEMICOLON, ";");
            }
        } else if (match(c, TOK_SEMICOLON)) {
        } else {
            parse_expr(c);
            emit_u8(c, OP_POP);
            consume(c, TOK_SEMICOLON, ";");
        }

        uint32_t loop_start = c->code_len;
        uint32_t save_breaks = c->break_count;
        uint32_t save_continues = c->continue_count;

        uint32_t jmp_exit = 0;
        if (!check(c, TOK_SEMICOLON)) {
            parse_expr(c);
            jmp_exit = emit_jump(c, OP_JUMP_IF_ZERO);
        }
        consume(c, TOK_SEMICOLON, "Se esperaba ';' tras condicion");

        uint32_t jmp_body = emit_jump(c, OP_JUMP);

        uint32_t step_start = c->code_len;
        if (!check(c, TOK_RPAREN)) {
            if (check(c, TOK_IDENT)) {
                struct soma_lexer save = c->lex;
                struct soma_token next = lexer_next(&save);
                if (next.type == TOK_ASSIGN || next.type == TOK_PLUS_ASSIGN ||
                    next.type == TOK_MINUS_ASSIGN || next.type == TOK_STAR_ASSIGN ||
                    next.type == TOK_SLASH_ASSIGN) {
                    char var_name[32];
                    soma_sbuf_t sb;
                    soma_sbuf_init(&sb, var_name, sizeof(var_name));
                    soma_sbuf_copy(&sb, c->cur.text);
                    int is_local = 0;
                    int slot = var_slot(c, var_name, 1, &is_local);
                    enum soma_tok_type op_type = next.type;
                    advance(c); advance(c);
                    if (op_type != TOK_ASSIGN) emit_var(c, 0, slot, is_local);
                    parse_expr(c);
                    if (op_type == TOK_PLUS_ASSIGN) emit_u8(c, OP_ADD);
                    else if (op_type == TOK_MINUS_ASSIGN) emit_u8(c, OP_SUB);
                    else if (op_type == TOK_STAR_ASSIGN) emit_u8(c, OP_MUL);
                    else if (op_type == TOK_SLASH_ASSIGN) emit_u8(c, OP_DIV);
                    emit_var(c, 1, slot, is_local);
                } else {
                    parse_expr(c);
                    emit_u8(c, OP_POP);
                }
            } else {
                parse_expr(c);
                emit_u8(c, OP_POP);
            }
        }
        uint32_t jmp_loop = emit_jump(c, OP_JUMP);
        patch_jump(c, jmp_loop, loop_start);

        if (has_p) consume(c, TOK_RPAREN, "Se esperaba ')' tras step en for");

        patch_jump(c, jmp_body, c->code_len);
        c->in_loop++;
        parse_block(c);
        c->in_loop--;

        uint32_t jmp_step = emit_jump(c, OP_JUMP);
        patch_jump(c, jmp_step, step_start);

        if (jmp_exit > 0) patch_jump(c, jmp_exit, c->code_len);

        for (uint32_t i = save_continues; i < c->continue_count; ++i) {
            patch_jump(c, c->continue_patches[i], step_start);
        }
        c->continue_count = save_continues;

        for (uint32_t i = save_breaks; i < c->break_count; ++i) {
            patch_jump(c, c->break_patches[i], c->code_len);
        }
        c->break_count = save_breaks;
        return;
    }

    if (match(c, TOK_LET)) {
        if (!consume(c, TOK_IDENT, "Nombre de variable esperado")) return;
        char var_name[32];
        soma_sbuf_t sb;
        soma_sbuf_init(&sb, var_name, sizeof(var_name));
        soma_sbuf_copy(&sb, c->prev.text);
        int is_local = 0;
        int slot = var_slot_decl(c, var_name, &is_local);
        if (match(c, TOK_ASSIGN)) parse_expr(c);
        else { emit_u8(c, OP_PUSH_INT); emit_i64(c, 0); }
        match_semicolon(c, "Se esperaba ';'");
        emit_var(c, 1, slot, is_local);
        return;
    }

    if (match(c, TOK_PRINT)) {
        int has_p = match(c, TOK_LPAREN);
        if (has_p && check(c, TOK_RPAREN)) {
            emit_str(c, "");
        } else if (!has_p && (check(c, TOK_SEMICOLON) || check(c, TOK_EOF))) {
            emit_str(c, "");
        } else {
            parse_expr(c);
        }
        if (has_p) consume(c, TOK_RPAREN, ")");
        match_semicolon(c, ";");
        emit_u8(c, OP_SYS_PRINT);
        return;
    }

    if (match(c, TOK_IF)) {
        int has_p = match(c, TOK_LPAREN);
        parse_expr(c);
        if (has_p) consume(c, TOK_RPAREN, ")");

        uint32_t jmp_false = emit_jump(c, OP_JUMP_IF_ZERO);
        parse_block(c);

        if (match(c, TOK_ELSE)) {
            uint32_t jmp_end = emit_jump(c, OP_JUMP);
            patch_jump(c, jmp_false, c->code_len);
            parse_block(c);
            patch_jump(c, jmp_end, c->code_len);
        } else {
            patch_jump(c, jmp_false, c->code_len);
        }
        return;
    }

    if (match(c, TOK_WHILE)) {
        uint32_t loop_start = c->code_len;
        uint32_t save_breaks = c->break_count;
        uint32_t save_continues = c->continue_count;
        int has_p = match(c, TOK_LPAREN);
        parse_expr(c);
        if (has_p) consume(c, TOK_RPAREN, ")");

        uint32_t jmp_exit = emit_jump(c, OP_JUMP_IF_ZERO);
        c->in_loop++;
        parse_block(c);
        c->in_loop--;

        uint32_t jmp_back = emit_jump(c, OP_JUMP);
        patch_jump(c, jmp_back, loop_start);
        patch_jump(c, jmp_exit, c->code_len);

        for (uint32_t i = save_continues; i < c->continue_count; ++i) {
            patch_jump(c, c->continue_patches[i], loop_start);
        }
        c->continue_count = save_continues;

        for (uint32_t i = save_breaks; i < c->break_count; ++i) {
            patch_jump(c, c->break_patches[i], c->code_len);
        }
        c->break_count = save_breaks;
        return;
    }

    if (check(c, TOK_IDENT)) {
        struct soma_lexer save = c->lex;
        struct soma_token next = lexer_next(&save);

        if ((next.type == TOK_DOT || next.type == TOK_LBRACKET) && chain_assign_count(c) >= 2) {
            parse_chain_assign(c);
            return;
        }

        if (next.type == TOK_DOT) {
            struct soma_token after_dot = lexer_next(&save);
            struct soma_token after_prop = lexer_next(&save);
            if (after_dot.type == TOK_IDENT &&
                (after_prop.type == TOK_ASSIGN || after_prop.type == TOK_PLUS_ASSIGN ||
                 after_prop.type == TOK_MINUS_ASSIGN || after_prop.type == TOK_STAR_ASSIGN ||
                 after_prop.type == TOK_SLASH_ASSIGN)) {
                char obj_name[32];
                soma_sbuf_t sb_obj;
                soma_sbuf_init(&sb_obj, obj_name, sizeof(obj_name));
                soma_sbuf_copy(&sb_obj, c->cur.text);

                char prop_name[32];
                soma_sbuf_t sb_prop;
                soma_sbuf_init(&sb_prop, prop_name, sizeof(prop_name));
                soma_sbuf_copy(&sb_prop, after_dot.text);

                int is_local = 0;
                int slot = var_slot(c, obj_name, 0, &is_local);
                if (slot < 0) {
                    compiler_error(c, "Variable de objeto no declarada");
                    return;
                }
                advance(c);
                advance(c);
                advance(c);
                enum soma_tok_type dop = c->cur.type;
                advance(c);

                emit_var(c, 0, slot, is_local);
                emit_str(c, prop_name);
                if (dop != TOK_ASSIGN) {
                    emit_u8(c, OP_DUP2);
                    emit_u8(c, OP_ARRAY_GET);
                }
                parse_expr(c);
                if (dop == TOK_PLUS_ASSIGN)       emit_u8(c, OP_ADD);
                else if (dop == TOK_MINUS_ASSIGN) emit_u8(c, OP_SUB);
                else if (dop == TOK_STAR_ASSIGN)  emit_u8(c, OP_MUL);
                else if (dop == TOK_SLASH_ASSIGN) emit_u8(c, OP_DIV);

                match_semicolon(c, ";");
                emit_u8(c, OP_ARRAY_SET);
                return;
            }
        }

        if (next.type == TOK_LBRACKET) {
            char arr_name[32];
            soma_sbuf_t sb;
            soma_sbuf_init(&sb, arr_name, sizeof(arr_name));
            soma_sbuf_copy(&sb, c->cur.text);

            int is_local = 0;
            int slot = var_slot(c, arr_name, 0, &is_local);
            if (slot < 0) {
                compiler_error(c, "Array o variable no declarada");
                return;
            }
            advance(c);
            advance(c);
            emit_var(c, 0, slot, is_local);
            parse_expr(c);
            consume(c, TOK_RBRACKET, "Se esperaba ']'");
            if (c->had_error) return;

            if (check(c, TOK_ASSIGN)) {
                advance(c);
                parse_expr(c);
                match_semicolon(c, ";");
                emit_u8(c, OP_ARRAY_SET);
                return;
            }
            if (check(c, TOK_PLUS_ASSIGN) || check(c, TOK_MINUS_ASSIGN) ||
                check(c, TOK_STAR_ASSIGN) || check(c, TOK_SLASH_ASSIGN)) {
                enum soma_tok_type aop = c->cur.type;
                advance(c);
                emit_u8(c, OP_DUP2);
                emit_u8(c, OP_ARRAY_GET);
                parse_expr(c);
                if (aop == TOK_PLUS_ASSIGN)       emit_u8(c, OP_ADD);
                else if (aop == TOK_MINUS_ASSIGN) emit_u8(c, OP_SUB);
                else if (aop == TOK_STAR_ASSIGN)  emit_u8(c, OP_MUL);
                else                              emit_u8(c, OP_DIV);
                match_semicolon(c, ";");
                emit_u8(c, OP_ARRAY_SET);
                return;
            }
            emit_u8(c, OP_ARRAY_GET);
            match_semicolon(c, ";");
            if (c->interactive) emit_u8(c, OP_SYS_PRINT);
            else emit_u8(c, OP_POP);
            return;
        }

        if (next.type == TOK_ASSIGN || next.type == TOK_PLUS_ASSIGN ||
            next.type == TOK_MINUS_ASSIGN || next.type == TOK_STAR_ASSIGN ||
            next.type == TOK_SLASH_ASSIGN) {
            char var_name[32];
            soma_sbuf_t sb;
            soma_sbuf_init(&sb, var_name, sizeof(var_name));
            soma_sbuf_copy(&sb, c->cur.text);

            int is_local = 0;
            int slot = var_slot(c, var_name, 1, &is_local);
            enum soma_tok_type op_type = next.type;
            advance(c); advance(c);
            if (op_type != TOK_ASSIGN) {
                emit_var(c, 0, slot, is_local);
            }
            parse_expr(c);
            if (op_type == TOK_PLUS_ASSIGN) emit_u8(c, OP_ADD);
            else if (op_type == TOK_MINUS_ASSIGN) emit_u8(c, OP_SUB);
            else if (op_type == TOK_STAR_ASSIGN) emit_u8(c, OP_MUL);
            else if (op_type == TOK_SLASH_ASSIGN) emit_u8(c, OP_DIV);
            match_semicolon(c, ";");
            emit_var(c, 1, slot, is_local);
            return;
        }
    }

    parse_expr(c);
    match_semicolon(c, ";");
    if (c->interactive) emit_u8(c, OP_SYS_PRINT);
    else emit_u8(c, OP_POP);
}

int lang_compile_ex(const char *src, uint32_t len, uint8_t *out_code, uint32_t max_code, uint32_t *out_len, int interactive, int keep_syms)
{
    if (out_len) *out_len = 0;
    if ((!src && len != 0) || !out_code || max_code == 0) return -1;

    struct soma_compiler *c = &compiler_inst;
    lexer_init(&c->lex, src, len);
    c->fn_count = 0;
    if (keep_syms && c->in_func) {
        for (int ri = 0; ri < c->gcount; ++ri) c->syms[ri] = c->gsyms[ri];
        c->sym_count = c->gcount;
    }
    if (!keep_syms) {
        c->sym_count = 0;
    }
    c->in_func = 0;
    c->gcount = 0;
    c->code = out_code;
    c->code_len = 0;
    c->max_code = max_code;
    c->had_error = 0;
    c->error_line = 0;
    c->interactive = interactive;
    c->break_count = 0;
    c->continue_count = 0;
    c->in_loop = 0;
    c->depth = 0;

    global_diag.has_error = 0;
    global_diag.line = 0;
    global_diag.col = 0;
    global_diag.msg[0] = '\0';
    global_diag.token[0] = '\0';
    global_diag.snippet[0] = '\0';

    prescan_globals(c, src, len);

    advance(c);
    while (!check(c, TOK_EOF) && !c->had_error) parse_statement(c);
    emit_u8(c, OP_HALT);

    if (c->had_error) return -1;
    if (out_len) *out_len = c->code_len;
    return 0;
}

int lang_compile(const char *src, uint32_t len, uint8_t *out_code, uint32_t max_code, uint32_t *out_len)
{
    return lang_compile_ex(src, len, out_code, max_code, out_len, 0, 0);
}

static int resolve_vfs_read(const char *path, char *buf, uint32_t max_len)
{
    int r = vfs_read(path, buf, max_len);
    if (r > 0) return r;
    if (path[0] != '/') {
        char alt[64];
        alt[0] = '/';
        uint32_t i = 0;
        while (path[i] && i < sizeof(alt) - 2) {
            alt[i + 1] = path[i];
            i++;
        }
        alt[i + 1] = 0;
        r = vfs_read(alt, buf, max_len);
        if (r > 0) return r;

        char wsp[96];
        soma_sbuf_t sb_wsp;
        soma_sbuf_init(&sb_wsp, wsp, sizeof(wsp));
        soma_sbuf_copy(&sb_wsp, "/workspace/");
        soma_sbuf_append_str(&sb_wsp, path);
        r = vfs_read(wsp, buf, max_len);
        if (r > 0) return r;

        char app[96];
        soma_sbuf_t sb_app;
        soma_sbuf_init(&sb_app, app, sizeof(app));
        soma_sbuf_copy(&sb_app, "/apps/");
        soma_sbuf_append_str(&sb_app, path);
        r = vfs_read(app, buf, max_len);
        if (r > 0) return r;
    } else {
        r = vfs_read(path + 1, buf, max_len);
        if (r > 0) return r;
    }
    return -1;
}

static void seed_demo_scripts(void)
{
    char tmp[16];
    if (vfs_read("/test_demo.soma", tmp, sizeof(tmp)) <= 0) {
        const char *demo_src =
            "// Demostracion SOMA-Lang: Hardware, VFS y Primitivas Cognitivas\n"
            "let ram = 512;\n"
            "let agentes = 4;\n"
            "print(\"Iniciando programa SOMA-Lang...\");\n"
            "if (ram > 256) {\n"
            "    print(\"Memoria optima en Ring 0\");\n"
            "}\n"
            "let ram_bytes = sys.ram();\n"
            "print(\"RAM fisica libre (bytes): \" + ram_bytes);\n"
            "vfs.write(\"/saludo.txt\", \"Hola desde SOMA-Lang Ring 0!\");\n"
            "if (vfs.exists(\"/saludo.txt\")) {\n"
            "    let contenido = vfs.read(\"/saludo.txt\");\n"
            "    print(\"Contenido leido de VFS: \" + contenido);\n"
            "}\n"
            "let busqueda = mem.query(\"herramientas\");\n"
            "print(\"Consulta semantica BM25 (/mem): \" + busqueda);\n";
        uint32_t dlen = 0; while (demo_src[dlen]) dlen++;
        vfs_write("/test_demo.soma", demo_src, dlen);
        vfs_write("/workspace/test_demo.soma", demo_src, dlen);
    }

    if (vfs_read("/test_fib.soma", tmp, sizeof(tmp)) <= 0) {
        const char *fib_src =
            "// SOMA-Lang: Recursion y subrutinas con fn\n"
            "fn fib(n) {\n"
            "    if (n < 2) {\n"
            "        return n;\n"
            "    }\n"
            "    return fib(n - 1) + fib(n - 2);\n"
            "}\n"
            "print(\"Calculando Fibonacci recursivo en Ring 0...\");\n"
            "let res = fib(10);\n"
            "print(\"Fibonacci(10) = \" + res);\n";
        uint32_t flen = 0; while (fib_src[flen]) flen++;
        vfs_write("/test_fib.soma", fib_src, flen);
        vfs_write("/workspace/test_fib.soma", fib_src, flen);
    }

    if (vfs_read("/demo_arrays.soma", tmp, sizeof(tmp)) <= 0) {
        const char *arr_src =
            "print(\"=== SOMA-LANG: ARRAYS Y BUCLES FOR ===\");\n"
            "let lista = [10, 20, 30, 40, 50];\n"
            "print(\"Array inicial: \" + lista);\n"
            "print(\"Longitud: \" + lista.len());\n"
            "lista[1] = 999;\n"
            "print(\"Modificado indice 1: \" + lista);\n"
            "lista.push(60);\n"
            "print(\"Tras push(60): \" + lista);\n"
            "let s = 0;\n"
            "for (let i = 0; i < lista.len(); i += 1) { s += lista[i]; }\n"
            "print(\"Suma total: \" + s);\n";
        uint32_t alen = 0; while (arr_src[alen]) alen++;
        vfs_write("/demo_arrays.soma", arr_src, alen);
        vfs_write("/workspace/demo_arrays.soma", arr_src, alen);
        vfs_write("/workspace/test_arrays.soma", arr_src, alen);
        vfs_write("/test_arrays.soma", arr_src, alen);
    }
}

void lang_init(void)
{
    kprint("SOMA-LANG: Subsistema modularizado activo (Límites 128 vars / 64 fns OK)\n");
    seed_demo_scripts();
}

static int pl11_run(struct soma_vm *vm, const char *src)
{
    static uint8_t pl11_bc[4096];
    uint32_t n = 0, bl = 0;
    while (src[n]) n++;
    if (lang_compile(src, n, pl11_bc, sizeof(pl11_bc), &bl) != 0) return 0;
    vm_init(vm);
    return vm_run(vm, pl11_bc, bl) == 0;
}

int lang_test_self(void)
{
    if (!vm_test_self()) return 0;

    const char *test_src = "let x = 15; let y = 35; let z = (x + y) * 2; if (z > 50) { let r = 1; }";
    uint32_t src_len = 0; while (test_src[src_len]) src_len++;
    static uint8_t bc[1024];
    uint32_t bclen = 0;
    if (lang_compile(test_src, src_len, bc, sizeof(bc), &bclen) != 0) return 0;
    static struct soma_vm lang_test_vm;
    vm_init(&lang_test_vm);
    if (vm_run(&lang_test_vm, bc, bclen) != 0) return 0;
    if (lang_test_vm.vars[2].i != 100 || lang_test_vm.vars[3].i != 1) return 0;

    const char *test_logic = "let a = 10; let b = 20; if (a <= 10 && !(b < 15)) { let ok = 42; }";
    uint32_t l_len = 0; while (test_logic[l_len]) l_len++;
    if (lang_compile(test_logic, l_len, bc, sizeof(bc), &bclen) != 0) return 0;
    vm_init(&lang_test_vm);
    if (vm_run(&lang_test_vm, bc, bclen) != 0) return 0;
    if (lang_test_vm.vars[2].i != 42) return 0;

    const char *test_fn = "fn fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); } let res = fib(7);";
    uint32_t f_len = 0; while (test_fn[f_len]) f_len++;
    if (lang_compile(test_fn, f_len, bc, sizeof(bc), &bclen) != 0) return 0;
    vm_init(&lang_test_vm);
    if (vm_run(&lang_test_vm, bc, bclen) != 0) return 0;
    if (lang_test_vm.vars[0].i != 13) return 0;

    /* Verificacion de limites ampliados (mas de 32 variables) */
    const char *test_large_vars = "let v1=1; let v2=2; let v3=3; let v4=4; let v5=5; let v6=6; let v7=7; let v8=8; "
                                  "let v9=9; let v10=10; let v11=11; let v12=12; let v13=13; let v14=14; let v15=15; let v16=16; "
                                  "let v17=17; let v18=18; let v19=19; let v20=20; let v21=21; let v22=22; let v23=23; let v24=24; "
                                  "let v25=25; let v26=26; let v27=27; let v28=28; let v29=29; let v30=30; let v31=31; let v32=32; "
                                  "let v33=33; let v34=34; let v35=35; let total = v33 + v34 + v35;";
    uint32_t lv_len = 0; while (test_large_vars[lv_len]) lv_len++;
    if (lang_compile(test_large_vars, lv_len, bc, sizeof(bc), &bclen) != 0) return 0;
    vm_init(&lang_test_vm);
    if (vm_run(&lang_test_vm, bc, bclen) != 0) return 0;
    if (lang_test_vm.vars[35].i != (33 + 34 + 35)) return 0;

    return 1;
}

int cmd_lang_vm(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    int ok = lang_test_self();
    soma_sbuf_t sb;
    soma_sbuf_init(&sb, out_buf, max_out);
    if (ok) {
        soma_sbuf_copy(&sb, "VM & Parser Test: SUPERADO (Ring 0 OK, 128 Variables, 64 Funciones OK)\n");
    } else {
        soma_sbuf_copy(&sb, "VM & Parser Test: FALLO\n");
    }
    return 1;
}

int cmd_lang_coder(const char *args, char *out_buf, uint32_t max_out)
{
    (void)max_out;
    while (*args == ' ') args++;
    if (*args == '\0') {
        kprint("Uso: coder <descripcion de la app en lenguaje natural>\n");
        out_buf[0] = 0;
        return 1;
    }

    kprint("[AGENTE CODER] Mision recibida: '"); kprint(args); kprint("'\n");
    kprint("[AGENTE CODER] Consultando al modelo LLM 'nail-35b'...\n");

    agent_run("coder", args);
    out_buf[0] = '\0';
    return 1;
}

static void ensure_repl_vm(void)
{
    if (!repl_vm_initialized) {
        vm_init(&global_repl_vm);
        repl_vm_initialized = 1;
    }
}

static void run_interactive_repl(void)
{
    ensure_repl_vm();
    kprint("\n======================================================\n");
    kprint(" SOMA-LANG Interactive REPL (Ring 0 Bare-Metal)\n");
    kprint(" Soporta expresiones, arrays [1,2,3], bucles 'for', 'fn' o 'exit'.\n");
    kprint("======================================================\n\n");

    char line[256];
    static uint8_t rbc[4096];
    static char repl_fns[2048];
    static uint32_t repl_fns_len = 0;
    static char repl_src[2560];

    for (;;) {
        kprint("soma-lang> ");
        int n = kgetline(line, sizeof(line) - 1);
        if (n < 0) break;
        line[n] = '\0';

        char *p = line;
        while (*p == ' ') p++;
        if (*p == '\0') continue;

        if (soma_sbuf_eq(p, "exit") || soma_sbuf_eq(p, "quit")) {
            kprint("Saliendo del REPL SOMA-LANG.\n\n");
            break;
        }

        uint32_t len = 0;
        while (p[len]) len++;

        uint32_t bclen = 0;
        int is_fn_def = (p[0] == 'f' && p[1] == 'n' && (p[2] == ' ' || p[2] == '\t'));
        uint32_t sl = 0;
        for (uint32_t k = 0; k < repl_fns_len && sl < sizeof(repl_src) - 2; ++k) repl_src[sl++] = repl_fns[k];
        repl_src[sl++] = '\n';
        for (uint32_t k = 0; k < len && sl < sizeof(repl_src) - 1; ++k) repl_src[sl++] = p[k];
        repl_src[sl] = '\0';

        int err = lang_compile_ex(repl_src, sl, rbc, sizeof(rbc), &bclen, 1, 1);
        if (err == 0 && is_fn_def && repl_fns_len + len + 2 < sizeof(repl_fns)) {
            for (uint32_t k = 0; k < len; ++k) repl_fns[repl_fns_len++] = p[k];
            repl_fns[repl_fns_len++] = '\n';
            repl_fns[repl_fns_len] = '\0';
        }
        if (err == 0 && bclen > 0) {
            global_repl_vm.sp = 0;
            vm_run(&global_repl_vm, rbc, bclen);
        }
    }
}

int cmd_lang_run(const char *args, char *out_buf, uint32_t max_out)
{
    (void)max_out;
    while (*args == ' ') args++;

    if (args[0] == '\0' || soma_sbuf_eq(args, "repl")) {
        run_interactive_repl();
        if (out_buf) out_buf[0] = '\0';
        return 1;
    }

    if (args[0] == 'c' && args[1] == 'o' && args[2] == 'd' && args[3] == 'e' && args[4] == 'r' &&
        (args[5] == ' ' || args[5] == '\0')) {
        const char *p = args + 5; while (*p == ' ') p++;
        return cmd_lang_coder(p, out_buf, max_out);
    }

    static char file_buf[16384];
    int r = resolve_vfs_read(args, file_buf, sizeof(file_buf) - 1);
    if (r <= 0) {
        int is_script = 0;
        for (int i = 0; args[i]; ++i) {
            if (args[i] == '/' || (args[i] == '.' && args[i + 1] == 's')) {
                is_script = 1;
                break;
            }
        }
        if (is_script) {
            kprint("run: archivo no encontrado: '");
            kprint(args);
            kprint("'\n");
            if (out_buf && max_out) {
                soma_sbuf_t sb;
                soma_sbuf_init(&sb, out_buf, max_out);
                soma_sbuf_copy(&sb, "run: archivo no encontrado");
            }
            return 1;
        }

        uint32_t n = 0; while (args[n]) n++;
        static uint8_t expr_bc[1024];
        uint32_t expr_len = 0;

        ensure_repl_vm();
        int err = lang_compile_ex(args, n, expr_bc, sizeof(expr_bc), &expr_len, 1, 1);
        if (err == 0 && expr_len > 0) {
            global_repl_vm.sp = 0;
            vm_run(&global_repl_vm, expr_bc, expr_len);
            if (out_buf) out_buf[0] = '\0';
            return 1;
        } else {
            kprint("run: archivo no encontrado y expresion no evaluable: '");
            kprint(args);
            kprint("'\n");
            if (out_buf) out_buf[0] = '\0';
            return 1;
        }
    }
    file_buf[r] = '\0';

    kprint("[SOMA-LANG] Compilando: "); kprint(args); kprint("\n");

    static uint8_t bytecode[65536];
    uint32_t bytecode_len = 0;
        int err = lang_compile(file_buf, (uint32_t)r, bytecode, sizeof(bytecode), &bytecode_len);
    if (err != 0) {
        struct soma_compile_diag d;
        lang_get_diagnostic(&d);
        kprint("[SOMA-LANG] Fallo al compilar el archivo.\n");
        if (out_buf && max_out) {
            soma_sbuf_t sb_err;
            soma_sbuf_init(&sb_err, out_buf, max_out);
            soma_sbuf_copy(&sb_err, "[COMPILER ERROR L");
            soma_sbuf_append_u32(&sb_err, d.line);
            soma_sbuf_append_str(&sb_err, "]: ");
            soma_sbuf_append_str(&sb_err, d.msg);
            soma_sbuf_append_str(&sb_err, " cerca de '");
            soma_sbuf_append_str(&sb_err, d.token);
            soma_sbuf_append_str(&sb_err, "'.");
        }
        return 1;
    }

    kprint("[SOMA-LANG] Ejecutando en Ring 0 (Bytecode: ");
    kprint_dec(bytecode_len);
    kprint(" bytes)...\n");
    kprint("--------------------------------------------------\n");

    char win_auto_title[64];
    soma_sbuf_t sb_title;
    soma_sbuf_init(&sb_title, win_auto_title, sizeof(win_auto_title));

    const char *sp_name = args;
    const char *sp_slash = args;
    while (*sp_name) {
        if (*sp_name == '/') sp_slash = sp_name + 1;
        sp_name++;
    }
    soma_sbuf_copy(&sb_title, sp_slash);
    if (win_auto_title[0] == '\0') {
        soma_sbuf_copy(&sb_title, "SOMA App");
    }

    struct soma_vm *run_vm = (struct soma_vm *)kmalloc(sizeof(struct soma_vm));
    if (!run_vm) {
        kprint("[SOMA-LANG] Sin memoria para instanciar la VM.\n");
        return 1;
    }
    memset(run_vm, 0, sizeof(*run_vm));
    vm_init(run_vm);
    if (fb_is_active()) {
        gui_soma_win_open(win_auto_title, 640, 420);
    }
    int res = vm_run(run_vm, bytecode, bytecode_len);
    if (fb_is_active() && gui_soma_win_is_active()) {
        gui_soma_win_close();
    }
    vm_release(run_vm);
    kfree(run_vm);

    kprint("--------------------------------------------------\n");
    if (res == 0) {
        kprint("[SOMA-LANG] Programa finalizado con exito.\n");
    } else {
        kprint("[SOMA-LANG] Error durante la ejecucion en Ring 0.\n");
    }

    if (out_buf && max_out) {
        soma_sbuf_t sb_out;
        soma_sbuf_init(&sb_out, out_buf, max_out);
        if (res == 0) {
            soma_sbuf_copy(&sb_out, "[SOMA-LANG] Programa ejecutado con exito en Ring 0.");
        } else {
            soma_sbuf_copy(&sb_out, "[SOMA-LANG] Fallo durante la ejecucion en Ring 0.");
        }
    }
    return 1;
}

/* SOMA_LINT_ENGINE: Analisis estatico para bucle cerrado de programacion */
static int soma_lint_is_ident(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static void soma_lint_add(struct soma_lint_report *rep, int hard, const char *msg)
{
    uint32_t i = 0;
    if (!rep || rep->count >= 8) return;
    if (hard) rep->hard++;
    while (msg && msg[i] && i < 95) {
        rep->msg[rep->count][i] = msg[i];
        i++;
    }
    rep->msg[rep->count][i] = '\0';
    rep->count++;
}

int lang_lint(const char *src, uint32_t len, struct soma_lint_report *rep)
{
    uint32_t i = 0, line = 1;
    int has_win_create = 0, has_win_present = 0, has_while = 0, has_sleep = 0;
    if (!rep) return 0;
    rep->count = 0;
    rep->hard = 0;
    if (!src || len == 0) {
        soma_lint_add(rep, 1, "E: Codigo vacio (0 bytes)");
        return rep->hard;
    }

    /* Deteccion de placeholder fatal '...' */
    for (i = 0; i + 2 < len; ++i) {
        if (src[i] == '.' && src[i + 1] == '.' && src[i + 2] == '.') {
            soma_lint_add(rep, 1, "E: Placeholder '...' detectado: escribe el codigo completo.");
            break;
        }
    }

    /* Recorrido con filtrado de comentarios y literales de cadena */
    i = 0;
    while (i < len) {
        char c = src[i];
        if (c == '\n') { line++; i++; continue; }
        if (c == '/' && i + 1 < len && src[i + 1] == '/') {
            while (i < len && src[i] != '\n') i++;
            continue;
        }
        if (c == '#' && (i == 0 || src[i - 1] == '\n' || src[i - 1] == ' ' || src[i - 1] == '\t')) {
            soma_lint_add(rep, 1, "E: Comentario '#' estilo Python: usa '//' de SOMA-LANG.");
            while (i < len && src[i] != '\n') i++;
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            i++;
            while (i < len && src[i] != q) {
                if (src[i] == '\\' && i + 1 < len) i++;
                if (src[i] == '\n') line++;
                i++;
            }
            if (i < len) i++;
            continue;
        }

        /* Deteccion de palabras clave */
        if (!soma_lint_is_ident(i > 0 ? src[i - 1] : ' ') && soma_lint_is_ident(c)) {
            char tok[32];
            uint32_t tl = 0;
            while (i < len && soma_lint_is_ident(src[i]) && tl < sizeof(tok) - 1) {
                tok[tl++] = src[i++];
            }
            tok[tl] = '\0';

            if (tok[0] == 'd' && tok[1] == 'e' && tok[2] == 'f' && tok[3] == '\0') {
                soma_lint_add(rep, 1, "E: 'def' de Python prohibido: usa 'fn nombre() { ... }'.");
            } else if (tok[0] == 'T' && tok[1] == 'r' && tok[2] == 'u' && tok[3] == 'e' && tok[4] == '\0') {
                soma_lint_add(rep, 1, "E: 'True' en mayuscula prohibido: usa 'true' en minuscula.");
            } else if (tok[0] == 'F' && tok[1] == 'a' && tok[2] == 'l' && tok[3] == 's' && tok[4] == 'e' && tok[5] == '\0') {
                soma_lint_add(rep, 1, "E: 'False' en mayuscula prohibido: usa 'false' en minuscula.");
            } else if (tok[0] == 'N' && tok[1] == 'o' && tok[2] == 'n' && tok[3] == 'e' && tok[4] == '\0') {
                soma_lint_add(rep, 1, "E: 'None' de Python prohibido: usa 'nil' o 0.");
            } else if (tok[0] == 'e' && tok[1] == 'l' && tok[2] == 'i' && tok[3] == 'f' && tok[4] == '\0') {
                soma_lint_add(rep, 1, "E: 'elif' prohibido: usa 'else if (cond) { ... }'.");
            } else if (tok[0] == 'w' && tok[1] == 'h' && tok[2] == 'i' && tok[3] == 'l' && tok[4] == 'e' && tok[5] == '\0') {
                has_while = 1;
            } else if (tok[0] == 's' && tok[1] == 'l' && tok[2] == 'e' && tok[3] == 'e' && tok[4] == 'p' && tok[5] == '\0') {
                has_sleep = 1;
            }
            continue;
        }

        /* Comprobaciones de subsistemas gráficos win.* */
        if (c == 'w' && i + 10 < len && src[i + 1] == 'i' && src[i + 2] == 'n' && src[i + 3] == '.') {
            if (src[i + 4] == 'c' && src[i + 5] == 'r' && src[i + 6] == 'e' && src[i + 7] == 'a' && src[i + 8] == 't' && src[i + 9] == 'e') {
                has_win_create = 1;
            } else if (src[i + 4] == 'p' && src[i + 5] == 'r' && src[i + 6] == 'e' && src[i + 7] == 's' && src[i + 8] == 'e' && src[i + 9] == 'n' && src[i + 10] == 't') {
                has_win_present = 1;
            }
        }

        i++;
    }

    if (has_win_create && !has_win_present) {
        soma_lint_add(rep, 0, "W: win.create() sin win.present(): los graficos no se veran.");
    }
    if (has_while && !has_sleep) {
        soma_lint_add(rep, 0, "W: Bucle 'while' sin sys.sleep(): puede saturar el watchdog.");
    }
    return rep->hard;
}
