#include "MeshRocHandlers.h"
#include "kernel/config/MeshROCConfig.h"
#include "kernel/net/MeshRocPacket.h"
#include "kernel/net/NextHopRouter.h"

#include <Arduino.h>
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <HTTPSServer.hpp>

using namespace httpsserver;

// 全局自研配置实例（反转后由 FlashKV 加载；当前为默认构造）
static meshroc::config::MeshROCConfig gMeshRocConfig;

namespace {

void setJsonHeaders(HTTPResponse* res)
{
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET, PUT, POST, OPTIONS");
    res->setHeader("Access-Control-Allow-Headers", "Content-Type");
}

// GET /api/v1/meshroc/config
void handleMeshRocConfig(HTTPRequest* req, HTTPResponse* res)
{
    if (req->getMethod() == "OPTIONS") { res->setStatusCode(204); res->print(""); return; }
    setJsonHeaders(res);
    const auto& c = gMeshRocConfig;
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"deviceRole\":%d,"
        "\"lora\":{\"hopLimit\":%d,\"channel\":%d,\"region\":%d},"
        "\"mqtt\":{\"enabled\":%s},"
        "\"compat\":{\"emitNativeTextPort\":%s,\"dualSendChannel0\":%s},"
        "\"rap\":{\"enabled\":%s,\"helloIntervalMs\":%u,\"attachTimeoutMs\":%u},"
        "\"gateway\":{\"upstreamUrl\":\"%s\",\"autoReconnect\":%s},"
        "\"rf\":{\"envProfile\":%d,\"tdma\":{\"enabled\":%s,\"slotCount\":%d},\"fragEnabled\":%s},"
        "\"crypto\":{\"mode\":%d,\"pskFallback\":%s},"
        "\"ack\":{\"priorityLevels\":%s},"
        "\"power\":{\"smartManagement\":%s,\"lowBattRelayOff\":%s},"
        "\"offlineCache\":{\"enabled\":%s,\"replicas\":%d},"
        "\"relayAllowed\":%s}",
        (int)c.deviceRole,
        c.lora.hopLimit, c.lora.channel, c.lora.region,
        c.mqtt.enabled ? "true" : "false",
        c.compat.emitNativeTextPort ? "true" : "false",
        c.compat.dualSendChannel0 ? "true" : "false",
        c.rap.enabled ? "true" : "false", c.rap.helloIntervalMs, c.rap.attachTimeoutMs,
        c.gateway.upstreamUrl, c.gateway.autoReconnect ? "true" : "false",
        (int)c.rf.envProfile, c.rf.tdma.enabled ? "true" : "false", c.rf.tdma.slotCount,
        c.rf.fragEnabled ? "true" : "false",
        (int)c.crypto.mode, c.crypto.pskFallback ? "true" : "false",
        c.ack.priorityLevels ? "true" : "false",
        c.power.smartManagement ? "true" : "false", c.power.lowBattRelayOff ? "true" : "false",
        c.offlineCache.enabled ? "true" : "false", c.offlineCache.replicas,
        meshroc::config::isRelayAllowed(c.deviceRole) ? "true" : "false");
    res->print(buf);
}

// GET /api/v1/meshroc/role
void handleMeshRocRole(HTTPRequest* req, HTTPResponse* res)
{
    setJsonHeaders(res);
    const auto& c = gMeshRocConfig;
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"role\":%d,\"rapClass\":%d,\"relayAllowed\":%s,\"ttlMs\":%u}",
        (int)c.deviceRole, (int)c.deviceRole,
        meshroc::config::isRelayAllowed(c.deviceRole) ? "true" : "false",
        meshroc::config::rapTtlMs(c.deviceRole));
    res->print(buf);
}

// GET /api/v1/meshroc/rap/topology  (占位，反转后接 kernel/net/rap)
void handleMeshRocRapTopology(HTTPRequest* req, HTTPResponse* res)
{
    setJsonHeaders(res);
    res->print("{\"nodes\":[],\"status\":\"stub\"}");
}

// GET /api/v1/meshroc/rap/routes  (占位)
void handleMeshRocRapRoutes(HTTPRequest* req, HTTPResponse* res)
{
    setJsonHeaders(res);
    res->print("{\"routes\":[],\"status\":\"stub\"}");
}

// GET /api/v1/meshroc/gateway/status  (占位)
void handleMeshRocGatewayStatus(HTTPRequest* req, HTTPResponse* res)
{
    setJsonHeaders(res);
    res->print("{\"online\":false,\"lastUpstreamMs\":0,\"status\":\"stub\"}");
}

// POST /api/v1/meshroc/gateway/reconnect  (占位)
void handleMeshRocGatewayReconnect(HTTPRequest* req, HTTPResponse* res)
{
    if (req->getMethod() == "OPTIONS") { res->setStatusCode(204); res->print(""); return; }
    res->setStatusCode(202);
    setJsonHeaders(res);
    res->print("{\"status\":\"accepted\"}");
}

void registerOn(HTTPServer* srv)
{
    if (!srv) return;
    srv->registerNode(new ResourceNode("/api/v1/meshroc/config", "GET", &handleMeshRocConfig));
    srv->registerNode(new ResourceNode("/api/v1/meshroc/config", "OPTIONS", &handleMeshRocConfig));
    srv->registerNode(new ResourceNode("/api/v1/meshroc/role",   "GET", &handleMeshRocRole));
    srv->registerNode(new ResourceNode("/api/v1/meshroc/role",   "OPTIONS", &handleMeshRocRole));
    srv->registerNode(new ResourceNode("/api/v1/meshroc/rap/topology", "GET", &handleMeshRocRapTopology));
    srv->registerNode(new ResourceNode("/api/v1/meshroc/rap/routes",   "GET", &handleMeshRocRapRoutes));
    srv->registerNode(new ResourceNode("/api/v1/meshroc/gateway/status", "GET", &handleMeshRocGatewayStatus));
    srv->registerNode(new ResourceNode("/api/v1/meshroc/gateway/reconnect", "POST", &handleMeshRocGatewayReconnect));
    srv->registerNode(new ResourceNode("/api/v1/meshroc/gateway/reconnect", "OPTIONS", &handleMeshRocGatewayReconnect));
}

}  // namespace

void registerMeshRocHandlers(HTTPServer* insecureServer, HTTPServer* secureServer)
{
    registerOn(insecureServer);
    registerOn(secureServer);
}
