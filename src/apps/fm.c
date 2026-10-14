#include "fm.h"
#include "fb.h"
#include "gui_font.h"
#include "mouse.h"
#include "console.h"
#include "fs.h"
#include "mem.h"
#include "arch.h"
#include "rtc.h"
#include "sysinfo.h"
#include "idt.h"
#include "shell.h"

uint64_t timer_get_uptime_ms(void);

#define MAX_FM_FILES 128
#define FM_ROW_H     24

enum fm_modal_mode {
    FM_MODAL_NONE = 0,
    FM_MODAL_NEW_FILE,
    FM_MODAL_NEW_FOLDER,
    FM_MODAL_RENAME,
    FM_MODAL_DELETE_CONFIRM
};

enum fm_sort_mode {
    FM_SORT_NAME_ASC = 0,
    FM_SORT_NAME_DESC,
    FM_SORT_SIZE_ASC,
    FM_SORT_SIZE_DESC,
    FM_SORT_TYPE_ASC,
    FM_SORT_TYPE_DESC
};

struct fm_item {
    char name[48];
    char full_path[80];
    uint32_t size;
    uint8_t is_dir;
    uint8_t is_img;
    uint8_t is_html;
    uint8_t is_txt;
    uint8_t is_soma;
    uint16_t sub_count;
};

static struct fm_item file_list[MAX_FM_FILES];
static int file_count = 0;
static int selected_idx = -1;
static int scroll_offset = 0;
static enum fm_sort_mode sort_mode = FM_SORT_NAME_ASC;

static char current_folder[80] = "/workspace";
static char preview_text[256] = "";
static char preview_extra[64] = "";

static uint64_t last_click_time = 0;
static int last_click_idx = -1;

static enum fm_modal_mode modal_mode = FM_MODAL_NONE;
static char modal_input[48] = "";
static uint32_t modal_input_len = 0;
static char modal_target[80] = "";

/* --- Utilidades de cadena seguras en Ring 0 --- */
static void str_copy(char *dst, const char *src, uint32_t max) {
    uint32_t i = 0;
    while (src && src[i] && i + 1 < max) { dst[i] = src[i]; i++; }
    if (max > 0) dst[i] = '\0';
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return (*a == *b);
}

static uint32_t str_len(const char *s) {
    uint32_t n = 0; while (s && s[n]) n++; return n;
}

static int str_starts_with(const char *s, const char *prefix) {
    if (!s || !prefix) return 0;
    while (*prefix) { if (*s++ != *prefix++) return 0; }
    return 1;
}

static int fm_ends_with_ci(const char *s, const char *suf) {
    uint32_t sl = str_len(s), xl = str_len(suf);
    if (xl > sl) return 0;
    for (uint32_t i = 0; i < xl; ++i) {
        char a = s[sl - xl + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

static int str_cmp_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void fm_build_path(const char *dir, const char *item, char *out, uint32_t max) {
    uint32_t p = 0;
    while (dir && dir[p] && p + 1 < max) { out[p] = dir[p]; p++; }
    if (p > 0 && out[p - 1] != '/' && p + 1 < max) out[p++] = '/';
    uint32_t ip = 0;
    while (item && item[ip] && p + 1 < max) out[p++] = item[ip++];
    out[p] = '\0';
}

static void fm_get_parent_folder(const char *dir, char *parent, uint32_t max) {
    if (!dir || str_eq(dir, "/") || dir[0] == '\0') {
        str_copy(parent, "/", max);
        return;
    }
    uint32_t len = str_len(dir);
    while (len > 1 && dir[len - 1] == '/') len--;
    int last_slash = -1;
    for (int i = (int)len - 1; i >= 0; --i) {
        if (dir[i] == '/') { last_slash = i; break; }
    }
    if (last_slash <= 0) {
        str_copy(parent, "/", max);
    } else {
        uint32_t cp = (uint32_t)last_slash < max - 1 ? (uint32_t)last_slash : max - 1;
        for (uint32_t i = 0; i < cp; ++i) parent[i] = dir[i];
        parent[cp] = '\0';
    }
}

static int fm_find_item_index(const char *name) {
    for (int i = 0; i < file_count; ++i) {
        if (str_eq(file_list[i].name, name)) return i;
    }
    return -1;
}

static int fm_type_rank(const struct fm_item *it) {
    if (it->is_dir)  return 0;
    if (it->is_soma) return 1;
    if (it->is_txt)  return 2;
    if (it->is_html) return 3;
    if (it->is_img)  return 4;
    return 5;
}

/* ====================================================================
 * RENDERIZADORES PROCEDURALES DE ICONOGRAFÍA VECTORIAL
 * ==================================================================== */

/* 1. Flecha Subir Nivel */
static void fm_draw_vec_up(int32_t cx, int32_t cy, uint32_t col) {
    /* Tallo vertical */
    fb_draw_line(cx, cy + 6, cx, cy - 4, col);
    fb_draw_line(cx - 1, cy + 6, cx - 1, cy - 4, col);
    /* Cabeza de flecha */
    fb_draw_line(cx - 5, cy + 1, cx, cy - 4, col);
    fb_draw_line(cx + 5, cy + 1, cx, cy - 4, col);
    fb_draw_line(cx - 4, cy + 1, cx, cy - 3, col);
    fb_draw_line(cx + 4, cy + 1, cx, cy - 3, col);
}

/* 2. Nuevo Archivo (Hoja doblada + insignia '+') */
static void fm_draw_vec_new_file(int32_t cx, int32_t cy, uint32_t col, uint32_t plus_col) {
    /* Contorno de papel */
    fb_draw_line(cx - 6, cy - 7, cx + 1, cy - 7, col);
    fb_draw_line(cx - 6, cy - 7, cx - 6, cy + 6, col);
    fb_draw_line(cx - 6, cy + 6, cx + 1, cy + 6, col);
    fb_draw_line(cx + 5, cy - 3, cx + 5, cy + 1, col);
    /* Esquina plegada */
    fb_draw_line(cx + 1, cy - 7, cx + 5, cy - 3, col);
    fb_draw_line(cx + 1, cy - 7, cx + 1, cy - 3, col);
    fb_draw_line(cx + 1, cy - 3, cx + 5, cy - 3, col);
    /* Líneas interiores */
    fb_draw_line(cx - 3, cy - 2, cx + 1, cy - 2, col);
    fb_draw_line(cx - 3, cy + 1, cx + 1, cy + 1, col);
    /* Cruz '+' */
    fb_draw_line(cx + 2, cy + 4, cx + 7, cy + 4, plus_col);
    fb_draw_line(cx + 2, cy + 5, cx + 7, cy + 5, plus_col);
    fb_draw_line(cx + 4, cy + 2, cx + 4, cy + 7, plus_col);
    fb_draw_line(cx + 5, cy + 2, cx + 5, cy + 7, plus_col);
}

/* 3. Nueva Carpeta (Silueta de carpeta + insignia '+') */
static void fm_draw_vec_new_folder(int32_t cx, int32_t cy, uint32_t col, uint32_t plus_col) {
    /* Pestaña superior */
    fb_draw_line(cx - 8, cy - 6, cx - 3, cy - 6, col);
    fb_draw_line(cx - 3, cy - 6, cx - 1, cy - 4, col);
    fb_draw_line(cx - 1, cy - 4, cx + 6, cy - 4, col);
    /* Cuerpo de carpeta */
    fb_draw_line(cx - 8, cy - 6, cx - 8, cy + 5, col);
    fb_draw_line(cx - 8, cy + 5, cx + 1, cy + 5, col);
    fb_draw_line(cx + 6, cy - 4, cx + 6, cy + 1, col);
    /* Cruz '+' */
    fb_draw_line(cx + 2, cy + 4, cx + 7, cy + 4, plus_col);
    fb_draw_line(cx + 2, cy + 5, cx + 7, cy + 5, plus_col);
    fb_draw_line(cx + 4, cy + 2, cx + 4, cy + 7, plus_col);
    fb_draw_line(cx + 5, cy + 2, cx + 5, cy + 7, plus_col);
}

/* 4. Actualizar / Sincronizar (Ciclo orbital) */
static void fm_draw_vec_refresh(int32_t cx, int32_t cy, uint32_t col) {
    /* Arco superior */
    fb_draw_line(cx - 4, cy - 2, cx - 1, cy - 5, col);
    fb_draw_line(cx - 1, cy - 5, cx + 3, cy - 5, col);
    fb_draw_line(cx + 3, cy - 5, cx + 5, cy - 2, col);
    /* Flecha superior derecha */
    fb_draw_line(cx + 5, cy - 2, cx + 2, cy - 2, col);
    fb_draw_line(cx + 5, cy - 2, cx + 5, cy - 5, col);

    /* Arco inferior */
    fb_draw_line(cx + 4, cy + 2, cx + 1, cy + 5, col);
    fb_draw_line(cx + 1, cy + 5, cx - 3, cy + 5, col);
    fb_draw_line(cx - 3, cy + 5, cx - 5, cy + 2, col);
    /* Flecha inferior izquierda */
    fb_draw_line(cx - 5, cy + 2, cx - 2, cy + 2, col);
    fb_draw_line(cx - 5, cy + 2, cx - 5, cy + 5, col);
}

/* 5. Abrir / Ejecutar (Play triangular / Apertura) */
static void fm_draw_vec_open(int32_t cx, int32_t cy, uint32_t col, int is_soma, int is_dir) {
    if (is_soma) {
        /* Triángulo Play sólido */
        for (int i = 0; i <= 10; ++i) {
            int h = (10 - i) * 6 / 10;
            fb_draw_line(cx - 4 + i, cy - h, cx - 4 + i, cy + h, col);
        }
    } else if (is_dir) {
        /* Carpeta con flecha hacia dentro */
        fb_draw_line(cx - 7, cy - 5, cx - 3, cy - 5, col);
        fb_draw_line(cx - 3, cy - 5, cx - 1, cy - 3, col);
        fb_draw_line(cx - 1, cy - 3, cx + 6, cy - 3, col);
        fb_draw_line(cx - 7, cy - 5, cx - 7, cy + 5, col);
        fb_draw_line(cx - 7, cy + 5, cx + 6, cy + 5, col);
        fb_draw_line(cx + 6, cy - 3, cx + 6, cy + 5, col);
        /* Flecha derecha */
        fb_draw_line(cx - 2, cy + 1, cx + 3, cy + 1, col);
        fb_draw_line(cx + 1, cy - 1, cx + 3, cy + 1, col);
        fb_draw_line(cx + 1, cy + 3, cx + 3, cy + 1, col);
    } else {
        /* Documento saliente con flecha diagonal */
        fb_draw_rect(cx - 6, cy - 6, 8, 12, col);
        fb_draw_line(cx, cy, cx + 6, cy - 6, col);
        fb_draw_line(cx + 3, cy - 6, cx + 6, cy - 6, col);
        fb_draw_line(cx + 6, cy - 3, cx + 6, cy - 6, col);
    }
}

/* 6. Renombrar (Lápiz / Estilete geométrico) */
static void fm_draw_vec_rename(int32_t cx, int32_t cy, uint32_t col) {
    /* Cuerpo del lápiz a 45 grados */
    fb_draw_line(cx - 3, cy + 4, cx + 4, cy - 3, col);
    fb_draw_line(cx - 2, cy + 5, cx + 5, cy - 2, col);
    /* Tapa trasera */
    fb_draw_line(cx + 4, cy - 4, cx + 6, cy - 2, col);
    /* Punta */
    fb_draw_line(cx - 5, cy + 5, cx - 6, cy + 6, col);
    fb_draw_line(cx - 4, cy + 6, cx - 6, cy + 6, col);
}

/* 7. Borrar (Papelera con tapa, asa y ranuras) */
static void fm_draw_vec_delete(int32_t cx, int32_t cy, uint32_t col) {
    /* Tapa y Asa */
    fb_draw_line(cx - 6, cy - 4, cx + 6, cy - 4, col);
    fb_draw_line(cx - 2, cy - 6, cx + 2, cy - 6, col);
    fb_draw_line(cx - 2, cy - 6, cx - 2, cy - 4, col);
    fb_draw_line(cx + 2, cy - 6, cx + 2, cy - 4, col);
    /* Cuerpo de la papelera */
    fb_draw_line(cx - 5, cy - 3, cx - 4, cy + 6, col);
    fb_draw_line(cx + 5, cy - 3, cx + 4, cy + 6, col);
    fb_draw_line(cx - 4, cy + 6, cx + 4, cy + 6, col);
    /* Ranuras verticales */
    fb_draw_line(cx - 2, cy - 1, cx - 2, cy + 4, col);
    fb_draw_line(cx + 2, cy - 1, cx + 2, cy + 4, col);
}

/* ====================================================================
 * ESCANEO Y ORDENACIÓN DEL SISTEMA DE ARCHIVOS
 * ==================================================================== */
static void fm_sort_list(void) {
    if (file_count <= 1) return;
    for (int i = 0; i < file_count - 1; ++i) {
        for (int j = i + 1; j < file_count; ++j) {
            int swap = 0;
            if (file_list[i].is_dir != file_list[j].is_dir) {
                swap = file_list[j].is_dir ? 1 : 0;
            } else {
                switch (sort_mode) {
                case FM_SORT_NAME_ASC:
                    swap = (str_cmp_ci(file_list[i].name, file_list[j].name) > 0);
                    break;
                case FM_SORT_NAME_DESC:
                    swap = (str_cmp_ci(file_list[i].name, file_list[j].name) < 0);
                    break;
                case FM_SORT_SIZE_ASC:
                    if (file_list[i].size != file_list[j].size)
                        swap = (file_list[i].size > file_list[j].size);
                    else
                        swap = (str_cmp_ci(file_list[i].name, file_list[j].name) > 0);
                    break;
                case FM_SORT_SIZE_DESC:
                    if (file_list[i].size != file_list[j].size)
                        swap = (file_list[i].size < file_list[j].size);
                    else
                        swap = (str_cmp_ci(file_list[i].name, file_list[j].name) > 0);
                    break;
                case FM_SORT_TYPE_ASC: {
                    int r1 = fm_type_rank(&file_list[i]);
                    int r2 = fm_type_rank(&file_list[j]);
                    if (r1 != r2) swap = (r1 > r2);
                    else swap = (str_cmp_ci(file_list[i].name, file_list[j].name) > 0);
                    break;
                }
                case FM_SORT_TYPE_DESC: {
                    int r1 = fm_type_rank(&file_list[i]);
                    int r2 = fm_type_rank(&file_list[j]);
                    if (r1 != r2) swap = (r1 < r2);
                    else swap = (str_cmp_ci(file_list[i].name, file_list[j].name) > 0);
                    break;
                }
                }
            }
            if (swap) {
                struct fm_item tmp = file_list[i];
                file_list[i] = file_list[j];
                file_list[j] = tmp;
            }
        }
    }
}

void fm_refresh(void) {
    file_count = 0;
    selected_idx = -1;
    preview_text[0] = '\0';
    preview_extra[0] = '\0';

    static char buf[4096];
    int len = vfs_format_list_ext(1, 0, buf, sizeof(buf) - 1);
    if (len <= 0) return;
    buf[len] = '\0';

    uint32_t cur_len = str_len(current_folder);
    int is_root = str_eq(current_folder, "/");

    const char *p = buf;
    if (p[0] == 'A' && p[1] == 'r') {
        while (*p && *p != ':') p++;
        if (*p == ':') p++;
    }

    static char sz_probe[16384];

    while (*p && file_count < MAX_FM_FILES) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;

        char fname[80];
        uint32_t fp = 0;
        while (*p && *p != ',' && fp < sizeof(fname) - 1) fname[fp++] = *p++;
        fname[fp] = '\0';

        int matches = 0;
        const char *rel = 0;
        if (is_root) {
            if (fname[0] == '/') { matches = 1; rel = fname + 1; }
        } else {
            if (str_starts_with(fname, current_folder)) {
                if (fname[cur_len] == '/') { matches = 1; rel = fname + cur_len + 1; }
            }
        }

        if (matches && rel && *rel) {
            const char *slash = 0;
            for (const char *s = rel; *s; ++s) { if (*s == '/') { slash = s; break; } }

            if (slash) {
                char dirname[48];
                uint32_t dlen = (uint32_t)(slash - rel);
                if (dlen >= sizeof(dirname)) dlen = sizeof(dirname) - 1;
                for (uint32_t k = 0; k < dlen; ++k) dirname[k] = rel[k];
                dirname[dlen] = '\0';

                int existing = fm_find_item_index(dirname);
                if (existing >= 0) {
                    file_list[existing].sub_count++;
                } else if (file_count < MAX_FM_FILES) {
                    str_copy(file_list[file_count].name, dirname, sizeof(file_list[0].name));
                    fm_build_path(current_folder, dirname, file_list[file_count].full_path, sizeof(file_list[0].full_path));
                    file_list[file_count].size = 0;
                    file_list[file_count].is_dir = 1;
                    file_list[file_count].is_img = 0;
                    file_list[file_count].is_html = 0;
                    file_list[file_count].is_txt = 0;
                    file_list[file_count].is_soma = 0;
                    file_list[file_count].sub_count = 1;
                    file_count++;
                }
            } else {
                if (!str_eq(rel, ".dir") && file_count < MAX_FM_FILES) {
                    str_copy(file_list[file_count].name, rel, sizeof(file_list[0].name));
                    str_copy(file_list[file_count].full_path, fname, sizeof(file_list[0].full_path));

                    int sz = vfs_read(fname, sz_probe, sizeof(sz_probe));
                    file_list[file_count].size = (sz >= 0) ? (uint32_t)sz : 0;
                    file_list[file_count].is_dir = 0;
                    file_list[file_count].is_soma = fm_ends_with_ci(rel, ".soma");
                    file_list[file_count].is_img  = fm_ends_with_ci(rel, ".bmp");
                    file_list[file_count].is_html = (fm_ends_with_ci(rel, ".html") || fm_ends_with_ci(rel, ".htm"));
                    file_list[file_count].is_txt  = (fm_ends_with_ci(rel, ".txt") || fm_ends_with_ci(rel, ".sh") ||
                                                     fm_ends_with_ci(rel, ".c")   || fm_ends_with_ci(rel, ".h")  ||
                                                     fm_ends_with_ci(rel, ".log") || fm_ends_with_ci(rel, ".md"));
                    file_list[file_count].sub_count = 0;
                    file_count++;
                }
            }
        }
    }
    fm_sort_list();
    if (scroll_offset > file_count - 1) scroll_offset = 0;
}

void fm_init(void) {
    str_copy(current_folder, "/workspace", sizeof(current_folder));
    scroll_offset = 0;
    selected_idx = -1;
    modal_mode = FM_MODAL_NONE;
    fm_refresh();
}

void fm_open_folder(const char *folder) {
    if (folder && *folder) {
        str_copy(current_folder, folder, sizeof(current_folder));
        uint32_t l = str_len(current_folder);
        if (l > 1 && current_folder[l - 1] == '/') current_folder[l - 1] = '\0';
    }
    scroll_offset = 0;
    selected_idx = -1;
    fm_refresh();
}

static void fm_delete_recursive(const char *target_path, int is_directory) {
    if (!target_path || !*target_path) return;
    if (!is_directory) {
        vfs_delete(target_path);
        return;
    }

    static char buf[4096];
    int len = vfs_format_list_ext(1, 0, buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        uint32_t target_len = str_len(target_path);
        const char *p = buf;
        if (p[0] == 'A' && p[1] == 'r') { while (*p && *p != ':') p++; if (*p == ':') p++; }

        while (*p) {
            while (*p == ' ' || *p == ',') p++;
            if (!*p) break;
            char fn[80]; uint32_t fp = 0;
            while (*p && *p != ',' && fp < sizeof(fn) - 1) fn[fp++] = *p++;
            fn[fp] = '\0';

            if (str_starts_with(fn, target_path) && (fn[target_len] == '/' || fn[target_len] == '\0')) {
                vfs_delete(fn);
            }
        }
    }

    char marker[96];
    fm_build_path(target_path, ".dir", marker, sizeof(marker));
    vfs_delete(marker);
}

static void fm_update_preview(int idx) {
    if (idx < 0 || idx >= file_count) {
        preview_text[0] = '\0';
        preview_extra[0] = '\0';
        return;
    }

    struct fm_item *it = &file_list[idx];
    if (it->is_dir) {
        str_copy(preview_text, "Carpeta de archivos del sistema", sizeof(preview_text));
        str_copy(preview_extra, "Directorio", sizeof(preview_extra));
        return;
    }

    static char raw[160];
    int r = vfs_read(it->full_path, raw, sizeof(raw) - 1);
    if (r > 0) {
        raw[r] = '\0';
        if (it->is_img && r >= 26 && raw[0] == 'B' && raw[1] == 'M') {
            uint32_t w = *(uint32_t *)(raw + 18);
            uint32_t h = *(uint32_t *)(raw + 22);
            uint32_t bpp = (uint32_t)(*(uint16_t *)(raw + 28));
            char inf[64];
            uint32_t ip = 0;
            const char *pfx = "Imagen BMP: ";
            while (*pfx) inf[ip++] = *pfx++;
            char wb[10]; int wn = 0; uint32_t tw = w;
            if (tw == 0) wb[wn++] = '0'; else { while (tw) { wb[wn++] = '0' + tw % 10; tw /= 10; } }
            while (wn) inf[ip++] = wb[--wn];
            inf[ip++] = 'x';
            char hb[10]; int hn = 0; uint32_t th = h;
            if (th == 0) hb[hn++] = '0'; else { while (th) { hb[hn++] = '0' + th % 10; th /= 10; } }
            while (hn) inf[ip++] = hb[--hn];
            inf[ip++] = ' '; inf[ip++] = '(';
            if (bpp == 32) { inf[ip++] = '3'; inf[ip++] = '2'; } else { inf[ip++] = '2'; inf[ip++] = '4'; }
            inf[ip++] = 'b'; inf[ip++] = ')'; inf[ip] = '\0';
            str_copy(preview_text, inf, sizeof(preview_text));
        } else {
            for (int k = 0; k < r; ++k) {
                if (raw[k] == '\r' || raw[k] == '\n') raw[k] = ' ';
            }
            str_copy(preview_text, raw, sizeof(preview_text));
        }
    } else {
        str_copy(preview_text, "(Archivo vacio)", sizeof(preview_text));
    }

    if (it->is_soma) str_copy(preview_extra, "Script SOMA-LANG", sizeof(preview_extra));
    else if (it->is_img) str_copy(preview_extra, "Grafico TrueColor", sizeof(preview_extra));
    else if (it->is_html) str_copy(preview_extra, "Documento Web", sizeof(preview_extra));
    else if (it->is_txt) str_copy(preview_extra, "Documento Texto", sizeof(preview_extra));
    else str_copy(preview_extra, "Archivo Binario", sizeof(preview_extra));
}

/* ====================================================================
 * RENDERIZADO VISUAL "HER-AURA"
 * ==================================================================== */
void fm_render_window(uint32_t win_x, uint32_t win_y, uint32_t win_w, uint32_t win_h, int show_cursor) {
    (void)show_cursor;

    fb_fill_rect(win_x, win_y, win_w, win_h, 0x000A101D);
    fb_draw_rect(win_x, win_y, win_w, win_h, 0x0024344D);

    /* 1. Barra de Herramientas y Navegación */
    uint32_t tool_y = win_y + 34;
    fb_fill_rect(win_x + 12, tool_y, win_w - 24, 38, 0x000F172A);
    fb_draw_rect(win_x + 12, tool_y, win_w - 24, 38, 0x001E293B);

    /* Botón Gráfico [Subir] (34x28 px) */
    uint32_t b_up_x = win_x + 18;
    int is_root = str_eq(current_folder, "/");
    fb_fill_rounded_rect((int32_t)b_up_x, (int32_t)(tool_y + 5), 34, 28, 4, is_root ? 0x00151E2E : 0x00132238);
    fb_draw_rounded_rect((int32_t)b_up_x, (int32_t)(tool_y + 5), 34, 28, 4, is_root ? 0x00334155 : 0x0038BDF8);
    fm_draw_vec_up((int32_t)(b_up_x + 17), (int32_t)(tool_y + 19), is_root ? 0x00475569 : 0x0038BDF8);

    /* Botones Gráficos Toolbar (Derecha) */
    uint32_t r_margin = win_x + win_w - 20;

    /* [Actualizar] (34x28 px) */
    r_margin -= 34;
    uint32_t b_ref_x = r_margin;
    fb_fill_rounded_rect((int32_t)b_ref_x, (int32_t)(tool_y + 5), 34, 28, 4, 0x0011271D);
    fb_draw_rounded_rect((int32_t)b_ref_x, (int32_t)(tool_y + 5), 34, 28, 4, 0x0010B981);
    fm_draw_vec_refresh((int32_t)(b_ref_x + 17), (int32_t)(tool_y + 19), 0x0010B981);

    /* [+ Carpeta] (34x28 px) */
    r_margin -= 42;
    uint32_t b_nfol_x = r_margin;
    fb_fill_rounded_rect((int32_t)b_nfol_x, (int32_t)(tool_y + 5), 34, 28, 4, 0x00132238);
    fb_draw_rounded_rect((int32_t)b_nfol_x, (int32_t)(tool_y + 5), 34, 28, 4, 0x0038BDF8);
    fm_draw_vec_new_folder((int32_t)(b_nfol_x + 17), (int32_t)(tool_y + 19), 0x0038BDF8, 0x0038BDF8);

    /* [+ Archivo] (34x28 px) */
    r_margin -= 42;
    uint32_t b_nfil_x = r_margin;
    fb_fill_rounded_rect((int32_t)b_nfil_x, (int32_t)(tool_y + 5), 34, 28, 4, 0x00231D12);
    fb_draw_rounded_rect((int32_t)b_nfil_x, (int32_t)(tool_y + 5), 34, 28, 4, 0x00F59E0B);
    fm_draw_vec_new_file((int32_t)(b_nfil_x + 17), (int32_t)(tool_y + 19), 0x00F59E0B, 0x00F59E0B);

    /* Campo de Ruta actual protegido por clipping */
    uint32_t path_x = b_up_x + 42;
    uint32_t path_max_w = (b_nfil_x > path_x + 50) ? (b_nfil_x - path_x - 12) : 60;
    fb_set_clip((int32_t)path_x, (int32_t)(tool_y + 4), (int32_t)path_max_w, 30);
    font_draw_string(path_x, tool_y + 14, "Ruta:", 0x0094A3B8, 1);
    font_draw_string(path_x + 44, tool_y + 14, current_folder, 0x00F8FAFC, 1);
    fb_reset_clip();

    /* 2. Barra Lateral de Lugares */
    uint32_t side_w = (win_w < 600) ? 130 : 160;
    uint32_t main_y = tool_y + 44;
    uint32_t main_h = (win_h > 180) ? win_h - 180 : 80;

    fb_fill_rect(win_x + 12, main_y, side_w, main_h, 0x00060913);
    fb_draw_rect(win_x + 12, main_y, side_w, main_h, 0x001E293B);
    font_draw_string(win_x + 22, main_y + 12, "LUGARES", 0x0064748B, 1);

    const char *places[] = { "/workspace", "/mis_archivos", "/etc", "/proc", "/" };
    const char *labels[] = { "Workspace", "Mis Archivos", "Config (etc)", "Kernel (proc)", "Raiz (/)" };

    for (int i = 0; i < 5; ++i) {
        uint32_t py = main_y + 36 + (uint32_t)(i * 28);
        int is_cur = str_eq(current_folder, places[i]);
        if (is_cur) {
            fb_fill_rect(win_x + 14, py - 4, side_w - 4, 24, 0x0017253B);
            fb_fill_rect(win_x + 14, py - 4, 3, 24, 0x0038BDF8);
        }
        font_draw_string(win_x + 24, py + 2, labels[i], is_cur ? 0x0038BDF8 : 0x00CBD5E1, 1);
    }

    /* 3. Panel Principal de Lista de Archivos */
    uint32_t list_x = win_x + 12 + side_w + 8;
    uint32_t list_w = win_w - 24 - side_w - 8;

    fb_fill_rect(list_x, main_y, list_w, main_h, 0x00030712);
    fb_draw_rect(list_x, main_y, list_w, main_h, 0x001F2937);

    fb_fill_rect(list_x + 1, main_y + 1, list_w - 2, 26, 0x000D1526);
    font_draw_string(list_x + 12, main_y + 10, "TIPO", (sort_mode >= FM_SORT_TYPE_ASC) ? 0x0038BDF8 : 0x0094A3B8, 1);
    font_draw_string(list_x + 68, main_y + 10, "NOMBRE", (sort_mode <= FM_SORT_NAME_DESC) ? 0x0038BDF8 : 0x0094A3B8, 1);
    font_draw_string(list_x + list_w - 128, main_y + 10, "TAMANO", (sort_mode == FM_SORT_SIZE_ASC || sort_mode == FM_SORT_SIZE_DESC) ? 0x0038BDF8 : 0x0094A3B8, 1);

    uint32_t items_y = main_y + 28;
    uint32_t items_h = (main_h > 28) ? main_h - 28 : 1;
    uint32_t vis_rows = items_h / FM_ROW_H;
    if (vis_rows == 0) vis_rows = 1;

    uint32_t content_w = (list_w > 18) ? list_w - 18 : list_w;
    fb_set_clip((int32_t)list_x, (int32_t)items_y, (int32_t)content_w, (int32_t)items_h);

    if (file_count == 0) {
        font_draw_string(list_x + 24, items_y + 20, "(Carpeta vacia)", 0x0064748B, 1);
    } else {
        for (uint32_t r = 0; r < vis_rows; ++r) {
            int idx = scroll_offset + (int)r;
            if (idx >= file_count) break;

            uint32_t ry = items_y + r * FM_ROW_H;
            int is_sel = (idx == selected_idx);

            if (is_sel) {
                fb_fill_rect(list_x + 2, ry, content_w - 4, FM_ROW_H, 0x001E3A5F);
                fb_fill_rect(list_x + 2, ry, 2, FM_ROW_H, 0x0038BDF8);
            } else if (r & 1) {
                fb_fill_rect(list_x + 2, ry, content_w - 4, FM_ROW_H, 0x00060A14);
            }

            struct fm_item *it = &file_list[idx];
            if (it->is_dir) {
                font_draw_string(list_x + 12, ry + 6, "[DIR]", 0x00FBBF24, 1);
                font_draw_string(list_x + 68, ry + 6, it->name, 0x00F8FAFC, 1);
                font_draw_string(list_x + 68 + str_len(it->name) * 8, ry + 6, "/", 0x00FBBF24, 1);

                char cnt_s[24]; uint32_t cp = 0;
                uint32_t v = (uint32_t)it->sub_count; char tb[10]; int tn = 0;
                if (v == 0) cnt_s[cp++] = '0'; else { while (v) { tb[tn++] = '0' + v % 10; v /= 10; } while (tn) cnt_s[cp++] = tb[--tn]; }
                const char *sfx = " elem"; while (*sfx) cnt_s[cp++] = *sfx++; cnt_s[cp] = '\0';
                font_draw_string(list_x + list_w - 128, ry + 6, cnt_s, 0x0064748B, 1);
            } else {
                if (it->is_soma)      font_draw_string(list_x + 12, ry + 6, "[APP]", 0x00EC4899, 1);
                else if (it->is_img)  font_draw_string(list_x + 12, ry + 6, "[IMG]", 0x00F59E0B, 1);
                else if (it->is_html) font_draw_string(list_x + 12, ry + 6, "[WEB]", 0x0010B981, 1);
                else if (it->is_txt)  font_draw_string(list_x + 12, ry + 6, "[TXT]", 0x00818CF8, 1);
                else                  font_draw_string(list_x + 12, ry + 6, "[DOC]", 0x0094A3B8, 1);

                font_draw_string(list_x + 68, ry + 6, it->name, is_sel ? 0x00FFFFFF : 0x00E2E8F0, 1);

                char sz_str[24]; uint32_t sp = 0, v = it->size;
                if (v >= 1024 * 1024) {
                    uint32_t mb = v / (1024 * 1024);
                    char tb[10]; int tn = 0; while (mb) { tb[tn++] = '0' + mb % 10; mb /= 10; }
                    while (tn) sz_str[sp++] = tb[--tn];
                    const char *l = " MiB"; while (*l) sz_str[sp++] = *l++;
                } else if (v >= 1024) {
                    uint32_t kb = v / 1024;
                    char tb[10]; int tn = 0; while (kb) { tb[tn++] = '0' + kb % 10; kb /= 10; }
                    while (tn) sz_str[sp++] = tb[--tn];
                    const char *l = " KiB"; while (*l) sz_str[sp++] = *l++;
                } else {
                    char tb[10]; int tn = 0;
                    if (v == 0) sz_str[sp++] = '0'; else { while (v) { tb[tn++] = '0' + v % 10; v /= 10; } while (tn) sz_str[sp++] = tb[--tn]; }
                    const char *l = " B"; while (*l) sz_str[sp++] = *l++;
                }
                sz_str[sp] = '\0';
                font_draw_string(list_x + list_w - 128, ry + 6, sz_str, 0x0094A3B8, 1);
            }
        }
    }
    fb_reset_clip();

    /* 4. Barra Lateral de Desplazamiento (Scrollbar) */
    uint32_t sb_x = list_x + list_w - 14;
    uint32_t sb_y = items_y;
    uint32_t sb_h = items_h;

    fb_fill_rect(sb_x, sb_y, 14, sb_h, 0x000A0F1D);
    fb_draw_rect(sb_x, sb_y, 14, sb_h, 0x001E293B);

    /* Flecha Arriba [▲] */
    fb_fill_rect(sb_x, sb_y, 14, 14, 0x00131E2A);
    font_draw_string(sb_x + 3, sb_y + 3, "^", 0x0094A3B8, 1);

    /* Flecha Abajo [▼] */
    fb_fill_rect(sb_x, sb_y + sb_h - 14, 14, 14, 0x00131E2A);
    font_draw_string(sb_x + 3, sb_y + sb_h - 12, "v", 0x0094A3B8, 1);

    /* Tirador (Thumb) */
    uint32_t track_h = (sb_h > 28) ? sb_h - 28 : 1;
    if (file_count > (int)vis_rows) {
        uint32_t thumb_h = (track_h * vis_rows) / (uint32_t)file_count;
        if (thumb_h < 16) thumb_h = 16;
        uint32_t max_scroll = (uint32_t)(file_count - (int)vis_rows);
        uint32_t thumb_y = sb_y + 14 + ((track_h - thumb_h) * (uint32_t)scroll_offset) / max_scroll;

        fb_fill_rounded_rect((int32_t)(sb_x + 2), (int32_t)thumb_y, 10, thumb_h, 3, 0x00334155);
        fb_draw_rounded_rect((int32_t)(sb_x + 2), (int32_t)thumb_y, 10, thumb_h, 3, 0x0038BDF8);
    }

    /* 5. Panel Inferior de Previsualización y Acciones Gráficas */
    uint32_t prev_y = win_y + win_h - 88;
    fb_fill_rect(win_x + 12, prev_y, win_w - 24, 52, 0x000F172A);
    fb_draw_rect(win_x + 12, prev_y, win_w - 24, 52, 0x001E293B);

    /* Posición de botones de acción vectoriales (a la derecha) */
    uint32_t btn_px = win_x + win_w - 20;

    /* [Borrar / Papelera] (36 px) */
    btn_px -= 36;
    uint32_t b_del_x = btn_px;

    /* [Renombrar / Lápiz] (36 px) */
    btn_px -= 44;
    uint32_t b_ren_x = btn_px;

    /* [Abrir / Play] (44 px) */
    btn_px -= 52;
    uint32_t b_opn_x = btn_px;

    if (selected_idx >= 0 && selected_idx < file_count) {
        struct fm_item *it = &file_list[selected_idx];

        /* ÁREA DE TEXTO CON CLIPPING PROTEGIDO */
        uint32_t text_max_w = (b_opn_x > win_x + 36) ? (b_opn_x - win_x - 36) : 60;
        fb_set_clip((int32_t)(win_x + 22), (int32_t)prev_y, (int32_t)text_max_w, 52);

        char safe_prev[128];
        uint32_t max_chars = text_max_w / 8;
        if (max_chars > sizeof(safe_prev) - 4) max_chars = sizeof(safe_prev) - 4;
        uint32_t plen = str_len(preview_text);
        if (plen > max_chars) {
            for (uint32_t i = 0; i < max_chars; ++i) safe_prev[i] = preview_text[i];
            if (max_chars >= 3) {
                safe_prev[max_chars - 3] = '.';
                safe_prev[max_chars - 2] = '.';
                safe_prev[max_chars - 1] = '.';
            }
            safe_prev[max_chars] = '\0';
        } else {
            str_copy(safe_prev, preview_text, sizeof(safe_prev));
        }

        font_draw_string(win_x + 22, prev_y + 8, it->name, 0x0038BDF8, 1);
        uint32_t name_px = win_x + 22 + str_len(it->name) * 8 + 12;
        if (name_px < win_x + 22 + text_max_w) {
            font_draw_string(name_px, prev_y + 8, preview_extra, 0x00F59E0B, 1);
        }
        font_draw_string(win_x + 22, prev_y + 28, safe_prev, 0x0094A3B8, 1);
        fb_reset_clip();

        /* Botón Gráfico: [Borrar / Papelera] */
        fb_fill_rounded_rect((int32_t)b_del_x, (int32_t)(prev_y + 12), 36, 28, 4, 0x002B1115);
        fb_draw_rounded_rect((int32_t)b_del_x, (int32_t)(prev_y + 12), 36, 28, 4, 0x00EF4444);
        fm_draw_vec_delete((int32_t)(b_del_x + 18), (int32_t)(prev_y + 26), 0x00EF4444);

        /* Botón Gráfico: [Renombrar / Lápiz] */
        fb_fill_rounded_rect((int32_t)b_ren_x, (int32_t)(prev_y + 12), 36, 28, 4, 0x00132238);
        fb_draw_rounded_rect((int32_t)b_ren_x, (int32_t)(prev_y + 12), 36, 28, 4, 0x0038BDF8);
        fm_draw_vec_rename((int32_t)(b_ren_x + 18), (int32_t)(prev_y + 26), 0x0038BDF8);

        /* Botón Gráfico: [Abrir / Play] */
        uint32_t opn_bg = it->is_soma ? 0x002B0F1E : (it->is_dir ? 0x0011271D : 0x00132238);
        uint32_t opn_col = it->is_soma ? 0x00EC4899 : (it->is_dir ? 0x0010B981 : 0x0038BDF8);
        fb_fill_rounded_rect((int32_t)b_opn_x, (int32_t)(prev_y + 12), 44, 28, 4, opn_bg);
        fb_draw_rounded_rect((int32_t)b_opn_x, (int32_t)(prev_y + 12), 44, 28, 4, opn_col);
        fm_draw_vec_open((int32_t)(b_opn_x + 22), (int32_t)(prev_y + 26), opn_col, it->is_soma, it->is_dir);

    } else {
        uint32_t text_max_w = (win_w > 50) ? (win_w - 50) : 100;
        fb_set_clip((int32_t)(win_x + 22), (int32_t)prev_y, (int32_t)text_max_w, 52);
        font_draw_string(win_x + 24, prev_y + 18, "Selecciona un archivo o carpeta para ver acciones y detalles.", 0x0064748B, 1);
        fb_reset_clip();
    }

    /* 6. Barra de Estado Inferior */
    uint32_t st_y = win_y + win_h - 28;
    fb_fill_rect(win_x + 12, st_y, win_w - 24, 22, 0x000F172A);
    fb_draw_rect(win_x + 12, st_y, win_w - 24, 22, 0x001E293B);

    char st_buf[80]; uint32_t stp = 0;
    const char *p1 = "Elementos: "; while (*p1) st_buf[stp++] = *p1++;
    uint32_t vv = (uint32_t)file_count; char tbb[10]; int tnn = 0;
    if (vv == 0) st_buf[stp++] = '0'; else { while (vv) { tbb[tnn++] = '0' + vv % 10; vv /= 10; } while (tnn) st_buf[stp++] = tbb[--tnn]; }
    const char *p2 = "  |  VFS RamFS Activo  |  SOMA Bare-Metal"; while (*p2) st_buf[stp++] = *p2++;
    st_buf[stp] = '\0';
    font_draw_string(win_x + 20, st_y + 6, st_buf, 0x0094A3B8, 1);

    /* 7. Diálogo Modal */
    if (modal_mode != FM_MODAL_NONE) {
        uint32_t mw = 380, mh = 140;
        uint32_t mx = win_x + (win_w > mw ? (win_w - mw) / 2 : 10);
        uint32_t my = win_y + (win_h > mh ? (win_h - mh) / 2 : 10);

        fb_fill_rounded_rect((int32_t)mx, (int32_t)my, mw, mh, 8, 0x000F172A);
        fb_draw_rounded_rect((int32_t)mx, (int32_t)my, mw, mh, 8, 0x0038BDF8);
        fb_fill_rounded_rect((int32_t)mx, (int32_t)my, mw, 28, 8, 0x001E293B);

        const char *mtitle = "ACCION";
        if (modal_mode == FM_MODAL_NEW_FILE)        mtitle = "NUEVO ARCHIVO";
        else if (modal_mode == FM_MODAL_NEW_FOLDER) mtitle = "NUEVA CARPETA";
        else if (modal_mode == FM_MODAL_RENAME)     mtitle = "RENOMBRAR ELEMENTO";
        else if (modal_mode == FM_MODAL_DELETE_CONFIRM) mtitle = "CONFIRMAR ELIMINACION";
        font_draw_string(mx + 14, my + 8, mtitle, 0x00F8FAFC, 1);

        if (modal_mode == FM_MODAL_DELETE_CONFIRM) {
            font_draw_string(mx + 18, my + 42, "Seguro que deseas eliminar permanentemente?", 0x00E2E8F0, 1);
            font_draw_string(mx + 18, my + 60, modal_target, 0x00F87171, 1);
        } else {
            const char *prompt = "Introduce el nombre:";
            if (modal_mode == FM_MODAL_NEW_FILE) prompt = "Nombre del nuevo archivo:";
            else if (modal_mode == FM_MODAL_NEW_FOLDER) prompt = "Nombre de la carpeta:";
            else if (modal_mode == FM_MODAL_RENAME) prompt = "Nuevo nombre:";
            font_draw_string(mx + 18, my + 38, prompt, 0x0094A3B8, 1);

            fb_fill_rect(mx + 18, my + 56, mw - 36, 26, 0x00020617);
            fb_draw_rect(mx + 18, my + 56, mw - 36, 26, 0x0038BDF8);
            font_draw_string(mx + 24, my + 64, modal_input, 0x00FFFFFF, 1);
            fb_fill_rect(mx + 24 + modal_input_len * 8, my + 62, 2, 14, 0x0038BDF8);
        }

        uint32_t btn_ok_x = mx + mw - 190;
        fb_fill_rounded_rect((int32_t)btn_ok_x, (int32_t)(my + mh - 38), 80, 26, 4,
                             (modal_mode == FM_MODAL_DELETE_CONFIRM) ? 0x00991B1B : 0x00064E3B);
        fb_draw_rounded_rect((int32_t)btn_ok_x, (int32_t)(my + mh - 38), 80, 26, 4,
                             (modal_mode == FM_MODAL_DELETE_CONFIRM) ? 0x00EF4444 : 0x0010B981);
        font_draw_string(btn_ok_x + 12, my + mh - 30, (modal_mode == FM_MODAL_DELETE_CONFIRM) ? "Borrar" : "Aceptar", 0x00FFFFFF, 1);

        uint32_t btn_can_x = mx + mw - 95;
        fb_fill_rounded_rect((int32_t)btn_can_x, (int32_t)(my + mh - 38), 80, 26, 4, 0x001E293B);
        fb_draw_rounded_rect((int32_t)btn_can_x, (int32_t)(my + mh - 38), 80, 26, 4, 0x00475569);
        font_draw_string(btn_can_x + 10, my + mh - 30, "Cancelar", 0x00E2E8F0, 1);
    }
}

static int fm_open_selected(char *out_path, uint32_t max_out) {
    if (selected_idx < 0 || selected_idx >= file_count) return 0;
    struct fm_item *it = &file_list[selected_idx];

    if (it->is_dir) {
        fm_open_folder(it->full_path);
        return 0;
    }

    str_copy(out_path, it->full_path, max_out);
    if (it->is_soma) {
        char cmd[128]; uint32_t p = 0; const char *pre = "app ";
        while (*pre) cmd[p++] = *pre++;
        uint32_t k = 0; while (out_path[k] && p < sizeof(cmd) - 1) cmd[p++] = out_path[k++];
        cmd[p] = '\0';
        char dummy[64]; dispatch_command(cmd, dummy, sizeof(dummy));
        return 0;
    }
    if (it->is_img) return 2;
    if (it->is_html) return 3;
    return 4;
}

static void fm_modal_confirm(void) {
    if (modal_mode == FM_MODAL_NEW_FILE) {
        if (modal_input_len > 0) {
            char full[80];
            fm_build_path(current_folder, modal_input, full, sizeof(full));
            vfs_write(full, "", 0);
            fm_refresh();
            selected_idx = fm_find_item_index(modal_input);
            fm_update_preview(selected_idx);
        }
    } else if (modal_mode == FM_MODAL_NEW_FOLDER) {
        if (modal_input_len > 0) {
            char folder_path[80];
            fm_build_path(current_folder, modal_input, folder_path, sizeof(folder_path));
            char marker[96];
            fm_build_path(folder_path, ".dir", marker, sizeof(marker));
            vfs_write(marker, "", 0);
            fm_refresh();
            selected_idx = fm_find_item_index(modal_input);
            fm_update_preview(selected_idx);
        }
    } else if (modal_mode == FM_MODAL_RENAME) {
        if (modal_input_len > 0 && selected_idx >= 0 && selected_idx < file_count) {
            struct fm_item *it = &file_list[selected_idx];
            char new_full[80];
            fm_build_path(current_folder, modal_input, new_full, sizeof(new_full));

            char *mv_tmp = (char *)kmalloc(16384);
            if (mv_tmp) {
                int r = vfs_read(it->full_path, mv_tmp, 16383);
                if (r >= 0) {
                    if (vfs_write(new_full, mv_tmp, (uint32_t)r) >= 0) {
                        vfs_delete(it->full_path);
                    }
                }
                kfree(mv_tmp);
            }
            fm_refresh();
            selected_idx = fm_find_item_index(modal_input);
            fm_update_preview(selected_idx);
        }
    } else if (modal_mode == FM_MODAL_DELETE_CONFIRM) {
        if (selected_idx >= 0 && selected_idx < file_count) {
            fm_delete_recursive(file_list[selected_idx].full_path, file_list[selected_idx].is_dir);
            selected_idx = -1;
            fm_refresh();
        }
    }
    modal_mode = FM_MODAL_NONE;
}

int fm_handle_click(int32_t mx, int32_t my, uint32_t win_x, uint32_t win_y, uint32_t win_w, uint32_t win_h, char *out_path, uint32_t max_out) {
    if (modal_mode != FM_MODAL_NONE) {
        uint32_t mw = 380, mh = 140;
        uint32_t bx = win_x + (win_w > mw ? (win_w - mw) / 2 : 10);
        uint32_t by = win_y + (win_h > mh ? (win_h - mh) / 2 : 10);

        uint32_t btn_ok_x = bx + mw - 190;
        uint32_t btn_can_x = bx + mw - 95;
        uint32_t btn_y = by + mh - 38;

        if (point_in_rect(mx, my, btn_ok_x, btn_y, 80, 26)) {
            fm_modal_confirm();
            return 0;
        }
        if (point_in_rect(mx, my, btn_can_x, btn_y, 80, 26)) {
            modal_mode = FM_MODAL_NONE;
            return 0;
        }
        return 0;
    }

    uint32_t tool_y = win_y + 34;
    uint32_t b_up_x = win_x + 18;

    /* Botón Gráfico [Subir] (34 px) */
    if (point_in_rect(mx, my, b_up_x, tool_y + 5, 34, 28)) {
        char parent[80];
        fm_get_parent_folder(current_folder, parent, sizeof(parent));
        fm_open_folder(parent);
        return 0;
    }

    /* Botones Gráficos Toolbar */
    uint32_t r_margin = win_x + win_w - 20;
    r_margin -= 34;
    uint32_t b_ref_x = r_margin;
    if (point_in_rect(mx, my, b_ref_x, tool_y + 5, 34, 28)) { fm_refresh(); return 0; }

    r_margin -= 42;
    uint32_t b_nfol_x = r_margin;
    if (point_in_rect(mx, my, b_nfol_x, tool_y + 5, 34, 28)) {
        modal_mode = FM_MODAL_NEW_FOLDER;
        modal_input[0] = '\0';
        modal_input_len = 0;
        return 0;
    }

    r_margin -= 42;
    uint32_t b_nfil_x = r_margin;
    if (point_in_rect(mx, my, b_nfil_x, tool_y + 5, 34, 28)) {
        modal_mode = FM_MODAL_NEW_FILE;
        modal_input[0] = '\0';
        modal_input_len = 0;
        return 0;
    }

    /* Lugares */
    uint32_t side_w = (win_w < 600) ? 130 : 160;
    uint32_t main_y = tool_y + 44;
    const char *places[] = { "/workspace", "/mis_archivos", "/etc", "/proc", "/" };
    for (int i = 0; i < 5; ++i) {
        uint32_t py = main_y + 36 + (uint32_t)(i * 28);
        if (point_in_rect(mx, my, win_x + 14, py - 4, side_w - 4, 24)) {
            fm_open_folder(places[i]);
            return 0;
        }
    }

    /* Panel de Lista y Scrollbar */
    uint32_t list_x = win_x + 12 + side_w + 8;
    uint32_t list_w = win_w - 24 - side_w - 8;
    uint32_t main_h = (win_h > 180) ? win_h - 180 : 80;
    uint32_t items_y = main_y + 28;
    uint32_t items_h = (main_h > 28) ? main_h - 28 : 1;
    uint32_t vis_rows = items_h / FM_ROW_H;
    if (vis_rows == 0) vis_rows = 1;

    /* Ordenación de columnas */
    if (point_in_rect(mx, my, list_x, main_y, list_w - 14, 26)) {
        if (mx < (int32_t)(list_x + 60)) {
            sort_mode = (sort_mode == FM_SORT_TYPE_ASC) ? FM_SORT_TYPE_DESC : FM_SORT_TYPE_ASC;
        } else if (mx > (int32_t)(list_x + list_w - 140)) {
            sort_mode = (sort_mode == FM_SORT_SIZE_ASC) ? FM_SORT_SIZE_DESC : FM_SORT_SIZE_ASC;
        } else {
            sort_mode = (sort_mode == FM_SORT_NAME_ASC) ? FM_SORT_NAME_DESC : FM_SORT_NAME_ASC;
        }
        fm_sort_list();
        return 0;
    }

    /* Scrollbar */
    uint32_t sb_x = list_x + list_w - 14;
    if (point_in_rect(mx, my, sb_x, items_y, 14, items_h)) {
        if (my <= (int32_t)(items_y + 14)) {
            if (scroll_offset > 0) scroll_offset--;
        } else if (my >= (int32_t)(items_y + items_h - 14)) {
            if (scroll_offset + (int)vis_rows < file_count) scroll_offset++;
        } else if (file_count > (int)vis_rows) {
            uint32_t track_h = items_h - 28;
            int32_t rel_y = my - (int32_t)(items_y + 14);
            if (rel_y < 0) rel_y = 0;
            if (rel_y > (int32_t)track_h) rel_y = (int32_t)track_h;
            scroll_offset = (int)((rel_y * (file_count - (int)vis_rows)) / track_h);
        }
        return 0;
    }

    /* Clic en filas de archivos */
    uint32_t content_w = (list_w > 18) ? list_w - 18 : list_w;
    if (point_in_rect(mx, my, list_x, items_y, content_w, items_h)) {
        int clicked_row = (int)((my - (int32_t)items_y) / FM_ROW_H);
        int real_idx = scroll_offset + clicked_row;

        if (real_idx >= 0 && real_idx < file_count) {
            uint64_t now = timer_get_uptime_ms();
            if (selected_idx == real_idx && (now - last_click_time) < 500) {
                last_click_time = 0;
                last_click_idx = -1;
                return fm_open_selected(out_path, max_out);
            }
            last_click_time = now;
            last_click_idx = real_idx;
            selected_idx = real_idx;
            fm_update_preview(selected_idx);
            return 0;
        }
    }

    /* Barra inferior de acciones gráficas vectoriales */
    uint32_t prev_y = win_y + win_h - 88;
    uint32_t btn_px = win_x + win_w - 20;

    btn_px -= 36;
    uint32_t b_del_x = btn_px;

    btn_px -= 44;
    uint32_t b_ren_x = btn_px;

    btn_px -= 52;
    uint32_t b_opn_x = btn_px;

    if (selected_idx >= 0 && selected_idx < file_count) {
        if (point_in_rect(mx, my, b_del_x, prev_y + 12, 36, 28)) {
            modal_mode = FM_MODAL_DELETE_CONFIRM;
            str_copy(modal_target, file_list[selected_idx].full_path, sizeof(modal_target));
            return 0;
        }
        if (point_in_rect(mx, my, b_ren_x, prev_y + 12, 36, 28)) {
            modal_mode = FM_MODAL_RENAME;
            str_copy(modal_input, file_list[selected_idx].name, sizeof(modal_input));
            modal_input_len = str_len(modal_input);
            str_copy(modal_target, file_list[selected_idx].full_path, sizeof(modal_target));
            return 0;
        }
        if (point_in_rect(mx, my, b_opn_x, prev_y + 12, 44, 28)) {
            return fm_open_selected(out_path, max_out);
        }
    }

    return 0;
}

void fm_handle_key(char c) {
    if (modal_mode != FM_MODAL_NONE) {
        if (c == 27) {
            modal_mode = FM_MODAL_NONE;
        } else if (c == '\r' || c == '\n') {
            fm_modal_confirm();
        } else if (c == '\b' || (uint8_t)c == 127) {
            if (modal_input_len > 0) {
                modal_input[--modal_input_len] = '\0';
            }
        } else if ((uint8_t)c >= 32 && (uint8_t)c < 127 && modal_input_len < sizeof(modal_input) - 1) {
            modal_input[modal_input_len++] = c;
            modal_input[modal_input_len] = '\0';
        }
        return;
    }

    if (c == 16) { /* Arriba */
        if (selected_idx > 0) selected_idx--;
        else if (selected_idx == -1 && file_count > 0) selected_idx = 0;
        if (selected_idx < scroll_offset) scroll_offset = selected_idx;
        fm_update_preview(selected_idx);
    } else if (c == 14) { /* Abajo */
        if (selected_idx < file_count - 1) selected_idx++;
        if (selected_idx >= scroll_offset + 10) scroll_offset = selected_idx - 9;
        fm_update_preview(selected_idx);
    } else if (c == '\r' || c == '\n') {
        if (selected_idx >= 0 && selected_idx < file_count) {
            if (file_list[selected_idx].is_dir) {
                fm_open_folder(file_list[selected_idx].full_path);
            } else if (file_list[selected_idx].is_soma) {
                char dummy[64], cmd[128];
                str_copy(cmd, "app ", sizeof(cmd));
                uint32_t p = 4, k = 0;
                while (file_list[selected_idx].full_path[k] && p < sizeof(cmd) - 1)
                    cmd[p++] = file_list[selected_idx].full_path[k++];
                cmd[p] = '\0';
                dispatch_command(cmd, dummy, sizeof(dummy));
            }
        }
    } else if (c == '\b' || c == 27 || c == 'u' || c == 'U') {
        char parent[80];
        fm_get_parent_folder(current_folder, parent, sizeof(parent));
        fm_open_folder(parent);
    } else if (c == 'n' || c == 'N') {
        modal_mode = FM_MODAL_NEW_FILE;
        modal_input[0] = '\0';
        modal_input_len = 0;
    } else if (c == 'f' || c == 'F') {
        modal_mode = FM_MODAL_NEW_FOLDER;
        modal_input[0] = '\0';
        modal_input_len = 0;
    } else if (c == 'r' || c == 'R') {
        fm_refresh();
    } else if ((c == 'd' || c == 'D' || (uint8_t)c == 127) && selected_idx >= 0 && selected_idx < file_count) {
        modal_mode = FM_MODAL_DELETE_CONFIRM;
        str_copy(modal_target, file_list[selected_idx].full_path, sizeof(modal_target));
    }
}
