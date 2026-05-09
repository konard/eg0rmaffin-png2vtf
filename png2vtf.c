/*
 * png2vtf - convert a PNG to a Valve Texture Format (VTF) file.
 *
 * Default behaviour produces a full-color L4D2-compatible spray
 * (BGRA8888, no mipmaps). The legacy alpha-only output is still
 * available via "--format a8".
 *
 * Originally based on a libpng example by Guillaume Cottenceau,
 * adapted by Maxime Biais (http://twitter.com/maximeb).
 *
 * This software may be freely redistributed under the terms
 * of the X11 license.
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#define PNG_DEBUG 3
#include <png.h>
#include "vtf_format.h"

static void abort_(const char *s, ...) {
    va_list args;
    va_start(args, s);
    vfprintf(stderr, s, args);
    fprintf(stderr, "\n");
    va_end(args);
    exit(1);
}

static int width, height;
static png_byte color_type;
static png_byte bit_depth;

static png_structp png_ptr;
static png_infop info_ptr;
static png_bytep *row_pointers;

static unsigned char *vtf_data;
static size_t vtf_data_size;
static VTFHEADER vtf_header;

/* Output formats supported by the --format flag. */
typedef enum {
    OUT_FMT_BGRA8888 = 0, /* default */
    OUT_FMT_RGBA8888,
    OUT_FMT_A8
} output_format_t;

static int is_power_of_two(int v) {
    return v > 0 && (v & (v - 1)) == 0;
}

static void read_png_file(const char *file_name) {
    png_byte header[8];

    FILE *fp = fopen(file_name, "rb");
    if (!fp)
        abort_("[read_png_file] File %s could not be opened for reading", file_name);
    if (fread(header, 1, 8, fp) != 8)
        abort_("[read_png_file] File %s could not be read", file_name);
    if (png_sig_cmp(header, 0, 8))
        abort_("[read_png_file] File %s is not recognized as a PNG file", file_name);

    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr)
        abort_("[read_png_file] png_create_read_struct failed");

    info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr)
        abort_("[read_png_file] png_create_info_struct failed");

    if (setjmp(png_jmpbuf(png_ptr)))
        abort_("[read_png_file] Error during init_io");

    png_init_io(png_ptr, fp);
    png_set_sig_bytes(png_ptr, 8);

    png_read_info(png_ptr, info_ptr);

    width = png_get_image_width(png_ptr, info_ptr);
    height = png_get_image_height(png_ptr, info_ptr);
    color_type = png_get_color_type(png_ptr, info_ptr);
    bit_depth = png_get_bit_depth(png_ptr, info_ptr);

    if (!is_power_of_two(width) || !is_power_of_two(height))
        abort_("[read_png_file] Image %dx%d: width and height must be powers of two "
               "(e.g. 256x256, 512x512)", width, height);

    /* Normalise everything to 8-bit RGBA so the rest of the code can
       treat each pixel as 4 bytes regardless of the input PNG layout. */
    if (bit_depth == 16)
        png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);
    /* For RGB (no alpha), append a fully opaque alpha channel. */
    png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);

    (void) png_set_interlace_handling(png_ptr);
    png_read_update_info(png_ptr, info_ptr);

    if (setjmp(png_jmpbuf(png_ptr)))
        abort_("[read_png_file] Error during read_image");

    row_pointers = (png_bytep *) malloc(sizeof(png_bytep) * height);
    for (int y = 0; y < height; y++)
        row_pointers[y] = (png_byte *) malloc(png_get_rowbytes(png_ptr, info_ptr));

    png_read_image(png_ptr, row_pointers);

    fclose(fp);
}

static void free_row_pointers(void) {
    if (!row_pointers) return;
    for (int y = 0; y < height; y++)
        free(row_pointers[y]);
    free(row_pointers);
    row_pointers = NULL;
}

static void convert_file(output_format_t fmt) {
    printf("height=%d, width=%d\n", height, width);

    size_t pixels = (size_t) width * (size_t) height;
    size_t bpp = (fmt == OUT_FMT_A8) ? 1 : 4;
    vtf_data_size = pixels * bpp;
    vtf_data = (unsigned char *) malloc(vtf_data_size);
    if (!vtf_data)
        abort_("[convert_file] out of memory");

    for (int y = 0; y < height; y++) {
        png_byte *row = row_pointers[y];
        for (int x = 0; x < width; x++) {
            png_byte *ptr = &row[x * 4]; /* row is RGBA after read_png_file */
            unsigned char r = ptr[0];
            unsigned char g = ptr[1];
            unsigned char b = ptr[2];
            unsigned char a = ptr[3];

            size_t i = ((size_t) y * (size_t) width + (size_t) x) * bpp;
            switch (fmt) {
            case OUT_FMT_BGRA8888:
                vtf_data[i + 0] = b;
                vtf_data[i + 1] = g;
                vtf_data[i + 2] = r;
                vtf_data[i + 3] = a;
                break;
            case OUT_FMT_RGBA8888:
                vtf_data[i + 0] = r;
                vtf_data[i + 1] = g;
                vtf_data[i + 2] = b;
                vtf_data[i + 3] = a;
                break;
            case OUT_FMT_A8:
                vtf_data[i] = a;
                break;
            }
        }
    }
}

static void init_vtf_header(output_format_t fmt) {
    memset(&vtf_header, 0, sizeof(vtf_header));

    vtf_header.signature[0] = 'V';
    vtf_header.signature[1] = 'T';
    vtf_header.signature[2] = 'F';
    vtf_header.signature[3] = 0;
    vtf_header.version[0] = 7;
    vtf_header.version[1] = 2;
    vtf_header.headerSize = sizeof(VTFHEADER); /* 80 */
    vtf_header.width = (unsigned short) width;
    vtf_header.height = (unsigned short) height;
    vtf_header.frames = 1;
    vtf_header.firstFrame = 0;
    vtf_header.reflectivity[0] = 1.0f;
    vtf_header.reflectivity[1] = 1.0f;
    vtf_header.reflectivity[2] = 1.0f;
    vtf_header.bumpmapScale = 1.0f;
    vtf_header.mipmapCount = 1;
    /* No low-res thumbnail. */
    vtf_header.lowResImageFormat = (unsigned int) IMAGE_FORMAT_NONE; /* 0xFFFFFFFF */
    vtf_header.lowResImageWidth = 0;
    vtf_header.lowResImageHeight = 0;
    vtf_header.depth = 1;

    /* Sensible flags for a 2D spray decal. */
    vtf_header.flags = TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD |
                       TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT |
                       TEXTUREFLAGS_EIGHTBITALPHA;

    switch (fmt) {
    case OUT_FMT_BGRA8888:
        vtf_header.highResImageFormat = IMAGE_FORMAT_BGRA8888;
        break;
    case OUT_FMT_RGBA8888:
        vtf_header.highResImageFormat = IMAGE_FORMAT_RGBA8888;
        break;
    case OUT_FMT_A8:
        vtf_header.highResImageFormat = IMAGE_FORMAT_A8;
        /* A8 has no real RGB color, so no eight-bit-alpha hint flag. */
        vtf_header.flags = TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD |
                           TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT;
        break;
    }
}

static void write_vtf_header(FILE *fp, const VTFHEADER *h) {
    fwrite(h, sizeof(*h), 1, fp);
}

static void write_vtf_file(const char *file_name, output_format_t fmt) {
    init_vtf_header(fmt);

    FILE *fp = fopen(file_name, "wb");
    if (!fp)
        abort_("File %s could not be opened for writing", file_name);

    write_vtf_header(fp, &vtf_header);
    fwrite(vtf_data, 1, vtf_data_size, fp);
    free(vtf_data);
    vtf_data = NULL;
    fclose(fp);
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [--format <bgra8888|rgba8888|a8>] <input.png> <output.vtf>\n"
        "\n"
        "Default format is bgra8888 (full-color, L4D2-compatible spray).\n",
        prog);
}

int main(int argc, char **argv) {
    output_format_t fmt = OUT_FMT_BGRA8888;
    const char *in_path = NULL;
    const char *out_path = NULL;

    int positional = 0;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--format") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                abort_("--format requires an argument");
            }
            const char *v = argv[++i];
            if (strcmp(v, "bgra8888") == 0) fmt = OUT_FMT_BGRA8888;
            else if (strcmp(v, "rgba8888") == 0) fmt = OUT_FMT_RGBA8888;
            else if (strcmp(v, "a8") == 0) fmt = OUT_FMT_A8;
            else {
                usage(argv[0]);
                abort_("Unknown --format value: %s", v);
            }
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (positional == 0) {
            in_path = arg;
            positional++;
        } else if (positional == 1) {
            out_path = arg;
            positional++;
        } else {
            usage(argv[0]);
            abort_("Unexpected argument: %s", arg);
        }
    }

    if (!in_path || !out_path) {
        usage(argv[0]);
        abort_("Missing input or output file");
    }

    read_png_file(in_path);
    convert_file(fmt);
    write_vtf_file(out_path, fmt);
    free_row_pointers();

    return 0;
}
