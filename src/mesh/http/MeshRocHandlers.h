#pragma once
/**
 * MeshROC 自研 Web/APP 接口 handler（§12.3）
 * 在保留原版 /api/v1/* 基础上叠加 /api/v1/meshroc/* 端点。
 * 返回 application/json，与原版 protobuf 端点协议隔离。
 */
// 前置声明 httpsserver 命名空间的类型，避免用全局 `class HTTPServer`（会与库的
// httpsserver::HTTPServer 歧义，并污染其它 include 本头的文件如 ContentHandler.h）。
namespace httpsserver {
class HTTPServer;
}
void registerMeshRocHandlers(httpsserver::HTTPServer *insecureServer, httpsserver::HTTPServer *secureServer);
