#include "lexer.h"
#include "mem.h"

void lexer_init(struct soma_lexer *lex, const char *src, uint32_t len)
{
    lex->src = src;
    lex->len = len;
    lex->pos = 0;
    lex->line = 1;
}

static int is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_digit(char c)
{
    return (c >= '0' && c <= '9');
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == *b);
}

const char* token_type_name(enum soma_tok_type t)
{
    switch (t) {
    case TOK_EOF: return "EOF";
    case TOK_INT: return "INT";
    case TOK_STRING: return "STRING";
    case TOK_IDENT: return "IDENT";
    case TOK_FN: return "fn";
    case TOK_LET: return "let";
    case TOK_IF: return "if";
    case TOK_ELSE: return "else";
    case TOK_WHILE: return "while";
    case TOK_RETURN: return "return";
    case TOK_PRINT: return "print";
    case TOK_INPUT: return "input";
    case TOK_IMPORT: return "import";
    case TOK_PROC: return "proc";
    case TOK_LOOP: return "loop";
    case TOK_BREAK: return "break";
    case TOK_AGENT: return "agent";
    case TOK_SPAWN: return "spawn";
    case TOK_ASK: return "ask";
    case TOK_MEM: return "mem";
    case TOK_VFS: return "vfs";
    case TOK_SYS: return "sys";
        case TOK_FB: return "fb";
    case TOK_WIN: return "win"; /*PL4_TYPENAME*/
    case TOK_COLON: return ":";
    case TOK_FOR: return "for";
    case TOK_CONTINUE: return "continue";
    case TOK_LE: return "<=";
    case TOK_GE: return ">=";
    case TOK_AND: return "&&";
    case TOK_OR: return "||";
    case TOK_NOT: return "!";
    case TOK_MOD: return "%";
    case TOK_TRUE: return "true";
    case TOK_FALSE: return "false";
    case TOK_NIL: return "nil";
    case TOK_LBRACKET: return "[";
    case TOK_RBRACKET: return "]";
    case TOK_PLUS_ASSIGN: return "+=";
    case TOK_MINUS_ASSIGN: return "-=";
    case TOK_STAR_ASSIGN: return "*=";
    case TOK_SLASH_ASSIGN: return "/=";
    default: return "SYMBOL";
    }
}

struct soma_token lexer_next(struct soma_lexer *lex)
{
    struct soma_token tok;
    tok.type = TOK_EOF;
    tok.int_val = 0;
    tok.text[0] = '\0';
    tok.line = lex->line;

    while (lex->pos < lex->len) {
        char c = lex->src[lex->pos];

        if (c == ' ' || c == '\t' || c == '\r') {
            lex->pos++;
            continue;
        }
        if (c == '\n') {
            lex->line++;
            lex->pos++;
            continue;
        }

        /* Comentarios con '#' o '//' */
        if (c == '#' || (c == '/' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '/')) {
            while (lex->pos < lex->len && lex->src[lex->pos] != '\n') {
                lex->pos++;
            }
            continue;
        }

        tok.line = lex->line;

        if (is_digit(c)) {
            int64_t val = 0;
            uint32_t tp = 0;

            /* Soporte para hexadecimales 0x / 0X */
            if (c == '0' && lex->pos + 1 < lex->len && (lex->src[lex->pos + 1] == 'x' || lex->src[lex->pos + 1] == 'X')) {
                if (tp < sizeof(tok.text) - 1) tok.text[tp++] = '0';
                if (tp < sizeof(tok.text) - 1) tok.text[tp++] = lex->src[lex->pos + 1];
                lex->pos += 2;
                while (lex->pos < lex->len) {
                    char h = lex->src[lex->pos];
                    int digit = -1;
                    if (h >= '0' && h <= '9') digit = h - '0';
                    else if (h >= 'a' && h <= 'f') digit = h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') digit = h - 'A' + 10;
                    else break;
                    val = (val * 16) + digit;
                    if (tp < sizeof(tok.text) - 1) tok.text[tp++] = h;
                    lex->pos++;
                }
                tok.text[tp] = '\0';
                tok.type = TOK_INT;
                tok.int_val = val;
                return tok;
            }

            /* Decimal ordinario */
            while (lex->pos < lex->len && is_digit(lex->src[lex->pos])) {
                char d = lex->src[lex->pos++];
                val = val * 10 + (d - '0');
                if (tp < sizeof(tok.text) - 1) tok.text[tp++] = d;
            }
            tok.text[tp] = '\0';
            tok.type = TOK_INT;
            tok.int_val = val;
            return tok;
        }

        /* Cadenas con comillas dobles '"' o simples (ASCII 39) */
        if (c == '"' || c == 39) {
            char quote_char = c;
            lex->pos++;
            uint32_t tp = 0;
            int overflow = 0;
            while (lex->pos < lex->len && lex->src[lex->pos] != quote_char) { /*PL1_LEXESC*/
                char sc = lex->src[lex->pos];
                if (sc == '\n') lex->line++;
                if (sc == '\\' && lex->pos + 1 < lex->len) {
                    char nx = lex->src[lex->pos + 1];
                    char rep = 0;
                    if (nx == 'n') rep = '\n';
                    else if (nx == 't') rep = '\t';
                    else if (nx == 'r') rep = '\r';
                    else if (nx == '\\') rep = '\\';
                    else if (nx == '"') rep = '"';
                    else if (nx == 39) rep = 39;
                    if (rep) { sc = rep; lex->pos++; }
                }
                if (tp < sizeof(tok.text) - 1) tok.text[tp++] = sc;
                else overflow = 1;
                lex->pos++;
            }
            int closed = (lex->pos < lex->len && lex->src[lex->pos] == quote_char);
            if (closed) lex->pos++;
            tok.text[tp] = '\0';
            tok.type = (closed && !overflow) ? TOK_STRING : TOK_ERROR;
            return tok;
        }

        if (is_alpha(c)) {
            uint32_t tp = 0;
            while (lex->pos < lex->len && (is_alpha(lex->src[lex->pos]) || is_digit(lex->src[lex->pos]))) {
                if (tp < sizeof(tok.text) - 1) tok.text[tp++] = lex->src[lex->pos];
                lex->pos++;
            }
            tok.text[tp] = '\0';
            /* lang.c almacena nombres en campos de 32 bytes (31 + NUL). */
            if (tp > 31) {
                tok.type = TOK_ERROR;
                return tok;
            }

            if (str_eq(tok.text, "fn"))        tok.type = TOK_FN;
            else if (str_eq(tok.text, "let"))      tok.type = TOK_LET;
            else if (str_eq(tok.text, "if"))       tok.type = TOK_IF;
            else if (str_eq(tok.text, "else"))     tok.type = TOK_ELSE;
            else if (str_eq(tok.text, "while"))    tok.type = TOK_WHILE;
            else if (str_eq(tok.text, "for"))      tok.type = TOK_FOR;
            else if (str_eq(tok.text, "continue")) tok.type = TOK_CONTINUE;
            else if (str_eq(tok.text, "return"))   tok.type = TOK_RETURN;
            else if (str_eq(tok.text, "print"))    tok.type = TOK_PRINT;
            else if (str_eq(tok.text, "input"))    tok.type = TOK_INPUT;
            else if (str_eq(tok.text, "import"))   tok.type = TOK_IMPORT;
            else if (str_eq(tok.text, "proc"))     tok.type = TOK_PROC;
            else if (str_eq(tok.text, "loop"))     tok.type = TOK_LOOP;
            else if (str_eq(tok.text, "break"))    tok.type = TOK_BREAK;
            else if (str_eq(tok.text, "agent"))    tok.type = TOK_AGENT;
            else if (str_eq(tok.text, "spawn"))    tok.type = TOK_SPAWN;
            else if (str_eq(tok.text, "ask"))      tok.type = TOK_ASK;
            else if (str_eq(tok.text, "mem"))      tok.type = TOK_MEM;
            else if (str_eq(tok.text, "vfs"))      tok.type = TOK_VFS;
            else if (str_eq(tok.text, "sys"))      tok.type = TOK_SYS;
                        else if (str_eq(tok.text, "fb"))       tok.type = TOK_FB;
            else if (str_eq(tok.text, "win"))      tok.type = TOK_WIN; /*PL4_KEYWIN*/
            else if (str_eq(tok.text, "true"))     tok.type = TOK_TRUE;
            else if (str_eq(tok.text, "false"))    tok.type = TOK_FALSE;
            else if (str_eq(tok.text, "nil") || str_eq(tok.text, "null")) tok.type = TOK_NIL;
            else                                   tok.type = TOK_IDENT;

            return tok;
        }

        lex->pos++;
        switch (c) {
        case '{': tok.type = TOK_LBRACE; tok.text[0] = '{'; tok.text[1] = 0; return tok;
        case '}': tok.type = TOK_RBRACE; tok.text[0] = '}'; tok.text[1] = 0; return tok;
        case '[': tok.type = TOK_LBRACKET; tok.text[0] = '['; tok.text[1] = 0; return tok;
        case ']': tok.type = TOK_RBRACKET; tok.text[0] = ']'; tok.text[1] = 0; return tok;
        case '(': tok.type = TOK_LPAREN; tok.text[0] = '('; tok.text[1] = 0; return tok;
        case ')': tok.type = TOK_RPAREN; tok.text[0] = ')'; tok.text[1] = 0; return tok;
                case ';': tok.type = TOK_SEMICOLON; tok.text[0] = ';'; tok.text[1] = 0; return tok;
        case ':': tok.type = TOK_COLON; tok.text[0] = ':'; tok.text[1] = 0; return tok; /*PL4_COLON*/
        case ',': tok.type = TOK_COMMA; tok.text[0] = ','; tok.text[1] = 0; return tok;
        case '.': tok.type = TOK_DOT; tok.text[0] = '.'; tok.text[1] = 0; return tok;
        case '%': tok.type = TOK_MOD; tok.text[0] = '%'; tok.text[1] = 0; return tok;
        case '+':
            if (lex->pos < lex->len && lex->src[lex->pos] == '=') {
                lex->pos++;
                tok.type = TOK_PLUS_ASSIGN;
                tok.text[0] = '+'; tok.text[1] = '='; tok.text[2] = 0;
            } else {
                tok.type = TOK_PLUS;
                tok.text[0] = '+'; tok.text[1] = 0;
            }
            return tok;
        case '-':
            if (lex->pos < lex->len && lex->src[lex->pos] == '=') {
                lex->pos++;
                tok.type = TOK_MINUS_ASSIGN;
                tok.text[0] = '-'; tok.text[1] = '='; tok.text[2] = 0;
            } else {
                tok.type = TOK_MINUS;
                tok.text[0] = '-'; tok.text[1] = 0;
            }
            return tok;
        case '*':
            if (lex->pos < lex->len && lex->src[lex->pos] == '=') {
                lex->pos++;
                tok.type = TOK_STAR_ASSIGN;
                tok.text[0] = '*'; tok.text[1] = '='; tok.text[2] = 0;
            } else {
                tok.type = TOK_STAR;
                tok.text[0] = '*'; tok.text[1] = 0;
            }
            return tok;
        case '/':
            if (lex->pos < lex->len && lex->src[lex->pos] == '=') {
                lex->pos++;
                tok.type = TOK_SLASH_ASSIGN;
                tok.text[0] = '/'; tok.text[1] = '='; tok.text[2] = 0;
            } else {
                tok.type = TOK_SLASH;
                tok.text[0] = '/'; tok.text[1] = 0;
            }
            return tok;
        case '=':
            if (lex->pos < lex->len && lex->src[lex->pos] == '=') {
                lex->pos++;
                tok.type = TOK_EQ;
                tok.text[0] = '='; tok.text[1] = '='; tok.text[2] = 0;
            } else {
                tok.type = TOK_ASSIGN;
                tok.text[0] = '='; tok.text[1] = 0;
            }
            return tok;
        case '!':
            if (lex->pos < lex->len && lex->src[lex->pos] == '=') {
                lex->pos++;
                tok.type = TOK_NEQ;
                tok.text[0] = '!'; tok.text[1] = '='; tok.text[2] = 0;
            } else {
                tok.type = TOK_NOT;
                tok.text[0] = '!'; tok.text[1] = 0;
            }
            return tok;
        case '<':
            if (lex->pos < lex->len && lex->src[lex->pos] == '=') {
                lex->pos++;
                tok.type = TOK_LE;
                tok.text[0] = '<'; tok.text[1] = '='; tok.text[2] = 0;
            } else {
                tok.type = TOK_LT;
                tok.text[0] = '<'; tok.text[1] = 0;
            }
            return tok;
        case '>':
            if (lex->pos < lex->len && lex->src[lex->pos] == '=') {
                lex->pos++;
                tok.type = TOK_GE;
                tok.text[0] = '>'; tok.text[1] = '='; tok.text[2] = 0;
            } else {
                tok.type = TOK_GT;
                tok.text[0] = '>'; tok.text[1] = 0;
            }
            return tok;
        case '&':
            if (lex->pos < lex->len && lex->src[lex->pos] == '&') {
                lex->pos++;
                tok.type = TOK_AND;
                tok.text[0] = '&'; tok.text[1] = '&'; tok.text[2] = 0;
                return tok;
            }
            break;
        case '|':
            if (lex->pos < lex->len && lex->src[lex->pos] == '|') {
                lex->pos++;
                tok.type = TOK_OR;
                tok.text[0] = '|'; tok.text[1] = '|'; tok.text[2] = 0;
                return tok;
            }
            break;
        }
        /*PL2_LEXERR*/
        tok.type = TOK_ERROR;
        tok.text[0] = c;
        tok.text[1] = '\0';
        return tok;
    }

    return tok;
}
