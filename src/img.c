/* Аватарки и картинки Discord как цветной ASCII (см. img.h).
 *
 * Безопасность. Всё из сети — враждебно: адреса собираются только из проверенных частей (id — цифры,
 * хэш — hex, имя файла — [A-Za-z0-9._-] и %XX), хосты — только cdn.discordapp.com и
 * media.discordapp.net, только https, без перенаправлений, без заголовка Authorization, ответ
 * <= 256 КБ. JPEG разбирает libjpeg-turbo (проверяется OSS-Fuzz) с ограничениями: <= 512x512,
 * 8 бит, 1 или 3 компоненты, <= 64 сканов, <= 32 предупреждений, <= 1 МБ памяти; ошибки — через
 * setjmp/longjmp, без вывода. Память libjpeg берётся через наш системный слой (jpeg_get_small и
 * др. ниже — вместо jmemnobs.c из библиотеки): каждый блок затирается при освобождении.
 *
 * Нулевое логирование: ни файлов, ни вывода; ошибки только превращаются в «нет картинки».
 *
 * Память: скачанные байты (обычно 0,5–3 КБ) и строка пикселей (<= 1,5 КБ) живут только во время
 * разбора и затираются; разбор идёт построчно, накапливая лишь две строки «четвертей клеток».
 * В кэше — только готовые клетки (7 байт на клетку). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <curl/curl.h>
#include <jpeglib.h>
#include <jerror.h>
#include "mc.h"
#include "img.h"

void net_curl_common(CURL *e);                  /* net.c: TLS 1.2+, проверка сертификата, UA, CA */

#define DL_MAX (256u << 10)                     /* предел ответа CDN (JPEG 128-512 px обычно 5-60 КБ) */
#define DEC_MAX_DIM 512                         /* предел картинки до масштабирования */
#define DEC_MEM_MAX (1u << 20)                  /* жёсткий предел памяти libjpeg на разбор: прогрессивный 512x256 4:4:4 ~ 0,8 МБ */
#define DEC_MAX_SCANS 64                        /* прогрессивный JPEG: не больше сканов */
#define DEC_MAX_WARN 32                         /* битые данные: не больше предупреждений */

/* ======================= память libjpeg: затирание при освобождении =======================
 * Эти функции заменяют jmemnobs.c из libjpeg.a (тот же интерфейс jmemsys.h): компоновщик берёт
 * наши, и объект jmemnobs.o из архива не подтягивается. Размер блока храним сами (не доверяем
 * размеру при освобождении). Предел — DEC_MEM_MAX на все блоки сразу. */
typedef struct { size_t n, pad; } JHdr;         /* 16 байт: выравнивание данных сохраняется; pad 1 — блок потока со своим пределом */
static size_t s_jmem, s_jpeak;
/* Поток со своим пределом (камера «Astra DS Video»: разбор MJPG 720p в своём потоке): свой счёт, общий предел
 * картинок не трогается и не делится между потоками. */
static _Thread_local size_t t_jlim, t_jmem;
void img_jpeg_thread_limit(size_t max);
void img_jpeg_thread_limit(size_t max) { t_jlim = max; }

static void *jm_get(size_t n) {
    if (t_jlim) {
        if (n > t_jlim || t_jmem + n > t_jlim) return NULL;
        JHdr *h = malloc(sizeof(JHdr) + n);
        if (!h) return NULL;
        h->n = n; h->pad = 1;
        t_jmem += n;
        return h + 1;
    }
    if (n > DEC_MEM_MAX || s_jmem + n > DEC_MEM_MAX) return NULL;   /* libjpeg: «нет памяти» -> отказ */
    JHdr *h = malloc(sizeof(JHdr) + n);
    if (!h) return NULL;
    h->n = n; h->pad = 0;
    s_jmem += n;
    if (s_jmem > s_jpeak) s_jpeak = s_jmem;
    return h + 1;
}
static void jm_free(void *p) {
    if (!p) return;
    JHdr *h = (JHdr *)p - 1;
    if (h->pad == 1) t_jmem -= h->n;
    else s_jmem -= h->n;
    wipe(p, h->n);
    free(h);
}
void *jpeg_get_small(j_common_ptr c, size_t n);
void jpeg_free_small(j_common_ptr c, void *p, size_t n);
void *jpeg_get_large(j_common_ptr c, size_t n);
void jpeg_free_large(j_common_ptr c, void *p, size_t n);
size_t jpeg_mem_available(j_common_ptr c, size_t min_need, size_t max_need, size_t already);
struct backing_store_struct;
void jpeg_open_backing_store(j_common_ptr c, struct backing_store_struct *info, long total);
long jpeg_mem_init(j_common_ptr c);
void jpeg_mem_term(j_common_ptr c);

void *jpeg_get_small(j_common_ptr c, size_t n) { (void)c; return jm_get(n); }
void jpeg_free_small(j_common_ptr c, void *p, size_t n) { (void)c; (void)n; jm_free(p); }
void *jpeg_get_large(j_common_ptr c, size_t n) { (void)c; return jm_get(n); }
void jpeg_free_large(j_common_ptr c, void *p, size_t n) { (void)c; (void)n; jm_free(p); }
size_t jpeg_mem_available(j_common_ptr c, size_t min_need, size_t max_need, size_t already) {
    (void)c; (void)min_need; (void)already;
    size_t lim = t_jlim ? t_jlim : DEC_MEM_MAX, used = t_jlim ? t_jmem : s_jmem;
    size_t left = used < lim ? lim - used : 0;
    return max_need < left ? max_need : left;
}
void jpeg_open_backing_store(j_common_ptr c, struct backing_store_struct *info, long total) {
    (void)info; (void)total;
    c->err->msg_code = JERR_NO_BACKING_STORE;   /* временных файлов не бывает: отказ */
    c->err->error_exit(c);
}
long jpeg_mem_init(j_common_ptr c) { (void)c; return DEC_MEM_MAX; }
void jpeg_mem_term(j_common_ptr c) { (void)c; }

/* ======================= разбор JPEG в клетки ======================= */
typedef struct { u32 r, g, b, n; } Acc;         /* сумма цвета одной из 8 частей клетки */
typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
    int warns;
} JErr;
typedef struct {
    struct jpeg_decompress_struct ci;
    JErr je;
    struct jpeg_progress_mgr pg;
    bool created;
    Acc *acc;                                   /* четыре строки частей (2 x 4 на клетку) — одна строка клеток */
    JSAMPLE row[DEC_MAX_DIM * 3];
    int xs[2 * IMG_MAX_W + 1];
} Dec;

static void j_exit(j_common_ptr c) { longjmp(((JErr *)c->err)->jb, 1); }
static void j_output(j_common_ptr c) { (void)c; }          /* никакого вывода */
static void j_emit(j_common_ptr c, int level) {
    JErr *e = (JErr *)c->err;
    if (level < 0 && ++e->warns > DEC_MAX_WARN) longjmp(e->jb, 1);
}
static void j_progress(j_common_ptr c) {
    if (c->is_decompressor && ((j_decompress_ptr)c)->input_scan_number > DEC_MAX_SCANS)
        longjmp(((JErr *)c->err)->jb, 1);
}

/* ---------- клетка из 8 частей (2 x 4 пикселя) ----------
 * На экране (особенно на мелкой сетке) глаз видит не форму буквы, а смесь цветов: часть i клетки
 * выглядит как bg + c_i * (fg - bg), где c_i — доля закраски этой части символом. Поэтому символ
 * подбирается по таблице закраски шрифта (2 x 4 части), а цвета символа и фона — методом наименьших
 * квадратов, чтобы смесь повторяла 8 цветов картинки: и средний цвет, и форма (край, точка, линия)
 * сохраняются. Таблица снята с Consolas (шрифт окна на Windows); у misc-fixed (Linux) пропорции
 * близки. */
/* Consolas, доля закраски 2 x 4 частей символа (слева направо, сверху вниз), 0..255;
 * снято со шрифта на 96 пт (обвязка gen_cov.py, Pillow). */
static const u8 GLYPH_COV[95][8] = {
    {   0,   0,   0,   0,   0,   0,   0,   0 }, /* ' ' */
    {  32,  35,  37,  42,  31,  34,   1,   1 }, /* '!' */
    {  68,  66,  13,  12,   0,   0,   0,   0 }, /* '"' */
    {  36,  35, 104, 107, 110, 102,   0,   0 }, /* '#' */
    {  60,  86, 104,  75, 100, 109,  30,   0 }, /* '$' */
    { 112,  57,  84,  61,  76, 149,   0,   3 }, /* '%' */
    {  74,  61, 128,  82, 112, 159,   3,   0 }, /* '&' */
    {  33,  37,   6,   7,   0,   0,   0,   0 }, /* ''' */
    {  17,  54,  81,   1,  81,   4,  12,  52 }, /* '(' */
    {  54,  20,   1,  80,   2,  78,  49,  13 }, /* ')' */
    {  51,  56,  61,  66,   0,   0,   0,   0 }, /* '*' */
    {   0,   0,  75,  80,  46,  51,   0,   0 }, /* '+' */
    {   0,   0,   0,   0,  32,  38,  47,  21 }, /* ',' */
    {   0,   0,  32,  33,  11,  11,   0,   0 }, /* '-' */
    {   0,   0,   0,   0,  37,  36,   1,   1 }, /* '.' */
    {   0,  55,  19,  55,  75,   1,  31,   0 }, /* '/' */
    {  58,  61, 113, 127, 113,  92,   2,   2 }, /* '0' */
    {  56,  36,  26,  67,  68, 107,   0,   0 }, /* '1' */
    {  54,  58,   1,  86, 116,  78,   0,   0 }, /* '2' */
    {  48,  57,  30,  95,  47,  91,   6,   0 }, /* '3' */
    {  12,  66,  80,  80,  84, 124,   0,   0 }, /* '4' */
    {  67,  47,  82,  58,  45,  90,   6,   0 }, /* '5' */
    {  41,  49, 112,  66,  96,  95,   2,   2 }, /* '6' */
    {  63,  80,   2,  80,  70,  19,   0,   0 }, /* '7' */
    {  59,  63, 103, 100, 107, 104,   2,   2 }, /* '8' */
    {  61,  57, 103, 110,  55,  76,   0,   0 }, /* '9' */
    {   0,   0,  31,  34,  30,  33,   1,   1 }, /* ':' */
    {   0,   0,  31,  34,  29,  41,  44,  24 }, /* ';' */
    {   0,   7,  60,  47,  47,  53,   0,   0 }, /* '<' */
    {   0,   0,  57,  57,  57,  57,   0,   0 }, /* '=' */
    {   7,   0,  46,  61,  52,  48,   0,   0 }, /* '>' */
    {  36,  59,  34,  73,  56,  10,   2,   0 }, /* '?' */
    {  56,  71, 118, 131, 139, 127,  73,  29 }, /* '@' */
    {  38,  41,  81,  83, 119, 120,   0,   0 }, /* 'A' */
    {  74,  61, 111, 105, 111,  98,   0,   0 }, /* 'B' */
    {  54,  63,  88,   0, 102,  58,   0,   4 }, /* 'C' */
    {  80,  56,  82,  84, 119,  92,   0,   0 }, /* 'D' */
    {  68,  50, 105,  47, 106,  52,   0,   0 }, /* 'E' */
    {  69,  52, 106,  46,  84,   0,   0,   0 }, /* 'F' */
    {  56,  64,  88,  67, 101, 108,   1,   4 }, /* 'G' */
    {  44,  43, 119, 117,  82,  79,   0,   0 }, /* 'H' */
    {  61,  62,  38,  44,  79,  84,   0,   0 }, /* 'I' */
    {  49,  62,   0,  80,  51,  83,   3,   0 }, /* 'J' */
    {  44,  49, 138,  33,  91,  88,   0,   0 }, /* 'K' */
    {  45,   0,  83,   0, 103,  58,   0,   0 }, /* 'L' */
    {  65,  65, 136, 131,  81,  81,   0,   0 }, /* 'M' */
    {  73,  40, 132,  99,  77, 140,   0,   0 }, /* 'N' */
    {  61,  66,  86,  83, 104,  97,   2,   2 }, /* 'O' */
    {  74,  61, 104,  98,  90,   4,   0,   0 }, /* 'P' */
    {  62,  67,  86,  84, 105,  97,  14,  84 }, /* 'Q' */
    {  72,  57, 110,  91,  82,  87,   0,   0 }, /* 'R' */
    {  62,  48,  88,  48,  59, 102,   4,   1 }, /* 'S' */
    {  74,  75,  38,  44,  38,  44,   0,   0 }, /* 'T' */
    {  44,  43,  82,  79, 101,  95,   2,   2 }, /* 'U' */
    {  50,  45,  89,  80,  80,  73,   0,   0 }, /* 'V' */
    {  41,  38, 107, 104, 131, 130,   0,   0 }, /* 'W' */
    {  52,  48,  79,  78,  96,  95,   0,   0 }, /* 'X' */
    {  53,  48,  86,  80,  38,  44,   0,   0 }, /* 'Y' */
    {  61,  82,  25,  66, 128,  66,   0,   0 }, /* 'Z' */
    {  69,  34,  76,   0,  76,   0,  63,  33 }, /* '[' */
    {  57,   0,  52,  23,   0,  73,   0,  29 }, /* '\\' */
    {  33,  69,   0,  74,   0,  74,  32,  63 }, /* ']' */
    {  31,  33,  50,  53,   0,   0,   0,   0 }, /* '^' */
    {   0,   0,   0,   0,   0,   0,  69,  68 }, /* '_' */
    {  39,   5,   0,   0,   0,   0,   0,   0 }, /* '`' */
    {   0,   0,  58, 109, 117, 118,   3,   0 }, /* 'a' */
    {  60,   0, 114, 100, 103,  90,   3,   1 }, /* 'b' */
    {   0,   0,  87,  51,  94,  47,   1,   4 }, /* 'c' */
    {   0,  58,  90, 107, 101, 109,   3,   0 }, /* 'd' */
    {   0,   0, 104, 109, 112,  71,   1,   4 }, /* 'e' */
    {  35,  72, 112,  66,  70,  11,   0,   0 }, /* 'f' */
    {   0,   0,  94, 116, 131,  89,  89,  84 }, /* 'g' */
    {  60,   0, 113,  96,  80,  77,   0,   0 }, /* 'h' */
    {  24,  27,  62,  52,  67,  92,   0,   0 }, /* 'i' */
    {   0,  51,  50,  88,   0,  78,  57,  57 }, /* 'j' */
    {  60,   0, 103,  71,  96,  85,   0,   0 }, /* 'k' */
    {  57,  41,  24,  55,  67,  92,   0,   0 }, /* 'l' */
    {   0,   0, 134, 127, 107, 110,   0,   0 }, /* 'm' */
    {   0,   0, 107,  97,  80,  77,   0,   0 }, /* 'n' */
    {   0,   0,  95,  98, 100,  95,   2,   2 }, /* 'o' */
    {   0,   0, 109, 100, 106,  90,  63,   1 }, /* 'p' */
    {   0,   0,  90, 105, 101, 111,   3,  59 }, /* 'q' */
    {   0,   0, 103,  88,  81,   0,   0,   0 }, /* 'r' */
    {   0,   0, 102,  52,  58, 110,   4,   2 }, /* 's' */
    {  43,   0, 121,  52,  74,  51,   0,   5 }, /* 't' */
    {   0,   0,  77,  74,  99, 109,   3,   0 }, /* 'u' */
    {   0,   0,  85,  79,  76,  72,   0,   0 }, /* 'v' */
    {   0,   0,  93,  92, 127, 127,   0,   0 }, /* 'w' */
    {   0,   0,  86,  82,  89,  89,   0,   0 }, /* 'x' */
    {   0,   0,  85,  79,  75,  75,  86,  11 }, /* 'y' */
    {   0,   0,  54, 104, 111,  61,   0,   0 }, /* 'z' */
    {  28,  57,  84,  13,  59,  20,  24,  54 }, /* '{' */
    {  34,  40,  34,  40,  34,  40,  26,  30 }, /* '|' */
    {  54,  32,   9,  87,  13,  62,  51,  28 }, /* '}' */
    {   0,   0,  71,  35,  18,  54,   0,   0 }, /* '~' */
};
#define NG 95                                   /* символы 0x20..0x7E */
/* Ровная клетка: символ по яркости из рампы со слабым разносом цвета — текстура ASCII без потери
 * среднего цвета (закраска символа берётся из той же таблицы). */
static const char RAMP[10] = { ' ', '.', ':', '-', '=', '+', '*', '#', '%', '@' };
#define UNIFORM_T 20                            /* разброс яркости частей, ниже — клетка ровная */
#define RAMP_D 24                               /* разнос цвета символа и фона в ровной клетке */

static int lum(int r, int g, int b) { return (77 * r + 150 * g + 29 * b) >> 8; }
static u8 clamp8(double v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5)); }
static double glyph_mean(int g) {
    int s = 0;
    for (int i = 0; i < 8; i++) s += GLYPH_COV[g][i];
    return s / (8.0 * 255.0);
}

/* Подбор цветов под символ g. fix == NULL — оба цвета свободны; иначе один из них закреплён за
 * fix (which: 1 — фон = fix, 2 — символ = fix). Возвращает ошибку (сумму квадратов по 8 частям). */
static double fit_glyph(const double p[8][3], int g, const u8 *fix, int which, double fg[3], double bg[3]) {
    const u8 *cv = GLYPH_COV[g];
    double c[8], cm = 0;
    for (int i = 0; i < 8; i++) { c[i] = cv[i] / 255.0; cm += c[i]; }
    cm /= 8;
    double scc = 0;
    for (int i = 0; i < 8; i++) scc += (c[i] - cm) * (c[i] - cm);
    for (int k = 0; k < 3; k++) {
        if (which == 1) {                       /* фон закреплён: p = fix + c (fg - fix) */
            double num = 0, den = 0;
            for (int i = 0; i < 8; i++) { num += c[i] * (p[i][k] - fix[k]); den += c[i] * c[i]; }
            bg[k] = fix[k];
            fg[k] = den > 1e-9 ? fix[k] + num / den : fix[k];
        } else if (which == 2) {                /* символ закреплён: p = bg (1 - c) + c fix */
            double num = 0, den = 0;
            for (int i = 0; i < 8; i++) { num += (1 - c[i]) * (p[i][k] - c[i] * fix[k]); den += (1 - c[i]) * (1 - c[i]); }
            fg[k] = fix[k];
            bg[k] = den > 1e-9 ? num / den : fix[k];
        } else {                                /* оба свободны: прямая регрессия p по c */
            double pm = 0, scp = 0;
            for (int i = 0; i < 8; i++) pm += p[i][k];
            pm /= 8;
            for (int i = 0; i < 8; i++) scp += (c[i] - cm) * (p[i][k] - pm);
            double slope = scc > 1e-9 ? scp / scc : 0;
            bg[k] = pm - slope * cm;
            fg[k] = bg[k] + slope;
        }
        fg[k] = clamp8(fg[k]);
        bg[k] = clamp8(bg[k]);
    }
    double err = 0;
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 3; k++) {
            double d = bg[k] + c[i] * (fg[k] - bg[k]) - p[i][k];
            err += d * d;
        }
    return err;
}

/* Символы края круга: только «дуги» — на обычном шрифте край читается как кружок, а не как буквы. */
static const char ARC_SET[] = ".,'`\"_-()/\\|[]<>^~:;";
/* Символы заливки картинки: знаки плюс строчные «высоты x» (o a m n u w — единственные символы,
 * закрашивающие нижние 2/3 клетки без верха). Сравнение на снимках (сцены и значки с CDN, 128x36):
 * на мелкой сетке узнаваемость та же, что со всеми 95 символами, на обычной — ASCII-арт вместо
 * «каши из букв», и подбор втрое быстрее (38 символов вместо 95). */
static const char FILL_SET[] = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~oamnuw";
#ifndef IMG_FILL_SET
#define IMG_FILL_SET FILL_SET
#endif

/* p — 8 цветов частей (слева направо, сверху вниз). fix/which — как в fit_glyph (0 — свободно;
 * 3 — пробовать оба закрепления: для края круга). set — из каких символов выбирать (NULL — все). */
static void cell_fit(const double p[8][3], const u8 *fix, int which, const char *set, ImgCell *out) {
    double best = 1e30, fg[3], bg[3], bfg[3] = { 0, 0, 0 }, bbg[3] = { 0, 0, 0 };
    int bg_ = 0;
    for (int g = 1; g < NG; g++) {
        if (set && !strchr(set, 0x20 + g)) continue;
        for (int m = which == 3 ? 1 : which; m <= (which == 3 ? 2 : which); m++) {
            double e = fit_glyph(p, g, fix, m, fg, bg);
            if (e < best) { best = e; bg_ = g; memcpy(bfg, fg, sizeof fg); memcpy(bbg, bg, sizeof bg); }
        }
    }
    out->ch = (char)(0x20 + bg_);
    for (int k = 0; k < 3; k++) { out->fg[k] = (u8)bfg[k]; out->bg[k] = (u8)bbg[k]; }
}

/* Цвет части i клетки, каким его увидит глаз (обратное к подбору). ch == 0 — прозрачная. */
static void cell_part(const ImgCell *c, int i, const u8 *colbg, double out[3]) {
    if (!c->ch) { for (int k = 0; k < 3; k++) out[k] = colbg[k]; return; }
    int g = (unsigned char)c->ch - 0x20;
    double cv = (g >= 0 && g < NG) ? GLYPH_COV[g][i] / 255.0 : 0;
    for (int k = 0; k < 3; k++) out[k] = c->bg[k] + cv * (c->fg[k] - c->bg[k]);
}

static void cell_make(const Acc *q[8], ImgCell *c) {
    double p[8][3];
    int ymin = 256, ymax = -1;
    double m[3] = { 0, 0, 0 };
    for (int i = 0; i < 8; i++) {
        u32 n = q[i]->n ? q[i]->n : 1;
        p[i][0] = (double)q[i]->r / n; p[i][1] = (double)q[i]->g / n; p[i][2] = (double)q[i]->b / n;
        int y = lum((int)p[i][0], (int)p[i][1], (int)p[i][2]);
        if (y < ymin) ymin = y;
        if (y > ymax) ymax = y;
        for (int k = 0; k < 3; k++) m[k] += p[i][k] / 8;
    }
    if (ymax - ymin < UNIFORM_T) {
        int k = lum((int)m[0], (int)m[1], (int)m[2]) * 10 / 256;
        double cov = glyph_mean(RAMP[k] - 0x20);
        c->ch = RAMP[k];
        for (int j = 0; j < 3; j++) {
            c->fg[j] = clamp8(m[j] + RAMP_D * (1 - cov));
            c->bg[j] = clamp8(m[j] - RAMP_D * cov);
        }
        return;
    }
    cell_fit(p, NULL, 0, IMG_FILL_SET, c);
}

/* Тело разбора; любая ошибка libjpeg — longjmp в img_decode. */
static bool dec_body(Dec *d, const u8 *jpg, size_t n, int w, int h, ImgCell *out) {
    struct jpeg_decompress_struct *ci = &d->ci;
    jpeg_create_decompress(ci);
    d->created = true;
    ci->progress = &d->pg;
    ci->mem->max_memory_to_use = DEC_MEM_MAX;
    jpeg_mem_src(ci, jpg, (unsigned long)n);
    if (jpeg_read_header(ci, TRUE) != JPEG_HEADER_OK) return false;
    unsigned W = ci->image_width, H = ci->image_height;
    if (!W || !H || W > DEC_MAX_DIM || H > DEC_MAX_DIM || ci->data_precision != 8) return false;
    if (!((ci->num_components == 1 && ci->jpeg_color_space == JCS_GRAYSCALE) ||
          (ci->num_components == 3 && (ci->jpeg_color_space == JCS_YCbCr || ci->jpeg_color_space == JCS_RGB))))
        return false;
    ci->out_color_space = JCS_RGB;
    ci->dct_method = JDCT_ISLOW;
    ci->do_fancy_upsampling = FALSE;
    ci->do_block_smoothing = FALSE;
    ci->quantize_colors = FALSE;
    ci->buffered_image = FALSE;
    /* DCT-масштабирование M/8: наименьшее, при котором картинка не меньше нужной (2x4 на клетку) */
    unsigned tw = 2u * (unsigned)w, th = 4u * (unsigned)h, M = 1;
    while (M < 8 && ((W * M + 7) / 8 < tw || (H * M + 7) / 8 < th)) M++;
    ci->scale_num = M;
    ci->scale_denom = 8;
    if (!jpeg_start_decompress(ci)) return false;
    unsigned OW = ci->output_width, OH = ci->output_height;
    if (!OW || !OH || OW > DEC_MAX_DIM || OH > DEC_MAX_DIM || ci->output_components != 3) return false;
    int qw = 2 * w, qh = 4 * h;
    for (int i = 0; i <= qw; i++) d->xs[i] = (int)((u64)i * OW / (unsigned)qw);
    int qy = 0;
    while (ci->output_scanline < OH) {
        JSAMPROW rp = d->row;
        if (jpeg_read_scanlines(ci, &rp, 1) != 1) return false;
        int y = (int)ci->output_scanline - 1;
        while (qy < qh) {
            int y0 = (int)((u64)qy * OH / (unsigned)qh), y1 = (int)((u64)(qy + 1) * OH / (unsigned)qh);
            if (y1 < y0 + 1) y1 = y0 + 1;
            if (y < y0) break;
            Acc *a = d->acc + (qy & 3) * qw;
            for (int qx = 0; qx < qw; qx++) {
                int x0 = d->xs[qx], x1 = d->xs[qx + 1];
                if (x1 < x0 + 1) x1 = x0 + 1;
                for (int x = x0; x < x1; x++) {
                    a[qx].r += d->row[3 * x]; a[qx].g += d->row[3 * x + 1]; a[qx].b += d->row[3 * x + 2];
                    a[qx].n++;
                }
            }
            if (y != y1 - 1) break;
            if ((qy & 3) == 3) {                /* все 4 строки частей строки клеток собраны */
                ImgCell *o = out + (qy / 4) * w;
                for (int cx = 0; cx < w; cx++) {
                    const Acc *q[8];
                    for (int r = 0; r < 4; r++) { q[2 * r] = &d->acc[r * qw + 2 * cx]; q[2 * r + 1] = &d->acc[r * qw + 2 * cx + 1]; }
                    cell_make(q, &o[cx]);
                }
                memset(d->acc, 0, sizeof(Acc) * 4 * (size_t)qw);
            }
            qy++;
        }
    }
    return qy == qh;
}

bool img_decode(const u8 *jpg, size_t n, int w, int h, ImgCell *out) {
    if (!jpg || n < 4 || n > DL_MAX || !out || w < 1 || h < 1 || w > IMG_MAX_W || h > IMG_MAX_H) return false;
    if (jpg[0] != 0xFF || jpg[1] != 0xD8 || jpg[2] != 0xFF) return false;
    Dec *d = calloc(1, sizeof *d);
    if (!d) return false;
    d->acc = calloc(8 * (size_t)w, sizeof(Acc));
    if (!d->acc) { free(d); return false; }
    s_jpeak = s_jmem;
    d->ci.err = jpeg_std_error(&d->je.pub);
    d->je.pub.error_exit = j_exit;
    d->je.pub.output_message = j_output;
    d->je.pub.emit_message = j_emit;
    d->pg.progress_monitor = j_progress;
    volatile bool ok = false;                   /* переживает longjmp */
    if (!setjmp(d->je.jb)) ok = dec_body(d, jpg, n, w, h, out);
    if (d->created) jpeg_destroy_decompress(&d->ci);
    wipe(d->acc, 8 * (size_t)w * sizeof(Acc));
    free(d->acc);
    wipe(d, sizeof *d);                         /* строка пикселей и состояние разбора */
    free(d);
    if (!ok) memset(out, 0, sizeof(ImgCell) * (size_t)w * (size_t)h);
    return ok;
}
size_t img_decode_peak(void) { return s_jpeak; }

/* ======================= адреса ======================= */
static bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
static bool is_dig(char c) { return c >= '0' && c <= '9'; }

/* Хэш аватара: 32 hex или "a_" + 32 hex (анимированный: .jpg даёт первый кадр). */
static bool avatar_hash_ok(const char *s) {
    if (!strncmp(s, "a_", 2)) s += 2;
    int i = 0;
    for (; s[i]; i++) if (i >= 32 || !is_hex(s[i])) return false;
    return i == 32;
}

static u64 fnv(u64 hsh, const void *p, size_t n) {
    const u8 *b = p;
    for (size_t i = 0; i < n; i++) { hsh ^= b[i]; hsh *= 1099511628211ULL; }
    return hsh;
}
#define FNV0 14695981039346656037ULL

/* Вложение: https://(media.discordapp.net|cdn.discordapp.com)/attachments/<цифры>/<цифры>/<имя>
 * [?ex=hex&is=hex&hm=hex&...]. Результат — адрес медиапрокси (он умеет уменьшать и перекодировать)
 * с подписью ex/is/hm и нашими format/width/height; прочие параметры отбрасываются.
 * *key — от пути (подпись меняется со временем, картинка — нет). */
static bool att_url(const char *u, int pw, int ph, char *out, size_t outn, u64 *key) {
    static const char M[] = "https://media.discordapp.net/", C[] = "https://cdn.discordapp.com/";
    const char *p;
    if (!strncmp(u, M, sizeof M - 1)) p = u + sizeof M - 1;
    else if (!strncmp(u, C, sizeof C - 1)) p = u + sizeof C - 1;
    else return false;
    const char *path = p;
    if (strncmp(p, "attachments/", 12)) return false;
    p += 12;
    for (int part = 0; part < 2; part++) {
        int k = 0;
        while (is_dig(p[k])) k++;
        if (k < 1 || k > 20 || p[k] != '/') return false;
        p += k + 1;
    }
    int k = 0;
    for (;; k++) {
        char c = p[k];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || is_dig(c) || c == '.' || c == '_' || c == '-') continue;
        if (c == '%' && is_hex(p[k + 1] | 0x20) && is_hex(p[k + 2] | 0x20)) { k += 2; continue; }
        break;
    }
    if (k < 1 || k > 200 || (p[k] && p[k] != '?')) return false;
    size_t pathn = (size_t)(p + k - path);
    const char *q = p[k] == '?' ? p + k + 1 : p + k;
    char sig[3][130] = { "", "", "" };
    static const char *const SK[3] = { "ex", "is", "hm" };
    while (*q) {
        size_t n = strcspn(q, "&");
        const char *eq = memchr(q, '=', n);
        if (eq) {
            for (int i = 0; i < 3; i++) {
                if ((size_t)(eq - q) != 2 || memcmp(q, SK[i], 2)) continue;
                size_t vn = n - 3;
                if (vn < 1 || vn >= sizeof sig[i] || sig[i][0]) return false;   /* повтор — подозрительно */
                for (size_t j = 0; j < vn; j++) if (!is_hex(eq[1 + j])) return false;
                memcpy(sig[i], eq + 1, vn);
                sig[i][vn] = 0;
            }
        }
        q += n;
        if (*q == '&') q++;
    }
    int r = snprintf(out, outn, "%s%.*s?", M, (int)pathn, path);
    for (int i = 0; i < 3 && r > 0 && (size_t)r < outn; i++)
        if (sig[i][0]) r += snprintf(out + r, outn - (size_t)r, "%s=%s&", SK[i], sig[i]);
    if (r <= 0 || (size_t)r >= outn) return false;
    r += snprintf(out + r, outn - (size_t)r, "format=jpeg&width=%d&height=%d", pw, ph);
    if ((size_t)r >= outn) return false;
    *key = fnv(FNV0, path, pathn);
    return true;
}

/* ======================= только видимое: клетки текущего кадра и очередь =======================
 * Картинки не хранятся (требование владельца): готовые клетки живут, только пока картинка на
 * экране. img_frame() в начале кадра затирает и освобождает всё, что не запрашивали в прошлом
 * кадре (и прерывает его загрузку). Вернулась на экран — скачать и перекодировать заново.
 * Исключение (решение владельца 11.11.2026): готовые аватары и значки (K_AV), ушедшие с экрана, остаются
 * в памяти, пока их клетки вместе не больше AV_KEEP_BYTES, — при прокрутке ленты те же люди не
 * скачиваются заново. Вытесняется тот, кого дольше всех не показывали; место под видимое важнее.
 * Только в памяти (на диск ничего), при освобождении — затирание; превью вложений (K_PV) — как раньше.
 * Чтобы не долбить CDN при прокрутке, загрузка начинается, только если картинка видна не меньше
 * LOAD_DELAY_MS подряд; одновременно — не больше одной загрузки. Неудачи помнятся только ключом
 * (64 бита, без данных картинки), чтобы не повторять их по кругу. */
#define NENT 192                                /* картинок на экране одновременно, не больше */
#define FRAME_MAX_BYTES (512u << 10)            /* жёсткий предел клеток видимого кадра */
#define LOAD_DELAY_MS 150                       /* картинка должна быть видна столько, прежде чем качать */
#define NFAIL 32                                /* ключи неудачных загрузок */
#define AV_KEEP_BYTES (64u << 10)               /* аватары вне экрана: до 64 КБ клеток (~70 аватаров 16x8) */
enum { ST_FREE, ST_WANT, ST_LOAD, ST_READY, ST_FAIL };
enum { K_AV = 1, K_PV = 2 };
typedef struct {
    u64 key;
    u8 state, kind;
    u16 w, h;                                   /* размер в клетках */
    u32 gen;                                    /* кадр последнего запроса */
    i64 seen_at;                                /* когда впервые запрошено (пауза перед загрузкой) */
    char *url;                                  /* пока не готово */
    ImgCell *cells;                             /* ST_READY */
} Ent;
static Ent s_e[NENT];
static bool s_on, s_frames;
static u32 s_gen;
static size_t s_bytes;
static struct { u64 key; i64 until; } s_fail[NFAIL];
static int s_nfail;
static void dl_end(void);
static Ent *dl_ent(void);

static size_t ent_bytes(const Ent *e) { return sizeof(ImgCell) * e->w * e->h; }
static void ent_free(Ent *e) {
    if (e->state == ST_LOAD && dl_ent() == e) dl_end();
    if (e->cells) { s_bytes -= ent_bytes(e); wipe(e->cells, ent_bytes(e)); free(e->cells); }
    free(e->url);
    memset(e, 0, sizeof *e);
}
static int count_used(void) {
    int n = 0;
    for (int i = 0; i < NENT; i++) if (s_e[i].state != ST_FREE) n++;
    return n;
}
static void fail_add(u64 key, i64 until) {
    for (int i = 0; i < NFAIL; i++) if (s_fail[i].key == key) { s_fail[i].until = until; return; }
    s_fail[s_nfail].key = key;
    s_fail[s_nfail].until = until;
    s_nfail = (s_nfail + 1) % NFAIL;
}
static bool failed_recently(u64 key) {
    for (int i = 0; i < NFAIL; i++) if (s_fail[i].key == key && s_fail[i].until && now_ms() < s_fail[i].until) return true;
    return false;
}

/* Аватар, ушедший с экрана (не запрошен в этом кадре), которого дольше всех не показывали; NULL — нет. */
static Ent *kept_lru(void) {
    Ent *o = NULL;
    for (int i = 0; i < NENT; i++) {
        Ent *e = &s_e[i];
        if (e->state == ST_READY && e->kind == K_AV && e->gen != s_gen && (!o || (int32_t)(e->gen - o->gen) < 0)) o = e;
    }
    return o;
}
static size_t kept_bytes(void) {
    size_t n = 0;
    for (int i = 0; i < NENT; i++)
        if (s_e[i].state == ST_READY && s_e[i].kind == K_AV && s_e[i].gen != s_gen) n += ent_bytes(&s_e[i]);
    return n;
}
static Ent *find(u64 key, int kind, int w, int h) {
    for (int i = 0; i < NENT; i++)
        if (s_e[i].state != ST_FREE && s_e[i].key == key && s_e[i].kind == kind && s_e[i].w == w && s_e[i].h == h) return &s_e[i];
    return NULL;
}
static void want(u64 key, int kind, int w, int h, const char *url) {
    if (failed_recently(key)) return;
    Ent *e = NULL;
    for (int i = 0; i < NENT && !e; i++) if (s_e[i].state == ST_FREE) e = &s_e[i];
    if (!e && (e = kept_lru()) != NULL) ent_free(e);   /* место видимому — за счёт аватара вне экрана */
    if (!e) return;                             /* на экране больше NENT картинок: остальные — инициалы */
    e->key = key; e->kind = (u8)kind; e->w = (u16)w; e->h = (u16)h;
    e->state = ST_WANT;
    e->gen = s_gen;
    e->seen_at = now_ms();
    e->url = xstrdup(url);
}
/* Готовые клетки -> в кадр (cells переходят во владение модуля). Сверх FRAME_MAX_BYTES не держим:
 * картинка остаётся инициалами, пока на экране. Для ориентира: аватар 16x8 = 0,9 КБ, значок 24x12 =
 * 2 КБ, превью 128x32 = 28 КБ, просмотр 192x48 = 64,5 КБ; видимое в худшем случае ~ 280 КБ. */
static void put_ready(Ent *e, ImgCell *cells) {
    size_t need = sizeof(ImgCell) * e->w * e->h;
    free(e->url);
    e->url = NULL;
    for (Ent *k; s_bytes + need > FRAME_MAX_BYTES && (k = kept_lru()) != NULL && k != e;) ent_free(k);
    if (s_bytes + need > FRAME_MAX_BYTES) {
        wipe(cells, need);
        free(cells);
        e->state = ST_FAIL;
        return;
    }
    e->cells = cells;
    s_bytes += need;
    e->state = ST_READY;
}
/* Запрос в этом кадре: отдать готовое; иначе отметить, что всё ещё видно. */
static bool take(Ent *e, ImgCell *out) {
    e->gen = s_gen;
    if (e->state == ST_READY) { memcpy(out, e->cells, ent_bytes(e)); return true; }
    return false;
}

void img_enable(bool on) { if (!on) img_forget(); s_on = on; }
/* Начало кадра: всё, что не запросили в прошлом кадре, ушло с экрана — затереть и забыть; готовые
 * аватары — оставить в пределах AV_KEEP_BYTES (самые давние — вон). */
void img_frame(void) {
    if (s_frames)
        for (int i = 0; i < NENT; i++)
            if (s_e[i].state != ST_FREE && s_e[i].gen != s_gen && !(s_e[i].state == ST_READY && s_e[i].kind == K_AV)) ent_free(&s_e[i]);
    s_frames = true;
    s_gen++;
    for (Ent *k; kept_bytes() > AV_KEEP_BYTES && (k = kept_lru()) != NULL;) ent_free(k);
}

/* Стандартный аватар Discord: цвет по (id >> 22) % 6, «глаза» белым — без сети. */
static void default_avatar(u64 uid, int w, int h, ImgCell *out) {
    static const u8 COL[6][3] = { { 0x58, 0x65, 0xf2 }, { 0x75, 0x7e, 0x8a }, { 0x3b, 0xa5, 0x5c },
                                  { 0xfa, 0xa6, 0x1a }, { 0xed, 0x42, 0x45 }, { 0xeb, 0x45, 0x9e } };
    const u8 *c = COL[(uid >> 22) % 6];
    for (int i = 0; i < w * h; i++) {
        out[i].ch = ' ';
        memcpy(out[i].bg, c, 3);
        memset(out[i].fg, 0xff, 3);
    }
    if (w >= 4) {
        int row = (h - 1) / 2;
        out[row * w + w / 4].ch = 'o';
        out[row * w + w - 1 - w / 4].ch = 'o';
    }
}

/* Квадратная картинка CDN по id и хэшу: dir — "avatars" (пользователь) или "icons" (сервер).
 * Ключ кэша включает dir: аватар и значок с одинаковыми id/хэшем не путаются. */
static u64 square_key(const char *dir, u64 id, const char *hash) {
    return fnv(fnv(fnv(FNV0, dir, strlen(dir)), &id, sizeof id), hash, strlen(hash));
}
/* size= для аватара/значка: CDN отдаёт только степени двойки 16..4096. Требование владельца — не
 * меньше 128 px; больше — если под мелкую сетку нужно больше 2x4 пикселя на клетку (до 512).
 * Уменьшение до клеток — DCT-масштабированием при разборе, пик памяти от этого не растёт. */
#define SRC_MIN_PX 128
static int square_size(int w, int h) {
    int px = 2 * w > 4 * h ? 2 * w : 4 * h, size = SRC_MIN_PX;
    while (size < px && size < DEC_MAX_DIM) size *= 2;
    return size;
}
/* Пиксели, которые заказать у медиапрокси для превью cw x ch клеток: 2x4 на клетку, но меньшая
 * сторона не меньше 256 (требование владельца: >= 128, для превью/просмотра 256), большая — не
 * больше 512 (предел разбора), и не больше исходника sw x sh (0 — неизвестен). Пропорции — клеток. */
#define PV_MIN_SIDE 256
static void att_px(int cw, int ch, int sw, int sh, int *pw, int *ph) {
    double w = 2.0 * cw, h = 4.0 * ch, mn = w < h ? w : h, mx = w > h ? w : h, k = 1;
    if (mn < PV_MIN_SIDE) k = PV_MIN_SIDE / mn;
    if (mx * k > DEC_MAX_DIM) k = DEC_MAX_DIM / mx;
    if (sw > 0 && w * k > sw) k = sw / w;
    if (sh > 0 && h * k > sh) k = sh / h;
    if (k < 1 && (sw <= 0 || w <= sw) && (sh <= 0 || h <= sh)) k = 1;   /* меньше 2x4 на клетку не берём, если исходник позволяет */
    *pw = (int)(w * k + 0.5);
    *ph = (int)(h * k + 0.5);
    if (*pw < 1) *pw = 1;
    if (*ph < 1) *ph = 1;
}

static bool cdn_square(const char *dir, u64 id, const char *hash, int w, int h, ImgCell *out) {
    if (!id || !avatar_hash_ok(hash)) return false;
    u64 key = square_key(dir, id, hash);
    Ent *e = find(key, K_AV, w, h);
    if (e) return take(e, out);
    if (O.demo) return false;
    int size = square_size(w, h);
    char url[160];
    snprintf(url, sizeof url, "https://cdn.discordapp.com/%s/%llu/%s.jpg?size=%d", dir, (unsigned long long)id, hash, size);
    want(key, K_AV, w, h, url);
    return false;
}

bool img_avatar(u64 uid, const char *hash, int w, int h, ImgCell *out) {
    if (!s_on || !out || w < 1 || h < 1 || w > IMG_MAX_W || h > IMG_MAX_H) return false;
    if (!hash || !*hash) { default_avatar(uid, w, h, out); return true; }
    return cdn_square("avatars", uid, hash, w, h, out);
}

/* Значок сервера; без значка — false (интерфейс рисует сокращение названия, как Discord). */
bool img_icon(u64 gid, const char *hash, int w, int h, ImgCell *out) {
    if (!s_on || !out || !hash || !*hash || w < 1 || h < 1 || w > IMG_MAX_W || h > IMG_MAX_H) return false;
    return cdn_square("icons", gid, hash, w, h, out);
}

/* ---------- круглая маска ----------
 * Эллипс вписан в w x h клеток с учётом того, что клетка вдвое выше ширины (при w = 2h — круг).
 * Для каждой из 8 частей клетки считается доля площади внутри (выборка 4x4 точки на часть).
 * Целиком внутри — клетка картинки как есть; почти снаружи — ch = 0 (прозрачная: не рисовать,
 * оставить фон колонки); на границе цвета частей смешиваются с фоном колонки по доле площади и
 * символ подбирается заново тем же способом, что и для картинки, но один из двух цветов клетки —
 * ровно фон колонки. Так дуга получается тонкой и гладкой на любом размере (2x1 ... 48x24), без
 * ступенек и без «коробки» вокруг кружка. */
#define ROUND_SS 4                                    /* выборка на часть по каждой оси */
static int round_parts(int x, int y, int w, int h, double a[8]) {
    /* единицы: ширина клетки = 1, высота клетки = 2; часть — 0,5 x 0,5 */
    double cx = w / 2.0, cy = h, rx = w / 2.0, ry = h;
    int tot = 0;
    for (int part = 0; part < 8; part++) {
        int px0 = part & 1, py0 = part >> 1, in = 0;
        for (int j = 0; j < ROUND_SS; j++)
            for (int i = 0; i < ROUND_SS; i++) {
                double px = x + 0.5 * px0 + 0.5 * (i + 0.5) / ROUND_SS, py = 2.0 * y + 0.5 * py0 + 0.5 * (j + 0.5) / ROUND_SS;
                double ex = (px - cx) / rx, ey = (py - cy) / ry;
                if (ex * ex + ey * ey <= 1.0) in++;
            }
        a[part] = (double)in / (ROUND_SS * ROUND_SS);
        tot += in;
    }
    return tot;                                 /* из 8 * ROUND_SS * ROUND_SS */
}
#define ROUND_DROP 8                            /* меньше 8 из 128 точек (6%) — прозрачная */

void img_round(ImgCell *c, int w, int h, const uint8_t colbg[3]) {
    if (!c || w < 1 || h < 1 || w > IMG_MAX_W || h > IMG_MAX_H) return;
    static const uint8_t black[3] = { 0, 0, 0 };
    if (!colbg) colbg = black;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            ImgCell *k = &c[y * w + x];
            double a[8], p[8][3];
            int tot = round_parts(x, y, w, h, a);
            if (tot == 8 * ROUND_SS * ROUND_SS) continue;
            if (tot < ROUND_DROP) { k->ch = 0; memcpy(k->fg, colbg, 3); memcpy(k->bg, colbg, 3); continue; }
            for (int i = 0; i < 8; i++) {
                cell_part(k, i, colbg, p[i]);
                for (int j = 0; j < 3; j++) p[i][j] = a[i] * p[i][j] + (1 - a[i]) * colbg[j];
            }
            cell_fit(p, colbg, 3, ARC_SET, k);
        }
}

bool img_avatar_round(u64 uid, const char *hash, int w, int h, const uint8_t colbg[3], ImgCell *out) {
    if (!img_avatar(uid, hash, w, h, out)) return false;
    img_round(out, w, h, colbg);
    return true;
}
bool img_icon_round(u64 gid, const char *hash, int w, int h, const uint8_t colbg[3], ImgCell *out) {
    if (!img_icon(gid, hash, w, h, out)) return false;
    img_round(out, w, h, colbg);
    return true;
}

/* Готовые клетки превью прямо из кэша (без копии в буфер вызывающего): указатель живёт до следующего
 * img_frame()/img_service()/img_forget() — то есть только в пределах текущего кадра отрисовки. */
const ImgCell *img_attachment_peek(const char *url, int sw, int sh, int maxw, int maxh, int *outw, int *outh) {
    if (!s_on || !url || !outw || !outh || maxw < 1 || maxh < 1 || maxw > IMG_MAX_W || maxh > IMG_MAX_H) return NULL;
    if (sw < 0 || sh < 0 || sw > 65535 || sh > 65535) return NULL;
    /* вписать в maxw x maxh с сохранением пропорций (клетка — 2x4 пикселя), без увеличения */
    int cw = maxw, ch = maxh;
    if (sw > 0 && sh > 0) {
        ch = (int)((i64)sh * cw * 2 / (4 * (i64)sw));
        if (ch > maxh) { ch = maxh; cw = (int)((i64)sw * ch * 4 / (2 * (i64)sh)); }
        if (cw > (sw + 1) / 2) cw = (sw + 1) / 2;
        if (ch > (sh + 3) / 4) ch = (sh + 3) / 4;
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;
        if (cw > maxw) cw = maxw;
    }
    char u[700];
    u64 key;
    int pw, ph;
    att_px(cw, ch, sw, sh, &pw, &ph);
    if (!att_url(url, pw, ph, u, sizeof u, &key)) return NULL;
    *outw = cw;
    *outh = ch;
    Ent *e = find(key, K_PV, cw, ch);
    if (e) { e->gen = s_gen; return e->state == ST_READY ? e->cells : NULL; }
    if (O.demo) return NULL;
    want(key, K_PV, cw, ch, u);
    return NULL;
}
bool img_attachment(const char *url, int sw, int sh, int maxw, int maxh, ImgCell *out, int *outw, int *outh) {
    if (!out) return false;
    const ImgCell *c = img_attachment_peek(url, sw, sh, maxw, maxh, outw, outh);
    if (!c) return false;
    memcpy(out, c, (size_t)*outw * (size_t)*outh * sizeof(ImgCell));
    return true;
}

/* ======================= загрузка ======================= */
static struct {
    CURLM *m;
    CURL *e;
    Ent *ent;
    u8 *buf;
    size_t len, cap;
    bool over;
} D;

static size_t dl_write(char *p, size_t sz, size_t n, void *ud) {
    (void)ud;
    size_t t = sz * n;
    if (D.len + t > DL_MAX) { D.over = true; return 0; }
    if (D.len + t > D.cap) {                    /* рост с затиранием старого блока */
        size_t c = D.cap ? D.cap : 4096;
        while (c < D.len + t) c *= 2;
        if (c > DL_MAX) c = DL_MAX;
        u8 *nb = malloc(c);
        if (!nb) return 0;
        if (D.len) memcpy(nb, D.buf, D.len);
        if (D.buf) { wipe(D.buf, D.cap); free(D.buf); }
        D.buf = nb;
        D.cap = c;
    }
    memcpy(D.buf + D.len, p, t);
    D.len += t;
    return t;
}

static void dl_drop_buf(void) {
    if (D.buf) { wipe(D.buf, D.cap); free(D.buf); }
    D.buf = NULL;
    D.len = D.cap = 0;
    D.over = false;
}
static void dl_end(void) {
    if (D.e) {
        if (D.m) curl_multi_remove_handle(D.m, D.e);
        curl_easy_cleanup(D.e);
        D.e = NULL;
    }
    D.ent = NULL;
    dl_drop_buf();
}

/* Маршрут как у всего трафика (net.c): VLESS выключен — напрямую; включён — только через его SOCKS5;
 * включён, но не работает — NULL: ждём, напрямую не идём. */
static const char *route(void) { return vless_state() == VL_OFF ? "" : vless_proxy_url(); }

static Ent *dl_ent(void) { return D.ent; }

/* Видна в последнем кадре (или кадры не размечаются) и видна не меньше LOAD_DELAY_MS. */
static bool visible(const Ent *e) { return !s_frames || e->gen == s_gen; }
static bool eligible(const Ent *e, i64 now) {
    return e->state == ST_WANT && visible(e) && now - e->seen_at >= LOAD_DELAY_MS;
}

static void dl_fail(Ent *e, i64 retry_ms) {
    free(e->url);
    e->url = NULL;
    e->state = ST_FAIL;                         /* пока на экране — не повторять */
    fail_add(e->key, now_ms() + retry_ms);      /* и после возврата на экран — не раньше срока */
}

static void dl_start(void) {
    Ent *best = NULL;
    bool waiting = false;
    i64 now = now_ms();
    for (int i = 0; i < NENT; i++) {
        Ent *e = &s_e[i];
        if (e->state == ST_WANT && visible(e)) waiting = true;
        if (eligible(e, now) && (!best || e->seen_at < best->seen_at)) best = e;   /* дольше всех видна */
    }
    if (!best) {                                /* очередь пуста: соединение и multi не держим */
        if (!waiting && D.m) { curl_multi_cleanup(D.m); D.m = NULL; }
        return;
    }
    const char *proxy = route();
    if (!proxy) return;
    /* последняя линия обороны: только два хоста и только https */
    if (strncmp(best->url, "https://cdn.discordapp.com/", 27) && strncmp(best->url, "https://media.discordapp.net/", 29)) {
        ent_free(best);
        return;
    }
    if (!D.m) D.m = curl_multi_init();
    D.e = D.m ? curl_easy_init() : NULL;
    if (!D.e) { dl_fail(best, 30000); return; }
    static struct curl_slist acc = { "Accept: image/jpeg", NULL };
    net_curl_common(D.e);
    curl_easy_setopt(D.e, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(D.e, CURLOPT_PROXY, proxy);
    curl_easy_setopt(D.e, CURLOPT_NOPROXY, "");
    if (proxy[0]) curl_easy_setopt(D.e, CURLOPT_SOCKS5_AUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(D.e, CURLOPT_URL, best->url);
    curl_easy_setopt(D.e, CURLOPT_HTTPHEADER, &acc);
    curl_easy_setopt(D.e, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(D.e, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(D.e, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)DL_MAX);
    curl_easy_setopt(D.e, CURLOPT_WRITEFUNCTION, dl_write);
    curl_easy_setopt(D.e, CURLOPT_WRITEDATA, NULL);
    if (curl_multi_add_handle(D.m, D.e) != CURLM_OK) {
        curl_easy_cleanup(D.e);
        D.e = NULL;
        dl_fail(best, 30000);
        return;
    }
    best->state = ST_LOAD;
    D.ent = best;
}

static void dl_finish(CURLcode rc) {
    Ent *e = D.ent;
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(D.e, CURLINFO_RESPONSE_CODE, &status);
    ImgCell *cells = NULL;
    bool ok = false;
    if (status == 200 && !D.over && D.len >= 4) {
        cells = malloc(sizeof(ImgCell) * e->w * e->h);
        ok = cells && img_decode(D.buf, D.len, e->w, e->h, cells);
    }
    dl_end();                                   /* затирает скачанные байты */
    if (ok) {
        put_ready(e, cells);
        ui_dirty();
        return;
    }
    free(cells);
    /* нет связи — повтор через 30 с; ответ есть, но не картинка — не раньше чем через 10 мин */
    dl_fail(e, status == 0 ? 30000 : 600000);
}

void img_service(void) {
    if (!s_on) return;
    if (!D.e) dl_start();
    if (!D.e) return;
    int running = 0;
    curl_multi_perform(D.m, &running);
    CURLMsg *m;
    int left;
    while ((m = curl_multi_info_read(D.m, &left)))
        if (m->msg == CURLMSG_DONE && m->easy_handle == D.e) { dl_finish(m->data.result); break; }
}

int img_waitables(Waitable *out, int max) { (void)out; (void)max; return 0; }

i64 img_next_wake(void) {
    if (!s_on) return INT64_MAX;
    if (D.e) return now_ms() + 15;              /* опрос только пока идёт загрузка */
    if (!route()) return INT64_MAX;             /* ждём VLESS (он разбудит цикл) */
    i64 t = INT64_MAX;
    for (int i = 0; i < NENT; i++)              /* ближайший конец паузы видимой картинки */
        if (s_e[i].state == ST_WANT && visible(&s_e[i]) && s_e[i].seen_at + LOAD_DELAY_MS < t)
            t = s_e[i].seen_at + LOAD_DELAY_MS;
    if (t != INT64_MAX) return t;
    return D.m ? now_ms() : INT64_MAX;          /* закрыть простаивающее соединение */
}

void img_forget(void) {
    dl_end();
    if (D.m) { curl_multi_cleanup(D.m); D.m = NULL; }
    for (int i = 0; i < NENT; i++) if (s_e[i].state != ST_FREE) ent_free(&s_e[i]);
    s_bytes = 0;
    s_gen = 0;
    s_frames = false;
    memset(s_fail, 0, sizeof s_fail);
    s_nfail = 0;
}

size_t img_cache_bytes(void) { return s_bytes; }

/* ======================= самопроверка (без сети) ======================= */
/* Тестовые JPEG 16x16 (4:4:4, четверти: красная, зелёная / синяя, белая), то же прогрессивное,
 * и серый градиент 32x8. Сделаны кодировщиком libjpeg-turbo 3.2.1. */
static const uint8_t T_RGB16[638] = {
    0xff,0xd8,0xff,0xdb,0x00,0x43,0x00,0x03,0x02,0x02,0x03,0x02,0x02,0x03,0x03,0x03,
    0x03,0x04,0x03,0x03,0x04,0x05,0x08,0x05,0x05,0x04,0x04,0x05,0x0a,0x07,0x07,0x06,
    0x08,0x0c,0x0a,0x0c,0x0c,0x0b,0x0a,0x0b,0x0b,0x0d,0x0e,0x12,0x10,0x0d,0x0e,0x11,
    0x0e,0x0b,0x0b,0x10,0x16,0x10,0x11,0x13,0x14,0x15,0x15,0x15,0x0c,0x0f,0x17,0x18,
    0x16,0x14,0x18,0x12,0x14,0x15,0x14,0xff,0xdb,0x00,0x43,0x01,0x03,0x04,0x04,0x05,
    0x04,0x05,0x09,0x05,0x05,0x09,0x14,0x0d,0x0b,0x0d,0x14,0x14,0x14,0x14,0x14,0x14,
    0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,
    0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,
    0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0xff,0xc0,0x00,0x11,
    0x08,0x00,0x10,0x00,0x10,0x03,0x01,0x11,0x00,0x02,0x11,0x01,0x03,0x11,0x01,0xff,
    0xc4,0x00,0x1f,0x00,0x00,0x01,0x05,0x01,0x01,0x01,0x01,0x01,0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,
    0xff,0xc4,0x00,0xb5,0x10,0x00,0x02,0x01,0x03,0x03,0x02,0x04,0x03,0x05,0x05,0x04,
    0x04,0x00,0x00,0x01,0x7d,0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,
    0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,
    0xc1,0x15,0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,
    0x1a,0x25,0x26,0x27,0x28,0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,
    0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,
    0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,
    0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,
    0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,
    0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,
    0xd8,0xd9,0xda,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,
    0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,0xff,0xc4,0x00,0x1f,0x01,0x00,0x03,0x01,0x01,
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x02,0x03,
    0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0xff,0xc4,0x00,0xb5,0x11,0x00,0x02,0x01,
    0x02,0x04,0x04,0x03,0x04,0x07,0x05,0x04,0x04,0x00,0x01,0x02,0x77,0x00,0x01,0x02,
    0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,0x13,0x22,0x32,
    0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,0x15,0x62,0x72,
    0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,0x27,0x28,0x29,
    0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,
    0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,
    0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,
    0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,
    0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,
    0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe2,0xe3,0xe4,
    0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,0xff,
    0xda,0x00,0x0c,0x03,0x01,0x00,0x02,0x11,0x03,0x11,0x00,0x3f,0x00,0xf9,0xd2,0xbf,
    0x0c,0x3f,0xd5,0x33,0xec,0x6a,0xfc,0x48,0xff,0x00,0x98,0xf3,0xf2,0xf2,0xbf,0xe8,
    0xcc,0xfe,0xe5,0x3f,0xa5,0xba,0xff,0x00,0x2b,0x0f,0xb8,0x3f,0xff,0xd9,
};
static const uint8_t T_PROG16[520] = {
    0xff,0xd8,0xff,0xdb,0x00,0x43,0x00,0x03,0x02,0x02,0x03,0x02,0x02,0x03,0x03,0x03,
    0x03,0x04,0x03,0x03,0x04,0x05,0x08,0x05,0x05,0x04,0x04,0x05,0x0a,0x07,0x07,0x06,
    0x08,0x0c,0x0a,0x0c,0x0c,0x0b,0x0a,0x0b,0x0b,0x0d,0x0e,0x12,0x10,0x0d,0x0e,0x11,
    0x0e,0x0b,0x0b,0x10,0x16,0x10,0x11,0x13,0x14,0x15,0x15,0x15,0x0c,0x0f,0x17,0x18,
    0x16,0x14,0x18,0x12,0x14,0x15,0x14,0xff,0xdb,0x00,0x43,0x01,0x03,0x04,0x04,0x05,
    0x04,0x05,0x09,0x05,0x05,0x09,0x14,0x0d,0x0b,0x0d,0x14,0x14,0x14,0x14,0x14,0x14,
    0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,
    0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,
    0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0xff,0xc2,0x00,0x11,
    0x08,0x00,0x10,0x00,0x10,0x03,0x01,0x11,0x00,0x02,0x11,0x01,0x03,0x11,0x01,0xff,
    0xc4,0x00,0x16,0x00,0x01,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x07,0x08,0x09,0xff,0xc4,0x00,0x18,0x01,0x00,0x03,0x01,0x01,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x06,0x08,0x09,0x05,
    0x07,0xff,0xda,0x00,0x0c,0x03,0x01,0x00,0x02,0x10,0x03,0x10,0x00,0x00,0x01,0x39,
    0x05,0x6a,0x58,0xc2,0x66,0x3c,0xbd,0x46,0x7b,0x9e,0x96,0xaa,0xdb,0x9f,0xff,0xc4,
    0x00,0x14,0x10,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x20,0xff,0xda,0x00,0x08,0x01,0x01,0x00,0x01,0x05,0x02,0x1f,0xff,
    0xc4,0x00,0x14,0x11,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x20,0xff,0xda,0x00,0x08,0x01,0x03,0x01,0x01,0x3f,0x01,0x1f,
    0xff,0xc4,0x00,0x14,0x11,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x20,0xff,0xda,0x00,0x08,0x01,0x02,0x01,0x01,0x3f,0x01,
    0x1f,0xff,0xc4,0x00,0x14,0x10,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x20,0xff,0xda,0x00,0x08,0x01,0x01,0x00,0x06,0x3f,
    0x02,0x1f,0xff,0xc4,0x00,0x14,0x10,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x20,0xff,0xda,0x00,0x08,0x01,0x01,0x00,0x01,
    0x3f,0x21,0x1f,0xff,0xda,0x00,0x0c,0x03,0x01,0x00,0x02,0x00,0x03,0x00,0x00,0x00,
    0x10,0xf5,0x4f,0xff,0xc4,0x00,0x14,0x11,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x20,0xff,0xda,0x00,0x08,0x01,0x03,0x01,
    0x01,0x3f,0x10,0x1f,0xff,0xc4,0x00,0x14,0x11,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x20,0xff,0xda,0x00,0x08,0x01,0x02,
    0x01,0x01,0x3f,0x10,0x1f,0xff,0xc4,0x00,0x14,0x10,0x01,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x20,0xff,0xda,0x00,0x08,0x01,
    0x01,0x00,0x01,0x3f,0x10,0x1f,0xff,0xd9,
};
static const uint8_t T_GRAY32x8[344] = {
    0xff,0xd8,0xff,0xdb,0x00,0x43,0x00,0x03,0x02,0x02,0x03,0x02,0x02,0x03,0x03,0x03,
    0x03,0x04,0x03,0x03,0x04,0x05,0x08,0x05,0x05,0x04,0x04,0x05,0x0a,0x07,0x07,0x06,
    0x08,0x0c,0x0a,0x0c,0x0c,0x0b,0x0a,0x0b,0x0b,0x0d,0x0e,0x12,0x10,0x0d,0x0e,0x11,
    0x0e,0x0b,0x0b,0x10,0x16,0x10,0x11,0x13,0x14,0x15,0x15,0x15,0x0c,0x0f,0x17,0x18,
    0x16,0x14,0x18,0x12,0x14,0x15,0x14,0xff,0xc0,0x00,0x0b,0x08,0x00,0x08,0x00,0x20,
    0x01,0x01,0x11,0x00,0xff,0xc4,0x00,0x1f,0x00,0x00,0x01,0x05,0x01,0x01,0x01,0x01,
    0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x02,0x03,0x04,0x05,0x06,
    0x07,0x08,0x09,0x0a,0x0b,0xff,0xc4,0x00,0xb5,0x10,0x00,0x02,0x01,0x03,0x03,0x02,
    0x04,0x03,0x05,0x05,0x04,0x04,0x00,0x00,0x01,0x7d,0x01,0x02,0x03,0x00,0x04,0x11,
    0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,
    0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,
    0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,0x29,0x2a,0x34,0x35,0x36,0x37,
    0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,
    0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,
    0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,
    0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,
    0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,
    0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,
    0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,0xff,0xda,0x00,0x08,
    0x01,0x01,0x00,0x00,0x3f,0x00,0xfc,0xf5,0xf8,0x69,0xd6,0x2f,0xc2,0xbe,0xb0,0xf8,
    0x69,0xff,0x00,0x2c,0x7f,0x0a,0xfa,0xbf,0xe1,0xa7,0xfc,0xb1,0xfc,0x2b,0xeb,0x0f,
    0x86,0x9f,0xf2,0xcb,0xf0,0xaf,0xff,0xd9,
};

static bool cells_ascii(const ImgCell *c, int n) {
    for (int i = 0; i < n; i++) if (c[i].ch < 0x20 || c[i].ch > 0x7E) return false;
    return true;
}
/* Средний видимый цвет частей [from, to) клетки (смесь символа и фона по таблице закраски). */
static void seen(const ImgCell *c, int from, int to, double out[3]) {
    static const u8 black[3] = { 0, 0, 0 };
    double p[3];
    out[0] = out[1] = out[2] = 0;
    for (int i = from; i < to; i++) {
        cell_part(c, i, black, p);
        for (int k = 0; k < 3; k++) out[k] += p[k] / (to - from);
    }
}
static u32 rnd(u32 *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }
static int sof_at(const u8 *p, size_t n) {
    for (size_t i = 2; i + 9 < n; i++) if (p[i] == 0xFF && (p[i + 1] == 0xC0 || p[i + 1] == 0xC2)) return (int)i;
    return -1;
}

int img_selftest(void (*check)(bool ok, const char *what)) {
    ImgCell c[IMG_MAX_W * 4];
    bool ok;
    /* 1. четверти в 2x1 клетки: слева красный над синим, справа зелёный над белым. Проверяется то,
     * что увидит глаз: смесь символа и фона в верхней и нижней половине клетки. */
    double t[3], b[3];
    ok = img_decode(T_RGB16, sizeof T_RGB16, 2, 1, c);
    seen(&c[0], 0, 4, t); seen(&c[0], 4, 8, b);
    bool q1 = ok && t[0] > b[0] + 20 && b[2] > t[2] + 20 && t[1] < 40 && b[1] < 40;   /* смесь: закраска символа <= ~50% */
    seen(&c[1], 0, 4, t); seen(&c[1], 4, 8, b);
    q1 = q1 && t[1] > 200 && b[1] > 200 && b[0] > t[0] + 20 && b[2] > t[2] + 20;
    check(q1, "картинки: JPEG 16x16 -> 2x1 клетки: верх и низ клетки видны своими цветами");
    check(img_decode_peak() > 0 && img_decode_peak() < (64u << 10) && s_jmem == 0,
          "картинки: пик памяти libjpeg < 64 КБ, всё освобождено");
    ok = img_decode(T_PROG16, sizeof T_PROG16, 2, 1, c);
    seen(&c[0], 0, 4, t); seen(&c[0], 4, 8, b);
    check(ok && t[0] > b[0] + 20 && b[2] > t[2] + 20, "картинки: прогрессивный JPEG");
    ok = img_decode(T_RGB16, sizeof T_RGB16, 8, 4, c);
    seen(&c[0], 0, 8, t);
    check(ok && cells_ascii(c, 32) && c[0].ch != ' ' && t[0] > 170 && t[1] < 80 && t[2] < 80,
          "картинки: ровная клетка -> символ по яркости, средний цвет сохранён");
    ok = img_decode(T_GRAY32x8, sizeof T_GRAY32x8, 4, 2, c);
    seen(&c[0], 0, 8, t); seen(&c[3], 0, 8, b);
    check(ok && t[0] + 60 < b[0], "картинки: серый JPEG, слева темнее, чем справа");
    /* мелкая сетка: 16x8 клеток (2x4 пикселя на клетку) — каждая четверть картинки своим цветом */
    ok = img_decode(T_RGB16, sizeof T_RGB16, 16, 8, c);
    double q[4][3];
    seen(&c[1 * 16 + 2], 0, 8, q[0]); seen(&c[1 * 16 + 13], 0, 8, q[1]);
    seen(&c[6 * 16 + 2], 0, 8, q[2]); seen(&c[6 * 16 + 13], 0, 8, q[3]);
    check(ok && q[0][0] > 170 && q[0][1] < 80 && q[1][1] > 170 && q[1][0] < 80 && q[2][2] > 170 && q[2][0] < 80 &&
          q[3][0] > 200 && q[3][1] > 200 && q[3][2] > 200, "картинки: мелкая сетка 16x8 — четверти своими цветами");
    ok = img_decode(T_RGB16, sizeof T_RGB16, IMG_MAX_W, 4, c);
    check(ok && cells_ascii(c, IMG_MAX_W * 4), "картинки: увеличение (картинка меньше сетки)");
    /* 2. размеры: больше 512 — отказ; ноль — отказ */
    u8 big[sizeof T_RGB16];
    memcpy(big, T_RGB16, sizeof big);
    int sof = sof_at(big, sizeof big);
    bool sz = sof > 0;
    if (sz) {
        big[sof + 5] = 0x02; big[sof + 6] = 0x58; big[sof + 7] = 0x02; big[sof + 8] = 0x58;   /* 600x600 */
        sz = !img_decode(big, sizeof big, 2, 1, c);
        big[sof + 5] = 0; big[sof + 6] = 0;                                                   /* высота 0 */
        sz = sz && !img_decode(big, sizeof big, 2, 1, c);
    }
    check(sz && s_jmem == 0, "картинки: больше 512x512 и нулевой размер отвергнуты");
    /* 3. усечённые, испорченные, мусор: без падений, только ASCII, без утечек */
    bool safe = true;
    for (size_t n = 0; n < sizeof T_RGB16; n++) {
        memset(c, 0x7f, sizeof c);
        if (img_decode(T_RGB16, n, 4, 2, c) && !cells_ascii(c, 8)) safe = false;
        if (s_jmem) safe = false;
    }
    check(safe, "картинки: усечённый JPEG любой длины — без сбоев");
    u32 seed = 0x9e3779b9u;
    int decoded = 0;
    for (int it = 0; it < 3000 && safe; it++) {
        const u8 *src = (it & 1) ? T_PROG16 : T_RGB16;
        size_t n = (it & 1) ? sizeof T_PROG16 : sizeof T_RGB16;
        memcpy(big, src, n);
        int flips = 1 + (int)(rnd(&seed) % 4);
        for (int f = 0; f < flips; f++) big[3 + rnd(&seed) % (n - 3)] = (u8)rnd(&seed);
        int w = 1 + (int)(rnd(&seed) % 8), h = 1 + (int)(rnd(&seed) % 4);
        if (img_decode(big, n, w, h, c)) { decoded++; if (!cells_ascii(c, w * h)) safe = false; }
        if (s_jmem) safe = false;
    }
    for (int it = 0; it < 500 && safe; it++) {
        size_t n = 4 + rnd(&seed) % (sizeof big - 4);
        for (size_t i = 0; i < n; i++) big[i] = (u8)rnd(&seed);
        big[0] = 0xFF; big[1] = 0xD8; big[2] = 0xFF;
        if (img_decode(big, n, 3, 2, c) && !cells_ascii(c, 6)) safe = false;
        if (s_jmem) safe = false;
    }
    check(safe && decoded > 0, "картинки: 3000 испорченных JPEG и 500 мусорных — без сбоев и утечек");
    /* 4. адреса */
    check(avatar_hash_ok("0123456789abcdef0123456789abcdef") && avatar_hash_ok("a_0123456789abcdef0123456789abcdef") &&
          !avatar_hash_ok("0123456789abcdef0123456789abcde") && !avatar_hash_ok("0123456789ABCDEF0123456789abcdef") &&
          !avatar_hash_ok("../../../x") && !avatar_hash_ok("0123456789abcdef0123456789abcdef0"),
          "картинки: хэш аватара проверяется строго");
    char u[700];
    u64 k1 = 0, k2 = 0;
    bool au = att_url("https://cdn.discordapp.com/attachments/123/456/cat_1.png?ex=6a1&is=6b2&hm=ff00&evil=1&x", 40, 24, u, sizeof u, &k1) &&
              !strcmp(u, "https://media.discordapp.net/attachments/123/456/cat_1.png?ex=6a1&is=6b2&hm=ff00&format=jpeg&width=40&height=24") &&
              att_url("https://media.discordapp.net/attachments/123/456/cat_1.png?ex=777", 8, 8, u, sizeof u, &k2) && k1 == k2;
    static const char *const BAD[] = {
        "http://media.discordapp.net/attachments/1/2/a.png",
        "https://media.discordapp.net.evil.com/attachments/1/2/a.png",
        "https://media.discordapp.net@evil.com/attachments/1/2/a.png",
        "https://evil.com/https://media.discordapp.net/attachments/1/2/a.png",
        "https://media.discordapp.net/attachments/1/2/../../x.png",
        "https://media.discordapp.net/attachments/1/2/a.png#x",
        "https://media.discordapp.net/attachments/1/2/a b.png",
        "https://media.discordapp.net/attachments/x/2/a.png",
        "https://media.discordapp.net/avatars/1/2/a.png",
        "https://media.discordapp.net/attachments/1/2/a.png?hm=zz",
        "https://media.discordapp.net/attachments/1/2/a.png?hm=1&hm=2",
    };
    for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++) if (att_url(BAD[i], 8, 8, u, sizeof u, &k1)) au = false;
    check(au, "картинки: адрес вложения нормализуется, чужие хосты и пути отвергнуты");
    /* 5. только видимое: ушло с экрана — затёрто и забыто; пауза перед загрузкой; предел кадра */
    bool was = s_on, demo = O.demo;
    s_on = true;
    O.demo = false;
    img_forget();
    ImgCell av[8];
    bool dflt = img_avatar(4u << 22, NULL, 4, 2, av) && cells_ascii(av, 8) && av[0].bg[0] == 0xed;
    char hsh[40];
    img_frame();                                /* кадр 1: на экране 100 аватаров, все «скачались» */
    for (int i = 0; i < 100; i++) {
        snprintf(hsh, sizeof hsh, "%032x", i);
        img_avatar(1000 + (u64)i, hsh, 4, 2, av);
        Ent *e = find(square_key("avatars", 1000 + (u64)i, hsh), K_AV, 4, 2);
        if (e) { ImgCell *cc = malloc(sizeof(ImgCell) * 8); memset(cc, ' ', sizeof(ImgCell) * 8); put_ready(e, cc); }
    }
    bool vis = count_used() == 100 && s_bytes == 100 * 8 * sizeof(ImgCell);
    img_frame();                                /* кадр 2: видны только 10 (прокрутили) */
    for (int i = 90; i < 100; i++) {
        snprintf(hsh, sizeof hsh, "%032x", i);
        vis = vis && img_avatar(1000 + (u64)i, hsh, 4, 2, av) && av[0].ch == ' ';
    }
    img_frame();                                /* начало кадра 3: невидимые 90 аватаров остались (5 КБ < AV_KEEP_BYTES) */
    vis = vis && count_used() == 100 && s_bytes == 100 * 8 * sizeof(ImgCell) &&
          img_avatar(1000, "00000000000000000000000000000000", 4, 2, av) && av[0].ch == ' ';   /* вернулся — сразу, без загрузки */
    img_avatar(5000, "0123456789abcdef0123456789abcdef", 4, 2, av);   /* новый — ждёт загрузки */
    Ent *back = find(square_key("avatars", 5000, "0123456789abcdef0123456789abcdef"), K_AV, 4, 2);
    vis = vis && back && back->state == ST_WANT && !eligible(back, now_ms());  /* пауза 150 мс */
    if (back) back->seen_at -= LOAD_DELAY_MS;
    vis = vis && back && eligible(back, now_ms());
    img_frame();
    img_frame();                                /* два кадра без запросов: незагруженный забыт, готовые аватары остались */
    vis = vis && count_used() == 100 && !find(square_key("avatars", 5000, "0123456789abcdef0123456789abcdef"), K_AV, 4, 2);
    /* предел аватаров вне экрана: 100 аватаров 16x8 (~90 КБ) -> остаются последние показанные, <= AV_KEEP_BYTES */
    img_forget();
    s_on = true;
    img_frame();
    ImgCell avb[128];
    for (int i = 0; i < 100; i++) {             /* каждый — в своём кадре: i-й показан позже (i-1)-го */
        snprintf(hsh, sizeof hsh, "%032x", i);
        img_avatar(2000 + (u64)i, hsh, 16, 8, avb);
        Ent *e = find(square_key("avatars", 2000 + (u64)i, hsh), K_AV, 16, 8);
        if (e) { ImgCell *cc = malloc(sizeof(ImgCell) * 128); memset(cc, ' ', sizeof(ImgCell) * 128); put_ready(e, cc); }
        img_frame();
    }
    img_frame();
    snprintf(hsh, sizeof hsh, "%032x", 99);
    Ent *last = find(square_key("avatars", 2099, hsh), K_AV, 16, 8);
    snprintf(hsh, sizeof hsh, "%032x", 0);
    Ent *first = find(square_key("avatars", 2000, hsh), K_AV, 16, 8);
    bool keep = kept_bytes() <= AV_KEEP_BYTES && kept_bytes() > AV_KEEP_BYTES - 128 * sizeof(ImgCell) && last && last->state == ST_READY && !first;
    img_forget();
    vis = vis && keep && count_used() == 0 && s_bytes == 0;
    check(dflt && vis, "картинки: стандартный аватар без сети; аватары вне экрана — до 64 КБ, вытесняются давние; пауза 150 мс");
    /* предел кадра и память неудач */
    img_frame();
    bool lim = true;
    for (int i = 0; i < 12; i++) {              /* 12 просмотров 256x64 (114 КБ каждый) */
        char au2[128], uu[700];
        u64 kk;
        snprintf(au2, sizeof au2, "https://media.discordapp.net/attachments/1/%d/p.jpg", i + 1);
        ImgCell *big2 = malloc(sizeof(ImgCell) * IMG_MAX_W * IMG_MAX_H);
        if (big2 && att_url(au2, 512, 128, uu, sizeof uu, &kk)) {
            want(kk, K_PV, IMG_MAX_W, IMG_MAX_H, uu);
            Ent *e = find(kk, K_PV, IMG_MAX_W, IMG_MAX_H);
            if (e) { memset(big2, ' ', sizeof(ImgCell) * IMG_MAX_W * IMG_MAX_H); put_ready(e, big2); big2 = NULL; }
        }
        free(big2);
        if (s_bytes > FRAME_MAX_BYTES) lim = false;
    }
    Ent *f = &s_e[0];
    dl_fail(f, 60000);
    u64 fk = f->key;
    img_forget();
    fail_add(square_key("avatars", 5, "0123456789abcdef0123456789abcdef"), now_ms() + 60000);
    lim = lim && !img_avatar(5, "0123456789abcdef0123456789abcdef", 4, 2, av) && count_used() == 0 && fk;
    img_forget();
    check(lim && s_bytes == 0, "картинки: предел клеток кадра 512 КБ; неудача помнится ключом и не качается снова");
    /* 6. значки серверов и размер запроса: не меньше 128 px (требование владельца) */
    ImgCell ic[18];
    bool icn = !img_icon(42, NULL, 6, 3, ic) && !img_icon(42, "", 6, 3, ic) && !img_icon(42, "zz", 6, 3, ic) &&
               !img_icon(42, "0123456789abcdef0123456789abcdef", 6, 3, ic) &&
               !img_icon_round(42, NULL, 6, 3, NULL, ic);
    Ent *ie = find(square_key("icons", 42, "0123456789abcdef0123456789abcdef"), K_AV, 6, 3);
    icn = icn && ie && ie->state == ST_WANT &&
          !strcmp(ie->url, "https://cdn.discordapp.com/icons/42/0123456789abcdef0123456789abcdef.jpg?size=128") &&
          !find(square_key("avatars", 42, "0123456789abcdef0123456789abcdef"), K_AV, 6, 3);
    img_forget();
    check(icn, "картинки: значок сервера — icons/{id}/{hash}.jpg?size=128, свой ключ, без значка false");
    img_icon(42, "0123456789abcdef0123456789abcdef", 24, 12, ic);
    img_avatar(7, "0123456789abcdef0123456789abcdef", 12, 6, ic);
    img_avatar(7, "0123456789abcdef0123456789abcdef", 16, 8, ic);
    img_avatar(7, "0123456789abcdef0123456789abcdef", 96, 48, ic);
    Ent *i24 = find(square_key("icons", 42, "0123456789abcdef0123456789abcdef"), K_AV, 24, 12);
    Ent *a12 = find(square_key("avatars", 7, "0123456789abcdef0123456789abcdef"), K_AV, 12, 6);
    Ent *a16 = find(square_key("avatars", 7, "0123456789abcdef0123456789abcdef"), K_AV, 16, 8);
    Ent *a96 = find(square_key("avatars", 7, "0123456789abcdef0123456789abcdef"), K_AV, 96, 48);
    int pw, ph, pw2, ph2, pw3, ph3;
    att_px(128, 32, 0, 0, &pw, &ph);            /* 256x128 -> меньшая сторона 256: 512x256 */
    att_px(10, 5, 0, 0, &pw2, &ph2);            /* 20x20 -> 256x256 */
    att_px(10, 5, 100, 100, &pw3, &ph3);        /* исходник 100x100: не больше него */
    check(i24 && a12 && a16 && a96 && a12 != a16 && strstr(i24->url, ".jpg?size=128") && strstr(a12->url, ".jpg?size=128") &&
          strstr(a16->url, ".jpg?size=128") && strstr(a96->url, ".jpg?size=256") &&
          pw == 512 && ph == 256 && pw2 == 256 && ph2 == 256 && pw3 == 100 && ph3 == 100,
          "картинки: размер запроса >= 128 px (96x48 -> 256), превью >= 256 по меньшей стороне, <= исходника; ключ с размером");
    img_forget();
    /* 7. круглая маска: снаружи прозрачно, середина цела, на краю один из цветов — фон колонки,
     * видимая площадь картинки ~ площадь эллипса (тонкая дуга без ступенек), левое = правому */
    static const u8 IMGC[3] = { 200, 100, 50 }, COLB[3] = { 10, 20, 30 };
    static const int RSZ[][2] = { { 2, 1 }, { 4, 2 }, { 6, 3 }, { 8, 4 }, { 12, 6 }, { 16, 8 }, { 24, 12 }, { 48, 24 } };
    bool rnd_ok = true;
    for (size_t s = 0; s < sizeof RSZ / sizeof *RSZ; s++) {
        int w = RSZ[s][0], h = RSZ[s][1];
        ImgCell rc[48 * 24];
        for (int i = 0; i < w * h; i++) { rc[i].ch = ' '; memcpy(rc[i].fg, IMGC, 3); memcpy(rc[i].bg, IMGC, 3); }
        img_round(rc, w, h, COLB);
        const int corner[4] = { 0, w - 1, (h - 1) * w, h * w - 1 };
        for (int k = 0; k < 4; k++) {
            const ImgCell *a = &rc[corner[k]];
            if (a->ch && memcmp(a->bg, COLB, 3) && memcmp(a->fg, COLB, 3)) rnd_ok = false;
        }
        if (h >= 3 && (rc[(h / 2) * w + w / 2].ch != ' ' || memcmp(rc[(h / 2) * w + w / 2].bg, IMGC, 3))) rnd_ok = false;
        double area = 0, d2 = 0;
        for (int k = 0; k < 3; k++) d2 += (double)(IMGC[k] - COLB[k]) * (IMGC[k] - COLB[k]);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const ImgCell *a = &rc[y * w + x], *m = &rc[y * w + (w - 1 - x)];
                if (a->ch && (a->ch < 0x20 || a->ch > 0x7E)) rnd_ok = false;
                bool same = a->ch == ' ' && !memcmp(a->bg, IMGC, 3);
                if (!same && a->ch && memcmp(a->bg, COLB, 3) && memcmp(a->fg, COLB, 3)) rnd_ok = false;
                if (!a->ch != !m->ch) rnd_ok = false;
                double p[3];
                for (int i = 0; i < 8; i++) {
                    cell_part(a, i, COLB, p);
                    double dot = 0;
                    for (int k = 0; k < 3; k++) dot += (p[k] - COLB[k]) * (IMGC[k] - COLB[k]);
                    area += dot / d2 / 8;
                }
            }
        double want_area = 3.14159265 / 4 * w * h;
        if (area < want_area * 0.85 || area > want_area * 1.15) rnd_ok = false;
    }
    check(rnd_ok, "картинки: круглая маска 2x1..48x24 — прозрачно снаружи, край на фоне колонки, площадь ~ круга, симметрия");
    s_on = was;
    O.demo = demo;
    return 0;
}
