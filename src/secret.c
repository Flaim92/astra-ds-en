/* Токен: живёт только в закреплённых страницах (не уходят в подкачку и в дампы),
 * хранится только в системном хранилище секретов, вводится скрыто. Копии затираются. */
#include <stdio.h>
#include <string.h>
#include "mc.h"
#include "plat.h"

#define TOKEN_MAX 256
#define SCRATCH_CAP 8192

#define VLESS_AREA 16384

/* Раскладка защищённой области: [токен][заголовок Authorization][scratch для IDENTIFY/RESUME]
 * [область VLESS: ссылка, разобранные поля, логин и пароль локального SOCKS — её размечает vless.c] */
typedef struct {
    char token[TOKEN_MAX];
    char auth[TOKEN_MAX + 32];
    char scratch[SCRATCH_CAP];
    union { char raw[VLESS_AREA]; max_align_t align; } vless;
} Secret;

/* Временно здесь, до переноса в mc.h (раздел secret.c): защищённая область для vless.c,
 * NULL — область не выделена (secret_init не вызван или уже secret_destroy). */
void *secret_vless_area(size_t *cap);

static Secret *g_sec;
static size_t g_sec_size;

bool secret_init(void) {
    g_sec_size = (sizeof(Secret) + 4095) & ~(size_t)4095;
    g_sec = plat_pages(g_sec_size);
    if (!g_sec) return false;
    plat_lock(g_sec, g_sec_size);
    return true;
}

static bool token_ok(const char *t, size_t n) {
    if (n < 20 || n >= TOKEN_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

void secret_set(const char *token, size_t n) {
    plat_wipe(g_sec->token, sizeof g_sec->token);
    plat_wipe(g_sec->auth, sizeof g_sec->auth);
    if (!token_ok(token, n)) return;
    memcpy(g_sec->token, token, n);
    memcpy(g_sec->auth, "Authorization: ", 15);
    memcpy(g_sec->auth + 15, token, n);
}
const char *secret_token(void) { return g_sec && g_sec->token[0] ? g_sec->token : NULL; }
const char *secret_auth_header(void) { return g_sec->auth; }
char *secret_scratch(size_t *cap) { *cap = SCRATCH_CAP; return g_sec->scratch; }
void *secret_vless_area(size_t *cap) {
    *cap = g_sec ? VLESS_AREA : 0;
    return g_sec ? g_sec->vless.raw : NULL;
}

void secret_destroy(void) {
    if (!g_sec) return;
    plat_wipe(g_sec, g_sec_size);
    plat_unlock(g_sec, g_sec_size);
    plat_pages_free(g_sec, g_sec_size);
    g_sec = NULL;
}

/* Временные буферы для обмена с хранилищем — тоже в защищённой области (scratch). */
bool cred_load(void) {
    size_t cap;
    char *buf = secret_scratch(&cap);
    int n = keyring_load("token", buf, TOKEN_MAX);
    bool ok = false;
    if (n > 0) { secret_set(buf, (size_t)n); ok = secret_token() != NULL; }
    plat_wipe(buf, cap);
    return ok;
}
bool cred_save(void) {
    const char *t = secret_token();
    return t && keyring_save("token", t, strlen(t));
}
void cred_delete(void) { keyring_delete("token"); }

bool read_hidden(const char *prompt) {
    size_t cap;
    char *buf = secret_scratch(&cap);
    bool ok = term_read_secret(prompt, buf, TOKEN_MAX);
    if (ok) secret_set(buf, strlen(buf));
    plat_wipe(buf, cap);
    return ok && secret_token() != NULL;
}
