#include <png.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int width = 4;
    int height = 4;
    FILE *fp = fopen("experiments/test.png", "wb");
    if (!fp) return 1;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    png_init_io(png, fp);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    png_byte row[16];
    for (int x = 0; x < 4; x++) {
        row[x*4+0] = 10*x;
        row[x*4+1] = 20*x;
        row[x*4+2] = 30*x;
        row[x*4+3] = 200; // alpha
    }
    for (int y = 0; y < height; y++) {
        png_write_row(png, row);
    }
    png_write_end(png, NULL);
    fclose(fp);
    png_destroy_write_struct(&png, &info);
    return 0;
}
