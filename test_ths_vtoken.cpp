/*
 * test_ths_vtoken.cpp —— ths_vToken 库的独立测试程序
 *
 *   1) 调用库生成令牌
 *   2) 用该令牌实际请求同花顺接口，校验是否被服务端接受
 *   3) 连续生成两枚令牌，确认可重复使用
 */

#include <cstdio>
#include <cstring>
#include <string>

#include "ths_vtoken.h"
#include "httplib.h"

static const char *kUA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/155.0.0.0 Safari/537.36 Edg/155.0.0.0";

/* 用令牌访问某个接口，返回 HTTP 状态码；body_len 输出响应长度 */
static int probe(const std::string &host, const std::string &path, const std::string &token,
                 const std::string &referer, size_t &body_len) {
    body_len = 0;
    httplib::Client cli(host);
    cli.set_follow_location(true);
    cli.enable_server_certificate_verification(false);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(25);

    httplib::Headers h = {
        {"User-Agent", kUA},
        {"Accept", "text/html,*/*;q=0.8"},
        {"Accept-Language", "zh-CN,zh;q=0.9"},
        {"Referer", referer},
        {"X-Requested-With", "XMLHttpRequest"},
        {"Cookie", "v=" + token},
    };
    auto res = cli.Get(path.c_str(), h);
    if (!res) return -1;
    body_len = res->body.size();
    return res->status;
}

static int one_round(int idx) {
    char tok[128] = {0};
    char err[512] = {0};

    std::printf("[%d] 生成令牌 ...\n", idx);
    int rc = ths_vtoken_generate(nullptr, tok, sizeof tok, err, sizeof err);
    if (rc != THS_VTOKEN_OK) {
        std::printf("    失败 rc=%d err=%s\n", rc, err);
        return 1;
    }
    std::printf("    token = %s\n", tok);
    std::printf("    长度   = %zu\n", std::strlen(tok));
    if (std::strlen(tok) != 72 || tok[0] != 'B') {
        std::printf("    警告：格式不是预期的 72 字符 / 版本4(B)\n");
    }

    size_t n = 0;
    int st = probe("https://data.10jqka.com.cn",
                   "/financial/ggjy/field/enddate/order/desc/page/3/ajax/1/", tok,
                   "https://data.10jqka.com.cn/financial/ggjy/", n);
    std::printf("    data.10jqka ajax  -> status=%d bytes=%zu %s\n", st, n,
                (st == 200 && n > 1000) ? "OK" : "FAIL");
    int bad = !(st == 200 && n > 1000);

    st = probe("http://q.10jqka.com.cn",
               "/thshy/detail/field/199112/order/desc/page/1/ajax/1/code/881121", tok,
               "http://q.10jqka.com.cn/", n);
    std::printf("    q.10jqka 成分股   -> status=%d bytes=%zu %s\n", st, n,
                (st == 200 && n > 1000) ? "OK" : "FAIL");
    bad += !(st == 200 && n > 1000);

    return bad;
}

int main(int argc, char **argv) {
    /* --gen [--out 文件]：只生成令牌（供脚本调用） */
    std::string out_file;
    bool gen_only = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--gen") gen_only = true;
        else if (a == "--out" && i + 1 < argc) out_file = argv[++i];
    }
    if (gen_only) {
        char tok[128] = {0};
        char err[512] = {0};
        int rc = ths_vtoken_generate(nullptr, tok, sizeof tok, err, sizeof err);
        if (rc != THS_VTOKEN_OK) {
            std::fprintf(stderr, "%s\n", err);
            return rc;
        }
        if (!out_file.empty()) {
            if (std::FILE *f = std::fopen(out_file.c_str(), "wb")) {
                std::fputs(tok, f);
                std::fclose(f);
            }
        }
        std::printf("%s\n", tok);
        return 0;
    }

    std::printf("ths_vToken 版本: %s\n", ths_vtoken_version());
    std::printf("=========================================\n");

    int fail = 0;
    fail += one_round(1);
    std::printf("-----------------------------------------\n");
    fail += one_round(2);

    std::printf("=========================================\n");
    if (fail) {
        std::printf("结果: 失败（%d 项）\n", fail);
        return 1;
    }
    std::printf("结果: 全部通过\n");
    return 0;
}
