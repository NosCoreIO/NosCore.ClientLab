/* Control channel for automated native runs (rt_control.c). Include after <windows.h>. */
#pragma once
#include <stdint.h>

void rt_log(const char* fmt, ...);
/* Reads the presented backbuffer of an IDirect3DDevice9 as 32-bit BGRA rows and hands them to sink. */
int rt_read_backbuffer(void* dev, void (*sink)(void* ctx, const uint8_t* bgra, UINT w, UINT h, int pitch), void* ctx);
int rt_backbuffer_size(void* dev, UINT* w, UINT* h);

void ctl_start(void);
void ctl_log(const char* line);
void ctl_set_window(HWND w);
void ctl_present(void* dev);
void* ctl_override(const char* dll, const char* name);
