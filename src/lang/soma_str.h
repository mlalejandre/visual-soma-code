#ifndef SOMA_STR_H
#define SOMA_STR_H

#include <stdint.h>
#include <stddef.h>

/* Buffer seguro de longitud delimitada con garantia estricta de terminador nulo */
typedef struct {
    char    *buf;
    uint32_t cap;
    uint32_t len;
} soma_sbuf_t;

static inline void soma_sbuf_init(soma_sbuf_t *sb, char *storage, uint32_t capacity)
{
    sb->buf = storage;
    sb->cap = capacity;
    sb->len = 0;
    if (sb->buf && sb->cap > 0) {
        sb->buf[0] = '\0';
    }
}

static inline void soma_sbuf_clear(soma_sbuf_t *sb)
{
    sb->len = 0;
    if (sb->buf && sb->cap > 0) {
        sb->buf[0] = '\0';
    }
}

static inline uint32_t soma_sbuf_avail(const soma_sbuf_t *sb)
{
    if (!sb->buf || sb->cap == 0 || sb->len + 1 >= sb->cap) return 0;
    return sb->cap - sb->len - 1;
}

static inline int soma_sbuf_append_char(soma_sbuf_t *sb, char c)
{
    if (!sb->buf || sb->cap == 0 || sb->len + 1 >= sb->cap) return 0;
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
    return 1;
}

static inline int soma_sbuf_append_str(soma_sbuf_t *sb, const char *src)
{
    if (!sb->buf || !src || sb->cap == 0) return 0;
    while (*src && sb->len + 1 < sb->cap) {
        sb->buf[sb->len++] = *src++;
    }
    sb->buf[sb->len] = '\0';
    return (*src == '\0');
}

static inline int soma_sbuf_copy(soma_sbuf_t *sb, const char *src)
{
    soma_sbuf_clear(sb);
    return soma_sbuf_append_str(sb, src);
}

static inline void soma_sbuf_append_u32(soma_sbuf_t *sb, uint32_t val)
{
    char tmp[12];
    int p = 0;
    if (val == 0) {
        soma_sbuf_append_char(sb, '0');
        return;
    }
    while (val > 0 && p < 11) {
        tmp[p++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (p > 0) {
        soma_sbuf_append_char(sb, tmp[--p]);
    }
}

static inline int soma_sbuf_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == *b);
}

static inline int soma_sbuf_ends_with(const char *str, const char *suffix)
{
    if (!str || !suffix) return 0;
    uint32_t slen = 0, suflen = 0;
    while (str[slen]) slen++;
    while (suffix[suflen]) suflen++;
    if (suflen > slen) return 0;
    for (uint32_t i = 0; i < suflen; ++i) {
        char a = str[slen - suflen + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

#endif
