#ifndef SOMA_CODER_APP_H
#define SOMA_CODER_APP_H

#include <stdint.h>

void coder_app_init(void);
void coder_app_render_window(uint32_t win_x, uint32_t win_y, uint32_t win_w, uint32_t win_h, int is_focused);
int  coder_app_handle_click(int32_t mx, int32_t my, uint32_t win_x, uint32_t win_y, uint32_t win_w, uint32_t win_h);
void coder_app_handle_key(char c);
void coder_app_notify_thought(const char *txt);
void coder_app_load_project(const char *path);

#endif
