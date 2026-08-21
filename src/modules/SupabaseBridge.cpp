#include "SupabaseBridge.h"

#if HAS_WIFI && defined(ARCH_ESP32)
#include <WiFi.h>
#include <HTTPClient.h>
#include "mesh/Channels.h" // 仅用于日志宏（若不可用可替换为 meshtastic 日志）
#endif

#include <Arduino.h>
#include <cstring>

namespace meshroc {

// 把字符串安全地塞进 JSON 字符串字段（转义双引号与反斜杠，截断超长）
static size_t jsonEscape(const char *src, char *dst, size_t dstCap)
{
    size_t j = 0;
    if (src) {
        for (size_t i = 0; src[i] && j + 2 < dstCap; i++) {
            char c = src[i];
            if (c == '"' || c == '\\') {
                if (j + 3 >= dstCap) break;
                dst[j++] = '\\';
                dst[j++] = c;
            } else if (c == '\n' || c == '\r' || c == '\t') {
                if (j + 3 >= dstCap) break;
                dst[j++] = '\\';
                dst[j++] = (c == '\n') ? 'n' : (c == '\r') ? 'r' : 't';
            } else {
                dst[j++] = c;
            }
        }
    }
    dst[j] = '\0';
    return j;
}

bool supabasePostComment(const char *baseUrl, const char *anonKey, const char *postId,
                         const char *content, const char *nickname)
{
    // 守卫：凭据缺失则安全跳过（不硬编码、不崩溃）
    if (!baseUrl || !baseUrl[0] || !anonKey || !anonKey[0])
        return false;
    if (!postId || !postId[0] || !content || !content[0])
        return false;
#if HAS_WIFI && defined(ARCH_ESP32)
    if (WiFi.status() != WL_CONNECTED)
        return false;

    // 构造 REST 端点：<baseUrl>/rest/v1/comments
    String endpoint = String(baseUrl);
    endpoint.trim();
    if (endpoint.endsWith("/"))
        endpoint.remove(endpoint.length() - 1);
    endpoint += "/rest/v1/comments";

    // 组装 JSON body
    char escPost[160] = {0}, escContent[256] = {0}, escNick[64] = {0};
    jsonEscape(postId, escPost, sizeof(escPost));
    jsonEscape(content, escContent, sizeof(escContent));
    jsonEscape(nickname ? nickname : "", escNick, sizeof(escNick));

    char body[512];
    snprintf(body, sizeof(body),
             "{\"post_id\":\"%s\",\"content\":\"%s\",\"nickname\":\"%s\"}",
             escPost, escContent, escNick);

    HTTPClient http;
    http.setTimeout(5000);
    if (!http.begin(endpoint)) {
        LOG_WARN("SupabaseBridge: http.begin failed");
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", anonKey);
    http.addHeader("Authorization", String("Bearer ") + anonKey);
    http.addHeader("Prefer", "return=minimal");

    int code = http.POST((uint8_t *)body, strlen(body));
    bool ok = (code >= 200 && code < 300);
    if (!ok)
        LOG_WARN("SupabaseBridge: POST comments -> HTTP %d", code);
    else
        LOG_INFO("SupabaseBridge: posted to %s", escPost);

    http.end();
    return ok;
#else
    (void)nickname;
    return false;
#endif
}

} // namespace meshroc
