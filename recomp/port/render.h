#pragma once
#include "port.h"

void r_init(uint32_t backbuffer);
void r_set_render_target(uint32_t surface);
void r_set_transform(uint32_t type, uint32_t addr);
void r_get_transform(uint32_t type, uint32_t addr);
void r_set_render_state(uint32_t s, uint32_t v);
uint32_t r_get_render_state(uint32_t s);
void r_set_tss(uint32_t stage, uint32_t type, uint32_t v);
uint32_t r_get_tss(uint32_t stage, uint32_t type);
void r_set_texture(uint32_t stage, uint32_t tex);
uint32_t r_get_texture(uint32_t stage);
void r_set_stream(uint32_t vb, uint32_t offset, uint32_t stride);
void r_set_indices(uint32_t ib);
void r_set_fvf(uint32_t fvf);
void r_set_viewport(uint32_t vp);
void r_draw(uint32_t prim, uint32_t start, uint32_t count);
void r_draw_up(uint32_t prim, uint32_t count, uint32_t data, uint32_t stride);
void r_draw_indexed(uint32_t prim, int32_t base_vertex, uint32_t start_index, uint32_t count);
void r_draw_indexed_up(uint32_t prim, uint32_t count, uint32_t indices, uint32_t idx_fmt, uint32_t data, uint32_t stride);
void r_clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color);
void r_dump_bmp(uint32_t surface, const char* path);
void r_stats(void);
