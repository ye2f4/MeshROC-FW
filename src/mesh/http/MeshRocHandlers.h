#pragma once
/**
 * MeshROC 自研 Web/APP 接口 handler（§12.3）
 * 在保留原版 /api/v1/* 基础上叠加 /api/v1/meshroc/* 端点。
 * 返回 application/json，与原版 protobuf 端点协议隔离。
 */
void registerMeshRocHandlers(class HTTPServer *insecureServer, class HTTPServer *secureServer);
