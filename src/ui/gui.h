#ifndef SOMA_GUI_H
#define SOMA_GUI_H

#include <stdint.h>

int  cmd_vsoma(const char *args, char *out_buf, uint32_t max_out);
void gui_open_browser(const char *url);
void gui_open_file_manager(const char *folder);
void gui_open_memview(void);
void gui_open_layout(void);
void gui_open_agents(void);
void gui_open_notepad(const char *path);
void gui_open_paint(const char *path);
void gui_open_coder_app(void);

/* HER-AURA v4: razonamiento en vivo y streaming de la respuesta */
void gui_thought_update(const char *txt);
void gui_stream_begin(void);
void gui_stream_push(const char *chunk);

/*PL12_SOMA_WIN_H*/
/* API de Ventana Estandar SOMA-LANG */
int         gui_soma_win_open(const char *title, int w, int h);
int         gui_soma_win_is_active(void);
void        gui_soma_win_close(void);
void        gui_soma_win_on_close(void);
void        gui_soma_win_clear(uint32_t color);
void        gui_soma_win_rect(int x, int y, int w, int h, uint32_t color);
void        gui_soma_win_pixel(int x, int y, uint32_t color);
void        gui_soma_win_line(int x0, int y0, int x1, int y1, uint32_t color);
void        gui_soma_win_circle(int cx, int cy, int r, uint32_t color);
void        gui_soma_win_text(int x, int y, const char *str, uint32_t color);
void        gui_soma_win_present(void);
void        gui_soma_win_get_mouse(int *out_rx, int *out_ry, int *out_btn, int *out_inside, int *out_clicked);
int         gui_soma_win_get_key(void);
void        gui_soma_win_push_key(int key);
void        gui_soma_win_record_click(void);
void        gui_soma_win_get_size(int *out_w, int *out_h);
const char *gui_soma_win_get_title(void);
void        gui_soma_win_render_body(uint32_t wx, uint32_t wy, uint32_t ww, uint32_t wh);

#endif
