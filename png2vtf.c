/*
 * png2vtf - convert a PNG to a Valve Texture Format (VTF) file.
 *
 * Default behaviour produces a full-color L4D2-compatible spray:
 * the input PNG (any dimensions, RGB or RGBA) is decoded to RGBA,
 * proportionally scaled to fit inside a 256x256 canvas, centered
 * with transparent padding, and written as BGRA8888 with no
 * mipmaps. The legacy alpha-only output is still available via
 * "--format a8".
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

/* Source image (after PNG decoding, always 8-bit RGBA). */
static int src_width, src_height;
static png_byte color_type;
static png_byte bit_depth;

static png_structp png_ptr;
static png_infop info_ptr;
static png_bytep *row_pointers;

/* Final canvas pixels written into the VTF (also 8-bit RGBA before
   per-format byte swizzling in convert_canvas_to_vtf). */
static unsigned char *canvas_rgba;
static int canvas_width, canvas_height;

static unsigned char *vtf_data;
static size_t vtf_data_size;
static VTFHEADER vtf_header;

/* Output formats supported by the --format flag. */
typedef enum {
    OUT_FMT_BGRA8888 = 0, /* default */
    OUT_FMT_RGBA8888,
    OUT_FMT_A8
} output_format_t;

/* How to fit the source image into the output canvas. */
typedef enum {
    FIT_CONTAIN = 0, /* default: preserve aspect, transparent padding */
    FIT_COVER,       /* preserve aspect, crop overflow */
    FIT_STRETCH      /* ignore aspect, resize directly */
} fit_mode_t;

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

    src_width = png_get_image_width(png_ptr, info_ptr);
    src_height = png_get_image_height(png_ptr, info_ptr);
    color_type = png_get_color_type(png_ptr, info_ptr);
    bit_depth = png_get_bit_depth(png_ptr, info_ptr);

    if (src_width <= 0 || src_height <= 0)
        abort_("[read_png_file] Image %dx%d has invalid dimensions",
               src_width, src_height);

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

    row_pointers = (png_bytep *) malloc(sizeof(png_bytep) * src_height);
    for (int y = 0; y < src_height; y++)
        row_pointers[y] = (png_byte *) malloc(png_get_rowbytes(png_ptr, info_ptr));

    png_read_image(png_ptr, row_pointers);

    fclose(fp);
}

static void free_row_pointers(void) {
    if (!row_pointers) return;
    for (int y = 0; y < src_height; y++)
        free(row_pointers[y]);
    free(row_pointers);
    row_pointers = NULL;
}

/* Pack the libpng row_pointers (already 8-bit RGBA) into a single
   contiguous buffer, so the resampler can do simple pointer math. */
static unsigned char *flatten_source_rgba(void) {
    size_t stride = (size_t) src_width * 4;
    unsigned char *buf = (unsigned char *) malloc(stride * (size_t) src_height);
    if (!buf) abort_("[flatten_source_rgba] out of memory");
    for (int y = 0; y < src_height; y++)
        memcpy(buf + (size_t) y * stride, row_pointers[y], stride);
    return buf;
}

/* Bilinear resize from src (sw x sh, RGBA8) to dst (dw x dh, RGBA8). */
static void bilinear_resize_rgba(const unsigned char *src, int sw, int sh,
                                 unsigned char *dst, int dw, int dh) {
    if (dw <= 0 || dh <= 0) return;

    if (sw == dw && sh == dh) {
        memcpy(dst, src, (size_t) sw * (size_t) sh * 4);
        return;
    }

    /* Map destination pixel centers to source pixel centers. When dw == 1
       or sw == 1 there is no meaningful axis, so collapse to a single
       sample to avoid division by zero. */
    double x_ratio = (dw > 1) ? ((double)(sw - 1) / (double)(dw - 1)) : 0.0;
    double y_ratio = (dh > 1) ? ((double)(sh - 1) / (double)(dh - 1)) : 0.0;

    for (int y = 0; y < dh; y++) {
        double sy = y_ratio * (double) y;
        int y0 = (int) sy;
        int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
        double fy = sy - (double) y0;

        for (int x = 0; x < dw; x++) {
            double sx = x_ratio * (double) x;
            int x0 = (int) sx;
            int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
            double fx = sx - (double) x0;

            const unsigned char *p00 = src + (((size_t) y0 * (size_t) sw + (size_t) x0) * 4);
            const unsigned char *p01 = src + (((size_t) y0 * (size_t) sw + (size_t) x1) * 4);
            const unsigned char *p10 = src + (((size_t) y1 * (size_t) sw + (size_t) x0) * 4);
            const unsigned char *p11 = src + (((size_t) y1 * (size_t) sw + (size_t) x1) * 4);

            double w00 = (1.0 - fx) * (1.0 - fy);
            double w01 = fx * (1.0 - fy);
            double w10 = (1.0 - fx) * fy;
            double w11 = fx * fy;

            unsigned char *d = dst + (((size_t) y * (size_t) dw + (size_t) x) * 4);
            for (int c = 0; c < 4; c++) {
                double v = p00[c] * w00 + p01[c] * w01 +
                           p10[c] * w10 + p11[c] * w11;
                if (v < 0.0) v = 0.0;
                if (v > 255.0) v = 255.0;
                d[c] = (unsigned char) (v + 0.5);
            }
        }
    }
}

/* Compose the final canvas_rgba (canvas_width x canvas_height, RGBA8)
   from the decoded source according to the chosen fit mode. */
static void build_canvas(int target_size, fit_mode_t fit) {
    canvas_width = target_size;
    canvas_height = target_size;

    size_t canvas_bytes = (size_t) canvas_width * (size_t) canvas_height * 4;
    canvas_rgba = (unsigned char *) calloc(1, canvas_bytes);
    if (!canvas_rgba) abort_("[build_canvas] out of memory");

    unsigned char *src_rgba = flatten_source_rgba();

    if (fit == FIT_STRETCH) {
        bilinear_resize_rgba(src_rgba, src_width, src_height,
                             canvas_rgba, canvas_width, canvas_height);
        free(src_rgba);
        return;
    }

    /* Determine the size of the resampled image inside the canvas. */
    int new_w, new_h;
    if (fit == FIT_CONTAIN) {
        double sx = (double) canvas_width / (double) src_width;
        double sy = (double) canvas_height / (double) src_height;
        double s = (sx < sy) ? sx : sy;
        new_w = (int) (src_width * s + 0.5);
        new_h = (int) (src_height * s + 0.5);
        if (new_w < 1) new_w = 1;
        if (new_h < 1) new_h = 1;
        if (new_w > canvas_width) new_w = canvas_width;
        if (new_h > canvas_height) new_h = canvas_height;
    } else { /* FIT_COVER */
        double sx = (double) canvas_width / (double) src_width;
        double sy = (double) canvas_height / (double) src_height;
        double s = (sx > sy) ? sx : sy;
        new_w = (int) (src_width * s + 0.5);
        new_h = (int) (src_height * s + 0.5);
        if (new_w < canvas_width) new_w = canvas_width;
        if (new_h < canvas_height) new_h = canvas_height;
    }

    unsigned char *resized =
        (unsigned char *) malloc((size_t) new_w * (size_t) new_h * 4);
    if (!resized) abort_("[build_canvas] out of memory");
    bilinear_resize_rgba(src_rgba, src_width, src_height,
                         resized, new_w, new_h);
    free(src_rgba);

    if (fit == FIT_CONTAIN) {
        int off_x = (canvas_width - new_w) / 2;
        int off_y = (canvas_height - new_h) / 2;
        for (int y = 0; y < new_h; y++) {
            unsigned char *dst_row =
                canvas_rgba + (((size_t)(y + off_y) * (size_t) canvas_width
                                + (size_t) off_x) * 4);
            const unsigned char *src_row =
                resized + ((size_t) y * (size_t) new_w * 4);
            memcpy(dst_row, src_row, (size_t) new_w * 4);
        }
    } else { /* FIT_COVER: crop center */
        int off_x = (new_w - canvas_width) / 2;
        int off_y = (new_h - canvas_height) / 2;
        for (int y = 0; y < canvas_height; y++) {
            const unsigned char *src_row =
                resized + ((((size_t)(y + off_y)) * (size_t) new_w
                            + (size_t) off_x) * 4);
            unsigned char *dst_row =
                canvas_rgba + ((size_t) y * (size_t) canvas_width * 4);
            memcpy(dst_row, src_row, (size_t) canvas_width * 4);
        }
    }

    free(resized);
}

static void convert_canvas_to_vtf(output_format_t fmt) {
    printf("output=%dx%d (from input %dx%d)\n",
           canvas_width, canvas_height, src_width, src_height);

    size_t pixels = (size_t) canvas_width * (size_t) canvas_height;
    size_t bpp = (fmt == OUT_FMT_A8) ? 1 : 4;
    vtf_data_size = pixels * bpp;
    vtf_data = (unsigned char *) malloc(vtf_data_size);
    if (!vtf_data)
        abort_("[convert_canvas_to_vtf] out of memory");

    for (int y = 0; y < canvas_height; y++) {
        for (int x = 0; x < canvas_width; x++) {
            const unsigned char *ptr =
                canvas_rgba + (((size_t) y * (size_t) canvas_width
                                + (size_t) x) * 4);
            unsigned char r = ptr[0];
            unsigned char g = ptr[1];
            unsigned char b = ptr[2];
            unsigned char a = ptr[3];

            size_t i = ((size_t) y * (size_t) canvas_width + (size_t) x) * bpp;
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
    vtf_header.width = (unsigned short) canvas_width;
    vtf_header.height = (unsigned short) canvas_height;
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
        "Usage: %s [options] <input.png> <output.vtf>\n"
        "\n"
        "Options:\n"
        "  --size <pixels>    Output canvas size (must be a power of two,\n"
        "                     e.g. 64, 128, 256, 512). Default: 256.\n"
        "  --fit <mode>       How to fit the source image into the canvas:\n"
        "                       contain (default) - preserve aspect, transparent padding\n"
        "                       cover             - preserve aspect, crop overflow\n"
        "                       stretch           - resize to canvas, may distort\n"
        "  --format <fmt>     VTF pixel format:\n"
        "                       bgra8888 (default) - full-color L4D2 spray\n"
        "                       rgba8888           - full-color, alternate channel order\n"
        "                       a8                 - legacy alpha-only\n"
        "  -h, --help         Show this help.\n"
        "\n"
        "By default an arbitrary PNG is decoded to RGBA, scaled to fit\n"
        "inside a 256x256 canvas with transparent padding, and written\n"
        "as a full-color BGRA8888 VTF that L4D2 accepts as a spray.\n",
        prog);
}

int main(int argc, char **argv) {
    output_format_t fmt = OUT_FMT_BGRA8888;
    fit_mode_t fit = FIT_CONTAIN;
    int target_size = 256;
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
        } else if (strcmp(arg, "--fit") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                abort_("--fit requires an argument");
            }
            const char *v = argv[++i];
            if (strcmp(v, "contain") == 0) fit = FIT_CONTAIN;
            else if (strcmp(v, "cover") == 0) fit = FIT_COVER;
            else if (strcmp(v, "stretch") == 0) fit = FIT_STRETCH;
            else {
                usage(argv[0]);
                abort_("Unknown --fit value: %s", v);
            }
        } else if (strcmp(arg, "--size") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                abort_("--size requires an argument");
            }
            const char *v = argv[++i];
            char *end = NULL;
            long n = strtol(v, &end, 10);
            if (!end || *end != '\0' || n <= 0 || n > 4096) {
                usage(argv[0]);
                abort_("Invalid --size value: %s", v);
            }
            if (!is_power_of_two((int) n)) {
                usage(argv[0]);
                abort_("--size must be a power of two (e.g. 64, 128, 256, 512), got %ld", n);
            }
            target_size = (int) n;
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
    build_canvas(target_size, fit);
    free_row_pointers();
    convert_canvas_to_vtf(fmt);
    free(canvas_rgba);
    canvas_rgba = NULL;
    write_vtf_file(out_path, fmt);

    return 0;
}
