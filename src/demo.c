/* Демо-режим: фальшивые данные без сети. Идут тем же путём, что и данные Discord
 * (JSON -> store), поэтому проверяют настоящий код разбора, прав и очистки текста.
 * Сервер «Уютный уголок» (выдуманный) — раскладка как у Discord; id каналов
 * 3001..3012 и сообщения #3001 нужны --selftest. Остальные серверы колонки приходят чуть позже —
 * событиями GUILD_CREATE, как «ленивые» серверы Discord (проверка «2 сервера в READY» не меняется). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc.h"
#include "demo_en.h"
#ifdef ASTRA_VIDEO
#define DEMO_STREAM ",\"self_stream\":true"    /* Video: в «ОБЩИЙ» двое транслируют (снимки панели просмотра) */
#else
#define DEMO_STREAM ""
#endif

/* ---------- i18n п.4: английский демо-набор ----------
 * Демо-данные написаны по-русски; при английском интерфейсе JSON-строки демо подменяются целиком по таблице
 * DEMO_EN (demo_en.h) прямо перед разбором (json_parse -> demo_en_json). Только в --demo: живые данные не трогаются.
 * selftest идёт по-русски (ui_lang_init(0)) — проверки демо-данных не меняются. */
extern bool g_lang_en;
static const char *demo_en_tok(const char *t, size_t n, size_t *digits) {
    *digits = 0;
    for (size_t i = 0; i < sizeof DEMO_EN / sizeof *DEMO_EN; i++) {
        const char *ru = DEMO_EN[i].ru;
        size_t rn = strlen(ru);
        if (rn == n && !memcmp(ru, t, n)) return DEMO_EN[i].en;
        if (rn > 2 && !strcmp(ru + rn - 2, "%d") && n > rn - 2 && !memcmp(ru, t, rn - 2)) {   /* «... %d» */
            size_t k = rn - 2;
            while (k < n && t[k] >= '0' && t[k] <= '9') k++;
            if (k == n) { *digits = n - (rn - 2); return DEMO_EN[i].en; }
        }
    }
    return NULL;
}
/* JSON-текст демо -> он же с английскими строками; NULL — подменять нечего (или не английский/не демо) */
char *demo_en_json(const char *s, size_t n, size_t *out_n) {
    if (!O.demo || !g_lang_en) return NULL;
    Buf b = {0};
    size_t done = 0;
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '"') continue;
        size_t j = i + 1;
        bool cyr = false;
        while (j < n && s[j] != '"') {
            if ((unsigned char)s[j] == 0xD0 || (unsigned char)s[j] == 0xD1) cyr = true;
            j += s[j] == '\\' ? 2 : 1;
        }
        if (j >= n) break;
        if (cyr) {
            size_t dg;
            const char *en = demo_en_tok(s + i + 1, j - i - 1, &dg);
            if (en) {
                buf_put(&b, s + done, i + 1 - done);
                if (dg) {                       /* «%d» -> те же цифры */
                    const char *pd = strstr(en, "%d");
                    buf_put(&b, en, (size_t)(pd - en));
                    buf_put(&b, s + j - dg, dg);
                    buf_puts(&b, pd + 2);
                } else buf_puts(&b, en);
                done = j;
                any = true;
            }
        }
        i = j;
    }
    if (!any) { free(b.p); return NULL; }
    buf_put(&b, s + done, n - done);
    *out_n = b.len;
    return b.p;
}
/* простая строка демо (не JSON: тема канала, уведомления) */
const char *demo_tr(const char *ru) {
    if (!O.demo || !g_lang_en) return ru;
    size_t dg;
    const char *en = demo_en_tok(ru, strlen(ru), &dg);
    return en && !dg ? en : ru;
}

static const char READY[] =
    "{\"session_id\":\"demo\",\"user\":{\"id\":\"1000\",\"username\":\"you\",\"global_name\":\"Вы\"},"
    "\"users\":[{\"id\":\"1001\",\"username\":\"alice\",\"global_name\":\"Алиса\"},"
    "{\"id\":\"1002\",\"username\":\"bob\",\"global_name\":\"Боб\"},"
    "{\"id\":\"1003\",\"username\":\"mallory\",\"global_name\":\"\\u001b[31mМэллори\\u202e\"},"
    "{\"id\":\"1004\",\"username\":\"filin\",\"global_name\":\"Ночной Филин\"},"
    "{\"id\":\"1005\",\"username\":\"mira\",\"global_name\":\"Мира\"},"
    "{\"id\":\"1006\",\"username\":\"sonny\",\"global_name\":\"Сонный Кот\"},"
    "{\"id\":\"1007\",\"username\":\"neznakomec\",\"global_name\":\"Незнакомец\"},"
    "{\"id\":\"1008\",\"username\":\"vanya\",\"global_name\":\"Ваня\"},"
    "{\"id\":\"1009\",\"username\":\"spamer\",\"global_name\":\"Спамер\"}],"
    /* друзья и заявки (фаза 4): разные статусы, входящая (1007 — объектом user, как без дедупликации),
     * исходящая, заблокированный; тип 5 (неявный) не хранится */
    "\"relationships\":[{\"id\":\"1001\",\"type\":1,\"user_id\":\"1001\"},{\"id\":\"1002\",\"type\":1,\"user_id\":\"1002\"},"
    "{\"id\":\"1004\",\"type\":1,\"user_id\":\"1004\"},{\"id\":\"1005\",\"type\":1,\"user_id\":\"1005\"},"
    "{\"id\":\"1006\",\"type\":1,\"user_id\":\"1006\"},"
    "{\"id\":\"1007\",\"type\":3,\"user\":{\"id\":\"1007\",\"username\":\"neznakomec\",\"global_name\":\"Незнакомец\"}},"
    "{\"id\":\"1008\",\"type\":4,\"user_id\":\"1008\"},{\"id\":\"1009\",\"type\":2,\"user_id\":\"1009\"},"
    "{\"id\":\"1003\",\"type\":5,\"user_id\":\"1003\"}],"
    "\"merged_presences\":{\"friends\":[{\"user_id\":\"1001\",\"status\":\"online\"},{\"user_id\":\"1002\",\"status\":\"idle\"},"
    "{\"user_id\":\"1004\",\"status\":\"dnd\"},{\"user_id\":\"1006\",\"status\":\"online\"},{\"user_id\":\"1009\",\"status\":\"online\"}],\"guilds\":[]},"
    "\"guilds\":[{\"id\":\"2000\",\"properties\":{\"name\":\"Уютный уголок\",\"owner_id\":\"1001\",\"icon\":\"0123456789abcdef0123456789abcdef\"},"
    "\"roles\":[{\"id\":\"2000\",\"permissions\":\"274881170497\"},{\"id\":\"2001\",\"permissions\":\"8\",\"color\":15548997,\"position\":3},"
    "{\"id\":\"2002\",\"permissions\":\"0\",\"color\":3447003,\"position\":1},{\"id\":\"2003\",\"permissions\":\"0\",\"color\":5763719,\"position\":2},{\"id\":\"2004\",\"permissions\":\"268435505\",\"position\":4}],"
    "\"channels\":["
    "{\"id\":\"3000\",\"type\":4,\"name\":\"Текстовые каналы\",\"position\":0},"
    "{\"id\":\"3001\",\"type\":0,\"name\":\"general\",\"position\":0,\"parent_id\":\"3000\",\"last_message_id\":\"9005\"},"
    "{\"id\":\"3002\",\"type\":0,\"name\":\"новости\",\"position\":3,\"parent_id\":\"3000\",\"last_message_id\":\"9100\"},"
    "{\"id\":\"3003\",\"type\":0,\"name\":\"секретный\",\"position\":2,\"parent_id\":\"3000\","
    "\"permission_overwrites\":[{\"id\":\"2000\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"}]},"
    "{\"id\":\"3004\",\"type\":0,\"name\":\"музыка\",\"position\":1,\"parent_id\":\"3000\",\"last_message_id\":\"9500\"},"
    "{\"id\":\"3005\",\"type\":0,\"name\":\"флуд-2\",\"position\":4,\"parent_id\":\"3000\",\"last_message_id\":\"9600\",\"rate_limit_per_user\":10},"
    "{\"id\":\"3006\",\"type\":0,\"name\":\"рисунки\",\"position\":5,\"parent_id\":\"3000\",\"last_message_id\":\"9700\"},"
    "{\"id\":\"3007\",\"type\":0,\"name\":\"флуд\",\"position\":6,\"parent_id\":\"3000\",\"last_message_id\":\"20150\"},"
    "{\"id\":\"3008\",\"type\":0,\"name\":\"игры\",\"position\":7,\"parent_id\":\"3000\",\"last_message_id\":\"9900\"},"
    /* форум (№95): посты — forum_open (демо отдаёт три поста) */
    "{\"id\":\"3020\",\"type\":15,\"name\":\"вопросы\",\"position\":9,\"parent_id\":\"3000\"},"
    /* медленный режим 15 с, на демо-меня действует (право «управлять каналами» здесь снято) */
    "{\"id\":\"3009\",\"type\":0,\"name\":\"медленный\",\"position\":8,\"parent_id\":\"3000\",\"rate_limit_per_user\":15,"
    "\"permission_overwrites\":[{\"id\":\"2004\",\"type\":0,\"allow\":\"0\",\"deny\":\"16\"}]},"
    "{\"id\":\"3010\",\"type\":4,\"name\":\"Голосовые каналы\",\"position\":1},"
    "{\"id\":\"3011\",\"type\":2,\"name\":\"ОБЩИЙ\",\"position\":0,\"parent_id\":\"3010\"},"
    "{\"id\":\"3012\",\"type\":2,\"name\":\"Игры\",\"position\":1,\"parent_id\":\"3010\"},"
    "{\"id\":\"3013\",\"type\":2,\"name\":\"Созвоны\",\"position\":2,\"parent_id\":\"3010\"},"
    "{\"id\":\"3014\",\"type\":2,\"name\":\"Музыка\",\"position\":3,\"parent_id\":\"3010\"},"
    "{\"id\":\"3015\",\"type\":2,\"name\":\"Кино\",\"position\":4,\"parent_id\":\"3010\"},"
    "{\"id\":\"3016\",\"type\":2,\"name\":\"Учёба\",\"position\":5,\"parent_id\":\"3010\"},"
    "{\"id\":\"3017\",\"type\":2,\"name\":\"Тихая\",\"position\":6,\"parent_id\":\"3010\"},"
    "{\"id\":\"3018\",\"type\":2,\"name\":\"Отдых\",\"position\":7,\"parent_id\":\"3010\",\"last_message_id\":\"9993\"}],"
    "\"voice_states\":[{\"user_id\":\"1006\",\"channel_id\":\"3011\",\"self_mute\":false,\"self_deaf\":false" DEMO_STREAM "},"
    "{\"user_id\":\"1002\",\"channel_id\":\"3011\",\"self_mute\":true,\"self_deaf\":false" DEMO_STREAM "}]},"
    "{\"id\":\"2100\",\"properties\":{\"name\":\"Другой сервер\",\"owner_id\":\"1002\",\"icon\":\"1123456789abcdef0123456789abcdef\"},"
    "\"roles\":[{\"id\":\"2100\",\"permissions\":\"3263553\"}],"
    "\"channels\":[{\"id\":\"3100\",\"type\":0,\"name\":\"чат\",\"position\":0,\"last_message_id\":\"9200\"}]},"
    /* №2: ещё три сервера для папок (папка «Игры» с цветом: 2100+2500; папка без имени: 2600+2700) */
    "{\"id\":\"2500\",\"properties\":{\"name\":\"Шахматный клуб\",\"owner_id\":\"1004\"},\"roles\":[{\"id\":\"2500\",\"permissions\":\"3263553\"}],"
    "\"channels\":[{\"id\":\"3500\",\"type\":0,\"name\":\"партии\",\"position\":0}]},"
    "{\"id\":\"2600\",\"properties\":{\"name\":\"Работа\",\"owner_id\":\"1005\"},\"roles\":[{\"id\":\"2600\",\"permissions\":\"3263553\"}],"
    "\"channels\":[{\"id\":\"3600\",\"type\":0,\"name\":\"общий\",\"position\":0}]},"
    "{\"id\":\"2700\",\"properties\":{\"name\":\"Курсы\",\"owner_id\":\"1006\"},\"roles\":[{\"id\":\"2700\",\"permissions\":\"3263553\"}],"
    "\"channels\":[{\"id\":\"3750\",\"type\":0,\"name\":\"лекции\",\"position\":0}]}],"
    /* папки — в protobuf, как у настоящего клиента (гоняет разбор user_settings_proto) */
    "\"user_settings_proto\":\"GAVybQopChA0CAAAAAAAAMQJAAAAAAAAEgIIARoKCgjQmNCz0YDRiyIFCNzK7gEKFgoQKAoAAAAAAACMCgAAAAAAABICCAISKNAHAAAAAAAANAgAAAAAAADECQAAAAAAACgKAAAAAAAAjAoAAAAAAAA=\","
    "\"merged_members\":[[{\"user_id\":\"1000\",\"roles\":[\"2004\"]},{\"user_id\":\"1004\",\"roles\":[\"2001\"]},"
    "{\"user_id\":\"1005\",\"roles\":[\"2003\"]},{\"user_id\":\"1006\",\"roles\":[\"2002\"]}],[{\"user_id\":\"1000\",\"roles\":[]}],"
    /* №128: в «Шахматном клубе» (2500) правила ещё не приняты — писать нельзя, есть «Принять правила» */
    "[{\"user_id\":\"1000\",\"roles\":[],\"pending\":true}],[{\"user_id\":\"1000\",\"roles\":[]}],[{\"user_id\":\"1000\",\"roles\":[]}]],"
    "\"private_channels\":[{\"id\":\"4000\",\"type\":1,\"recipient_ids\":[\"1001\"],\"last_message_id\":\"9301\"},"
    "{\"id\":\"4001\",\"type\":3,\"name\":\"\",\"recipient_ids\":[\"1001\",\"1002\"],\"last_message_id\":\"9400\"},"
    /* №109/111: запрос на переписку и спам (от не-друзей, с картинкой), группы — где я владелец и где нет */
    "{\"id\":\"4002\",\"type\":1,\"recipients\":[{\"id\":\"1020\",\"username\":\"stranger\",\"global_name\":\"Незнакомка\",\"avatar\":\"0123456789abcdef0123456789abcd20\"}],"
    "\"is_message_request\":true,\"last_message_id\":\"9420\"},"
    "{\"id\":\"4003\",\"type\":1,\"recipients\":[{\"id\":\"1021\",\"username\":\"freegift\",\"global_name\":\"Бесплатный Нитро\",\"avatar\":\"0123456789abcdef0123456789abcd21\"}],"
    "\"is_spam\":true,\"last_message_id\":\"9421\"},"
    "{\"id\":\"4004\",\"type\":3,\"name\":\"Поход в горы\",\"owner_id\":\"1000\",\"recipient_ids\":[\"1001\",\"1002\",\"1004\"],\"last_message_id\":\"9430\"},"
    "{\"id\":\"4005\",\"type\":3,\"name\":\"Учёба\",\"owner_id\":\"1001\",\"recipient_ids\":[\"1001\",\"1006\"],\"last_message_id\":\"9431\"}],"
    "\"read_state\":{\"entries\":[{\"id\":\"3001\",\"last_message_id\":\"9005\"},{\"id\":\"3002\",\"last_message_id\":\"9000\"},"
    "{\"id\":\"3004\",\"last_message_id\":\"9400\",\"mention_count\":2},{\"id\":\"3005\",\"last_message_id\":\"9550\",\"mention_count\":2},"
    "{\"id\":\"3006\",\"last_message_id\":\"9700\"},{\"id\":\"3007\",\"last_message_id\":\"20150\"},{\"id\":\"3008\",\"last_message_id\":\"9900\"},"
    "{\"id\":\"3018\",\"last_message_id\":\"9993\"},"
    "{\"id\":\"4000\",\"last_message_id\":\"9300\",\"mention_count\":1}]},"
    "\"user_guild_settings\":{\"entries\":[]}}";

/* Серверы, которые приходят после READY: со значком (картинка) и без (сокращение названия). */
static const struct { const char *id, *name, *icon; int ment; } MORE[] = {
    { "2200", "Котики", "2123456789abcdef0123456789abcdef", 15 },
    { "2201", "Кристалл", "3123456789abcdef0123456789abcdef", 0 },
    { "2202", "Огонь", "4123456789abcdef0123456789abcdef", 0 },
    { "2203", "Череп", "5123456789abcdef0123456789abcdef", 0 },
    { "2204", "Звезда", "6123456789abcdef0123456789abcdef", 0 },
    { "2205", "Стратеги", "", 0 },
    { "2206", "Кубики", "", 0 },
    { "2207", "Гонщики", "", 0 },
    { "2208", "Небо", "", 0 },
};

/* #flood (3007): длинная история 20001..20150 (больше одной подгрузки) — для «Вернуться к настоящему»
 * и подгрузки старых. Сообщения генерируются по номерам [from, to], новые первыми (как отвечает Discord). */
#define FLOOD_FIRST 20001
#define FLOOD_LAST 20150
static void flood_gen(Buf *b, int from, int to) {
    if (from < FLOOD_FIRST) from = FLOOD_FIRST;
    if (to > FLOOD_LAST) to = FLOOD_LAST;
    buf_puts(b, "[");
    for (int i = to; i >= from; i--)
        buf_printf(b, "%s{\"id\":\"%d\",\"channel_id\":\"3007\",\"author\":{\"id\":\"%s\",\"username\":\"%s\",\"global_name\":\"%s\"},"
                   "\"content\":\"флуд номер %d\",\"timestamp\":\"2026-11-08T%02d:%02d:00+00:00\"}",
                   i == to ? "" : ",", i, i % 2 ? "1001" : "1002", i % 2 ? "alice" : "bob", i % 2 ? "Алиса" : "Боб",
                   i - FLOOD_FIRST + 1, 6 + (i - FLOOD_FIRST) / 60, (i - FLOOD_FIRST) % 60);
    buf_puts(b, "]");
}
/* unix-секунды -> "ГГГГ-ММ-ДДTчч:мм:сс+00:00" (алгоритм Хиннанта) */
static void iso_at(i64 t, char *out, size_t n) {
    i64 days = t / 86400, sod = t % 86400;
    i64 z = days + 719468, era = z / 146097, doe = z - era * 146097;
    i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    i64 mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9, y = yoe + era * 400 + (m <= 2);
    snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02d+00:00", (int)y, (int)m, (int)d, (int)(sod / 3600), (int)(sod / 60 % 60), (int)(sod % 60));
}
/* #игры (3008): эмбеды (№53) и опросы (№41) для проверки карточек */
static void games_gen(Buf *b) {
    char exp[40];
    iso_at(wall_ms() / 1000 + 2 * 3600, exp, sizeof exp);   /* открытый опрос — ещё два часа */
    buf_puts(b, "["
        "{\"id\":\"9907\",\"application_id\":\"1100\",\"author\":{\"id\":\"1100\",\"username\":\"MemeBot\",\"bot\":true},\"content\":\"Плеер: что делаем?\",\"timestamp\":\"2026-11-08T15:15:00+00:00\","
        "\"components\":[{\"type\":1,\"components\":["
        "{\"type\":2,\"style\":1,\"label\":\"Играть\",\"custom_id\":\"play\",\"emoji\":{\"name\":\"\u25b6\"}},"
        "{\"type\":2,\"style\":2,\"label\":\"Пауза\",\"custom_id\":\"pause\"},"
        "{\"type\":2,\"style\":3,\"label\":\"В избранное\",\"custom_id\":\"fav\"},"
        "{\"type\":2,\"style\":4,\"label\":\"Очистить очередь\",\"custom_id\":\"clear\"},"
        "{\"type\":2,\"style\":2,\"label\":\"Назад\",\"custom_id\":\"prev\",\"disabled\":true}]},"
        "{\"type\":1,\"components\":[{\"type\":2,\"style\":5,\"label\":\"Текст песни\",\"url\":\"https://example.com/lyrics\"}]},"
        "{\"type\":1,\"components\":[{\"type\":3,\"custom_id\":\"vol\",\"placeholder\":\"Громкость\",\"min_values\":1,\"max_values\":1,\"options\":["
        "{\"label\":\"Тихо\",\"value\":\"20\"},{\"label\":\"Средне\",\"value\":\"50\"},{\"label\":\"Громко\",\"value\":\"90\"}]}]},"
        "{\"type\":1,\"components\":[{\"type\":3,\"custom_id\":\"genres\",\"placeholder\":\"Жанры (до трёх)\",\"min_values\":1,\"max_values\":3,\"options\":["
        "{\"label\":\"Рок\",\"value\":\"rock\"},{\"label\":\"Джаз\",\"value\":\"jazz\"},{\"label\":\"Электроника\",\"value\":\"edm\"},"
        "{\"label\":\"Классика\",\"value\":\"classic\"}]}]}]},"
        "{\"id\":\"9906\",\"flags\":8192,\"author\":{\"id\":\"1002\",\"username\":\"bob\",\"global_name\":\"Боб\"},\"content\":\"\",\"timestamp\":\"2026-11-08T15:10:00+00:00\","
        "\"attachments\":[{\"filename\":\"voice-message.ogg\",\"url\":\"https://cdn.discordapp.com/attachments/1/30/voice-message.ogg\",\"duration_secs\":7.2,\"waveform\":\"AAAA\"}]},"
        "{\"id\":\"9905\",\"author\":{\"id\":\"1001\",\"username\":\"alice\",\"global_name\":\"Алиса\"},\"content\":\"\",\"timestamp\":\"2026-11-08T15:05:00+00:00\","
        "\"poll\":{\"question\":{\"text\":\"Во что играем в пятницу?\"},\"answers\":["
        "{\"answer_id\":1,\"poll_media\":{\"text\":\"Шахматы\",\"emoji\":{\"name\":\"\\u265f\"}}},{\"answer_id\":2,\"poll_media\":{\"text\":\"Настолки\"}},"
        "{\"answer_id\":3,\"poll_media\":{\"text\":\"Кооператив\"}},{\"answer_id\":4,\"poll_media\":{\"text\":\"Ничего, спать\"}}],"
        "\"expiry\":\"");
    buf_puts(b, exp);
    buf_puts(b, "\",\"allow_multiselect\":false,\"results\":{\"is_finalized\":false,\"answer_counts\":["
        "{\"id\":1,\"count\":2,\"me_voted\":false},{\"id\":2,\"count\":4,\"me_voted\":true},{\"id\":3,\"count\":1,\"me_voted\":false}]}}},"
        "{\"id\":\"9904\",\"author\":{\"id\":\"1002\",\"username\":\"bob\",\"global_name\":\"Боб\"},\"content\":\"\",\"timestamp\":\"2026-11-08T15:00:00+00:00\","
        "\"poll\":{\"question\":{\"text\":\"Какие дни удобны? (можно несколько)\"},\"answers\":["
        "{\"answer_id\":1,\"poll_media\":{\"text\":\"Пятница\"}},{\"answer_id\":2,\"poll_media\":{\"text\":\"Суббота\"}},{\"answer_id\":3,\"poll_media\":{\"text\":\"Воскресенье\"}}],"
        "\"expiry\":\"2026-11-07T15:00:00+00:00\",\"allow_multiselect\":true,\"results\":{\"is_finalized\":true,\"answer_counts\":["
        "{\"id\":1,\"count\":3,\"me_voted\":true},{\"id\":2,\"count\":5,\"me_voted\":true},{\"id\":3,\"count\":1,\"me_voted\":false}]}}},"
        "{\"id\":\"9903\",\"author\":{\"id\":\"1100\",\"username\":\"MemeBot\",\"bot\":true},\"content\":\"новости игр\",\"timestamp\":\"2026-11-08T14:50:00+00:00\",\"embeds\":["
        "{\"type\":\"rich\",\"color\":5793266,\"provider\":{\"name\":\"Игровой вестник\"},\"author\":{\"name\":\"Редакция\"},"
        "\"title\":\"Вышло обновление 2.0\",\"url\":\"https://example.com/patch-2-0\","
        "\"description\":\"**Главное:** новый режим, *быстрее* загрузка и ~~старые баги~~.\\nПодробности — [на сайте](https://example.com/patch).\","
        "\"fields\":[{\"name\":\"Размер\",\"value\":\"1,2 ГБ\",\"inline\":true},{\"name\":\"Платформы\",\"value\":\"ПК, консоли\",\"inline\":true},"
        "{\"name\":\"Что дальше\",\"value\":\"Сезон 3 — весной\",\"inline\":false}],"
        "\"image\":{\"url\":\"https://cdn.discordapp.com/attachments/1/20/patch.png\"},\"footer\":{\"text\":\"8 ноября\"}},"
        "{\"type\":\"article\",\"title\":\"Внешняя картинка (не грузится)\",\"description\":\"превью со стороннего сайта через прокси Discord\","
        "\"thumbnail\":{\"url\":\"https://example.org/x.png\",\"proxy_url\":\"https://media.discordapp.net/external/abc/https/example.org/x.png\"}}]}]");
}
static const char *msgs_for(u64 ch) {
    switch (ch) {
    case 3008: {
        static Buf games;
        if (!games.p) games_gen(&games);
        return games.p;
    }
    case 3007: {
        static Buf latest;
        if (!latest.p) flood_gen(&latest, FLOOD_LAST - 49, FLOOD_LAST);
        return latest.p;
    }
    case 3004: return "["
        /* «видно только вам» (flags 64) — ответ бота на команду */
        "{\"id\":\"9550\",\"author\":{\"id\":\"1100\",\"username\":\"MemeBot\",\"bot\":true},\"flags\":64,\"content\":\"очередь пуста: добавьте трек командой !play\",\"timestamp\":\"2026-11-08T09:40:00+00:00\"},"
        "{\"id\":\"9500\",\"author\":{\"id\":\"1100\",\"username\":\"MemeBot\",\"bot\":true},\"content\":\"<@1000> сейчас играет: тёплый плейлист\",\"timestamp\":\"2026-11-08T09:30:00+00:00\",\"mentions\":[{\"id\":\"1000\",\"username\":\"you\"}]},"
        "{\"id\":\"9450\",\"author\":{\"id\":\"1100\",\"username\":\"MemeBot\",\"bot\":true},\"content\":\"<@1000> трек добавлен в очередь\",\"timestamp\":\"2026-11-08T08:00:00+00:00\",\"mentions\":[{\"id\":\"1000\",\"username\":\"you\"}]},"
        "{\"id\":\"9400\",\"author\":{\"id\":\"1005\",\"username\":\"mira\",\"global_name\":\"Мира\"},\"content\":\"!play тёплый плейлист\",\"timestamp\":\"2026-11-08T07:59:00+00:00\"}]";
    case 3005: return "["
        "{\"id\":\"9600\",\"author\":{\"id\":\"1004\",\"username\":\"filin\",\"global_name\":\"Ночной Филин\"},\"content\":\"<@1000> ты где?\",\"timestamp\":\"2026-11-08T09:10:00+00:00\",\"mentions\":[{\"id\":\"1000\",\"username\":\"you\"}]},"
        "{\"id\":\"9560\",\"author\":{\"id\":\"1004\",\"username\":\"filin\",\"global_name\":\"Ночной Филин\"},\"content\":\"<@1000> сбор в 9\",\"timestamp\":\"2026-11-08T09:00:00+00:00\",\"mentions\":[{\"id\":\"1000\",\"username\":\"you\"}]},"
        "{\"id\":\"9550\",\"author\":{\"id\":\"1006\",\"username\":\"sonny\",\"global_name\":\"Сонный Кот\"},\"content\":\"кто в доту\",\"timestamp\":\"2026-11-08T08:50:00+00:00\"}]";
    case 3001: return "["
        "{\"id\":\"9005\",\"author\":{\"id\":\"1002\",\"username\":\"bob\",\"global_name\":\"Боб\"},\"guild_id\":\"2000\",\"member\":{\"roles\":[\"2002\"]},\"content\":\"<@1000> держи ссылку https://example.org\","
        "\"timestamp\":\"2026-11-08T10:05:00+00:00\",\"mentions\":[{\"id\":\"1000\",\"username\":\"you\",\"global_name\":\"Вы\"}],"
        "\"reactions\":[{\"emoji\":{\"name\":\"\\ud83d\\udc4d\"},\"count\":2,\"me\":true},{\"emoji\":{\"id\":\"77\",\"name\":\"pepe\"},\"count\":1}]},"
        "{\"id\":\"9004\",\"author\":{\"id\":\"1003\",\"username\":\"mallory\"},\"content\":\"\\u001b[2J\\u001b]0;взлом\\u0007 очистка экрана? \\u202eтекст задом\\u202c и \\u009b31m C1\","
        "\"timestamp\":\"2026-11-08T10:04:00+00:00\"},"
        "{\"id\":\"9003\",\"author\":{\"id\":\"1001\",\"username\":\"alice\",\"global_name\":\"Алиса\"},\"guild_id\":\"2000\",\"member\":{\"roles\":[\"2001\"]},\"content\":\"Очень длинное сообщение, чтобы проверить перенос строк по словам: "
        "съешь же ещё этих мягких французских булок, да выпей чаю. The quick brown fox jumps over the lazy dog. 日本語のテキストも折り返す。\","
        "\"timestamp\":\"2026-11-08T10:03:00+00:00\",\"edited_timestamp\":\"2026-11-08T10:03:30+00:00\","
        "\"attachments\":[{\"filename\":\"кот.png\",\"url\":\"https://cdn.discordapp.com/attachments/1/2/cat.png\"}]},"
        "{\"id\":\"9002\",\"author\":{\"id\":\"1002\",\"username\":\"bob\",\"global_name\":\"Боб\"},\"guild_id\":\"2000\",\"member\":{\"roles\":[\"2002\"]},\"content\":\"привет! в <#3002> смешное\","
        "\"timestamp\":\"2026-11-08T10:02:00+00:00\",\"referenced_message\":{\"id\":\"9001\",\"author\":{\"id\":\"1001\",\"global_name\":\"Алиса\"},\"guild_id\":\"2000\",\"member\":{\"roles\":[\"2001\"]},\"content\":\"всем привет\"}},"
        "{\"id\":\"9001\",\"author\":{\"id\":\"1001\",\"username\":\"alice\",\"global_name\":\"Алиса\"},\"guild_id\":\"2000\",\"member\":{\"roles\":[\"2001\"]},\"content\":\"всем привет <:wave:123> <t:1762596000:t>\","
        "\"timestamp\":\"2026-11-08T10:01:00+00:00\"}]";
    case 3018: return "["
        "{\"id\":\"9993\",\"author\":{\"id\":\"1006\",\"username\":\"sonny\",\"global_name\":\"Сонный Кот\"},\"guild_id\":\"2000\",\"member\":{\"roles\":[\"2002\"]},\"content\":\"\","
        "\"timestamp\":\"2023-02-01T22:14:30+00:00\",\"attachments\":[{\"filename\":\"icon.jpg\",\"url\":\"https://cdn.discordapp.com/attachments/1/5/icon.jpg\"}]},"
        "{\"id\":\"9992\",\"author\":{\"id\":\"1005\",\"username\":\"mira\",\"global_name\":\"Мира\"},\"guild_id\":\"2000\",\"member\":{\"roles\":[\"2003\"]},\"content\":\"\","
        "\"timestamp\":\"2023-02-01T22:14:00+00:00\",\"attachments\":[{\"filename\":\"face.jpg\",\"url\":\"https://cdn.discordapp.com/attachments/1/4/face.jpg\"}]},"
        "{\"id\":\"9991\",\"author\":{\"id\":\"1004\",\"username\":\"filin\",\"global_name\":\"Ночной Филин\"},\"guild_id\":\"2000\",\"member\":{\"roles\":[\"2001\"]},\"content\":\"смотрите, что я нарисовала\","
        "\"timestamp\":\"2023-02-01T22:12:00+00:00\",\"attachments\":[{\"filename\":\"meme.jpg\",\"url\":\"https://cdn.discordapp.com/attachments/1/3/meme.jpg\"}]}]";
    case 4000: return "["
        "{\"id\":\"9301\",\"author\":{\"id\":\"1001\",\"username\":\"alice\",\"global_name\":\"Алиса\"},\"content\":\"созвонимся? F4 в этой беседе\","
        "\"timestamp\":\"2026-11-08T11:00:00+00:00\"}]";
    case 3002: return "["   /* волна 1: бот, пересланное, ответ на сообщение без текста */
        "{\"id\":\"9100\",\"author\":{\"id\":\"1100\",\"username\":\"MemeBot\",\"bot\":true},\"content\":\"мем дня готов\","
        "\"timestamp\":\"2026-11-08T12:03:00+00:00\",\"flags\":4096,\"message_reference\":{\"message_id\":\"9098\",\"channel_id\":\"3002\"},"
        "\"referenced_message\":{\"id\":\"9098\",\"author\":{\"id\":\"1002\",\"global_name\":\"Боб\"},\"content\":\"\",\"attachments\":[{\"filename\":\"a.png\",\"url\":\"https://cdn.discordapp.com/attachments/1/9/a.png\"}]}},"
        "{\"id\":\"9099\",\"author\":{\"id\":\"1001\",\"username\":\"alice\",\"global_name\":\"Алиса\"},\"content\":\"\",\"timestamp\":\"2026-11-08T12:02:00+00:00\","
        "\"message_reference\":{\"type\":1,\"message_id\":\"5\",\"channel_id\":\"77\"},"
        "\"message_snapshots\":[{\"message\":{\"content\":\"переслано из другого чата: завтра в 8\",\"timestamp\":\"2026-11-07T10:00:00+00:00\"}}]},"
        "{\"id\":\"9098\",\"author\":{\"id\":\"1002\",\"username\":\"bob\",\"global_name\":\"Боб\"},\"content\":\"\",\"timestamp\":\"2026-11-08T12:01:00+00:00\","
        "\"attachments\":[{\"filename\":\"SPOILER_a.png\",\"url\":\"https://cdn.discordapp.com/attachments/1/9/SPOILER_a.png\"}]},"
        "{\"id\":\"9097\",\"webhook_id\":\"55\",\"author\":{\"id\":\"55\",\"username\":\"GitHub\"},\"content\":\"новый коммит\",\"timestamp\":\"2026-11-08T12:00:00+00:00\"}]";
    case 3006: return "["   /* рисунки: картинки, одна под спойлером — проверка пропуска спойлера в просмотре */
        "{\"id\":\"9700\",\"author\":{\"id\":\"1001\",\"username\":\"alice\",\"global_name\":\"Алиса\"},\"content\":\"ещё одна\",\"timestamp\":\"2026-11-08T13:02:00+00:00\","
        "\"attachments\":[{\"filename\":\"cat2.png\",\"url\":\"https://cdn.discordapp.com/attachments/1/12/cat2.png\"}]},"
        "{\"id\":\"9695\",\"author\":{\"id\":\"1002\",\"username\":\"bob\",\"global_name\":\"Боб\"},\"content\":\"осторожно, спойлер\",\"timestamp\":\"2026-11-08T13:01:00+00:00\","
        "\"attachments\":[{\"filename\":\"SPOILER_end.png\",\"url\":\"https://cdn.discordapp.com/attachments/1/11/SPOILER_end.png\"}]},"
        "{\"id\":\"9690\",\"author\":{\"id\":\"1001\",\"username\":\"alice\",\"global_name\":\"Алиса\"},\"content\":\"\",\"timestamp\":\"2026-11-08T13:00:00+00:00\","
        "\"attachments\":[{\"filename\":\"meme2.jpg\",\"url\":\"https://cdn.discordapp.com/attachments/1/10/meme2.jpg\"}]}]";
    case 4002: return "["   /* запрос на переписку: превью/аватар — только после «Принять» (решает интерфейс) */
        "{\"id\":\"9420\",\"author\":{\"id\":\"1020\",\"username\":\"stranger\",\"global_name\":\"Незнакомка\",\"avatar\":\"0123456789abcdef0123456789abcd20\"},"
        "\"content\":\"Привет! Видела тебя на сервере, посмотри фото\",\"timestamp\":\"2026-11-08T16:00:00+00:00\","
        "\"attachments\":[{\"filename\":\"photo.jpg\",\"url\":\"https://cdn.discordapp.com/attachments/1/40/photo.jpg\"}]}]";
    case 4003: return "["
        "{\"id\":\"9421\",\"author\":{\"id\":\"1021\",\"username\":\"freegift\",\"global_name\":\"Бесплатный Нитро\",\"avatar\":\"0123456789abcdef0123456789abcd21\"},"
        "\"content\":\"Забери бесплатный Nitro по ссылке!\",\"timestamp\":\"2026-11-08T16:05:00+00:00\","
        "\"attachments\":[{\"filename\":\"gift.png\",\"url\":\"https://cdn.discordapp.com/attachments/1/41/gift.png\"}]}]";
    case 3201: return "["   /* посты форума #вопросы (№95): по два сообщения */
        "{\"id\":\"1568943164620800000\",\"channel_id\":\"3201\",\"author\":{\"id\":\"1002\",\"username\":\"bob\",\"global_name\":\"Боб\"},\"content\":\"Проверь усиление микрофона в настройках голоса\",\"timestamp\":\"2026-11-08T11:00:00+00:00\"},"
        "{\"id\":\"3201\",\"channel_id\":\"3201\",\"author\":{\"id\":\"1001\",\"username\":\"alice\",\"global_name\":\"Алиса\"},\"content\":\"Микрофон тихий, что крутить?\",\"timestamp\":\"2026-11-08T10:30:00+00:00\"}]";
    case 3202: return "["
        "{\"id\":\"1568958264115200000\",\"channel_id\":\"3202\",\"author\":{\"id\":\"1001\",\"username\":\"alice\",\"global_name\":\"Алиса\"},\"content\":\"У меня два монитора и кот\",\"timestamp\":\"2026-11-08T12:00:00+00:00\"},"
        "{\"id\":\"3202\",\"channel_id\":\"3202\",\"author\":{\"id\":\"1002\",\"username\":\"bob\",\"global_name\":\"Боб\"},\"content\":\"Покажите свои рабочие места!\",\"timestamp\":\"2026-11-08T11:30:00+00:00\"}]";
    case 3203: return "["
        "{\"id\":\"1568973363609600000\",\"channel_id\":\"3203\",\"author\":{\"id\":\"1003\",\"username\":\"mallory\"},\"content\":\"Понятно, спасибо\",\"timestamp\":\"2026-11-08T13:00:00+00:00\"},"
        "{\"id\":\"3203\",\"channel_id\":\"3203\",\"author\":{\"id\":\"1003\",\"username\":\"mallory\"},\"content\":\"1. Вежливость. 2. Без спама. 3. Вопросы — отдельным постом.\",\"timestamp\":\"2026-11-08T12:30:00+00:00\"}]";
    case 3101: return "["   /* ветка «обсуждение котов» (№91) */
        "{\"id\":\"9110\",\"channel_id\":\"3101\",\"author\":{\"id\":\"1002\",\"username\":\"bob\",\"global_name\":\"Боб\"},\"content\":\"а мой кот спит на клавиатуре\",\"timestamp\":\"2026-11-08T11:10:00+00:00\"},"
        "{\"id\":\"9105\",\"channel_id\":\"3101\",\"author\":{\"id\":\"1001\",\"username\":\"alice\",\"global_name\":\"Алиса\"},\"content\":\"делимся котами здесь, чтобы не засорять #general\",\"timestamp\":\"2026-11-08T11:05:00+00:00\"}]";
    default: return "[]";
    }
}

/* ---------- волна 1: закреплённые, упоминания, профиль (ответы в формате Discord -> store) ---------- */
static u64 demo_pinned[8] = { 9001, 9003 };     /* закреплённые в #general */
static u64 demo_pin_ch(u64 id) { return id >= 9001 && id <= 9005 ? 3001 : id >= 9097 && id <= 9100 ? 3002 : id >= 9991 && id <= 9993 ? 3018 : 0; }
void demo_pin(u64 channel_id, u64 msg_id, bool pin) {
    (void)channel_id;
    for (int i = 0; i < 8; i++) if (demo_pinned[i] == msg_id) { if (!pin) demo_pinned[i] = 0; return; }
    if (!pin) return;
    for (int i = 0; i < 8; i++) if (!demo_pinned[i]) { demo_pinned[i] = msg_id; return; }
}
void demo_pins(u64 channel_id) {
    const char *m = msgs_for(channel_id);
    Buf out = {0};
    int n = 0;
    buf_puts(&out, "{\"items\":[");
    cJSON *j = json_parse(m, strlen(m));
    const cJSON *o;
    cJSON_ArrayForEach(o, j) {
        u64 id = jid(o, "id");
        bool pinned = false;
        for (int i = 0; i < 8; i++) if (demo_pinned[i] == id && demo_pin_ch(id) == channel_id) pinned = true;
        if (!pinned) continue;
        char *t = cJSON_PrintUnformatted(o);
        if (t) buf_printf(&out, "%s{\"pinned_at\":\"2026-11-08T13:0%d:00+00:00\",\"message\":%s}", n ? "," : "", n % 10, t);
        n++;
    }
    json_done();
    buf_puts(&out, "],\"has_more\":false}");
    j = json_parse(out.p, out.len);
    if (j) pins_apply(j);
    json_done();
    buf_free(&out);
}
void demo_topic(u64 channel_id) {
    if (channel_id == 3001) topic_set(3001, demo_tr("Уютное общение без токсичности. Правила - в закреплённых"));
}
void demo_mentions(void) {
    static const char M[] = "["
        "{\"id\":\"9005\",\"channel_id\":\"3001\",\"author\":{\"id\":\"1002\",\"username\":\"bob\",\"global_name\":\"Боб\"},\"content\":\"<@1000> держи ссылку https://example.org\","
        "\"timestamp\":\"2026-11-08T10:05:00+00:00\",\"mentions\":[{\"id\":\"1000\",\"username\":\"you\",\"global_name\":\"Вы\"}]},"
        "{\"id\":\"9550\",\"channel_id\":\"3005\",\"author\":{\"id\":\"1004\",\"username\":\"filin\",\"global_name\":\"Ночной Филин\"},\"content\":\"@everyone сбор в 9\","
        "\"timestamp\":\"2026-11-08T09:00:00+00:00\",\"mention_everyone\":true},"
        "{\"id\":\"9450\",\"channel_id\":\"3004\",\"author\":{\"id\":\"1100\",\"username\":\"MemeBot\",\"bot\":true},\"content\":\"<@1000> трек добавлен\\u001b[2J\","
        "\"timestamp\":\"2026-11-08T08:00:00+00:00\"}]";
    cJSON *j = json_parse(M, sizeof M - 1);
    mentions_apply(j);
    json_done();
}
void demo_profile(u64 uid, u64 guild_id) {
    Buf b = {0};
    buf_printf(&b, "{\"user\":{\"id\":\"%llu\",\"username\":", (unsigned long long)uid);
    buf_json_str(&b, uid == 1001 ? "alice" : "user");
    buf_puts(&b, ",\"global_name\":");
    buf_json_str(&b, user_name(uid));
    if (uid == 1001)
        buf_puts(&b, "},\"user_profile\":{\"bio\":\"Люблю котиков и уютные каналы.\\nПишите в ЛС.\",\"pronouns\":\"она/её\",\"accent_color\":15548997}");
    else buf_puts(&b, "},\"user_profile\":{}");
    if (guild_id) buf_printf(&b, ",\"guild_member\":{\"nick\":null,\"roles\":[\"2001\"],\"joined_at\":\"2021-03-04T05:06:07+00:00\"}");
    /* №99: общие серверы/друзья и привязки (используются только в полном профиле) */
    buf_puts(&b, ",\"mutual_guilds\":[{\"id\":\"2000\"}],\"mutual_friends\":[{\"id\":\"1002\",\"username\":\"bob\",\"global_name\":\"Боб\"}],"
                 "\"connected_accounts\":[{\"type\":\"github\",\"name\":\"alice-dev\"}]");
    buf_puts(&b, "}");
    cJSON *j = json_parse(b.p, b.len);
    profile_apply(j);
    json_done();
    buf_free(&b);
}

static u64 next_id = 10000;
u64 demo_new_id(void) { return 50000 + next_id++; }

/* selftest: английский READY демо — без кириллицы в строках (кроме ключей таблицы), имена и сервер по-английски */
int demo_en_selftest(void (*check)(bool ok, const char *what)) {
    bool d = O.demo, e = g_lang_en;
    O.demo = true;
    g_lang_en = true;
    size_t n = 0;
    char *t = demo_en_json(READY, sizeof READY - 1, &n);
    int cyr = 0;
    for (size_t i = 0; t && i < n; i++) if ((unsigned char)t[i] == 0xD0 || (unsigned char)t[i] == 0xD1) cyr++;
    bool ok = t && !cyr && strstr(t, "\"Cozy Corner\"") && strstr(t, "\"Night Owl\"") && strstr(t, "Mallory");
    free(t);
    char fl[] = "[{\"content\":\"флуд номер 42\"}]";
    t = demo_en_json(fl, strlen(fl), &n);
    bool ok2 = t && n == strlen("[{\"content\":\"flood message 42\"}]") && !memcmp(t, "[{\"content\":\"flood message 42\"}]", n);
    free(t);
    g_lang_en = false;
    bool ok3 = !demo_en_json(fl, strlen(fl), &n);   /* русский интерфейс — без подмены */
    O.demo = d;
    g_lang_en = e;
    char w[256];
    snprintf(w, sizeof w, "i18n: английский демо-набор (READY без кириллицы: осталось %d байт), «флуд номер N», по-русски — без подмены", cyr);
    check(ok && ok2 && ok3, w);
    return 0;
}
void demo_load(void) {
    cJSON *j = json_parse(READY, sizeof READY - 1);
    if (j) store_ready(j);
    json_done();
}

/* Остальные серверы колонки — после первой загрузки истории (как GUILD_CREATE после READY). */
static void demo_more(void) {
    static bool done;
    if (done) return;
    done = true;
    for (size_t i = 0; i < sizeof MORE / sizeof *MORE; i++) {
        Buf b = {0};
        buf_printf(&b, "{\"id\":\"%s\",\"name\":", MORE[i].id);
        buf_json_str(&b, MORE[i].name);
        buf_printf(&b, ",\"icon\":%s%s%s,\"owner_id\":\"1001\",\"roles\":[{\"id\":\"%s\",\"permissions\":\"3263553\"}],"
                   "\"channels\":[{\"id\":\"%s1\",\"type\":0,\"name\":\"general\",\"position\":0,\"last_message_id\":\"9000\"}],"
                   "\"members\":[{\"user\":{\"id\":\"1000\"},\"roles\":[]}]}",
                   MORE[i].icon[0] ? "\"" : "", MORE[i].icon[0] ? MORE[i].icon : "null", MORE[i].icon[0] ? "\"" : "",
                   MORE[i].id, MORE[i].id);
        cJSON *j = json_parse(b.p, b.len);
        if (j) store_event("GUILD_CREATE", j);
        json_done();
        buf_free(&b);
        if (MORE[i].ment) {
            char a[160];
            snprintf(a, sizeof a, "{\"channel_id\":\"%s1\",\"message_id\":\"8000\",\"mention_count\":%d}", MORE[i].id, MORE[i].ment);
            j = json_parse(a, strlen(a));
            if (j) store_event("MESSAGE_ACK", j);
            json_done();
        }
    }
}

/* ,"timestamp":"..."}} — текущее время UTC, конец объекта сообщения */
static void put_now_and_close(Buf *b);

static void demo_post(u64 channel, const char *text, const char *fname, long long size) {
    Buf b = {0};
    buf_printf(&b, "{\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"%llu\",\"channel_id\":\"%llu\",\"author\":{\"id\":\"1000\",\"username\":\"you\",\"global_name\":\"Вы\"},\"content\":",
               (unsigned long long)next_id++, (unsigned long long)channel);
    buf_json_str(&b, text ? text : "");
    if (fname) {
        buf_puts(&b, ",\"attachments\":[{\"filename\":");
        buf_json_str(&b, fname);
        buf_printf(&b, ",\"size\":%lld,\"url\":\"(демо: файл не отправлялся)\"}]", size);
    }
    put_now_and_close(&b);
    cJSON *j = json_parse(b.p, b.len);
    if (j) store_event("MESSAGE_CREATE", jget(j, "d"));
    json_done();
    buf_free(&b);
}
void demo_file(u64 channel, const char *filename, long long size, const char *text) {
    demo_post(channel, text, filename, size);
    ui_notice("%s", demo_tr("файл отправлен (демо: в сеть ничего не ушло)"));
}

/* Поиск в демо: по сообщениям демо-каналов (подстрока), ответ собирается в формате Discord
 * {"total_results":N,"messages":[[{..,"channel_id":..,"hit":true}]]} и идёт настоящим разбором. */
void demo_search(u64 guild_id, u64 channel_id, const char *query) {
    u64 list[64];
    int nl = 0;
    if (channel_id) list[nl++] = channel_id;
    else for (int i = 0; i < S.nchans && nl < 64; i++) if (S.chans[i].guild_id == guild_id) list[nl++] = S.chans[i].id;
    Buf out = {0};
    int total = 0;
    buf_puts(&out, "{\"messages\":[");
    for (int k = 0; k < nl; k++) {
        const char *m = msgs_for(list[k]);
        cJSON *j = json_parse(m, strlen(m));
        cJSON *o;
        cJSON_ArrayForEach(o, j) {
            const char *c = jstr(o, "content");
            if (!c || !strstr(c, query)) continue;
            char cid[24];
            snprintf(cid, sizeof cid, "%llu", (unsigned long long)list[k]);
            cJSON_AddStringToObject(o, "channel_id", cid);
            cJSON_AddTrueToObject(o, "hit");
            char *s = cJSON_PrintUnformatted(o);            /* в арене JSON */
            if (!s) continue;
            if (total < SEARCH_PAGE) buf_printf(&out, "%s[%s]", total ? "," : "", s);
            total++;
        }
        json_done();
    }
    buf_printf(&out, "],\"total_results\":%d}", total);
    cJSON *j = json_parse(out.p, out.len);
    if (j) search_apply(j);
    json_done();
    buf_free(&out);
}

/* Переход к сообщению в демо: #flood — 50 вокруг него (лента «оторвана» от новых), прочие — вся история. */
void demo_around(u64 channel, u64 msg_id) {
    if (channel != 3007 || msg_id < FLOOD_FIRST || msg_id > FLOOD_LAST) { demo_send(channel, NULL); return; }
    Buf b = {0};
    flood_gen(&b, (int)msg_id - 25, (int)msg_id + 24);
    cJSON *j = json_parse(b.p, b.len);
    if (cJSON_IsArray(j)) store_set_messages(j, false);
    json_done();
    buf_free(&b);
}
/* Подгрузка старых в демо (ui.c при прокрутке вверх): #flood — следующие 50, прочие — конец истории. */
void demo_older(u64 channel) {
    if (channel != 3007 || !S.nmsgs || S.msgs[0].id <= FLOOD_FIRST) { S.history_end = true; ui_dirty(); return; }
    Buf b = {0};
    int first = (int)S.msgs[0].id;
    flood_gen(&b, first - 50, first - 1);
    cJSON *j = json_parse(b.p, b.len);
    if (cJSON_IsArray(j)) store_set_messages(j, true);
    json_done();
    buf_free(&b);
    ui_dirty();
}

void demo_send(u64 channel, const char *text) {
    if (!text) {
        const char *m = msgs_for(channel);
        cJSON *j = json_parse(m, strlen(m));
        if (cJSON_IsArray(j)) store_set_messages(j, false);
        json_done();
        demo_more();
        ui_dirty();
        return;
    }
    demo_post(channel, text, NULL, 0);
}

static void put_now_and_close(Buf *bp) {
    Buf b = *bp;
    /* метка времени в UTC: дни -> дата (алгоритм Хиннанта) */
    i64 now = wall_ms() / 1000, days = now / 86400, sod = now % 86400;
    i64 z = days + 719468, era = z / 146097, doe = z - era * 146097;
    i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    i64 mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9, y = yoe + era * 400 + (m <= 2);
    buf_printf(&b, ",\"timestamp\":\"%04d-%02d-%02dT%02d:%02d:%02d+00:00\"}}", (int)y, (int)m, (int)d,
               (int)(sod / 3600), (int)(sod / 60 % 60), (int)(sod % 60));
    *bp = b;
}
