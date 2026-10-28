/*
 * ths_vtoken.cpp —— ths_vToken 库实现
 *
 *   cpp-httplib : 取挑战页 / 取脚本（https，OpenSSL）
 *   QuickJS     : 执行由 qjsc 编译成 C 字节码数组的核心 JS
 *
 * 对外是纯 C 接口（见 ths_vtoken.h）。
 */

#include "ths_vtoken.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <regex>

#include "httplib.h"

extern "C" {
#include "quickjs.h"
#include "quickjs-libc.h"
}

/* qjsc 生成的字节码数组（thon_vToken_bc.c） */
extern "C" {
extern const uint8_t ths_vToken_core[];
extern const uint32_t ths_vToken_core_size;
}

namespace {

const char *kVersion = "1.0.1";

const char *kDefaultChallengeUrl =
    "https://data.10jqka.com.cn/financial/ggjy/field/enddate/order/desc/page/1/ajax/1/";

const char *kDefaultUA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/155.0.0.0 Safari/537.36 Edg/155.0.0.0";

std::string g_user_agent = kDefaultUA;

/* ------------------------------------------------------------------ 工具 */

void set_err(char *err, size_t err_len, const std::string &msg) {
    if (!err || err_len == 0) return;
    size_t n = msg.size() < err_len - 1 ? msg.size() : err_len - 1;
    std::memcpy(err, msg.data(), n);
    err[n] = '\0';
}

bool split_url(const std::string &url, std::string &scheme, std::string &host,
               std::string &path) {
    std::smatch m;
    static const std::regex re(R"(^(https?)://([^/?#]+)(.*)$)", std::regex::icase);
    if (!std::regex_match(url, m, re)) return false;
    scheme = m[1].str();
    host = m[2].str();
    path = m[3].str();
    if (path.empty()) path = "/";
    return true;
}

std::string resolve_url(const std::string &base_scheme, const std::string &base_host,
                        const std::string &src) {
    if (src.rfind("//", 0) == 0) return base_scheme + ":" + src;
    if (src.rfind("http://", 0) == 0 || src.rfind("https://", 0) == 0) return src;
    if (!src.empty() && src[0] == '/') return base_scheme + "://" + base_host + src;
    return base_scheme + "://" + base_host + "/" + src;
}

/* 从挑战页 HTML 里提取 <script src="..."> */
std::vector<std::string> extract_scripts(const std::string &html) {
    std::vector<std::string> out;
    static const std::regex re(R"(<script[^>]+src\s*=\s*["']([^"']+)["'])",
                               std::regex::icase);
    for (std::sregex_iterator it(html.begin(), html.end(), re), end; it != end; ++it)
        out.push_back((*it)[1].str());
    return out;
}

bool http_get_once(const std::string &url, const std::string &referer, std::string &body,
                   int &status, std::string &err) {
    std::string scheme, host, path;
    if (!split_url(url, scheme, host, path)) {
        err = "非法 URL: " + url;
        return false;
    }

    httplib::Client cli(scheme + "://" + host);
    cli.set_follow_location(true);
    cli.enable_server_certificate_verification(false);
    /* 本机 IPv6 到部分 CDN（s.thsi.cn 等）链路不通，而 getaddrinfo 往往先返回 AAAA，
       导致连接超时；这里强制走 IPv4。 */
    cli.set_address_family(AF_INET);
    cli.set_keep_alive(false); /* 每次新建连接，避开 CDN 的空闲连接回收 */
    cli.set_connection_timeout(10);
    cli.set_read_timeout(25);
    cli.set_write_timeout(10);

    httplib::Headers hdr = {
        {"User-Agent", g_user_agent},
        {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
        {"Accept-Language", "zh-CN,zh;q=0.9"},
        {"Connection", "close"},
        {"Sec-Fetch-Dest", "document"},
        {"Sec-Fetch-Mode", "navigate"},
        {"Sec-Fetch-Site", "none"},
        {"Upgrade-Insecure-Requests", "1"},
    };
    if (!referer.empty()) hdr.emplace("Referer", referer);

    auto res = cli.Get(path.c_str(), hdr);
    if (!res) {
        err = "请求失败(" + url + "): " + httplib::to_string(res.error());
        return false;
    }
    status = res->status;
    body = res->body;
    return true;
}

std::string with_cache_bust(const std::string &url) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    std::string sep = url.find('?') == std::string::npos ? "?" : "&";
    return url + sep + "_t=" + std::to_string(ms);
}

bool http_get(const std::string &url, const std::string &referer, std::string &body,
              int &status, std::string &err) {
    std::string last;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(250 * attempt));
        if (http_get_once(url, referer, body, status, last)) return true;
        err = last;
    }
    return false;
}

/* ------------------------------------------------------------------ JS */

int interrupt_cb(JSRuntime *, void *opaque) {
    auto *dl = static_cast<std::chrono::steady_clock::time_point *>(opaque);
    return std::chrono::steady_clock::now() > *dl ? 1 : 0;
}

std::string js_exception(JSContext *ctx) {
    JSValue e = JS_GetException(ctx);
    std::string out = "JS 异常";
    if (JS_IsObject(e)) {
        JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
        if (!JS_IsUndefined(stack)) {
            const char *s = JS_ToCString(ctx, stack);
            if (s) { out = s; JS_FreeCString(ctx, s); }
            JS_FreeValue(ctx, stack);
            JS_FreeValue(ctx, e);
            return out;
        }
        JS_FreeValue(ctx, stack);
    }
    const char *s = JS_ToCString(ctx, e);
    if (s) { out = s; JS_FreeCString(ctx, s); }
    JS_FreeValue(ctx, e);
    return out;
}

JSValue call_global(JSContext *ctx, const char *name, int argc, JSValue *argv) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, name);
    JSValue ret = JS_UNDEFINED;
    if (JS_IsFunction(ctx, fn)) {
        ret = JS_Call(ctx, fn, global, argc, argv);
    } else {
        ret = JS_ThrowTypeError(ctx, "缺少全局函数 %s", name);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, global);
    return ret;
}

/* 执行一个字符串调用，忽略返回值 */
bool call_global_void(JSContext *ctx, const char *name, JSValue *argv, int argc,
                      std::string &err) {
    JSValue r = call_global(ctx, name, argc, argv);
    if (JS_IsException(r)) {
        err = std::string(name) + ": " + js_exception(ctx);
        JS_FreeValue(ctx, r);
        return false;
    }
    JS_FreeValue(ctx, r);
    return true;
}

int run_js(const std::string &page_url, const std::vector<std::string> &scripts,
           std::string &token, std::string &err) {
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) {
        err = "JS_NewRuntime 失败";
        return THS_VTOKEN_ERR_JS;
    }
    JS_SetMemoryLimit(rt, 128u * 1024 * 1024);
    JS_SetMaxStackSize(rt, 2u * 1024 * 1024);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    JS_SetInterruptHandler(rt, interrupt_cb, &deadline);

    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        err = "JS_NewContext 失败";
        return THS_VTOKEN_ERR_JS;
    }

    int rc = THS_VTOKEN_OK;

    /* 1) 载入核心 JS 字节码 */
    js_std_eval_binary(ctx, ths_vToken_core, ths_vToken_core_size, 0);
    if (JS_HasException(ctx)) {
        err = "核心 JS 载入失败: " + js_exception(ctx);
        rc = THS_VTOKEN_ERR_JS;
        goto done;
    }

    /* 2) 设定页面 URL */
    {
        JSValue a = JS_NewString(ctx, page_url.c_str());
        bool ok = call_global_void(ctx, "__ths_setPage", &a, 1, err);
        JS_FreeValue(ctx, a);
        if (!ok) { rc = THS_VTOKEN_ERR_JS; goto done; }
    }

    /* 3) 依次执行挑战页脚本（单个脚本异常不致命） */
    for (size_t i = 0; i < scripts.size(); ++i) {
        JSValue v = JS_Eval(ctx, scripts[i].data(), scripts[i].size(), "chunk.js",
                            JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) {
            /* 记录但不中断：主脚本可能已经算出令牌 */
            if (err.empty()) err = "脚本#" + std::to_string(i) + ": " + js_exception(ctx);
        }
        JS_FreeValue(ctx, v);
    }

    /* 4) 触发就绪事件并跑定时器 */
    {
        JSValue a = JS_NewString(ctx, "DOMContentLoaded");
        call_global_void(ctx, "__ths_fire", &a, 1, err);
        JS_FreeValue(ctx, a);
        JSValue b = JS_NewString(ctx, "load");
        call_global_void(ctx, "__ths_fire", &b, 1, err);
        JS_FreeValue(ctx, b);
        JSValue n = JS_NewInt32(ctx, 300);
        call_global_void(ctx, "__ths_runTimers", &n, 1, err);
        JS_FreeValue(ctx, n);
    }

    /* 5) 取令牌 */
    {
        JSValue r = call_global(ctx, "__ths_token", 0, nullptr);
        if (JS_IsException(r)) {
            err = "__ths_token: " + js_exception(ctx);
            rc = THS_VTOKEN_ERR_JS;
        } else {
            const char *s = JS_ToCString(ctx, r);
            if (s && *s) {
                token = s;
                rc = THS_VTOKEN_OK;
            } else {
                rc = THS_VTOKEN_ERR_NO_TOKEN;
                if (err.empty()) err = "JS 执行完但没有得到 v";
            }
            if (s) JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, r);
    }

done:
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return rc;
}

} // namespace

/* ------------------------------------------------------------------ API */

extern "C" {

const char *ths_vtoken_version(void) { return kVersion; }

void ths_vtoken_set_user_agent(const char *ua) {
    g_user_agent = (ua && *ua) ? ua : kDefaultUA;
}

int ths_vtoken_run(const char *html, const char *page_url, const char *const *scripts,
                   int n, char *out, size_t out_len, char *err, size_t err_len) {
    (void)html;
    if (!page_url || !out || out_len == 0) {
        set_err(err, err_len, "参数为空");
        return THS_VTOKEN_ERR_ARG;
    }
    out[0] = '\0';

    std::vector<std::string> bodies;
    for (int i = 0; i < n; ++i)
        if (scripts && scripts[i]) bodies.emplace_back(scripts[i]);

    std::string token, e;
    int rc = run_js(page_url, bodies, token, e);
    if (rc == THS_VTOKEN_OK) {
        if (token.size() + 1 > out_len) {
            set_err(err, err_len, "输出缓冲太小");
            return THS_VTOKEN_ERR_ARG;
        }
        std::memcpy(out, token.c_str(), token.size() + 1);
    } else if (!e.empty()) {
        set_err(err, err_len, e);
    }
    return rc;
}

int ths_vtoken_generate(const char *challenge_url, char *out, size_t out_len, char *err,
                        size_t err_len) {
    if (!out || out_len == 0) {
        set_err(err, err_len, "输出缓冲为空");
        return THS_VTOKEN_ERR_ARG;
    }
    out[0] = '\0';
    if (err && err_len) err[0] = '\0';

    const std::string url = (challenge_url && *challenge_url) ? challenge_url
                                                              : kDefaultChallengeUrl;

    /* 1) 取挑战页 */
    std::string html, e;
    int status = 0;
    if (!http_get(url, "", html, status, e)) {
        set_err(err, err_len, e);
        return THS_VTOKEN_ERR_HTTP;
    }
    if (status != 401 && status != 200) {
        set_err(err, err_len, "挑战页状态码 " + std::to_string(status));
        return THS_VTOKEN_ERR_HTTP;
    }

    /* 2) 解析脚本地址 */
    std::string scheme, host, path;
    split_url(url, scheme, host, path);
    std::vector<std::string> srcs = extract_scripts(html);
    if (srcs.empty()) {
        set_err(err, err_len, "挑战页里没有脚本（可能已被放行或页面结构变化）");
        return THS_VTOKEN_ERR_CHALLENGE;
    }

    /* 3) 下载脚本 */
    std::vector<std::string> bodies;
    for (const auto &s : srcs) {
        std::string abs = resolve_url(scheme, host, s);
        std::string body, ee;
        int st = 0;
        /* 某些 CDN 缓存副本会让 HTTP/1.1 客户端读失败，加时间戳参数可绕过 */
        if (!http_get(with_cache_bust(abs), url, body, st, ee)) {
            set_err(err, err_len, "脚本下载失败: " + ee);
            return THS_VTOKEN_ERR_SCRIPT;
        }
        if (st != 200) {
            set_err(err, err_len, "脚本状态码 " + std::to_string(st) + ": " + abs);
            return THS_VTOKEN_ERR_SCRIPT;
        }
        bodies.push_back(std::move(body));
    }

    /* 4) 跑 JS */
    std::string token;
    int rc = run_js(url, bodies, token, e);
    if (rc != THS_VTOKEN_OK) {
        set_err(err, err_len, e.empty() ? "生成失败" : e);
        return rc;
    }
    if (token.size() + 1 > out_len) {
        set_err(err, err_len, "输出缓冲太小");
        return THS_VTOKEN_ERR_ARG;
    }
    std::memcpy(out, token.c_str(), token.size() + 1);
    return THS_VTOKEN_OK;
}

char *ths_vtoken_generate_alloc(const char *challenge_url, char *err, size_t err_len) {
    char buf[128];
    int rc = ths_vtoken_generate(challenge_url, buf, sizeof buf, err, err_len);
    if (rc != THS_VTOKEN_OK) return nullptr;
    size_t n = std::strlen(buf);
    char *p = static_cast<char *>(std::malloc(n + 1));
    if (p) std::memcpy(p, buf, n + 1);
    return p;
}

} // extern "C"
