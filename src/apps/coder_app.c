/* SOMA_CODER_IDE_V2
 * SOMA Studio :: Coder IDE -- programador agentico multipaso.
 *
 * Pipeline: 1 Plan (LLM) -> 2 Codigo (agente Coder) -> 3 Verificar (compile + lint)
 *           -> 4 Reparar (hasta 3 rondas) -> 5 Revision -> 6 Entrega.
 * Copia de seguridad previa y rollback automatico si el resultado final no compila.
 */
#include "lang.h"
#include "coder_app.h"
#include "fb.h"
#include "gui_font.h"
#include "fs.h"
#include "mem.h"
#include "agent.h"
#include "shell.h"
#include "thread.h"
#include "console.h"
#include "mouse.h"
#include "notepad.h"
#include "arch.h"
#include "soma_str.h"
#include "lang_diag.h"
#include "llm.h"
#include "gui.h"

extern uint64_t timer_get_uptime_ms(void);

#define CDR_MAX_PROJECTS 64
#define CDR_SRC_MAX      16384
#define CDR_LOG_N        8
#define CDR_LOG_LEN      88
#define CDR_LINT_N       6
#define CDR_LINT_LEN     96
#define CDR_MISSION_MAX  1024
#define CDR_FULL_MAX     1536
#define CDR_PLAN_MAX     480
#define CDR_THOUGHT_MAX  384
#define CDR_MAX_REPAIRS  3
#define CDR_PHASES       6
#define CDR_BAK_FILE     "/tmp/coder_prev.bak"
#define CDR_REPORT_FILE  "/agent/coder_log.txt"

#define CDR_C_BG       0x00080E1Au
#define CDR_C_PANEL    0x000F172Au
#define CDR_C_DARK     0x00050811u
#define CDR_C_BORDER   0x001E293Bu
#define CDR_C_BORDER2  0x00334155u
#define CDR_C_GREEN    0x0000FF88u
#define CDR_C_CYAN     0x0038BDF8u
#define CDR_C_TXT      0x00E2E8F0u
#define CDR_C_SOFT     0x00CBD5E1u
#define CDR_C_GRAY     0x0094A3B8u
#define CDR_C_DIM      0x0064748Bu
#define CDR_C_AMBER    0x00FBBF24u
#define CDR_C_RED      0x00F43F5Eu
#define CDR_C_WHITE    0x00FFFFFFu
#define CDR_C_KW       0x0038BDF8u
#define CDR_C_DECL     0x00F472B6u
#define CDR_C_STR      0x00FBBF24u
#define CDR_C_NUM      0x00A78BFAu
#define CDR_C_NS       0x0034D399u
#define CDR_C_COMMENT  0x0064748Bu

enum cdr_tab {
    CDR_TAB_NEW = 0,
    CDR_TAB_MODIFY = 1,
    CDR_TAB_PROJECTS = 2,
    CDR_TAB_LOG = 3
};

enum coder_job_state {
    CODER_JOB_IDLE = 0,
    CODER_JOB_WORKING,
    CODER_JOB_SUCCESS,
    CODER_JOB_ERROR
};

struct cdr_proj {
    char     name[48];
    char     path[80];
    uint32_t size;
    uint32_t lines;
    uint8_t  is_gui;
};

/* ------------------------------------------------------------------------
 * Utilidades de cadena internas (seguras en Ring 0)
 * ------------------------------------------------------------------------ */
static uint32_t cdr_sub(uint32_t a, uint32_t b)
{
    return (a > b) ? (a - b) : 0u;
}

static uint32_t cdr_len(const char *s)
{
    uint32_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

/* Hash ligero para detectar si el agente dejó intacto el archivo original. */
static uint32_t cdr_hash_bytes(const char *data, uint32_t len)
{
    uint32_t i, h = 2166136261u;
    for (i = 0; data && i < len; ++i) {
        h ^= (uint8_t)data[i];
        h *= 16777619u;
    }
    return h;
}

static void cdr_copy(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    if (!dst || cap == 0) return;
    while (src && src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void cdr_cat(char *dst, uint32_t cap, const char *src)
{
    uint32_t n, i = 0;
    if (!dst || cap == 0) return;
    n = cdr_len(dst);
    if (n + 1 >= cap) return;
    while (src && src[i] && n + 1 < cap) dst[n++] = src[i++];
    dst[n] = '\0';
}

/* Añade como máximo max_add caracteres y siempre conserva el terminador NUL. */
static void cdr_cat_limited(char *dst, uint32_t cap, const char *src, uint32_t max_add)
{
    uint32_t n, i = 0;
    if (!dst || cap == 0) return;
    n = cdr_len(dst);
    while (src && src[i] && i < max_add && n + 1 < cap) dst[n++] = src[i++];
    dst[n] = '\0';
}

static void cdr_cat_u32(char *dst, uint32_t cap, uint32_t v)
{
    char tmp[12];
    char out[12];
    int n = 0, i;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 11) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    for (i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
    cdr_cat(dst, cap, out);
}

static void cdr_num_pad(char *dst, uint32_t width, uint32_t v)
{
    char tmp[12];
    uint32_t n = 0, i = 0, pad;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 11) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    pad = (n < width) ? (width - n) : 0u;
    while (i < pad) dst[i++] = ' ';
    while (n > 0) dst[i++] = tmp[--n];
    dst[i] = '\0';
}

static int cdr_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int cdr_ends_with(const char *s, const char *suf)
{
    uint32_t ls = cdr_len(s), lf = cdr_len(suf);
    if (lf > ls) return 0;
    return cdr_eq(s + (ls - lf), suf);
}

static const char *cdr_find(const char *hay, const char *needle)
{
    if (!hay || !needle) return 0;
    if (!*needle) return hay;
    for (; *hay; hay++) {
        const char *h = hay;
        const char *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return hay;
    }
    return 0;
}

static int cdr_isalpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int cdr_isalnum(char c)
{
    return cdr_isalpha(c) || (c >= '0' && c <= '9');
}

static void cdr_clean_text(char *dst, uint32_t cap, const char *src)
{
    uint32_t n = 0;
    int prev_sp = 1;
    if (!dst || cap == 0) return;
    while (src && *src && n + 1 < cap) {
        unsigned char c = (unsigned char)*src++;
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c < 32 || c >= 127) continue;
        if (c == ' ') {
            if (prev_sp) continue;
            prev_sp = 1;
        } else {
            prev_sp = 0;
        }
        dst[n++] = (char)c;
    }
    while (n > 0 && dst[n - 1] == ' ') n--;
    dst[n] = '\0';
}

static void cdr_fmt_size(char *dst, uint32_t cap, uint32_t bytes)
{
    dst[0] = '\0';
    if (bytes < 1024u) {
        cdr_cat_u32(dst, cap, bytes);
        cdr_cat(dst, cap, " B");
        return;
    }
    cdr_cat_u32(dst, cap, bytes / 1024u);
    cdr_cat(dst, cap, ".");
    cdr_cat_u32(dst, cap, ((bytes % 1024u) * 10u) / 1024u);
    cdr_cat(dst, cap, " KB");
}

/* ------------------------------------------------------------------------
 * Bloqueo ligero (Spinlock Ring 0)
 * ------------------------------------------------------------------------ */
typedef struct {
    volatile int v;
} cdr_lock_t;

static inline void cdr_lock(cdr_lock_t *l)
{
    while (__atomic_test_and_set(&l->v, __ATOMIC_ACQUIRE)) {
        arch_pause();
    }
}

static inline void cdr_unlock(cdr_lock_t *l)
{
    __atomic_clear(&l->v, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------------
 * Estado sincronizado del trabajo (GUI <-> Hilo Worker)
 * ------------------------------------------------------------------------ */
struct cdr_job {
    cdr_lock_t           lock;
    enum coder_job_state state;
    volatile int         cancel;
    int                  phase;
    int                  attempt;
    int                  lint_n;
    int                  lint_hard;
    int                  log_n;
    uint32_t             started_ms;
    uint32_t             ended_ms;
    char                 phase_desc[128];
    char                 live_thought[CDR_THOUGHT_MAX];
    char                 target_file[96];
    char                 mission[CDR_MISSION_MAX];
    char                 plan[CDR_PLAN_MAX];
    char                 lint[CDR_LINT_N][CDR_LINT_LEN];
    char                 log[CDR_LOG_N][CDR_LOG_LEN];
};

static struct cdr_job g_job = {
    .lock = {0},
    .state = CODER_JOB_IDLE,
    .phase = 0,
    .attempt = 0,
    .lint_n = 0,
    .lint_hard = 0,
    .log_n = 0,
    .started_ms = 0,
    .ended_ms = 0,
    .phase_desc = "Listo para crear o modificar aplicaciones .soma",
    .live_thought = "",
    .target_file = "/workspace/calc.soma",
    .mission = "",
    .plan = ""
};

struct cdr_snap {
    enum coder_job_state state;
    int      phase;
    int      attempt;
    int      lint_n;
    int      lint_hard;
    int      log_n;
    uint32_t elapsed_s;
    char     phase_desc[128];
    char     live_thought[CDR_THOUGHT_MAX];
    char     target_file[96];
    char     plan[CDR_PLAN_MAX];
    char     lint[CDR_LINT_N][CDR_LINT_LEN];
    char     log[CDR_LOG_N][CDR_LOG_LEN];
};

static void cdr_snapshot(struct cdr_snap *s)
{
    int i;
    uint32_t now = (uint32_t)timer_get_uptime_ms();
    cdr_lock(&g_job.lock);
    s->state = g_job.state;
    s->phase = g_job.phase;
    s->attempt = g_job.attempt;
    s->lint_n = g_job.lint_n;
    s->lint_hard = g_job.lint_hard;
    s->log_n = g_job.log_n;
    if (g_job.started_ms) {
        uint32_t end = (g_job.state == CODER_JOB_WORKING || g_job.ended_ms == 0) ? now : g_job.ended_ms;
        s->elapsed_s = (end >= g_job.started_ms) ? (end - g_job.started_ms) / 1000u : 0u;
    } else {
        s->elapsed_s = 0;
    }
    cdr_copy(s->phase_desc, sizeof(s->phase_desc), g_job.phase_desc);
    cdr_copy(s->live_thought, sizeof(s->live_thought), g_job.live_thought);
    cdr_copy(s->target_file, sizeof(s->target_file), g_job.target_file);
    cdr_copy(s->plan, sizeof(s->plan), g_job.plan);
    for (i = 0; i < CDR_LINT_N; ++i) cdr_copy(s->lint[i], CDR_LINT_LEN, g_job.lint[i]);
    for (i = 0; i < CDR_LOG_N; ++i) cdr_copy(s->log[i], CDR_LOG_LEN, g_job.log[i]);
    cdr_unlock(&g_job.lock);
}

static void cdr_set_state(enum coder_job_state st, int phase, const char *desc, const char *thought)
{
    cdr_lock(&g_job.lock);
    g_job.state = st;
    g_job.phase = phase;
    if (desc) cdr_copy(g_job.phase_desc, sizeof(g_job.phase_desc), desc);
    if (thought) cdr_copy(g_job.live_thought, sizeof(g_job.live_thought), thought);
    if (st != CODER_JOB_WORKING) g_job.ended_ms = (uint32_t)timer_get_uptime_ms();
    cdr_unlock(&g_job.lock);
}

static void cdr_log(const char *msg)
{
    char line[CDR_LOG_LEN];
    uint32_t i;
    line[0] = '\0';
    if (g_job.state == CODER_JOB_WORKING && g_job.started_ms) {
        uint32_t el = ((uint32_t)timer_get_uptime_ms() - g_job.started_ms) / 1000u;
        cdr_copy(line, sizeof(line), "+");
        cdr_cat_u32(line, sizeof(line), el);
        cdr_cat(line, sizeof(line), "s ");
    }
    cdr_cat(line, sizeof(line), msg);
    cdr_lock(&g_job.lock);
    if (g_job.log_n >= CDR_LOG_N) {
        for (i = 0; i + 1 < CDR_LOG_N; ++i) cdr_copy(g_job.log[i], CDR_LOG_LEN, g_job.log[i + 1]);
        g_job.log_n = CDR_LOG_N - 1;
    }
    cdr_copy(g_job.log[g_job.log_n], CDR_LOG_LEN, line);
    g_job.log_n++;
    cdr_unlock(&g_job.lock);
}

/* ------------------------------------------------------------------------
 * Estado de la interfaz de usuario
 * ------------------------------------------------------------------------ */
static int      cdr_inited = 0;
static enum cdr_tab cdr_tab_cur = CDR_TAB_NEW;

static char     input_create_prompt[384] = "Crea una app interactiva que sea una calculadora con botones";
static uint32_t input_create_len = 0;
static char     input_create_file[64] = "/workspace/calc.soma";
static uint32_t input_create_file_len = 0;
static int      input_create_focus = 0;

static char     input_mod_prompt[384] = "Anade un boton de reset y cambia el color del fondo";
static uint32_t input_mod_len = 0;
static char     mod_current_file[80] = "/workspace/calc.soma";
static char     mod_code[CDR_SRC_MAX];
static int      mod_loaded = 0;
static int      mod_scroll = 0;
static int      mod_total_lines = 0;
static int      mod_vis_lines = 10;
static int      mod_err_line = 0;
static uint32_t mod_size = 0;

static char     cdr_check_msg[160];
static uint32_t cdr_check_col = 0x0094A3B8u;

static struct cdr_proj projects[CDR_MAX_PROJECTS];
static int      project_count = 0;
static int      project_selected = -1;
static int      proj_scroll = 0;
static int      proj_del_armed = -1;

static volatile int g_need_rescan = 0;
static volatile int g_need_reload = 0;

/* Buffers de trabajo estaticos */
static char     cdr_src[CDR_SRC_MAX];
static uint8_t  cdr_bc[32768];
static uint32_t cdr_bc_len = 0;
static uint32_t cdr_orig_len = 0;
static uint32_t cdr_orig_hash = 0;
static int cdr_target_unchanged = 0;
static int cdr_target_preexisted = 0;
static struct soma_lint_report cdr_rep;
static struct soma_compile_diag cdr_diag;
static int      cdr_diag_valid = 0;

static char     cdr_list_buf[8192];
static char     cdr_probe[CDR_SRC_MAX];

static char     cdr_w_target[96];
static char     cdr_w_mission[CDR_MISSION_MAX];
static char     cdr_w_full[CDR_FULL_MAX];
static char     cdr_w_repair[CDR_MISSION_MAX];
static char     cdr_plan_w[CDR_PLAN_MAX];
static char     cdr_prompt[3072];
static char     cdr_reply[4096];
static char     cdr_report[2048];

static void cdr_sync_inputs(void)
{
    input_create_len = cdr_len(input_create_prompt);
    input_create_file_len = cdr_len(input_create_file);
    input_mod_len = cdr_len(input_mod_prompt);
}

static void cdr_ensure_init(void)
{
    if (cdr_inited) return;
    cdr_inited = 1;
    cdr_sync_inputs();
}

static void cdr_set_check(const char *msg, uint32_t color)
{
    cdr_copy(cdr_check_msg, sizeof(cdr_check_msg), msg);
    cdr_check_col = color;
}

/* ------------------------------------------------------------------------
 * Verificación (compilador + lint), copia de seguridad y seguridad de rutas
 * ------------------------------------------------------------------------ */
static int cdr_verify(const char *path)
{
    int r;
    uint32_t bl = 0;
    cdr_diag_valid = 0;
    cdr_rep.count = 0;
    cdr_rep.hard = 0;
    cdr_bc_len = 0;
    r = vfs_read(path, cdr_src, CDR_SRC_MAX - 1);
    if (r <= 0) return -1;
    cdr_src[r] = '\0';
    lang_lint(cdr_src, (uint32_t)r, &cdr_rep);
    if (lang_compile(cdr_src, (uint32_t)r, cdr_bc, sizeof(cdr_bc), &bl) != 0) {
        lang_get_diagnostic(&cdr_diag);
        cdr_diag_valid = 1;
        return 0;
    }
    cdr_bc_len = bl;
    /* Un bytecode compilable no basta si el lint detecta errores graves. */
    if (cdr_rep.hard > 0) return 0;
    return 1;
}

static void cdr_publish_lint(void)
{
    int i;
    cdr_lock(&g_job.lock);
    g_job.lint_n = (cdr_rep.count > CDR_LINT_N) ? CDR_LINT_N : cdr_rep.count;
    g_job.lint_hard = cdr_rep.hard;
    for (i = 0; i < g_job.lint_n; ++i) cdr_copy(g_job.lint[i], CDR_LINT_LEN, cdr_rep.msg[i]);
    cdr_unlock(&g_job.lock);
}

static void cdr_diag_text(char *dst, uint32_t cap)
{
    int i;
    dst[0] = '\0';
    if (cdr_diag_valid) {
        cdr_copy(dst, cap, "L");
        cdr_cat_u32(dst, cap, (uint32_t)cdr_diag.line);
        cdr_cat(dst, cap, ": ");
        cdr_cat(dst, cap, cdr_diag.msg);
        return;
    }
    if (cdr_rep.hard > 0) {
        for (i = 0; i < (int)cdr_rep.count; ++i) {
            if (cdr_rep.msg[i][0] == 'E') {
                cdr_copy(dst, cap, cdr_rep.msg[i]);
                return;
            }
        }
        cdr_copy(dst, cap, "El lint ha detectado errores graves.");
        return;
    }
    cdr_copy(dst, cap, "error de compilacion");
}

static int cdr_backup_save(const char *target)
{
    char hdr[112];
    uint32_t hl, i;
    int r;
    cdr_orig_len = 0;
    cdr_orig_hash = 0;
    cdr_target_unchanged = 0;
    cdr_target_preexisted = 0;
    hdr[0] = '\0';
    cdr_copy(hdr, sizeof(hdr), "//BAK:");
    cdr_cat(hdr, sizeof(hdr), target);
    cdr_cat(hdr, sizeof(hdr), "\n");
    hl = cdr_len(hdr);
    r = vfs_read(target, cdr_src + 128, CDR_SRC_MAX - 129);
    if (r < 0) return 0; /* No existe: un archivo nuevo podrá eliminarse en rollback. */
    cdr_target_preexisted = 1;
    if (r == 0) {
        /* También respaldamos archivos vacíos; el encabezado por sí solo es válido. */
        if (vfs_write(CDR_BAK_FILE, hdr, hl) < 0) return -1;
        return 1;
    }
    if (r >= (int)(CDR_SRC_MAX - 129)) return -1;
    cdr_src[128 + r] = '\0';
    cdr_orig_len = (uint32_t)r;
    cdr_orig_hash = cdr_hash_bytes(cdr_src + 128, (uint32_t)r);
    for (i = 0; i < hl; ++i) cdr_src[128 - hl + i] = hdr[i];
    if (vfs_write(CDR_BAK_FILE, cdr_src + 128 - hl, hl + (uint32_t)r) < 0) return -1;
    return 1;
}

static int cdr_backup_restore(const char *target)
{
    char hdr[112];
    uint32_t hl, i;
    int r;
    hdr[0] = '\0';
    cdr_copy(hdr, sizeof(hdr), "//BAK:");
    cdr_cat(hdr, sizeof(hdr), target);
    cdr_cat(hdr, sizeof(hdr), "\n");
    hl = cdr_len(hdr);
    r = vfs_read(CDR_BAK_FILE, cdr_src, CDR_SRC_MAX - 1);
    if (r < (int)hl) return 0;
    for (i = 0; i < hl; ++i) {
        if (cdr_src[i] != hdr[i]) return 0;
    }
    return (vfs_write(target, cdr_src + hl, (uint32_t)r - hl) >= 0) ? 1 : 0;
}

static int cdr_target_matches_backup(const char *target)
{
    int r;
    if (!target || !*target || cdr_orig_len == 0) return 0;
    r = vfs_read(target, cdr_src, CDR_SRC_MAX - 1);
    if (r < 0) return 0;
    cdr_src[r] = '\0';
    return ((uint32_t)r == cdr_orig_len &&
            cdr_hash_bytes(cdr_src, (uint32_t)r) == cdr_orig_hash);
}

static int cdr_verify_after_change(const char *target)
{
    int i;
    if (cdr_orig_len > 0 && cdr_target_matches_backup(target)) {
        cdr_target_unchanged = 1;
        cdr_diag_valid = 0;
        cdr_bc_len = 0;
        cdr_rep.count = 0;
        cdr_rep.hard = 0;
        for (i = 0; i < 8; ++i) cdr_rep.msg[i][0] = '\0';
        cdr_log("El archivo objetivo sigue idéntico a la versión inicial; se solicitará otra reparación.");
        return 0;
    }
    cdr_target_unchanged = 0;
    return cdr_verify(target);
}

static int cdr_path_allowed(const char *p)
{
    static const char *const deny[] = { "/etc/", "/agent/", "/sys/", "/proc/", "/dev/", "/tmp/", "/mem/", 0 };
    const char *seg, *q;
    int i;
    if (!p || p[0] != '/' || p[1] == '\0') return 0;
    for (i = 0; deny[i]; ++i) {
        uint32_t k = 0;
        while (deny[i][k] && p[k] == deny[i][k]) k++;
        if (!deny[i][k]) return 0;
    }
    /* Evita traversal, segmentos vacíos y separadores alternativos en rutas VFS. */
    seg = p + 1;
    for (q = seg; ; ++q) {
        if (*q == '\\') return 0;
        if (*q == '/' || *q == '\0') {
            uint32_t sl = (uint32_t)(q - seg);
            if (sl == 0 || (sl == 1 && seg[0] == '.') ||
                (sl == 2 && seg[0] == '.' && seg[1] == '.')) return 0;
            if (*q == '\0') break;
            seg = q + 1;
        }
    }
    return 1;
}

static int cdr_normalize_target(const char *in, char *out, uint32_t cap)
{
    uint32_t i = 0, n;
    while (in[i] == ' ') i++;
    if (!in[i]) return -1;
    out[0] = '\0';
    if (in[i] != '/') cdr_copy(out, cap, "/workspace/");
    n = cdr_len(out);
    while (in[i] && n + 1 < cap) {
        out[n++] = (in[i] == ' ') ? '_' : in[i];
        i++;
    }
    out[n] = '\0';
    while (n > 0 && out[n - 1] == '_') out[--n] = '\0';
    if (!cdr_ends_with(out, ".soma")) {
        if (cdr_len(out) + 5u >= cap) return -2;
        cdr_cat(out, cap, ".soma");
    }
    if (!cdr_ends_with(out, ".soma")) return -2;
    if (cdr_len(out) >= (uint32_t)FS_NAME_MAX) return -2;
    if (!cdr_path_allowed(out)) return -3;
    return 1;
}

/* ------------------------------------------------------------------------
 * Proyectos y explorador
 * ------------------------------------------------------------------------ */
static void cdr_scan_projects(void)
{
    const char *p;
    int len;
    project_count = 0;
    len = vfs_format_list_ext(1, 0, cdr_list_buf, sizeof(cdr_list_buf) - 1);
    if (len <= 0) return;
    if (len >= (int)sizeof(cdr_list_buf)) len = (int)sizeof(cdr_list_buf) - 1;
    cdr_list_buf[len] = '\0';

    p = cdr_list_buf;
    if (p[0] == 'A' && p[1] == 'r') {
        while (*p && *p != ':') p++;
        if (*p == ':') p++;
    }

    while (*p && project_count < CDR_MAX_PROJECTS) {
        char path[80];
        uint32_t pl = 0;
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        while (*p && *p != ',' && *p != ' ' && pl + 1 < sizeof(path)) path[pl++] = *p++;
        path[pl] = '\0';
        while (*p && *p != ',' && *p != ' ') p++;

        if (cdr_ends_with(path, ".soma")) {
            struct cdr_proj *it = &projects[project_count];
            const char *last = path;
            const char *q = path;
            int r;
            uint32_t lc = 0;
            int k;

            while (*q) { if (*q == '/') last = q + 1; q++; }
            cdr_copy(it->path, sizeof(it->path), path);
            cdr_copy(it->name, sizeof(it->name), last);
            r = vfs_read(path, cdr_probe, CDR_SRC_MAX - 1);
            if (r >= 0) {
                cdr_probe[r] = '\0';
                it->size = (uint32_t)r;
                for (k = 0; k < r; ++k) if (cdr_probe[k] == '\n') lc++;
                if (r > 0 && cdr_probe[r - 1] != '\n') lc++;
                it->lines = lc;
                it->is_gui = (cdr_find(cdr_probe, "win.") != 0 || cdr_find(cdr_probe, "fb.") != 0) ? 1 : 0;
            } else {
                it->size = 0;
                it->lines = 0;
                it->is_gui = 0;
            }
            project_count++;
        }
    }
    if (project_selected >= project_count) project_selected = project_count - 1;
}

static void cdr_load_mod_file(const char *path)
{
    int r;
    uint32_t k;
    if (!path || !*path) return;
    cdr_copy(mod_current_file, sizeof(mod_current_file), path);
    r = vfs_read(path, mod_code, CDR_SRC_MAX - 1);
    mod_total_lines = 0;
    mod_size = 0;
    if (r >= 0) {
        mod_code[r] = '\0';
        mod_loaded = 1;
        mod_size = (uint32_t)r;
        for (k = 0; k < (uint32_t)r; ++k) if (mod_code[k] == '\n') mod_total_lines++;
        if (r > 0 && mod_code[r - 1] != '\n') mod_total_lines++;
    } else {
        cdr_copy(mod_code, sizeof(mod_code), "// No se pudo cargar el archivo");
        mod_loaded = 0;
        mod_total_lines = 1;
    }
    mod_scroll = 0;
    mod_err_line = 0;
}

static void cdr_poll_pending(void)
{
    if (g_need_rescan) {
        g_need_rescan = 0;
        cdr_scan_projects();
    }
    if (g_need_reload) {
        char t[96];
        g_need_reload = 0;
        cdr_lock(&g_job.lock);
        cdr_copy(t, sizeof(t), g_job.target_file);
        cdr_unlock(&g_job.lock);
        cdr_load_mod_file(t);
    }
}

/* ------------------------------------------------------------------------
 * Pipeline agéntico multipaso (Hilo trabajador en segundo plano)
 * ------------------------------------------------------------------------ */
static int cdr_cancelled(void)
{
    return g_job.cancel ? 1 : 0;
}

static void cdr_make_plan(void)
{
    static char exc[704];
    const char *t;
    const char *e;
    cdr_plan_w[0] = '\0';
    cdr_copy(cdr_prompt, sizeof(cdr_prompt),
        "Eres el arquitecto de software de SOMA OS. Redacta un PLAN breve (3 a 5 pasos numerados, "
        "maximo 380 caracteres en total) para esta tarea en SOMA-LANG, un lenguaje propio parecido a "
        "JavaScript simple (let, fn, if/else, while, for, arrays, tablas, ventanas win.*), NO Python. Tarea: ");
    cdr_cat(cdr_prompt, sizeof(cdr_prompt), cdr_w_mission);
    if (cdr_orig_len > 0) {
        cdr_clean_text(exc, sizeof(exc), cdr_src + 128);
        cdr_cat(cdr_prompt, sizeof(cdr_prompt), " CODIGO ACTUAL (extracto): ");
        cdr_cat(cdr_prompt, sizeof(cdr_prompt), exc);
    }
    cdr_cat(cdr_prompt, sizeof(cdr_prompt), " Responde SOLO con el plan, sin markdown ni explicaciones.");
    cdr_reply[0] = '\0';
    if (!llm_chat(cdr_prompt, cdr_reply, sizeof(cdr_reply), 45000)) {
        cdr_log("Planificador no disponible: se continua sin plan.");
        return;
    }
    t = cdr_reply;
    e = cdr_find(cdr_reply, "</think>");
    if (e) t = e + 8;
    cdr_clean_text(cdr_plan_w, sizeof(cdr_plan_w), t);
    /* No inyectar en la mision metarazonamiento, cabeceras de chat ni planes interminables. */
    if (cdr_len(cdr_plan_w) > 380u ||
        cdr_find(cdr_reply, "[Razonamiento]") ||
        cdr_find(cdr_reply, "Here's a thinking process") ||
        cdr_find(cdr_reply, "Analyze User Input") ||
        cdr_find(cdr_reply, "Role:") || cdr_find(cdr_reply, "Task:")) {
        cdr_plan_w[0] = '\0';
        cdr_lock(&g_job.lock);
        g_job.plan[0] = '\0';
        cdr_unlock(&g_job.lock);
        cdr_log("Plan descartado: contenia metatexto o superaba 380 caracteres; se continua sin contaminar la mision.");
        return;
    }
    cdr_lock(&g_job.lock);
    cdr_copy(g_job.plan, CDR_PLAN_MAX, cdr_plan_w);
    cdr_unlock(&g_job.lock);
    if (cdr_plan_w[0]) cdr_log("Plan elaborado por el arquitecto.");
    else cdr_log("El planificador no devolvio un plan util.");
}

static void cdr_build_full_mission(void)
{
    cdr_copy(cdr_w_full, sizeof(cdr_w_full), "Archivo objetivo obligatorio: ");
    cdr_cat(cdr_w_full, sizeof(cdr_w_full), cdr_w_target);
    /* El espacio antes del salto permite que el extractor de rutas del agente cierre la ruta. */
    cdr_cat(cdr_w_full, sizeof(cdr_w_full), " \nTarea del usuario: ");
    cdr_cat_limited(cdr_w_full, sizeof(cdr_w_full), cdr_w_mission, 720);
    if (cdr_len(cdr_w_mission) > 720u)
        cdr_cat(cdr_w_full, sizeof(cdr_w_full), " [tarea truncada por límite de contexto]");
    if (cdr_plan_w[0]) {
        cdr_cat(cdr_w_full, sizeof(cdr_w_full), " PLAN ACORDADO: ");
        cdr_cat_limited(cdr_w_full, sizeof(cdr_w_full), cdr_plan_w, 300);
    }
    /* Esta instrucción queda al final y no se pierde por desbordamiento silencioso. */
    cdr_cat(cdr_w_full, sizeof(cdr_w_full),
            " Escribe el programa SOMA-LANG COMPLETO en la ruta objetivo indicada; no uses placeholders. "
            "Después valida con compile y lint. No declares éxito sin cambios guardados y verificables.");
}

static void cdr_build_repair(const char *target, int vr)
{
    int i;
    cdr_copy(cdr_w_repair, sizeof(cdr_w_repair), "Corrige el programa en ");
    cdr_cat(cdr_w_repair, sizeof(cdr_w_repair), target);
    cdr_cat(cdr_w_repair, sizeof(cdr_w_repair), ": ");
    if (cdr_target_unchanged)
        cdr_cat(cdr_w_repair, sizeof(cdr_w_repair),
                "el archivo sigue idéntico a la versión inicial: aplica la misión solicitada y guarda el resultado. ");
    if (vr < 0) {
        cdr_cat(cdr_w_repair, sizeof(cdr_w_repair),
                "el archivo no existe o esta vacio. Escribe el programa COMPLETO con write. Tarea original: ");
        cdr_cat(cdr_w_repair, sizeof(cdr_w_repair), cdr_w_mission);
        return;
    }
    if (vr == 0 && cdr_diag_valid) {
        cdr_cat(cdr_w_repair, sizeof(cdr_w_repair), "[COMPILER ERROR L");
        cdr_cat_u32(cdr_w_repair, sizeof(cdr_w_repair), (uint32_t)cdr_diag.line);
        cdr_cat(cdr_w_repair, sizeof(cdr_w_repair), "]: ");
        cdr_cat(cdr_w_repair, sizeof(cdr_w_repair), cdr_diag.msg);
        cdr_cat(cdr_w_repair, sizeof(cdr_w_repair), " cerca de ");
        cdr_cat(cdr_w_repair, sizeof(cdr_w_repair), cdr_diag.token);
        cdr_cat(cdr_w_repair, sizeof(cdr_w_repair), ".");
        if (cdr_diag.snippet[0]) {
            cdr_cat(cdr_w_repair, sizeof(cdr_w_repair), " Linea con el fallo: ");
            cdr_cat(cdr_w_repair, sizeof(cdr_w_repair), cdr_diag.snippet);
            cdr_cat(cdr_w_repair, sizeof(cdr_w_repair), ".");
        }
    }
    for (i = 0; i < cdr_rep.count; ++i) {
        if (cdr_rep.msg[i][0] == 'E') {
            cdr_cat(cdr_w_repair, sizeof(cdr_w_repair), " ");
            cdr_cat(cdr_w_repair, sizeof(cdr_w_repair), cdr_rep.msg[i]);
            cdr_cat(cdr_w_repair, sizeof(cdr_w_repair), ".");
        }
    }
    cdr_cat(cdr_w_repair, sizeof(cdr_w_repair),
            " Reescribe el archivo COMPLETO corregido con write, valida con compile y revisa con lint.");
}

static void cdr_write_report(const char *target, int ok, int repairs, const char *summary)
{
    int i;
    cdr_report[0] = '\0';
    cdr_copy(cdr_report, sizeof(cdr_report), "SOMA CODER :: INFORME DEL ULTIMO TRABAJO\nArchivo: ");
    cdr_cat(cdr_report, sizeof(cdr_report), target);
    cdr_cat(cdr_report, sizeof(cdr_report), "\nResultado: ");
    cdr_cat(cdr_report, sizeof(cdr_report), ok ? "OK" : "ERROR");
    cdr_cat(cdr_report, sizeof(cdr_report), "\nReparaciones: ");
    cdr_cat_u32(cdr_report, sizeof(cdr_report), (uint32_t)repairs);
    cdr_cat(cdr_report, sizeof(cdr_report), "\nResumen: ");
    cdr_cat(cdr_report, sizeof(cdr_report), summary);
    cdr_cat(cdr_report, sizeof(cdr_report), "\nPlan: ");
    cdr_cat(cdr_report, sizeof(cdr_report), cdr_plan_w[0] ? cdr_plan_w : "(sin plan)");
    cdr_cat(cdr_report, sizeof(cdr_report), "\nRevision estatica:\n");
    for (i = 0; i < cdr_rep.count; ++i) {
        cdr_cat(cdr_report, sizeof(cdr_report), "  ");
        cdr_cat(cdr_report, sizeof(cdr_report), cdr_rep.msg[i]);
        cdr_cat(cdr_report, sizeof(cdr_report), "\n");
    }
    if (cdr_rep.count == 0) cdr_cat(cdr_report, sizeof(cdr_report), "  (sin avisos)\n");
    vfs_write(CDR_REPORT_FILE, cdr_report, cdr_len(cdr_report));
}

static void coder_worker_thread(void *arg)
{
    int ok = 0, cancelled = 0, vr = -1, attempt = 0, backup_state;
    char desc[128];
    char tmp[96];
    (void)arg;

    cdr_lock(&g_job.lock);
    cdr_copy(cdr_w_target, sizeof(cdr_w_target), g_job.target_file);
    cdr_copy(cdr_w_mission, sizeof(cdr_w_mission), g_job.mission);
    g_job.cancel = 0;
    g_job.attempt = 0;
    g_job.started_ms = (uint32_t)timer_get_uptime_ms();
    g_job.ended_ms = 0;
    g_job.lint_n = 0;
    g_job.lint_hard = 0;
    g_job.plan[0] = '\0';
    cdr_unlock(&g_job.lock);

    backup_state = cdr_backup_save(cdr_w_target);
    if (backup_state < 0) {
        cdr_set_state(CODER_JOB_ERROR, 0,
                      "No se pudo crear una copia de seguridad; operación detenida.",
                      "No se modificó el archivo porque no hay rollback seguro.");
        cdr_log("Copia de seguridad fallida; el pipeline no modificará el archivo.");
        cdr_write_report(cdr_w_target, 0, 0, "Copia de seguridad fallida; operación detenida sin escribir código.");
        g_need_rescan = 1;
        g_need_reload = 1;
        return;
    }
    if (backup_state > 0) cdr_log("Copia de seguridad creada en /tmp/coder_prev.bak");

    /* FASE 1: Planificacion */
    cdr_set_state(CODER_JOB_WORKING, 1,
                  "Fase 1/6: Elaborando plan de arquitectura con el LLM...",
                  "Analizando requisitos, contexto del archivo y diseno...");
    cdr_make_plan();
    if (cdr_cancelled()) { cancelled = 1; goto done; }

    /* FASE 2: Generacion de codigo */
    cdr_build_full_mission();
    cdr_set_state(CODER_JOB_WORKING, 2,
                  "Fase 2/6: Generando codigo con el Agente Coder...",
                  "Escribiendo programa con bucle de validacion en Ring 0...");
    agent_run("coder", cdr_w_full);
    if (cdr_cancelled()) { cancelled = 1; goto done; }

    /* FASE 3: Verificacion (Compilacion + Lint) */
    cdr_set_state(CODER_JOB_WORKING, 3,
                  "Fase 3/6: Verificando bytecode y analisis estatico...",
                  "Validando sintaxis estricta, tipos y patrones de ejecucion...");
    thread_sleep(60);
    vr = cdr_verify_after_change(cdr_w_target);
    cdr_publish_lint();

    /* FASE 4: Auto-reparacion iterativa */
    while (vr <= 0 && attempt < CDR_MAX_REPAIRS && !cdr_cancelled()) {
        attempt++;
        cdr_lock(&g_job.lock);
        g_job.attempt = attempt;
        cdr_unlock(&g_job.lock);

        desc[0] = '\0';
        cdr_copy(desc, sizeof(desc), "Fase 4/6: Auto-reparando (intento ");
        cdr_cat_u32(desc, sizeof(desc), (uint32_t)attempt);
        cdr_cat(desc, sizeof(desc), "/3)...");

        tmp[0] = '\0';
        cdr_diag_text(tmp, sizeof(tmp));
        cdr_set_state(CODER_JOB_WORKING, 4, desc, tmp);

        cdr_log("Fallo detectado; solicitando auto-reparacion guiada...");
        cdr_build_repair(cdr_w_target, vr);
        agent_run("coder", cdr_w_repair);

        if (cdr_cancelled()) { cancelled = 1; goto done; }
        vr = cdr_verify_after_change(cdr_w_target);
        cdr_publish_lint();
    }
    if (cdr_cancelled()) { cancelled = 1; goto done; }

    /* FASE 5: Revision final y Rollback si no compila */
    cdr_set_state(CODER_JOB_WORKING, 5,
                  "Fase 5/6: Revision de calidad y consistencia...",
                  "Comprobando integridad final del archivo...");
    thread_sleep(40);
    vr = cdr_verify_after_change(cdr_w_target);
    cdr_publish_lint();

    if (vr > 0) {
        ok = 1;
    } else {
        ok = 0;
        if (backup_state > 0) {
            cdr_log("Compilacion fallida: revirtiendo a la version anterior...");
            if (cdr_backup_restore(cdr_w_target)) {
                cdr_log("Rollback completado con exito.");
            } else {
                cdr_log("Error al aplicar rollback desde copia.");
            }
        } else if (!cdr_target_preexisted) {
            if (vfs_delete(cdr_w_target) == 0)
                cdr_log("Archivo nuevo eliminado porque no superó la verificación final.");
        }
    }

done:
    /* FASE 6: Entrega final */
    if (cancelled) {
        if (backup_state > 0) {
            if (cdr_backup_restore(cdr_w_target)) cdr_log("Rollback aplicado tras la cancelación.");
            else cdr_log("No se pudo restaurar la copia tras la cancelación.");
        } else if (!cdr_target_preexisted) {
            if (vfs_delete(cdr_w_target) == 0) cdr_log("Archivo nuevo eliminado tras la cancelación.");
        }
        cdr_set_state(CODER_JOB_ERROR, 6, "Cancelado por el usuario.", "Operación abortada; rollback intentado.");
        cdr_log("Trabajo cancelado por el usuario.");
        cdr_write_report(cdr_w_target, 0, attempt, "Cancelado.");
    } else if (ok) {
        desc[0] = '\0';
        cdr_copy(desc, sizeof(desc), "Fase 6/6: Programa verificado con exito (");
        cdr_cat_u32(desc, sizeof(desc), cdr_bc_len);
        cdr_cat(desc, sizeof(desc), " B bytecode, 0 errores).");
        cdr_set_state(CODER_JOB_SUCCESS, 6, desc,
                      "Codigo valido en Ring 0. Listo para ejecutar con '> PROBAR AHORA'.");
        cdr_log("Programa compilado y listo para produccion.");
        cdr_write_report(cdr_w_target, 1, attempt, "Compilado sin errores.");
    } else {
        tmp[0] = '\0';
        cdr_diag_text(tmp, sizeof(tmp));
        cdr_set_state(CODER_JOB_ERROR, 6, tmp,
                      "No se pudo compilar el resultado tras las rondas de reparacion.");
        cdr_log("El programa no compila.");
        cdr_write_report(cdr_w_target, 0, attempt, tmp);
    }

    g_need_rescan = 1;
    g_need_reload = 1;
}

static void cdr_launch_job(const char *target, const char *mission)
{
    char norm[96];
    int r;
    struct tcb *t;

    r = cdr_normalize_target(target, norm, sizeof(norm));
    if (r == -1) { cdr_set_check("Ruta de archivo vacia.", CDR_C_RED); return; }
    if (r == -2) { cdr_set_check("Ruta demasiado larga para VFS.", CDR_C_RED); return; }
    if (r == -3) { cdr_set_check("Ruta protegida del sistema.", CDR_C_RED); return; }

    cdr_lock(&g_job.lock);
    if (g_job.state == CODER_JOB_WORKING) {
        cdr_unlock(&g_job.lock);
        return;
    }
    cdr_copy(g_job.target_file, sizeof(g_job.target_file), norm);
    cdr_copy(g_job.mission, sizeof(g_job.mission), mission);
    g_job.state = CODER_JOB_WORKING;
    g_job.phase = 0;
    g_job.cancel = 0;
    g_job.log_n = 0;
    cdr_unlock(&g_job.lock);

    cdr_log("Iniciando nuevo pipeline del Coder...");
    t = thread_create("coder_bg", coder_worker_thread, 0);
    if (!t) {
        cdr_set_state(CODER_JOB_ERROR, 0,
                      "Error: no se pudo instanciar hilo de ejecucion.",
                      "Tabla de hilos saturada.");
    }
}

static void coder_start_creation(void)
{
    cdr_sync_inputs();
    if (input_create_len == 0) return;
    cdr_launch_job(input_create_file, input_create_prompt);
}

static void coder_start_modification(void)
{
    cdr_sync_inputs();
    if (input_mod_len == 0) return;
    cdr_launch_job(mod_current_file, input_mod_prompt);
}

static void coder_cancel_job(void)
{
    cdr_lock(&g_job.lock);
    if (g_job.state == CODER_JOB_WORKING) {
        g_job.cancel = 1;
    }
    cdr_unlock(&g_job.lock);
}

void coder_app_notify_thought(const char *txt)
{
    if (!txt || !*txt) return;
    cdr_lock(&g_job.lock);
    if (g_job.state == CODER_JOB_WORKING) {
        cdr_clean_text(g_job.live_thought, sizeof(g_job.live_thought), txt);
    }
    cdr_unlock(&g_job.lock);
}

void coder_app_load_project(const char *path)
{
    cdr_load_mod_file(path);
    cdr_tab_cur = CDR_TAB_MODIFY;
}

void coder_app_init(void)
{
    cdr_ensure_init();
    cdr_tab_cur = CDR_TAB_NEW;
    cdr_scan_projects();
    if (project_count > 0 && !mod_loaded) {
        cdr_load_mod_file(projects[0].path);
    } else {
        cdr_load_mod_file("/workspace/calc.soma");
    }
}

/* ------------------------------------------------------------------------
 * Renderizado de controles y vistas
 * ------------------------------------------------------------------------ */
static void cdr_draw_btn(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         const char *lbl, uint32_t bg, uint32_t brd, uint32_t txt)
{
    fb_fill_rounded_rect((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, 4, bg);
    fb_draw_rounded_rect((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, 4, brd);
    font_draw_string(x + 10, y + (h - 8) / 2, lbl, txt, 1);
}

static void cdr_draw_tab_btn(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             const char *title, int active)
{
    uint32_t bg = active ? 0x001E293Bu : 0x000F172Au;
    uint32_t border = active ? CDR_C_GREEN : CDR_C_BORDER2;
    uint32_t txt = active ? CDR_C_WHITE : CDR_C_GRAY;

    fb_fill_rounded_rect((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, 4, bg);
    fb_draw_rounded_rect((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, 4, border);
    if (active) {
        fb_fill_rect(x + 4, y + h - 3, w - 8, 2, CDR_C_GREEN);
    }
    font_draw_string(x + 12, y + 8, title, txt, 1);
}

static void cdr_draw_stepper(uint32_t x, uint32_t y, uint32_t w, int phase, enum coder_job_state st)
{
    static const char *const titles[CDR_PHASES] = {
        "1. Plan", "2. Codigo", "3. Lint", "4. Reparar", "5. Revision", "6. Entrega"
    };
    uint32_t pw = w / CDR_PHASES;
    int s;
    for (s = 0; s < CDR_PHASES; ++s) {
        uint32_t sx = x + (uint32_t)s * pw;
        uint32_t col = CDR_C_BORDER2;
        uint32_t tcol = CDR_C_DIM;
        int step_num = s + 1;

        if (st == CODER_JOB_SUCCESS) {
            col = CDR_C_GREEN; tcol = CDR_C_GREEN;
        } else if (phase > step_num) {
            col = CDR_C_GREEN; tcol = CDR_C_GREEN;
        } else if (phase == step_num) {
            if (st == CODER_JOB_ERROR) { col = CDR_C_RED; tcol = CDR_C_RED; }
            else { col = CDR_C_CYAN; tcol = CDR_C_WHITE; }
        }
        fb_fill_rect(sx, y, pw - 4, 3, col);
        font_draw_string(sx, y + 6, titles[s], tcol, 1);
    }
}

static uint32_t cdr_token_color(const char *t)
{
    if (cdr_eq(t, "let") || cdr_eq(t, "fn") || cdr_eq(t, "return")) return CDR_C_DECL;
    if (cdr_eq(t, "if") || cdr_eq(t, "else") || cdr_eq(t, "while") || cdr_eq(t, "for") ||
        cdr_eq(t, "break") || cdr_eq(t, "continue")) return CDR_C_KW;
    if (cdr_eq(t, "win") || cdr_eq(t, "fb") || cdr_eq(t, "sys") ||
        cdr_eq(t, "vfs") || cdr_eq(t, "mem")) return CDR_C_NS;
    if (cdr_eq(t, "true") || cdr_eq(t, "false") || cdr_eq(t, "nil")) return CDR_C_NUM;
    return CDR_C_TXT;
}

static void cdr_draw_code_line(uint32_t x, uint32_t y, const char *line, uint32_t len)
{
    uint32_t i = 0, px = x;
    while (i < len) {
        if (line[i] == ' ') { px += 8; i++; continue; }
        if (line[i] == '/' && i + 1 < len && line[i + 1] == '/') {
            char cm[128]; uint32_t cl = 0;
            while (i < len && cl + 1 < sizeof(cm)) cm[cl++] = line[i++];
            cm[cl] = '\0';
            font_draw_string(px, y, cm, CDR_C_COMMENT, 1);
            break;
        }
        if (line[i] == '"' || line[i] == '\'') {
            char q = line[i]; char st[128]; uint32_t sl = 0;
            st[sl++] = line[i++];
            while (i < len && line[i] != q && sl + 2 < sizeof(st)) st[sl++] = line[i++];
            if (i < len && line[i] == q) st[sl++] = line[i++];
            st[sl] = '\0';
            font_draw_string(px, y, st, CDR_C_STR, 1);
            px += sl * 8;
            continue;
        }
        if (cdr_isalpha(line[i])) {
            char id[32]; uint32_t il = 0;
            while (i < len && cdr_isalnum(line[i]) && il + 1 < sizeof(id)) id[il++] = line[i++];
            id[il] = '\0';
            font_draw_string(px, y, id, cdr_token_color(id), 1);
            px += il * 8;
            continue;
        }
        if (line[i] >= '0' && line[i] <= '9') {
            char nm[32]; uint32_t nl = 0;
            while (i < len && (cdr_isalnum(line[i]) || line[i] == 'x' || line[i] == 'X') && nl + 1 < sizeof(nm))
                nm[nl++] = line[i++];
            nm[nl] = '\0';
            font_draw_string(px, y, nm, CDR_C_NUM, 1);
            px += nl * 8;
            continue;
        }
        {
            char ch[2]; ch[0] = line[i++]; ch[1] = '\0';
            font_draw_string(px, y, ch, CDR_C_SOFT, 1);
            px += 8;
        }
    }
}

static void cdr_render_code_viewer(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    const char *p;
    uint32_t cur_line = 0, drawn = 0;
    fb_fill_rect(x, y, w, h, 0x0003050Bu);
    fb_draw_rect(x, y, w, h, CDR_C_BORDER);
    fb_fill_rect(x, y, 48, h, 0x0009101Eu);
    fb_fill_rect(x + 48, y, 1, h, CDR_C_BORDER);

    mod_vis_lines = (int)(h / 14u);
    if (mod_vis_lines < 1) mod_vis_lines = 1;
    fb_set_clip((int32_t)(x + 52), (int32_t)(y + 2), (int32_t)cdr_sub(w, 54), (int32_t)cdr_sub(h, 4));

    p = mod_code;
    while (*p && (int)drawn < mod_vis_lines) {
        const char *ls = p;
        uint32_t llen = 0;
        char num[8];
        while (*p && *p != '\n' && *p != '\r') { llen++; p++; }
        if ((int)cur_line >= mod_scroll) {
            uint32_t ly = y + 3 + drawn * 14;
            int is_err = (mod_err_line > 0 && (int)(cur_line + 1) == mod_err_line);
            if (is_err) {
                fb_fill_rect(x + 49, ly - 1, w - 50, 14, 0x003A121Du);
            }
            cdr_num_pad(num, 4, cur_line + 1);
            font_draw_string(x + 6, ly, num, is_err ? CDR_C_RED : CDR_C_DIM, 1);
            cdr_draw_code_line(x + 54, ly, ls, llen);
            drawn++;
        }
        cur_line++;
        if (*p == '\r') p++;
        if (*p == '\n') p++;
    }
    fb_reset_clip();

    if (mod_total_lines > mod_vis_lines) {
        uint32_t sb_h = (uint32_t)((mod_vis_lines * (int)h) / (mod_total_lines > 0 ? mod_total_lines : 1));
        uint32_t sb_y = y + (uint32_t)((mod_scroll * (int)cdr_sub(h, sb_h)) / (mod_total_lines - mod_vis_lines));
        if (sb_h < 10) sb_h = 10;
        fb_fill_rect(x + w - 4, y, 4, h, 0x00050811u);
        fb_fill_rect(x + w - 4, sb_y, 4, sb_h, CDR_C_BORDER2);
    }
}

void coder_app_render_window(uint32_t win_x, uint32_t win_y, uint32_t win_w, uint32_t win_h, int is_focused)
{
    struct cdr_snap snap;
    uint32_t tab_bar_y, content_y, content_h, tx;
    (void)is_focused;

    cdr_ensure_init();
    cdr_poll_pending();
    cdr_snapshot(&snap);

    fb_fill_rect(win_x, win_y, win_w, win_h, CDR_C_BG);
    fb_draw_rect(win_x, win_y, win_w, win_h, CDR_C_BORDER);

    tab_bar_y = win_y + 32;
    fb_fill_rect(win_x + 10, tab_bar_y, win_w - 20, 36, CDR_C_DARK);
    fb_draw_rect(win_x + 10, tab_bar_y, win_w - 20, 36, CDR_C_BORDER);

    tx = win_x + 14;
    cdr_draw_tab_btn(tx, tab_bar_y + 4, 130, 28, "1. CREAR APP", cdr_tab_cur == CDR_TAB_NEW);
    tx += 136;
    cdr_draw_tab_btn(tx, tab_bar_y + 4, 140, 28, "2. MODIFICAR", cdr_tab_cur == CDR_TAB_MODIFY);
    tx += 146;
    cdr_draw_tab_btn(tx, tab_bar_y + 4, 140, 28, "3. PROYECTOS", cdr_tab_cur == CDR_TAB_PROJECTS);
    tx += 146;
    cdr_draw_tab_btn(tx, tab_bar_y + 4, 120, 28, "4. REGISTRO", cdr_tab_cur == CDR_TAB_LOG);

    if (win_w > 720) {
        font_draw_string(win_x + win_w - 180, tab_bar_y + 12, "SOMA-LANG v0.2 [OK]", CDR_C_GREEN, 1);
    }

    content_y = tab_bar_y + 42;
    content_h = (win_h > 140) ? (win_h - 140) : 100;

    /* PESTANA 1: CREAR */
    if (cdr_tab_cur == CDR_TAB_NEW) {
        uint32_t box_y = content_y + 24;
        uint32_t box_w = win_w - 48;
        uint32_t f_y = content_y + 60;
        uint32_t chip_x;
        uint32_t act_btn_y = content_y + 92;
        int is_running = (snap.state == CODER_JOB_WORKING);
        uint32_t box_p_y = act_btn_y + 38;
        uint32_t box_p_h = (content_h > 140) ? (content_h - 135) : 80;

        fb_fill_rounded_rect((int32_t)(win_x + 14), (int32_t)content_y, (int32_t)(win_w - 28), 86, 6, CDR_C_PANEL);
        fb_draw_rounded_rect((int32_t)(win_x + 14), (int32_t)content_y, (int32_t)(win_w - 28), 86, 6, CDR_C_BORDER);
        font_draw_string(win_x + 24, content_y + 8, "ORDEN PARA EL AGENTE PROGRAMADOR (CODER IA):", CDR_C_CYAN, 1);

        fb_fill_rect(win_x + 24, box_y, box_w, 24, CDR_C_DARK);
        fb_draw_rect(win_x + 24, box_y, box_w, 24, (input_create_focus == 0) ? CDR_C_GREEN : CDR_C_BORDER2);
        fb_set_clip((int32_t)(win_x + 28), (int32_t)(box_y + 2), (int32_t)(box_w - 8), 20);
        font_draw_string(win_x + 28, box_y + 6, input_create_prompt, CDR_C_WHITE, 1);
        if (input_create_focus == 0) {
            fb_fill_rect(win_x + 28 + input_create_len * 8, box_y + 4, 2, 16, CDR_C_GREEN);
        }
        fb_reset_clip();

        font_draw_string(win_x + 24, f_y + 4, "Guardar en:", CDR_C_GRAY, 1);
        fb_fill_rect(win_x + 110, f_y, 220, 22, CDR_C_DARK);
        fb_draw_rect(win_x + 110, f_y, 220, 22, (input_create_focus == 1) ? CDR_C_GREEN : CDR_C_BORDER2);
        font_draw_string(win_x + 116, f_y + 4, input_create_file, CDR_C_AMBER, 1);
        if (input_create_focus == 1) {
            fb_fill_rect(win_x + 116 + input_create_file_len * 8, f_y + 3, 2, 16, CDR_C_GREEN);
        }

        chip_x = win_x + 340;
        font_draw_string(chip_x, f_y + 4, "Plantillas:", CDR_C_DIM, 1);
        chip_x += 80;
        cdr_draw_btn(chip_x, f_y, 90, 22, "Calculadora", CDR_C_PANEL, CDR_C_BORDER, CDR_C_SOFT);
        chip_x += 96;
        cdr_draw_btn(chip_x, f_y, 70, 22, "Pong 2D", CDR_C_PANEL, CDR_C_BORDER, CDR_C_SOFT);
        chip_x += 76;
        cdr_draw_btn(chip_x, f_y, 75, 22, "Contador", CDR_C_PANEL, CDR_C_BORDER, CDR_C_SOFT);

        cdr_draw_btn(win_x + 24, act_btn_y, 200, 30,
                     is_running ? "GENERANDO EN VIVO..." : "[ GENERAR CON CODER ]",
                     is_running ? 0x001A3A2Au : 0x00064E3Bu,
                     is_running ? 0x0034D399u : CDR_C_GREEN,
                     CDR_C_WHITE);

        if (is_running) {
            cdr_draw_btn(win_x + 232, act_btn_y, 110, 30, "CANCELAR", 0x003A121Du, CDR_C_RED, CDR_C_WHITE);
        } else if (snap.state == CODER_JOB_SUCCESS) {
            cdr_draw_btn(win_x + 232, act_btn_y, 140, 30, "> PROBAR AHORA", 0x001E3A8Au, CDR_C_CYAN, CDR_C_WHITE);
            cdr_draw_btn(win_x + 380, act_btn_y, 120, 30, "VER CODIGO", CDR_C_PANEL, CDR_C_BORDER2, CDR_C_SOFT);
        }

        fb_fill_rounded_rect((int32_t)(win_x + 14), (int32_t)box_p_y, (int32_t)(win_w - 28), box_p_h, 6, 0x00060A14u);
        fb_draw_rounded_rect((int32_t)(win_x + 14), (int32_t)box_p_y, (int32_t)(win_w - 28), box_p_h, 6, CDR_C_BORDER);
        font_draw_string(win_x + 24, box_p_y + 8, "ESTADO DEL PIPELINE AGENTICO:", CDR_C_GRAY, 1);
        cdr_draw_stepper(win_x + 24, box_p_y + 24, win_w - 48, snap.phase, snap.state);

        font_draw_string(win_x + 24, box_p_y + 46, snap.phase_desc,
                         (snap.state == CODER_JOB_SUCCESS) ? CDR_C_GREEN : ((snap.state == CODER_JOB_ERROR) ? CDR_C_RED : CDR_C_WHITE), 1);

        if (snap.plan[0]) {
            font_draw_string(win_x + 24, box_p_y + 64, "Plan acordado:", CDR_C_AMBER, 1);
            fb_set_clip((int32_t)(win_x + 24), (int32_t)(box_p_y + 78), (int32_t)(win_w - 48), 28);
            font_draw_string(win_x + 24, box_p_y + 78, snap.plan, CDR_C_SOFT, 1);
            fb_reset_clip();
        }

        fb_fill_rect(win_x + 24, box_p_y + box_p_h - 40, win_w - 48, 1, CDR_C_BORDER);
        font_draw_string(win_x + 24, box_p_y + box_p_h - 32, "Razonamiento en tiempo real:", CDR_C_DIM, 1);
        fb_set_clip((int32_t)(win_x + 24), (int32_t)(box_p_y + box_p_h - 18), (int32_t)(win_w - 48), 16);
        font_draw_string(win_x + 24, box_p_y + box_p_h - 18,
                         snap.live_thought[0] ? snap.live_thought : "(Esperando inicio de mision...)", CDR_C_CYAN, 1);
        fb_reset_clip();

    /* PESTANA 2: MODIFICAR */
    } else if (cdr_tab_cur == CDR_TAB_MODIFY) {
        uint32_t m_act_x;
        uint32_t mod_in_y = content_y + 54;
        uint32_t btn_m_y = mod_in_y + 16;
        uint32_t cv_y = mod_in_y + 48;
        uint32_t cv_h = (content_h > 115) ? (content_h - 110) : 80;
        char sz_txt[24];

        fb_fill_rounded_rect((int32_t)(win_x + 14), (int32_t)content_y, (int32_t)(win_w - 28), 48, 6, CDR_C_PANEL);
        fb_draw_rounded_rect((int32_t)(win_x + 14), (int32_t)content_y, (int32_t)(win_w - 28), 48, 6, CDR_C_BORDER);

        font_draw_string(win_x + 24, content_y + 6, "PROGRAMA SELECCIONADO:", CDR_C_CYAN, 1);
        font_draw_string(win_x + 24, content_y + 22, mod_current_file, CDR_C_AMBER, 1);
        cdr_fmt_size(sz_txt, sizeof(sz_txt), mod_size);
        font_draw_string(win_x + 260, content_y + 22, sz_txt, CDR_C_DIM, 1);

        m_act_x = win_x + win_w - 360;
        cdr_draw_btn(m_act_x, content_y + 12, 80, 24, "> PROBAR", 0x001E3A8Au, CDR_C_CYAN, CDR_C_WHITE);
        m_act_x += 86;
        cdr_draw_btn(m_act_x, content_y + 12, 85, 24, "NOTEPAD", CDR_C_PANEL, CDR_C_BORDER2, CDR_C_SOFT);
        m_act_x += 91;
        cdr_draw_btn(m_act_x, content_y + 12, 85, 24, "VERIFICAR", CDR_C_PANEL, CDR_C_BORDER2, CDR_C_SOFT);
        m_act_x += 91;
        cdr_draw_btn(m_act_x, content_y + 12, 80, 24, "REVERTIR", 0x003A121Du, CDR_C_RED, CDR_C_WHITE);

        font_draw_string(win_x + 24, mod_in_y, "INSTRUCCION DE CAMBIO PARA EL CODER:", CDR_C_GRAY, 1);
        fb_fill_rect(win_x + 24, btn_m_y, win_w - 200, 24, CDR_C_DARK);
        fb_draw_rect(win_x + 24, btn_m_y, win_w - 200, 24, CDR_C_GREEN);
        fb_set_clip((int32_t)(win_x + 28), (int32_t)(btn_m_y + 2), (int32_t)(win_w - 208), 20);
        font_draw_string(win_x + 28, btn_m_y + 6, input_mod_prompt, CDR_C_WHITE, 1);
        fb_reset_clip();

        cdr_draw_btn(win_x + win_w - 165, btn_m_y, 145, 24, "[ APLICAR CAMBIOS ]", 0x00064E3Bu, CDR_C_GREEN, CDR_C_WHITE);

        cdr_render_code_viewer(win_x + 14, cv_y, win_w - 28, cv_h);
        if (cdr_check_msg[0]) {
            font_draw_string(win_x + 24, cv_y + cv_h - 18, cdr_check_msg, cdr_check_col, 1);
        }

    /* PESTANA 3: PROYECTOS */
    } else if (cdr_tab_cur == CDR_TAB_PROJECTS) {
        uint32_t list_w = (win_w > 360) ? 320 : (win_w - 28);
        uint32_t row_y = content_y + 26;
        int i;

        fb_fill_rect(win_x + 14, content_y, list_w, content_h, CDR_C_DARK);
        fb_draw_rect(win_x + 14, content_y, list_w, content_h, CDR_C_BORDER);
        font_draw_string(win_x + 22, content_y + 8, "ARCHIVOS .SOMA DETECTADOS:", CDR_C_GRAY, 1);

        for (i = proj_scroll; i < project_count && (row_y + 22) < (content_y + content_h); ++i) {
            int is_sel = (i == project_selected);
            if (is_sel) fb_fill_rect(win_x + 16, row_y, list_w - 4, 22, 0x001E3A5Fu);
            font_draw_string(win_x + 20, row_y + 4, projects[i].is_gui ? "[GUI]" : "[CLI]",
                             projects[i].is_gui ? CDR_C_GREEN : CDR_C_CYAN, 1);
            font_draw_string(win_x + 64, row_y + 4, projects[i].name, is_sel ? CDR_C_WHITE : CDR_C_SOFT, 1);
            row_y += 22;
        }

        if (win_w > 380) {
            uint32_t det_x = win_x + 14 + list_w + 10;
            uint32_t det_w = win_w - 38 - list_w;
            fb_fill_rounded_rect((int32_t)det_x, (int32_t)content_y, (int32_t)det_w, content_h, 6, CDR_C_PANEL);
            fb_draw_rounded_rect((int32_t)det_x, (int32_t)content_y, (int32_t)det_w, content_h, 6, CDR_C_BORDER);

            if (project_selected >= 0 && project_selected < project_count) {
                struct cdr_proj *it = &projects[project_selected];
                char inf[64];
                uint32_t ab_y;

                font_draw_string(det_x + 16, content_y + 12, it->name, CDR_C_GREEN, 1);
                font_draw_string(det_x + 16, content_y + 30, it->path, CDR_C_GRAY, 1);

                inf[0] = '\0';
                cdr_copy(inf, sizeof(inf), "Lineas: ");
                cdr_cat_u32(inf, sizeof(inf), it->lines);
                cdr_cat(inf, sizeof(inf), "  |  Tamano: ");
                cdr_fmt_size(inf + cdr_len(inf), sizeof(inf) - cdr_len(inf), it->size);
                font_draw_string(det_x + 16, content_y + 48, inf, CDR_C_SOFT, 1);

                ab_y = content_y + 76;
                cdr_draw_btn(det_x + 16, ab_y, 120, 28, "> EJECUTAR", 0x001E3A8Au, CDR_C_CYAN, CDR_C_WHITE);
                cdr_draw_btn(det_x + 144, ab_y, 120, 28, "MODIFICAR", 0x00064E3Bu, CDR_C_GREEN, CDR_C_WHITE);

                ab_y += 36;
                cdr_draw_btn(det_x + 16, ab_y, 120, 28, "EN NOTEPAD", CDR_C_PANEL, CDR_C_BORDER2, CDR_C_SOFT);
                cdr_draw_btn(det_x + 144, ab_y, 120, 28, "DUPLICAR", CDR_C_PANEL, CDR_C_BORDER2, CDR_C_SOFT);

                ab_y += 36;
                if (proj_del_armed == project_selected) {
                    cdr_draw_btn(det_x + 16, ab_y, 248, 28, "CONFIRMAR BORRADO?", 0x007F1D1Du, CDR_C_RED, CDR_C_WHITE);
                } else {
                    cdr_draw_btn(det_x + 16, ab_y, 120, 28, "BORRAR", 0x003A121Du, CDR_C_RED, CDR_C_RED);
                }
            } else {
                font_draw_string(det_x + 20, content_y + 30, "Selecciona un archivo de la lista.", CDR_C_DIM, 1);
            }
        }

    /* PESTANA 4: REGISTRO / AUDITORIA */
    } else if (cdr_tab_cur == CDR_TAB_LOG) {
        int i;
        uint32_t ly = content_y + 14;
        fb_fill_rounded_rect((int32_t)(win_x + 14), (int32_t)content_y, (int32_t)(win_w - 28), content_h, 6, CDR_C_PANEL);
        fb_draw_rounded_rect((int32_t)(win_x + 14), (int32_t)content_y, (int32_t)(win_w - 28), content_h, 6, CDR_C_BORDER);

        font_draw_string(win_x + 24, ly, "HISTORIAL Y BITACORA DEL PIPELINE:", CDR_C_CYAN, 1);
        ly += 20;

        for (i = 0; i < snap.log_n && (ly + 16) < (content_y + content_h / 2); ++i) {
            font_draw_string(win_x + 28, ly, snap.log[i], CDR_C_SOFT, 1);
            ly += 14;
        }
        if (snap.log_n == 0) {
            font_draw_string(win_x + 28, ly, "(Sin eventos registrados aun)", CDR_C_DIM, 1);
            ly += 16;
        }

        ly = content_y + content_h / 2 + 6;
        fb_fill_rect(win_x + 24, ly - 4, win_w - 48, 1, CDR_C_BORDER);
        font_draw_string(win_x + 24, ly, "INFORME DE REVISION ESTATICA (LINT):", CDR_C_AMBER, 1);
        ly += 20;

        for (i = 0; i < snap.lint_n && (ly + 16) < (content_y + content_h - 10); ++i) {
            uint32_t col = (snap.lint[i][0] == 'E') ? CDR_C_RED : CDR_C_AMBER;
            font_draw_string(win_x + 28, ly, snap.lint[i], col, 1);
            ly += 14;
        }
        if (snap.lint_n == 0) {
            font_draw_string(win_x + 28, ly, "Sin avisos de analisis estatico (codigo 100% limpio).", CDR_C_GREEN, 1);
        }
    }
}

/* ------------------------------------------------------------------------
 * Manejo de eventos de ratón y teclado
 * ------------------------------------------------------------------------ */
int coder_app_handle_click(int32_t mx, int32_t my, uint32_t win_x, uint32_t win_y, uint32_t win_w, uint32_t win_h)
{
    uint32_t tab_bar_y = win_y + 32;
    uint32_t content_y = tab_bar_y + 42;
    uint32_t content_h = (win_h > 140) ? (win_h - 140) : 100;
    struct cdr_snap snap;

    cdr_snapshot(&snap);

    /* Clics en la barra de pestañas */
    if (point_in_rect(mx, my, win_x + 14, tab_bar_y + 4, 130, 28)) {
        cdr_tab_cur = CDR_TAB_NEW; return 0;
    }
    if (point_in_rect(mx, my, win_x + 150, tab_bar_y + 4, 140, 28)) {
        cdr_tab_cur = CDR_TAB_MODIFY; return 0;
    }
    if (point_in_rect(mx, my, win_x + 296, tab_bar_y + 4, 140, 28)) {
        cdr_tab_cur = CDR_TAB_PROJECTS; cdr_scan_projects(); return 0;
    }
    if (point_in_rect(mx, my, win_x + 442, tab_bar_y + 4, 120, 28)) {
        cdr_tab_cur = CDR_TAB_LOG; return 0;
    }

    /* PESTANA 1: CREAR */
    if (cdr_tab_cur == CDR_TAB_NEW) {
        uint32_t box_y = content_y + 24;
        uint32_t box_w = win_w - 48;
        uint32_t f_y = content_y + 60;
        uint32_t chip_x = win_x + 420;
        uint32_t act_btn_y = content_y + 92;

        if (point_in_rect(mx, my, win_x + 24, box_y, box_w, 24)) {
            input_create_focus = 0; return 0;
        }
        if (point_in_rect(mx, my, win_x + 110, f_y, 220, 22)) {
            input_create_focus = 1; return 0;
        }

        /* Chips de plantillas */
        if (point_in_rect(mx, my, chip_x, f_y, 90, 22)) {
            cdr_copy(input_create_prompt, sizeof(input_create_prompt), "Crea una calculadora interactiva con botones y display");
            cdr_copy(input_create_file, sizeof(input_create_file), "/workspace/calc.soma");
            cdr_sync_inputs();
            return 0;
        }
        chip_x += 96;
        if (point_in_rect(mx, my, chip_x, f_y, 70, 22)) {
            cdr_copy(input_create_prompt, sizeof(input_create_prompt), "Crea un juego Pong arcade interactivo en ventana 2D");
            cdr_copy(input_create_file, sizeof(input_create_file), "/workspace/pong_game.soma");
            cdr_sync_inputs();
            return 0;
        }
        chip_x += 76;
        if (point_in_rect(mx, my, chip_x, f_y, 75, 22)) {
            cdr_copy(input_create_prompt, sizeof(input_create_prompt), "Crea un contador grafico con botones +1, -1 y reset");
            cdr_copy(input_create_file, sizeof(input_create_file), "/workspace/contador.soma");
            cdr_sync_inputs();
            return 0;
        }

        /* Botón de acción principal */
        if (point_in_rect(mx, my, win_x + 24, act_btn_y, 200, 30)) {
            if (snap.state != CODER_JOB_WORKING) coder_start_creation();
            return 0;
        }
        if (snap.state == CODER_JOB_WORKING && point_in_rect(mx, my, win_x + 232, act_btn_y, 110, 30)) {
            coder_cancel_job();
            return 0;
        }
        if (snap.state == CODER_JOB_SUCCESS && point_in_rect(mx, my, win_x + 232, act_btn_y, 140, 30)) {
            char cmd[128];
            cmd[0] = '\0';
            cdr_copy(cmd, sizeof(cmd), "app ");
            cdr_cat(cmd, sizeof(cmd), snap.target_file);
            dispatch_command(cmd, 0, 0);
            return 0;
        }
        if (snap.state == CODER_JOB_SUCCESS && point_in_rect(mx, my, win_x + 380, act_btn_y, 120, 30)) {
            cdr_load_mod_file(snap.target_file);
            cdr_tab_cur = CDR_TAB_MODIFY;
            return 0;
        }

    /* PESTANA 2: MODIFICAR */
    } else if (cdr_tab_cur == CDR_TAB_MODIFY) {
        uint32_t m_act_x = win_x + win_w - 360;
        uint32_t mod_in_y = content_y + 54;
        uint32_t btn_m_y = mod_in_y + 16;
        uint32_t cv_y = mod_in_y + 48;
        uint32_t cv_h = (content_h > 115) ? (content_h - 110) : 80;

        if (point_in_rect(mx, my, m_act_x, content_y + 12, 80, 24)) {
            char cmd[128];
            cmd[0] = '\0';
            cdr_copy(cmd, sizeof(cmd), "app ");
            cdr_cat(cmd, sizeof(cmd), mod_current_file);
            dispatch_command(cmd, 0, 0);
            return 0;
        }
        m_act_x += 86;
        if (point_in_rect(mx, my, m_act_x, content_y + 12, 85, 24)) {
            gui_open_notepad(mod_current_file);
            return 0;
        }
        m_act_x += 91;
        if (point_in_rect(mx, my, m_act_x, content_y + 12, 85, 24)) {
            int vr = cdr_verify(mod_current_file);
            if (vr > 0) {
                cdr_set_check("Compilacion OK (0 errores).", CDR_C_GREEN);
                mod_err_line = 0;
            } else if (vr == 0) {
                char chk[128];
                cdr_diag_text(chk, sizeof(chk));
                cdr_set_check(chk, CDR_C_RED);
                mod_err_line = (int)cdr_diag.line;
            } else {
                cdr_set_check("Archivo vacio o no encontrado.", CDR_C_AMBER);
            }
            return 0;
        }
        m_act_x += 91;
        if (point_in_rect(mx, my, m_act_x, content_y + 12, 80, 24)) {
            if (cdr_backup_restore(mod_current_file)) {
                cdr_load_mod_file(mod_current_file);
                cdr_set_check("Version previa restaurada.", CDR_C_GREEN);
            } else {
                cdr_set_check("No hay copia previa para este archivo.", CDR_C_AMBER);
            }
            return 0;
        }

        if (point_in_rect(mx, my, win_x + win_w - 165, btn_m_y, 145, 24)) {
            if (snap.state != CODER_JOB_WORKING) coder_start_modification();
            return 0;
        }
        if (point_in_rect(mx, my, win_x + 14, cv_y, win_w - 28, cv_h)) {
            mod_err_line = 0;
            return 0;
        }

    /* PESTANA 3: PROYECTOS */
    } else if (cdr_tab_cur == CDR_TAB_PROJECTS) {
        uint32_t list_w = (win_w > 360) ? 320 : (win_w - 28);
        uint32_t row_y = content_y + 26;
        int i;

        for (i = proj_scroll; i < project_count && (row_y + 22) < (content_y + content_h); ++i) {
            if (point_in_rect(mx, my, win_x + 16, row_y, list_w - 4, 22)) {
                project_selected = i;
                proj_del_armed = -1;
                cdr_load_mod_file(projects[i].path);
                return 0;
            }
            row_y += 22;
        }

        if (project_selected >= 0 && project_selected < project_count && win_w > 380) {
            uint32_t det_x = win_x + 14 + list_w + 10;
            uint32_t ab_y = content_y + 76;

            if (point_in_rect(mx, my, det_x + 16, ab_y, 120, 28)) {
                char cmd[128];
                cmd[0] = '\0';
                cdr_copy(cmd, sizeof(cmd), "app ");
                cdr_cat(cmd, sizeof(cmd), projects[project_selected].path);
                dispatch_command(cmd, 0, 0);
                return 0;
            }
            if (point_in_rect(mx, my, det_x + 144, ab_y, 120, 28)) {
                cdr_load_mod_file(projects[project_selected].path);
                cdr_tab_cur = CDR_TAB_MODIFY;
                return 0;
            }
            ab_y += 36;
            if (point_in_rect(mx, my, det_x + 16, ab_y, 120, 28)) {
                gui_open_notepad(projects[project_selected].path);
                return 0;
            }
            if (point_in_rect(mx, my, det_x + 144, ab_y, 120, 28)) {
                char dup[96];
                dup[0] = '\0';
                cdr_copy(dup, sizeof(dup), projects[project_selected].path);
                if (cdr_ends_with(dup, ".soma")) dup[cdr_len(dup) - 5] = '\0';
                cdr_cat(dup, sizeof(dup), "_copia.soma");
                int r = vfs_read(projects[project_selected].path, cdr_src, CDR_SRC_MAX - 1);
                if (r >= 0) {
                    vfs_write(dup, cdr_src, (uint32_t)r);
                    cdr_scan_projects();
                }
                return 0;
            }
            ab_y += 36;
            if (point_in_rect(mx, my, det_x + 16, ab_y, 120, 28) && proj_del_armed == -1) {
                proj_del_armed = project_selected;
                return 0;
            }
            if (proj_del_armed == project_selected && point_in_rect(mx, my, det_x + 16, ab_y, 248, 28)) {
                vfs_delete(projects[project_selected].path);
                proj_del_armed = -1;
                project_selected = -1;
                cdr_scan_projects();
                return 0;
            }
        }
    }
    return 0;
}

void coder_app_handle_key(char c)
{
    cdr_ensure_init();

    if (cdr_tab_cur == CDR_TAB_NEW) {
        if (c == '\t') {
            input_create_focus = !input_create_focus;
            return;
        }
        if (c == '\r' || c == '\n') {
            coder_start_creation();
            return;
        }
        if (input_create_focus == 0) {
            if (c == '\b' || (uint8_t)c == 127) {
                if (input_create_len > 0) input_create_prompt[--input_create_len] = '\0';
            } else if ((uint8_t)c >= 32 && (uint8_t)c < 127 && input_create_len + 1 < sizeof(input_create_prompt)) {
                input_create_prompt[input_create_len++] = c;
                input_create_prompt[input_create_len] = '\0';
            }
        } else {
            if (c == '\b' || (uint8_t)c == 127) {
                if (input_create_file_len > 0) input_create_file[--input_create_file_len] = '\0';
            } else if ((uint8_t)c >= 32 && (uint8_t)c < 127 && input_create_file_len + 1 < sizeof(input_create_file)) {
                input_create_file[input_create_file_len++] = c;
                input_create_file[input_create_file_len] = '\0';
            }
        }
    } else if (cdr_tab_cur == CDR_TAB_MODIFY) {
        if (c == 16) { if (mod_scroll > 0) mod_scroll--; return; }
        if (c == 14) { if (mod_scroll + mod_vis_lines < mod_total_lines) mod_scroll++; return; }
        if (c == '\r' || c == '\n') {
            coder_start_modification();
            return;
        }
        if (c == '\b' || (uint8_t)c == 127) {
            if (input_mod_len > 0) input_mod_prompt[--input_mod_len] = '\0';
        } else if ((uint8_t)c >= 32 && (uint8_t)c < 127 && input_mod_len + 1 < sizeof(input_mod_prompt)) {
            input_mod_prompt[input_mod_len++] = c;
            input_mod_prompt[input_mod_len] = '\0';
        }
    } else if (cdr_tab_cur == CDR_TAB_PROJECTS) {
        if (c == 16 && project_selected > 0) {
            project_selected--;
            proj_del_armed = -1;
            cdr_load_mod_file(projects[project_selected].path);
        }
        if (c == 14 && project_selected + 1 < project_count) {
            project_selected++;
            proj_del_armed = -1;
            cdr_load_mod_file(projects[project_selected].path);
        }
        if (c == '\r' || c == '\n') {
            if (project_selected >= 0 && project_selected < project_count) {
                char cmd[128];
                cmd[0] = '\0';
                cdr_copy(cmd, sizeof(cmd), "app ");
                cdr_cat(cmd, sizeof(cmd), projects[project_selected].path);
                dispatch_command(cmd, 0, 0);
            }
        }
    }
}
