#pragma once
#include <cstdint>

/**
 * SupabaseBridge —— GATEWAY 角色上行写 Supabase 的轻量 helper。
 *
 * 设计要点（用户确认 2026-08-17）：
 *  - 凭据（URL + anon key）来自 MeshROCConfig.gateway.supabaseUrl / supabaseKey，
 *    运行时读取，绝不硬编码进固件。
 *  - 仅当 role==GATEWAY 且凭据非空时调用，fire-and-forget，不阻塞射频发送主路径。
 *  - 仅在 HAS_WIFI 且有联网能力时编译生效（ESP32）。
 *
 * 写入目标：Supabase 表 `comments`，字段对齐网页端留言板：
 *   post_id    频道留言板 ID（如 /meshroc-guestbook-longfast）
 *   content    消息文本
 *   nickname   发送节点标识（节点名或短 node id）
 *   user_id    留空（设备消息无云端账号，网页端以 device 标记区分）
 */
namespace meshroc {

// 把一条 LoRa / MQTT 文本消息写入 Supabase comments 表。
// 返回 true 表示已发起请求（不代表一定成功入库）。
bool supabasePostComment(const char *baseUrl, const char *anonKey, const char *postId,
                         const char *content, const char *nickname);

} // namespace meshroc
