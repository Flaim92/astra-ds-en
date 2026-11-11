/* macOS: окно проверки hCaptcha в процессе-помощнике (astra-ds --captcha-helper, см. captcha.c, ветка __APPLE__).
 *
 * WKWebView в окне NSWindow. Сам WebKit в исполняемый файл не компонуется: помощник подгружает системный
 * WebKit.framework через dlopen и берёт классы по имени (NSClassFromString), так клиент WebKit не грузит вовсе.
 * Хранилище — WKWebsiteDataStore nonPersistentDataStore (ничего на диск). Страница (общая с Linux, page_build
 * в captcha.c) открывается loadHTMLString:baseURL: под https://discord.com/astra-captcha, ответ приходит через
 * WKScriptMessageHandler "astra" и только от главного кадра с источником https://discord.com.
 * Переходы главного кадра после загрузки нашей страницы отменяются, новые окна не открываются, камера и
 * микрофон запрещены, RTCPeerConnection и родня убраны скриптом до загрузки страницы (WebKit не даёт
 * выключить WebRTC публичной настройкой).
 * VLESS: SOCKS5 туннеля через proxyConfigurations хранилища (macOS 14+, Network.framework), без обхода
 * напрямую. На более старых системах помощник с прокси не стартует (код 5), клиент туда и не доходит
 * (captcha_supported). Код рассчитан и на ARC, и на ручной подсчёт ссылок: процесс одноразовый, выходит _exit. */
#ifdef __APPLE__
#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>
#import <Network/Network.h>
#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "captcha.h"

#define PAGE_URL @"https://discord.com/astra-captcha"
#define MSG_NAME @"astra"
#define ANSWER_MAX 32000                        /* как у Linux: 40 частей по 800 символов */
#define WK_PATH "/System/Library/Frameworks/WebKit.framework/WebKit"

/* Убрать WebRTC из каждого кадра до любого кода страницы: иначе ICE по UDP может уйти мимо прокси (утечка IP). */
static NSString *const kNoRtc =
    @"(function(){var w=window,n=['RTCPeerConnection','webkitRTCPeerConnection','RTCDataChannel','RTCRtpSender',"
    @"'RTCRtpReceiver','RTCRtpTransceiver','RTCIceCandidate','RTCSessionDescription','RTCDtlsTransport',"
    @"'RTCIceTransport','RTCSctpTransport'];for(var i=0;i<n.length;i++){try{Object.defineProperty(w,n[i],"
    @"{value:undefined,writable:false,configurable:false})}catch(e){}}try{Object.defineProperty(navigator,"
    @"'mediaDevices',{value:undefined,writable:false,configurable:false})}catch(e){}})();";

static int g_code = 4;                          /* 4 — окно закрыто без ответа (как в Linux) */
static bool g_loaded;                           /* наша страница уже пошла в главный кадр */

static void mac_wipe(char *p, size_t n) {
    volatile char *v = p;
    while (p && n--) *v++ = 0;
}

static void finish(int code) __attribute__((noreturn));
static void finish(int code) {
    fflush(stdout);
    _exit(code);                                /* без деструкторов AppKit/WebKit: процесс одноразовый */
}

/* Ответ hCaptcha: те же символы, что пропустит клиент (cs_charset в captcha.c, набор "-_.:+/="). */
static bool answer_ok(const char *t, size_t n) {
    if (n < 20 || n > ANSWER_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)t[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || strchr("-_.:+/=", c))) return false;
    }
    return true;
}

@interface AstraCaptcha : NSObject <WKScriptMessageHandler, WKNavigationDelegate, WKUIDelegate, NSWindowDelegate>
@end

@implementation AstraCaptcha

/* Сообщение страницы: "ok:<ответ>" или "err:load". Только от главного кадра нашей страницы. */
- (void)userContentController:(WKUserContentController *)ucc didReceiveScriptMessage:(WKScriptMessage *)msg {
    (void)ucc;
    WKFrameInfo *fi = msg.frameInfo;
    if (!fi || ![fi isMainFrame]) return;
    WKSecurityOrigin *o = fi.securityOrigin;
    if (!o || ![o.protocol isEqualToString:@"https"] || ![o.host isEqualToString:@"discord.com"] || o.port != 0) return;
    id body = msg.body;
    if (![body isKindOfClass:[NSString class]]) return;
    NSString *s = (NSString *)body;
    if ([s isEqualToString:@"err:load"]) finish(3);
    if (![s hasPrefix:@"ok:"]) return;
    const char *t = [s UTF8String];
    if (!t) return;
    t += 3;
    size_t n = strlen(t);
    if (!answer_ok(t, n)) return;               /* непонятный ответ: окно остаётся, пользователь закроет сам */
    fwrite(t, 1, n, stdout);
    fputc('\n', stdout);
    finish(0);
}

/* Главный кадр: только первая загрузка (наша страница из loadHTMLString). Вложенные кадры hCaptcha: https и about:. */
- (void)webView:(WKWebView *)wv decidePolicyForNavigationAction:(WKNavigationAction *)act
        decisionHandler:(void (^)(WKNavigationActionPolicy))handler {
    (void)wv;
    WKFrameInfo *tf = act.targetFrame;
    NSString *scheme = act.request.URL.scheme;
    BOOL allow = NO;
    if (!tf) allow = NO;                        /* новое окно */
    else if ([tf isMainFrame]) { allow = !g_loaded; g_loaded = true; }
    else allow = scheme && ([scheme isEqualToString:@"https"] || [scheme isEqualToString:@"about"]);
    handler(allow ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}

/* Наша страница встала в главный кадр: дальше главный кадр никуда не переходит (на случай, если первую загрузку
 * WebKit провёл без вызова политики выше). */
- (void)webView:(WKWebView *)wv didCommitNavigation:(WKNavigation *)nav {
    (void)wv; (void)nav;
    g_loaded = true;
}

/* Процесс содержимого WebKit упал: продолжать нечем. */
- (void)webViewWebContentProcessDidTerminate:(WKWebView *)wv {
    (void)wv;
    finish(3);
}

/* Новые окна (window.open, target=_blank) не открываем. */
- (WKWebView *)webView:(WKWebView *)wv createWebViewWithConfiguration:(WKWebViewConfiguration *)cfg
        forNavigationAction:(WKNavigationAction *)act windowFeatures:(WKWindowFeatures *)feat {
    (void)wv; (void)cfg; (void)act; (void)feat;
    return nil;
}

/* Камера и микрофон: всегда отказ, без вопроса пользователю (macOS 12+). */
- (void)webView:(WKWebView *)wv requestMediaCapturePermissionForOrigin:(WKSecurityOrigin *)origin
        initiatedByFrame:(WKFrameInfo *)frame type:(WKMediaCaptureType)type
        decisionHandler:(void (^)(WKPermissionDecision))handler {
    (void)wv; (void)origin; (void)frame; (void)type;
    handler(WKPermissionDecisionDeny);
}

- (void)windowWillClose:(NSNotification *)note {
    (void)note;
    finish(g_code);
}

@end

static AstraCaptcha *g_del;                     /* делегаты WebKit слабые: держим объект сами */

bool captcha_mac_proxy_ok(void) {
    if (@available(macOS 14.0, *)) return true;
    return false;
}

int captcha_mac_run(const char *page, const char *title, const char *proxy_host, int proxy_port,
                    const char *proxy_user, char *proxy_pass, size_t proxy_pass_n) {
    @autoreleasepool {
        /* без сеанса WindowServer (вход по ssh без входа в систему) AppKit окно не создаст */
        CFDictionaryRef sess = CGSessionCopyCurrentDictionary();
        if (!sess) { mac_wipe(proxy_pass, proxy_pass_n); return 6; }
        CFRelease(sess);
        if (!dlopen(WK_PATH, RTLD_NOW | RTLD_LOCAL)) { mac_wipe(proxy_pass, proxy_pass_n); return 2; }
        Class cView = NSClassFromString(@"WKWebView"), cConf = NSClassFromString(@"WKWebViewConfiguration");
        Class cStore = NSClassFromString(@"WKWebsiteDataStore"), cUcc = NSClassFromString(@"WKUserContentController");
        Class cScript = NSClassFromString(@"WKUserScript");
        if (!cView || !cConf || !cStore || !cUcc || !cScript) { mac_wipe(proxy_pass, proxy_pass_n); return 2; }

        WKWebsiteDataStore *store = [cStore nonPersistentDataStore];
        if (!store) { mac_wipe(proxy_pass, proxy_pass_n); return 2; }
        if (proxy_host) {                       /* VLESS: только через туннель; не вышло — не открываем */
            bool set = false;
            if (@available(macOS 14.0, *)) {
                char ps[8];
                snprintf(ps, sizeof ps, "%d", proxy_port);
                nw_endpoint_t ep = proxy_port > 0 && proxy_port < 65536 ? nw_endpoint_create_host(proxy_host, ps) : nil;
                nw_proxy_config_t pc = ep ? nw_proxy_config_create_socksv5(ep) : nil;
                if (pc) {
                    if (proxy_user && proxy_user[0]) nw_proxy_config_set_username_and_password(pc, proxy_user, proxy_pass);
                    nw_proxy_config_set_failover_allowed(pc, false);   /* прокси недоступен — не напрямую */
                    store.proxyConfigurations = @[ pc ];
                    set = store.proxyConfigurations.count == 1;
                }
            }
            mac_wipe(proxy_pass, proxy_pass_n);
            if (!set) return 5;
        }
        mac_wipe(proxy_pass, proxy_pass_n);

        NSString *html = [NSString stringWithUTF8String:page];
        NSString *wtitle = title ? [NSString stringWithUTF8String:title] : nil;
        NSURL *base = [NSURL URLWithString:PAGE_URL];
        if (!html || !base) return 4;
        if (!wtitle) wtitle = @"Astra DS";

        g_del = [[AstraCaptcha alloc] init];
        WKWebViewConfiguration *cfg = [[cConf alloc] init];
        cfg.websiteDataStore = store;
        cfg.preferences.javaScriptCanOpenWindowsAutomatically = NO;
        cfg.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeAll;
        WKUserContentController *ucc = [[cUcc alloc] init];
        WKUserScript *rtc = [[cScript alloc] initWithSource:kNoRtc injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                            forMainFrameOnly:NO];
        [ucc addUserScript:rtc];
        [ucc addScriptMessageHandler:g_del name:MSG_NAME];
        cfg.userContentController = ucc;

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];   /* без пакета .app иначе нет фокуса клавиатуры */
        NSRect r = NSMakeRect(0, 0, 460, 660);
        NSWindow *win = [[NSWindow alloc] initWithContentRect:r
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                              NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        if (!win) return 6;
        win.releasedWhenClosed = NO;
        win.title = wtitle;
        win.level = NSFloatingWindowLevel;     /* поверх окна клиента, как keep_above в Linux */
        win.delegate = g_del;
        WKWebView *view = [[cView alloc] initWithFrame:r configuration:cfg];
        if (!view) return 2;
        view.navigationDelegate = g_del;
        view.UIDelegate = g_del;
        view.allowsBackForwardNavigationGestures = NO;
        view.allowsMagnification = NO;
        win.contentView = view;
        [win center];
        [view loadHTMLString:html baseURL:base];
        [win makeKeyAndOrderFront:nil];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [NSApp activateIgnoringOtherApps:YES];   /* в macOS 14 устарело, замена [NSApp activate] есть только с 14 */
#pragma clang diagnostic pop
        [NSTimer scheduledTimerWithTimeInterval:600 repeats:NO block:^(NSTimer *tm) {   /* 10 минут на проверку */
            (void)tm;
            finish(4);
        }];
        [NSApp run];
        finish(g_code);
    }
}
#endif
