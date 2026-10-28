# ths_vToken

同花顺数据中心（`data.10jqka.com.cn` / `q.10jqka.com.cn`）**`hexin-v` 令牌生成库**。

输入一个 URL，输出一串 72 字符的令牌；把它放进 `Cookie: v=<令牌>`（或请求头 `hexin-v`）即可正常访问同花顺那些原本返回 401 的接口。

- 语言：C 接口 / C++ 实现
- 依赖：QuickJS（执行反爬脚本，已预编译为 `3rd/quickjs/libquickjs.a`）、cpp-httplib + OpenSSL（取数据）
- 产物：`ths_vToken.dll`（导出 5 个 C 符号）+ 导入库 `libths_vToken.dll.a`

---

## 1. 它能做什么

| 场景 | 说明 |
|---|---|
| 榜单/排行类接口 | 涨跌幅排行、市盈率、资金流、创新高/低、连续涨跌、突破、量价等 |
| 财务类接口 | 业绩预告/快报/公告、高管持股 |
| 融资融券、限售解禁 | 余额总览、明细 |
| 宏观数据 | 其中 PPI、PMI 必须走 ajax（其余整页可直出） |
| 板块/指数成分股 | `q.10jqka.com.cn` 的翻页接口，这是最依赖令牌的一类 |

## 2. 它不做什么

- **不解析业务数据**：只负责拿令牌，返回 HTML 片段由调用方解析。
- **不缓存令牌**：每次调用都重新生成（见「性能」）。缓存与轮换策略由调用方决定。
- **不提供代理**：走系统直连；需要代理请自行在 `http_get` 里加（httplib 支持 `set_proxy`）。
- **不做多线程调度**：单次调用内部是串行的。

---

## 3. 原理

### 3.1 401 不是"没权限"，是"要做题"

不带有效令牌请求同花顺的 ajax 接口时，服务端返回的不是 JSON/HTML 数据，而是一个 **516 字节的挑战页**：

```html
HTTP/1.1 401 Unauthorized
Set-Cookie: vvvv=1; Path=/
Content-Type: text/html;charset=utf-8

<html><body>
<script src="//s.thsi.cn/js/chameleon/chameleon.1.13.min.1792774.js"></script>
<script src="//s.thsi.cn/js/chameleon/chameleon.1.7.min.1792774.js"></script>
<script src="//s.thsi.cn/js/chameleon/chameleon.1.7.min.1792774.js"></script>
<script>window.location.href="//data.10jqka.com.cn/financial/ggjy/.../ajax/1/";</script>
</body></html>
```

浏览器执行这三个脚本后，脚本会：

1. 采集环境指纹（UA 哈希、平台、插件数、画布、鼠标/键盘统计、自动化特征……）
2. 打包成二进制载荷 → 加密 → base64url
3. 写入 `document.cookie = "v=<令牌>"`
4. `window.location.href` 跳回原 URL —— 这次带上 `v`，服务端放行

所以 401 的真实含义是：**"你的请求没有通过 JS 挑战"**，而不是账号/权限问题。

### 3.2 令牌长什么样

| 项 | 值 |
|---|---|
| 长度 | 72 字符（旧版为 60 字符） |
| 首字符 | `B`（首字节 `0x04` = 版本号 4；旧版是 `0x03`） |
| 字符集 | base64url：`A-Z a-z 0-9 - _` |
| 内容 | 加密后的环境指纹（含服务端下发的时间戳 `TOKEN_SERVER_TIME`） |

脚本里的编码链路（静态分析所得）：

```js
// 载荷字段与位宽
r = new T([4,4,4,4,1,1,1,3,2,2,2,2,2,2,2,4,2,1,4,4,1]);
r[1]  = serverTimeNow();                     // 服务端时间戳
r[2]  = timeNow();                           // 客户端时间
r[3]  = strhash(navigator.userAgent);        // h = h*31 + c
r[4]  = getPlatform();  r[5] = getBrowserIndex();  r[6] = getPluginNum();
r[7]  = getMouseMove(); r[8] = getMouseClick();    r[9] = getMouseWhell();
r[10] = getKeyDown();   r[11..12] = getClickPos();
r[13] = getBrowserFeature(); r[14] = getPathLengthRatio();
r[15] = getComprehensiveAutomationFeatures();
r[18] = getInputIntervalVariance(); r[19] = getClickDurationVariance();

encode(r.toBuffer());   // 首字节 4 + 加密载荷 + base64url
```

加密函数被控制流平坦化 + 字符串表 + 跳板函数层层包裹（典型的瑞数式 VM），静态还原成本极高；但**它只需要一个浏览器环境**，所以本库的做法是：给脚本造一个最小可用的浏览器环境，让它自己算。

### 3.3 本库的做法

```
┌──────────────────────────────┐
│  调用方 (C/C++)               │
│  ths_vtoken_generate()        │
└──────────────┬───────────────┘
               │
   ① GET 挑战页（无 cookie）
               ▼
┌──────────────────────────────┐
│  cpp-httplib + OpenSSL        │  → 401 + 挑战页 HTML
│  解析 <script src>            │
│  下载 3 个 JS（约 90KB）       │
└──────────────┬───────────────┘
               │
   ② 交给 QuickJS 执行
               ▼
┌──────────────────────────────┐
│  QuickJS 上下文               │
│  ├─ 先执行 qjsc 编译的核心 JS  │  建立 window/document/navigator/...
│  ├─ __ths_setPage(url)         │  设置 location
│  ├─ 依次 eval 三个脚本         │  脚本采集指纹 → 写 cookie
│  ├─ __ths_fire('load')         │  触发就绪事件
│  ├─ __ths_runTimers(300)       │  跑挂起的定时器
│  └─ __ths_token()              │  读 document.cookie 的 v
└──────────────┬───────────────┘
               │
   ③ 返回 72 字符令牌
```

核心 JS（`ths_vToken.js`）提供的最小浏览器环境包括：

| 能力 | 说明 |
|---|---|
| `window` / `self` / `top` / `parent` | 全部指向 globalThis |
| `document` | `createElement`、`head/body`、`getElementsByTagName`、`cookie` 读写（内部 Map） |
| `navigator` | UA、平台、语言、`plugins`（3 个真实插件对象）、`javaEnabled` 等 |
| `location` | href/protocol/host/hostname/pathname/search |
| 画布 | `getContext('2d')` 返回桩对象，`getImageData` 返回零填充的 `Uint8ClampedArray`，`toDataURL` 返回固定 PNG |
| 事件 | `addEventListener` / `dispatchEvent` / `fire` |
| 定时器 | `setTimeout` 入队，由 `__ths_runTimers` 统一执行 |
| 构造器 | `Element` / `Node` / `XMLHttpRequest` 等（脚本会校验 `typeof === 'function'`） |
| 其他 | `crypto.getRandomValues`、`localStorage`、`performance`、`atob/btoa`、`chrome`、`external` |

### 3.4 为什么不用 jsdom / Node

- **零运行时依赖**：核心 JS 用 `qjsc` 编译成字节码数组（`ths_vToken_bc.c`），和宿主一起编译进 DLL，不需要 Node、不需要 npm 包。
- **性能**：Node + jsdom 启动约 200~600ms 且体积大；QuickJS 上下文创建是微秒级。
- **可移植**：整套逻辑只依赖 `globalThis`，理论上可以换到任何能跑 ECMAScript 2020 的宿主。

---

## 4. 目录结构

```
ths_vToken/
├─ ths_vToken.js        核心 JS（浏览器环境垫片 + 对外 API）
├─ ths_vToken_bc.c      qjsc 生成的字节码 C 数组（勿手改）
├─ ths_vtoken.h         C API 头文件（集成时只需要它）
├─ ths_vtoken.cpp       库实现：httplib 取数 + QuickJS 执行
├─ cli.js               Node 宿主（调试/对照用，非必需）
├─ test_ths_vtoken.cpp  测试程序（含 --gen 生成模式）
├─ CMakeLists.txt
└─ build-mingw/
   ├─ bin/ths_vToken.dll          ← 运行时库
   ├─ bin/test_ths_vtoken.exe     ← 测试程序
   └─ lib/libths_vToken.dll.a     ← 链接用导入库
```

---

## 5. 编译

环境：MinGW-w64 GCC（不支持 MSVC）、CMake ≥ 3.16、OpenSSL 在 `D:/mingw64/opt`。

```powershell
cd D:\murasakii\Desktop\stocksystem\ths_vToken

cmake -S . -B build-mingw -G "MinGW Makefiles" `
      -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ `
      -DCMAKE_MAKE_PROGRAM=D:/mingw64/bin/mingw32-make.exe

cmake --build build-mingw -j 8
```

> ⚠ 必须显式传 `-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++`，否则 CMake 会抓到本机的 MSVC 并失败。

产物：

```
build-mingw/bin/ths_vToken.dll        约 8.9 MB（含 QuickJS，带 Intl/Temporal）
build-mingw/bin/test_ths_vtoken.exe
build-mingw/lib/libths_vToken.dll.a
```

---

## 6. 集成到其他工程

### 6.1 CMake 子目录（推荐）

```cmake
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/ths_vToken ths_vToken_build)

add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE ths_vToken)
```

`ths_vToken` 目标已把 include 目录、OpenSSL、httplib、ws2_32 等依赖设为 PUBLIC/PRIVATE。

### 6.2 直接链接

```cmake
target_include_directories(my_app PRIVATE path/to/ths_vToken)
target_link_libraries(my_app PRIVATE path/to/libths_vToken.dll.a)
```

如果使用 `__declspec(dllimport)` 形式，请在编译时定义 `THS_VTOKEN_DLL`；MinGW 下不定义也能正常链接。

### 6.3 运行期依赖

`ths_vToken.dll` 必须和 exe 同目录（或 PATH 中），并需要：

```
ths_vToken.dll
libssl-3-x64.dll
libcrypto-3-x64.dll
libstdc++-6.dll
libgcc_s_seh-1.dll
libwinpthread-1.dll
```

CMake 构建 `test_ths_vtoken` 时会自动把这些 DLL 拷到 `bin/`，可作为参考。

---

## 7. API 参考

头文件：`ths_vtoken.h`

```c
const char *ths_vtoken_version(void);
void        ths_vtoken_set_user_agent(const char *ua);

int  ths_vtoken_generate(const char *challenge_url,
                         char *out, size_t out_len,
                         char *err, size_t err_len);

char *ths_vtoken_generate_alloc(const char *challenge_url,
                                char *err, size_t err_len);

int  ths_vtoken_run(const char *html, const char *page_url,
                    const char *const *scripts, int n,
                    char *out, size_t out_len,
                    char *err, size_t err_len);
```

### 7.1 `ths_vtoken_generate`

完整流程（取挑战页 → 下载脚本 → 执行 → 取令牌），**同步阻塞**。

| 参数 | 说明 |
|---|---|
| `challenge_url` | 取挑战页用的 URL。传 `NULL` 用默认值（`/financial/ggjy/.../ajax/1/`）。 |
| `out` / `out_len` | 输出缓冲，建议 ≥ 128 字节（令牌固定 72 字符）。 |
| `err` / `err_len` | 错误信息，可传 `NULL`。 |

### 7.2 `ths_vtoken_generate_alloc`

同上，但返回 `malloc` 的字符串，失败返回 `NULL`。调用方负责 `free()`。

### 7.3 `ths_vtoken_set_user_agent`

覆盖默认 UA。**必须在调用 `generate` 之前设置**；进程级全局变量，多线程下请先设置再并发。

### 7.4 `ths_vtoken_run`

离线模式：不联网，直接把挑战页 HTML + 脚本内容喂进来跑。用于：

- 单元测试（用抓下来的样本）
- 自行实现网络层（例如已有连接池）

```c
const char *scripts[3] = { js1, js2, js3 };
char tok[128], err[512];
int rc = ths_vtoken_run(html, "https://data.10jqka.com.cn/...", scripts, 3,
                        tok, sizeof tok, err, sizeof err);
```

> `html` 参数目前仅保留，脚本内容以 `scripts` 为准。

### 7.5 返回码

| 值 | 宏 | 含义 |
|---|---|---|
| 0 | `THS_VTOKEN_OK` | 成功 |
| 1 | `THS_VTOKEN_ERR_ARG` | 参数错误 / 缓冲太小 |
| 2 | `THS_VTOKEN_ERR_HTTP` | 网络或 HTTP 失败（超时、连接重置） |
| 3 | `THS_VTOKEN_ERR_CHALLENGE` | 挑战页异常：没有 `<script>`（可能被直接放行或页面改版） |
| 4 | `THS_VTOKEN_ERR_JS` | QuickJS 执行失败（核心 JS 载入失败、缺全局函数） |
| 5 | `THS_VTOKEN_ERR_SCRIPT` | 脚本下载失败（状态码非 200） |
| 6 | `THS_VTOKEN_ERR_NO_TOKEN` | JS 跑完但没有产出 `v` |

### 7.6 线程安全

- `ths_vtoken_generate` / `ths_vtoken_run` **可重入**：每次调用独立创建 `JSRuntime` / `JSContext`，互不影响。
- `ths_vtoken_set_user_agent` 修改的是进程级全局变量，**不是线程安全**的；请在启动阶段设置一次。

---

## 8. 使用示例

### 8.1 最小示例

```c
#include <stdio.h>
#include "ths_vtoken.h"

int main(void) {
    char tok[128] = {0};
    char err[512] = {0};

    int rc = ths_vtoken_generate(NULL, tok, sizeof tok, err, sizeof err);
    if (rc != THS_VTOKEN_OK) {
        fprintf(stderr, "生成失败 rc=%d: %s\n", rc, err);
        return 1;
    }
    printf("Cookie: v=%s\n", tok);
    return 0;
}
```

### 8.2 配合 httplib 发请求

```cpp
#include "ths_vtoken.h"
#include "httplib.h"

char tok[128], err[512];
if (ths_vtoken_generate(nullptr, tok, sizeof tok, err, sizeof err) != THS_VTOKEN_OK)
    return;

httplib::Client cli("https://data.10jqka.com.cn");
cli.enable_server_certificate_verification(false);
auto res = cli.Get("/financial/ggjy/field/enddate/order/desc/page/1/ajax/1/", {
    {"User-Agent", "Mozilla/5.0 ... Edg/155.0.0.0"},
    {"Cookie", std::string("v=") + tok},
});
// res->status == 200，res->body 是 GBK 编码的表格片段
```

### 8.3 401 自动换令牌（推荐用法）

令牌有配额（实测约 10 次请求后转 401），标准做法是遇到 401 就重新生成：

```c
char tok[128] = {0}, err[512];
ths_vtoken_generate(NULL, tok, sizeof tok, err, sizeof err);

for (int page = 1; page <= 50; ++page) {
    int status = do_request(page, tok);          // 你自己的 HTTP 封装
    if (status == 401) {
        if (ths_vtoken_generate(NULL, tok, sizeof tok, err, sizeof err) != THS_VTOKEN_OK) {
            fprintf(stderr, "换牌失败: %s\n", err);
            break;
        }
        status = do_request(page, tok);
    }
    if (status != 200) break;
}
```

### 8.4 只想要令牌（测试/脚本用）

```
build-mingw\bin\test_ths_vtoken.exe --gen --out token.txt
```

`--gen` 只生成并输出令牌，不做联网校验；`--out` 同时写文件。不带参数时跑完整测试（生成 2 枚令牌并各打两个接口）。

---

## 9. Node CLI（调试用）

核心 JS 与 C 库是同一份，Node 版可用于快速对照：

```powershell
cd ths_vToken
node cli.js --debug --out token.txt
```

| 参数 | 说明 |
|---|---|
| `--debug` | 打印诊断（挑战页状态、脚本数、每步 cookie、耗时） |
| `--url <URL>` | 指定取挑战页的 URL |
| `--out <FILE>` | 把令牌写入文件 |

典型输出：

```
[cli] 挑战页 401 516 bytes
[cli] 脚本数 3
[cli] 已执行 chameleon.1.13.min.1792776.js | cookie= v=BPSNwXAP28d...
[cli] 耗时 630 ms; cookie= v=BPSNwXAP28d...
```

> 注意：Node 21+ 自带只读的 `navigator` / `performance` / `crypto`，核心 JS 用 `Object.defineProperty` 覆盖，这在 QuickJS 下同样安全。

---

## 10. 修改核心 JS

`ths_vToken_bc.c` 是编译产物，**不要手改**。改动 `ths_vToken.js` 后必须重新生成：

```powershell
..\3rd\quickjs\qjsc.exe -s -c -N ths_vToken_core -o ths_vToken_bc.c ths_vToken.js
cmake --build build-mingw -j 8
```

参数说明：

| 参数 | 作用 |
|---|---|
| `-s` | 去掉调试信息（体积小） |
| `-c` | 输出字节码 C 数组 |
| `-N ths_vToken_core` | 生成的 C 数组名（C 里按这个名字引用） |

> ⚠ **字节码与 QuickJS 版本绑定**：`qjsc.exe` 和 `libquickjs.a` 必须来自同一份源码/同一次构建，否则加载字节码会失败（`JS_ReadObject` 报错）。

---

## 11. 性能

| 项 | 实测 |
|---|---|
| 单次生成 | 约 1~2 秒（TLS 握手 + 下载 3 个脚本约 90KB + 执行） |
| 令牌寿命 | 约 10 次请求后返回 401 |
| 单个 DLL | 8.9 MB（QuickJS 含 Intl/Temporal） |

每次 `generate` 都会重新下载脚本。如果需要高频调用，可以在 `http_get` 层加一个按 URL 的短时缓存（脚本内容不随时间变化，变的是挑战页里的 `TOKEN_SERVER_TIME`）。

---

## 12. 已知限制与排错

### 12.1 必须每次现取挑战页

令牌载荷里嵌了服务端下发的 `TOKEN_SERVER_TIME`（脚本首行 `var TOKEN_SERVER_TIME=...`）。**缓存挑战页 = 时间戳过期 = 令牌被拒**。库内部每次都重新 GET，这是刻意设计。

### 12.2 CDN 坏缓存（已内部处理）

实测 `s.thsi.cn` 上某些静态路径（如 `chameleon.1.7.min.xxx.js`）对 HTTP/1.1 客户端会直接断连（`Failed to read connection`），curl 却正常；给 URL 加任意查询参数即可恢复。库对脚本下载统一追加 `_t=<毫秒时间戳>` 绕过。

### 12.3 常见返回码对照

| 现象 | 可能原因 | 处理 |
|---|---|---|
| `rc=2` | 网络不通 / DNS / 代理 | 检查网络与防火墙 |
| `rc=3` | 挑战页里没有 `<script>` | 可能该 IP 已被放行（返回 200），或页面改版 |
| `rc=5` | 脚本 200 但下载失败 | 重试；或检查 CDN 可达性 |
| `rc=6` | JS 跑完没产出 `v` | 多半是同花顺改版，需要更新 `ths_vToken.js` 的环境垫片 |

### 12.4 同花顺改版怎么办

`chameleon` 脚本版本号会变（如 `1792773` → `1792774` → `1792776`），但这**不影响使用**——库每次都从挑战页解析最新地址。只有当脚本开始依赖垫片没实现的新 API 时，才需要：

1. `node cli.js --debug` 复现；
2. 在核心 JS 里补对应能力；
3. 重新 `qjsc` 并重编。

排查技巧：脚本的所有异常都被自己的 `try/catch` 吞掉，表现为"什么都没发生"。可在调试时把 `catch(t){return t}` 替换成打印 `t.stack`（`cli.js --debug` 已内置这个注入）。

### 12.5 IPv6 链路导致 "Connection timed out"

现象：

```
失败 rc=5 err=脚本下载失败: 请求失败(https://s.thsi.cn/js/chameleon/chameleon.1.13.min.1793199.js?_t=...): Connection timed out
```

原因：`s.thsi.cn` 同时有 A 和 AAAA 记录，`getaddrinfo` 往往先返回 IPv6 地址。如果本机虽分配了 IPv6 地址、但到该 CDN 的 IPv6 链路实际不通（国内网络常见），就会一直卡在连接阶段直到超时。

诊断：

```powershell
curl.exe -4 -o NUL -w "%{http_code} %{time_connect}" https://s.thsi.cn/js/...   # 正常
curl.exe -6 -o NUL -w "%{http_code} %{time_connect}" https://s.thsi.cn/js/...   # 超时
Resolve-DnsName s.thsi.cn -Type A     # 有 IPv4 记录
```

处理：库内部已固定 `cli.set_address_family(AF_INET)`（强制 IPv4）。如果你的环境只有 IPv6，需要把它改回 `AF_UNSPEC`，或按 `AF_INET6` 单独适配。

---

## 13. 版本记录

| 版本 | 说明 |
|---|---|
| 1.0.0 | 首个版本：QuickJS 字节码 + httplib，导出 5 个 C 符号；支持 401 自动换令牌 |
| 1.0.1 | 强制 IPv4（`AF_INET`），修复 IPv6 链路不通时的 `Connection timed out` |
