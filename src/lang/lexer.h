#ifndef SOMA_LANG_LEXER_H
#define SOMA_LANG_LEXER_H

#include <stdint.h>

enum soma_tok_type {
    TOK_EOF = 0,
    TOK_INT,
    TOK_STRING,
    TOK_IDENT,
    TOK_FN,
    TOK_LET,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_RETURN,
    TOK_PRINT,
    TOK_INPUT,
    TOK_IMPORT,
    TOK_PROC,
    TOK_LOOP,
    TOK_BREAK,
    TOK_AGENT,
    TOK_SPAWN,
    TOK_ASK,
    TOK_MEM,
    TOK_VFS,
    TOK_SYS,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_SEMICOLON,
    TOK_COMMA,
    TOK_DOT,
    TOK_ASSIGN,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_MOD,
    TOK_EQ,
    TOK_NEQ,
    TOK_LT,
    TOK_GT,
    TOK_LE,
    TOK_GE,
    TOK_AND,
    TOK_OR,
    TOK_NOT,
    TOK_TRUE,
    TOK_FALSE,
    TOK_NIL,
    /* FASE 6 TOKENS */
    TOK_FOR,
    TOK_CONTINUE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_PLUS_ASSIGN,
    TOK_MINUS_ASSIGN,
    TOK_STAR_ASSIGN,
    TOK_SLASH_ASSIGN,
        TOK_FB,
    TOK_COLON, /*PL4_LEXH*/
    TOK_WIN,
    TOK_ERROR /*PL2_TOKERR*/
};

struct soma_token {
    enum soma_tok_type type;
    int64_t            int_val;
    char               text[128]; /*PL1_LEXH*/
    uint32_t           line;
};

struct soma_lexer {
    const char *src;
    uint32_t    pos;
    uint32_t    len;
    uint32_t    line;
};

void lexer_init(struct soma_lexer *lex, const char *src, uint32_t len);
struct soma_token lexer_next(struct soma_lexer *lex);
const char* token_type_name(enum soma_tok_type t);

#endif
