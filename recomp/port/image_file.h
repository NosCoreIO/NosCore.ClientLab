#pragma once
#include <stdint.h>

/* D3DXIMAGE_FILEFORMAT values. */
enum { IMAGE_BMP, IMAGE_JPG, IMAGE_TGA, IMAGE_PNG, IMAGE_DDS, IMAGE_PPM, IMAGE_DIB, IMAGE_HDR, IMAGE_PFM };

int image_file_write(const char* path, int format, int w, int h, const uint32_t* px);
