#ifndef THS_VTOKEN_H
#define THS_VTOKEN_H

/*
 * ths_vToken —— 同花顺 (data.10jqka.com.cn / q.10jqka.com.cn) hexin-v(v) 令牌生成库
 *
 * 纯 C 接口；内部用 cpp-httplib 取挑战页与脚本，用 QuickJS 执行 chameleon JS。
 *
 * 典型用法：
 *     char tok[128], err[512];
 *     int rc = ths_vtoken_generate(NULL, tok, sizeof tok, err, sizeof err);
 *     if (rc == THS_VTOKEN_OK) { ... 把 tok 放进 Cookie: v=<tok> ... }
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(THS_VTOKEN_EXPORTS)
#    define THS_VTOKEN_API __declspec(dllexport)
#  elif defined(THS_VTOKEN_DLL)
#    define THS_VTOKEN_API __declspec(dllimport)
#  else
#    define THS_VTOKEN_API
#  endif
#elif defined(__GNUC__)
#  define THS_VTOKEN_API __attribute__((visibility("default")))
#else
#  define THS_VTOKEN_API
#endif

/* 返回码 */
#define THS_VTOKEN_OK             0   /* 成功 */
#define THS_VTOKEN_ERR_ARG        1   /* 参数错误 */
#define THS_VTOKEN_ERR_HTTP       2   /* 网络/HTTP 失败 */
#define THS_VTOKEN_ERR_CHALLENGE  3   /* 挑战页异常（没有脚本或无挑战） */
#define THS_VTOKEN_ERR_JS         4   /* QuickJS 执行失败 */
#define THS_VTOKEN_ERR_SCRIPT     5   /* 脚本下载失败 */
#define THS_VTOKEN_ERR_NO_TOKEN   6   /* 执行完但没有得到 v */

/* 库版本 */
THS_VTOKEN_API const char *ths_vtoken_version(void);

/* 设置 User-Agent（可选，默认是 Edge/Chrome 的 UA） */
THS_VTOKEN_API void ths_vtoken_set_user_agent(const char *ua);

/*
 * 生成令牌（同步阻塞）
 *
 *   challenge_url  取挑战页的 URL；传 NULL 用默认值
 *   out/out_len    输出缓冲，建议 128 字节（令牌固定 72 字符）
 *   err/err_len    错误信息缓冲，可传 NULL
 *
 * 返回 THS_VTOKEN_OK 或上面某个错误码。
 */
THS_VTOKEN_API int ths_vtoken_generate(const char *challenge_url,
                                       char *out, size_t out_len,
                                       char *err, size_t err_len);

/* 同上，但返回 malloc 出来的字符串，调用方 free() */
THS_VTOKEN_API char *ths_vtoken_generate_alloc(const char *challenge_url,
                                               char *err, size_t err_len);

/*
 * 仅跑 JS（不联网）：传入挑战页 HTML 和脚本内容，用于离线测试 / 自定义取数。
 *   html         挑战页 HTML
 *   page_url     该页面的 URL（用于 location）
 *   scripts      脚本内容数组
 *   n            脚本个数
 */
THS_VTOKEN_API int ths_vtoken_run(const char *html, const char *page_url,
                                  const char *const *scripts, int n,
                                  char *out, size_t out_len,
                                  char *err, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* THS_VTOKEN_H */
