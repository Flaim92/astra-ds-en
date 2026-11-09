/* tools/sec/gen_jpeg.c — генератор «злого» корпуса JPEG для фаззера декодера (tools/sec/fuzz_img.c).
 * Кодировщик — тот же libjpeg-turbo 3.2.1 (deps/prefix/lib/libjpeg.a, со своим jmemnobs).
 * Перебор: размеры 1..520 (в т.ч. за пределом 512), 1/3/4 компоненты, произвольные коэффициенты
 * дискретизации (1..4 у яркости, 1..2 у цветности), прогрессивный режим, lossless (SOF3, psv/pt),
 * интервал рестартов, оптимизация Хаффмана, качество 1..100, сглаживание.
 * Сборка и запуск — tools/sec/run_fuzz.sh. Только для стенда безопасности; в продукт не входит. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <jpeglib.h>

typedef struct { struct jpeg_error_mgr pub; jmp_buf jb; } Err;
static void e_exit(j_common_ptr c) { longjmp(((Err *)c->err)->jb, 1); }
static void e_out(j_common_ptr c) { (void)c; }

static unsigned s = 0x12345678u;
static unsigned rnd(void) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
static int pick(const int *a, int n) { return a[rnd() % (unsigned)n]; }

static int gen_one(const char *path, int idx) {
    static const int DIMS[] = { 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257, 500, 511, 512, 513, 520 };
    int W = (rnd() & 1) ? pick(DIMS, 25) : 1 + (int)(rnd() % 512);
    int H = (rnd() & 1) ? pick(DIMS, 25) : 1 + (int)(rnd() % 512);
    int nc = (rnd() % 10) == 0 ? 4 : (rnd() % 4) == 0 ? 1 : 3;
    int prog = (rnd() % 10) < 3, lossless = !prog && (rnd() % 7) == 0;
    struct jpeg_compress_struct c;
    Err e;
    c.err = jpeg_std_error(&e.pub);
    e.pub.error_exit = e_exit;
    e.pub.output_message = e_out;
    unsigned char *out = NULL;
    unsigned long outn = 0;
    unsigned char *row = malloc((size_t)W * 4);
    if (setjmp(e.jb)) { jpeg_destroy_compress(&c); free(row); free(out); return 0; }
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &out, &outn);
    c.image_width = (JDIMENSION)W;
    c.image_height = (JDIMENSION)H;
    c.input_components = nc;
    c.in_color_space = nc == 1 ? JCS_GRAYSCALE : nc == 3 ? JCS_RGB : JCS_CMYK;
    jpeg_set_defaults(&c);
    if (nc == 3 && (rnd() % 6) == 0) jpeg_set_colorspace(&c, JCS_RGB);   /* RGB без YCbCr (Adobe) */
    jpeg_set_quality(&c, 1 + (int)(rnd() % 100), TRUE);
    for (int i = 0; i < c.num_components; i++) {
        int mx = i == 0 ? 4 : 2;
        c.comp_info[i].h_samp_factor = 1 + (int)(rnd() % (unsigned)mx);
        c.comp_info[i].v_samp_factor = 1 + (int)(rnd() % (unsigned)mx);
    }
    if (rnd() % 3 == 0) c.restart_interval = (unsigned)(rnd() % 8);
    if (rnd() % 4 == 0) c.restart_in_rows = (int)(rnd() % 4);
    c.optimize_coding = (rnd() & 1) ? TRUE : FALSE;
    if (rnd() % 5 == 0) c.smoothing_factor = (int)(rnd() % 100);
    if (prog) jpeg_simple_progression(&c);
    if (lossless) jpeg_enable_lossless(&c, 1 + (int)(rnd() % 7), (int)(rnd() % 4));
    jpeg_start_compress(&c, TRUE);
    int mode = (int)(rnd() % 3);
    while (c.next_scanline < c.image_height) {
        int y = (int)c.next_scanline;
        for (int x = 0; x < W * nc; x++)
            row[x] = (unsigned char)(mode == 0 ? rnd() : mode == 1 ? (x * 255 / (W * nc) + y) : ((x / 8 + y / 8) & 1) * 255);
        JSAMPROW rp = row;
        jpeg_write_scanlines(&c, &rp, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    free(row);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(out, 1, outn, f); fclose(f); }
    free(out);
    (void)idx;
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "gen_jpeg <каталог> <сколько> [seed]\n"); return 2; }
    int n = atoi(argv[2]);
    if (argc > 3) s = (unsigned)strtoul(argv[3], NULL, 0);
    int ok = 0;
    char p[1024];
    for (int i = 0; i < n; i++) {
        snprintf(p, sizeof p, "%s/gen_%04d.jpg", argv[1], i);
        ok += gen_one(p, i);
    }
    printf("сгенерировано %d из %d\n", ok, n);
    return 0;
}
