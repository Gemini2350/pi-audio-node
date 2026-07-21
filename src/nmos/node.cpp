#include "node.h"
#include <chrono>
#include <fstream>
#include <cstring>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "civetweb.h"
#include "config.h"
#include "connection.h"
#include "dnssd.h"
#include "is12.h"
#include "log.h"
#include "ptp/ptpclient.h"
#include "rtp/rtpreceiver.h"
#include "rtp/rtpsender.h"

using namespace pan::nmos;
using json = nlohmann::json;

namespace
{
    struct UrlParts { std::string sHost; int nPort = 80; std::string sPath; };

    std::optional<UrlParts> ParseUrl(const std::string& sUrl)
    {
        auto nScheme = sUrl.find("://");
        if(nScheme == std::string::npos) { return std::nullopt; }
        auto sRest = sUrl.substr(nScheme+3);
        UrlParts parts;
        auto nSlash = sRest.find('/');
        auto sHostPort = nSlash == std::string::npos ? sRest : sRest.substr(0, nSlash);
        parts.sPath = nSlash == std::string::npos ? "/" : sRest.substr(nSlash);
        if(auto nColon = sHostPort.find(':'); nColon != std::string::npos)
        {
            parts.sHost = sHostPort.substr(0, nColon);
            parts.nPort = atoi(sHostPort.c_str()+nColon+1);
        }
        else
        {
            parts.sHost = sHostPort;
        }
        return parts;
    }
}

std::optional<json> pan::nmos::HttpJson(const std::string& sMethod, const std::string& sUrl,
                                        const json* pBody, int& nStatus)
{
    nStatus = 0;
    auto parts = ParseUrl(sUrl);
    if(!parts) { return std::nullopt; }

    char sError[256] = {0};
    mg_connection* pConn = mg_connect_client(parts->sHost.c_str(), parts->nPort, 0, sError, sizeof(sError));
    if(!pConn) { return std::nullopt; }

    std::string sBody = pBody ? pBody->dump() : "";
    mg_printf(pConn, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
                     "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
              sMethod.c_str(), parts->sPath.c_str(), parts->sHost.c_str(), sBody.size());
    if(!sBody.empty()) { mg_write(pConn, sBody.data(), sBody.size()); }

    int nResult = mg_get_response(pConn, sError, sizeof(sError), 3000);
    if(nResult < 0)
    {
        mg_close_connection(pConn);
        return std::nullopt;
    }
    const mg_response_info* pInfo = mg_get_response_info(pConn);
    nStatus = pInfo ? pInfo->status_code : 0;

    std::string sResponse;
    char buffer[4096];
    int nRead;
    while((nRead = mg_read(pConn, buffer, sizeof(buffer))) > 0)
    {
        sResponse.append(buffer, static_cast<size_t>(nRead));
    }
    mg_close_connection(pConn);

    if(sResponse.empty()) { return json(); }
    try { return json::parse(sResponse); }
    catch(...) { return json(); }
}

namespace
{
    std::string InterfaceMac(const std::string& sInterface)
    {
        std::ifstream ifs("/sys/class/net/" + sInterface + "/address");
        std::string sMac;
        if(ifs >> sMac)
        {
            for(auto& c : sMac) { if(c == ':') { c = '-'; } }
            return sMac;
        }
        return "00-00-00-00-00-00";
    }
}

std::string pan::nmos::MakeUuid(const std::string& sSeed)
{
    char sHash[33] = {0};
    mg_md5(sHash, sSeed.c_str(), nullptr);
    std::string s(sHash);
    //rfc 4122: version nibble = 3 (name based, md5), variant nibble in [89ab]
    s[12] = '3';
    const char* HEX = "0123456789abcdef";
    auto nVariant = static_cast<size_t>(strchr(HEX, s[16]) - HEX);
    s[16] = HEX[(nVariant & 0x3) | 0x8];
    return s.substr(0,8)+"-"+s.substr(8,4)+"-"+s.substr(12,4)+"-"+s.substr(16,4)+"-"+s.substr(20,12);
}

NmosNode::NmosNode(ptp::PtpClient& ptpClient, rtp::RtpSender& sender, rtp::RtpReceiver& receiver) :
    m_ptp(ptpClient),
    m_sender(sender),
    m_receiver(receiver)
{
}

NmosNode::~NmosNode()
{
    Stop();
}

std::string NmosNode::Version() const
{
    return std::to_string(time(nullptr)) + ":" + std::to_string(m_nVersion.load() % 1000000000);
}

void NmosNode::BumpVersion()
{
    m_nVersion++;
}

std::vector<std::string> NmosNode::GetHostIps() const
{
    std::vector<std::string> vIps;
    ifaddrs* pList = nullptr;
    if(getifaddrs(&pList) != 0) { return vIps; }
    for(auto* p = pList; p; p = p->ifa_next)
    {
        if(!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) { continue; }
        for(const auto& sInterface : m_vInterfaces)
        {
            if(sInterface == p->ifa_name)
            {
                char buffer[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr, buffer, sizeof(buffer));
                vIps.emplace_back(buffer);
            }
        }
    }
    freeifaddrs(pList);
    return vIps;
}

json NmosNode::BuildSelf() const
{
    auto sLabel = Config::Get().GetValue<std::string>("device.label", "pi-audio-node");
    json js;
    js["id"] = m_sNodeId;
    js["version"] = Version();
    js["label"] = sLabel;
    js["description"] = Config::Get().GetValue<std::string>("device.description", "");
    js["tags"] = json::object();
    js["hostname"] = m_sHostname;
    js["caps"] = json::object();
    js["api"]["versions"] = {"v1.3"};
    js["api"]["endpoints"] = json::array();
    js["services"] = json::array();
    js["clocks"] = json::array();
    js["interfaces"] = json::array();

    auto sGm = m_ptp.GrandmasterId();
    if(!sGm.empty())
    {
        for(auto& c : sGm) { if(c == ':') { c = '-'; } }    //IS-04 wants dashes
        js["clocks"].push_back({{"name", "clk0"}, {"ref_type", "ptp"}, {"traceable", false},
                                {"version", "IEEE1588-2008"}, {"gmid", sGm}, {"locked", m_ptp.IsSynced()}});
    }
    else
    {
        js["clocks"].push_back({{"name", "clk0"}, {"ref_type", "internal"}});
    }

    for(const auto& sIp : GetHostIps())
    {
        js["api"]["endpoints"].push_back({{"host", sIp}, {"port", m_nPort}, {"protocol", "http"}});
    }
    for(const auto& sInterface : m_vInterfaces)
    {
        js["interfaces"].push_back({{"name", sInterface}, {"chassis_id", nullptr}, {"port_id", InterfaceMac(sInterface)}});
    }
    js["href"] = js["api"]["endpoints"].empty() ? "" :
        "http://" + js["api"]["endpoints"][0]["host"].get<std::string>() + ":" + std::to_string(m_nPort);
    return js;
}

json NmosNode::BuildDevice() const
{
    json js;
    js["id"] = m_sDeviceId;
    js["version"] = Version();
    js["label"] = Config::Get().GetValue<std::string>("device.label", "pi-audio-node");
    js["description"] = "AES67 sender/receiver";
    js["tags"] = json::object();
    js["type"] = "urn:x-nmos:device:generic";
    js["node_id"] = m_sNodeId;
    js["senders"] = {m_sSenderId};
    js["receivers"] = {m_sReceiverId};
    js["controls"] = json::array();
    for(const auto& sIp : GetHostIps())
    {
        js["controls"].push_back({{"type", "urn:x-nmos:control:sr-ctrl/v1.1"},
                                  {"href", "http://"+sIp+":"+std::to_string(m_nPort)+"/x-nmos/connection/v1.1"}});
        js["controls"].push_back({{"type", "urn:x-nmos:control:ncp/v1.0"},
                                  {"href", "ws://"+sIp+":"+std::to_string(m_nPort)+"/x-nmos/ncp/v1.0"}});
    }
    return js;
}

json NmosNode::BuildSource() const
{
    json js;
    js["id"] = m_sSourceId;
    js["version"] = Version();
    js["label"] = Config::Get().GetValue<std::string>("device.label", "pi-audio-node") + " source";
    js["description"] = "";
    js["tags"] = json::object();
    js["device_id"] = m_sDeviceId;
    js["parents"] = json::array();
    js["clock_name"] = "clk0";
    js["caps"] = json::object();
    js["format"] = "urn:x-nmos:format:audio";
    js["channels"] = {{{"label", "Left"}, {"symbol", "L"}}, {{"label", "Right"}, {"symbol", "R"}}};
    return js;
}

json NmosNode::BuildFlow() const
{
    json js;
    js["id"] = m_sFlowId;
    js["version"] = Version();
    js["label"] = Config::Get().GetValue<std::string>("device.label", "pi-audio-node") + " flow";
    js["description"] = "";
    js["tags"] = json::object();
    js["device_id"] = m_sDeviceId;
    js["source_id"] = m_sSourceId;
    js["parents"] = json::array();
    js["format"] = "urn:x-nmos:format:audio";
    js["media_type"] = "audio/L24";
    js["sample_rate"] = {{"numerator", 48000}};
    js["bit_depth"] = 24;
    return js;
}

json NmosNode::BuildSender() const
{
    json js;
    js["id"] = m_sSenderId;
    js["version"] = Version();
    js["label"] = Config::Get().GetValue<std::string>("device.label", "pi-audio-node") + " sender";
    js["description"] = "";
    js["tags"] = json::object();
    js["flow_id"] = m_sFlowId;
    js["transport"] = "urn:x-nmos:transport:rtp.mcast";
    js["device_id"] = m_sDeviceId;
    js["interface_bindings"] = m_vInterfaces;
    auto vIps = GetHostIps();
    js["manifest_href"] = vIps.empty() ? "" :
        "http://"+vIps[0]+":"+std::to_string(m_nPort)+"/x-nmos/connection/v1.1/single/senders/"+m_sSenderId+"/transportfile";
    js["subscription"] = {{"receiver_id", nullptr}, {"active", m_sender.IsRunning()}};
    return js;
}

json NmosNode::BuildReceiver() const
{
    json js;
    js["id"] = m_sReceiverId;
    js["version"] = Version();
    js["label"] = Config::Get().GetValue<std::string>("device.label", "pi-audio-node") + " receiver";
    js["description"] = "";
    js["tags"] = json::object();
    js["device_id"] = m_sDeviceId;
    js["transport"] = "urn:x-nmos:transport:rtp.mcast";
    js["interface_bindings"] = m_vInterfaces;
    js["format"] = "urn:x-nmos:format:audio";
    js["caps"]["media_types"] = {"audio/L24", "audio/L16"};
    js["subscription"] = {{"sender_id", m_pConnection ? m_pConnection->ReceiverSenderId() : json()},
                          {"active", m_receiver.IsRunning()}};
    return js;
}

bool NmosNode::Start(int nPort, const std::vector<std::string>& vInterfaces, Callbacks callbacks)
{
    Stop();
    m_nPort = nPort;
    m_vInterfaces = vInterfaces;

    char sHostname[128] = {0};
    gethostname(sHostname, sizeof(sHostname)-1);
    m_sHostname = sHostname;

    m_sNodeId = MakeUuid(m_sHostname + ":pan:node");
    m_sDeviceId = MakeUuid(m_sHostname + ":pan:device");
    m_sSourceId = MakeUuid(m_sHostname + ":pan:source");
    m_sFlowId = MakeUuid(m_sHostname + ":pan:flow");
    m_sSenderId = MakeUuid(m_sHostname + ":pan:sender");
    m_sReceiverId = MakeUuid(m_sHostname + ":pan:receiver");

    std::string sPort = std::to_string(nPort);
    const char* options[] = {
        "listening_ports", sPort.c_str(),
        "num_threads", "6",
        "enable_directory_listing", "no",
        nullptr
    };
    m_pServer = mg_start(nullptr, nullptr, options);
    if(!m_pServer)
    {
        LOG_ERROR("nmos") << "cannot start http server on port " << nPort;
        return false;
    }

    m_pConnection = std::make_unique<ConnectionApi>(*this, std::move(callbacks.onReceiver), std::move(callbacks.onSender));
    m_pConnection->Register(m_pServer);
    m_pIs12 = std::make_unique<Is12Server>(*this);
    m_pIs12->Register(m_pServer);
    RegisterNodeApi();

    m_bRun = true;
    m_regThread = std::thread(&NmosNode::RegistrationLoop, this);
    LOG_INFO("nmos") << "node " << m_sNodeId << " on port " << nPort;
    return true;
}

void NmosNode::Stop()
{
    m_bRun = false;
    if(m_regThread.joinable()) { m_regThread.join(); }
    if(m_pServer)
    {
        mg_stop(m_pServer);
        m_pServer = nullptr;
    }
    m_pConnection.reset();
    m_pIs12.reset();
}

bool NmosNode::RegisterAll(const std::string& sRegistryUrl)
{
    auto Post = [&](const std::string& sType, const json& jsData) -> bool
    {
        json jsBody = {{"type", sType}, {"data", jsData}};
        int nStatus = 0;
        auto jsResponse = HttpJson("POST", sRegistryUrl + "/x-nmos/registration/v1.3/resource", &jsBody, nStatus);
        if(nStatus == 200 || nStatus == 201) { return true; }
        LOG_WARN("nmos") << "register " << sType << " -> " << nStatus
                         << (jsResponse ? " "+jsResponse->dump().substr(0, 300) : "");
        return false;
    };

    return Post("node", BuildSelf()) && Post("device", BuildDevice()) && Post("source", BuildSource())
        && Post("flow", BuildFlow()) && Post("sender", BuildSender()) && Post("receiver", BuildReceiver());
}

void NmosNode::RegistrationLoop()
{
    int nSinceDiscovery = 999;
    std::vector<RegistryEntry> vRegistries;
    bool bRegistered = false;

    while(m_bRun)
    {
        auto sOverride = Config::Get().GetValue<std::string>("nmos.registry_override", "");

        if(!bRegistered)
        {
            if(!sOverride.empty())
            {
                vRegistries = {{sOverride, 0, ""}};
            }
            else if(++nSinceDiscovery >= 5)
            {
                vRegistries = DiscoverRegistries();
                nSinceDiscovery = 0;
            }

            for(const auto& registry : vRegistries)
            {
                if(RegisterAll(registry.sUrl))
                {
                    std::lock_guard<std::mutex> lg(m_mutex);
                    m_sRegistry = registry.sUrl;
                    m_sRegistryStatus = "registered";
                    bRegistered = true;
                    m_nRegisteredVersion = m_nVersion.load();
                    LOG_INFO("nmos") << "registered with " << registry.sUrl;
                    break;
                }
            }
            if(!bRegistered)
            {
                std::lock_guard<std::mutex> lg(m_mutex);
                m_sRegistry.clear();
                m_sRegistryStatus = vRegistries.empty() ? "no registry found" : "registration failed";
            }
        }
        else
        {
            //heartbeat + push changed resources
            std::string sRegistry;
            {
                std::lock_guard<std::mutex> lg(m_mutex);
                sRegistry = m_sRegistry;
            }
            int nStatus = 0;
            json jsEmpty = json::object();
            HttpJson("POST", sRegistry + "/x-nmos/registration/v1.3/health/nodes/" + m_sNodeId, &jsEmpty, nStatus);
            if(nStatus == 404)
            {
                bRegistered = RegisterAll(sRegistry);   //registry lost us - re-register
            }
            else if(nStatus < 200 || nStatus >= 300)
            {
                LOG_WARN("nmos") << "heartbeat -> " << nStatus << " - rediscovering";
                bRegistered = false;
                nSinceDiscovery = 999;
            }
            else if(m_nVersion.load() != m_nRegisteredVersion.load())
            {
                if(RegisterAll(sRegistry)) { m_nRegisteredVersion = m_nVersion.load(); }
            }
        }

        for(int i = 0; i < 25 && m_bRun; i++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(bRegistered ? 200 : 80));
        }
    }

    //unregister on shutdown
    std::string sRegistry;
    {
        std::lock_guard<std::mutex> lg(m_mutex);
        sRegistry = m_sRegistry;
    }
    if(!sRegistry.empty())
    {
        int nStatus = 0;
        HttpJson("DELETE", sRegistry + "/x-nmos/registration/v1.3/resource/nodes/" + m_sNodeId, nullptr, nStatus);
    }
}

namespace
{
    void SendJson(mg_connection* pConn, int nStatus, const json& js)
    {
        auto sBody = js.dump();
        mg_printf(pConn, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                         "Access-Control-Allow-Origin: *\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                  nStatus, nStatus < 300 ? "OK" : "Error", sBody.size());
        mg_write(pConn, sBody.data(), sBody.size());
    }
}

void NmosNode::RegisterNodeApi()
{
    //IS-04 node api (read only) - one handler for the whole subtree
    mg_set_request_handler(m_pServer, "/x-nmos/node", [](mg_connection* pConn, void* pUser) -> int
    {
        auto* pNode = static_cast<NmosNode*>(pUser);
        std::string sUri(mg_get_request_info(pConn)->local_uri);
        if(!sUri.empty() && sUri.back() == '/') { sUri.pop_back(); }

        auto Collection = [&](const json& jsItem) { return json::array({jsItem}); };

        if(sUri == "/x-nmos/node")                    { SendJson(pConn, 200, json::array({"v1.3/"})); }
        else if(sUri == "/x-nmos/node/v1.3")          { SendJson(pConn, 200, json::array({"self/", "devices/", "sources/", "flows/", "senders/", "receivers/", "subscriptions/"})); }
        else if(sUri == "/x-nmos/node/v1.3/self")     { SendJson(pConn, 200, pNode->BuildSelf()); }
        else if(sUri == "/x-nmos/node/v1.3/devices")  { SendJson(pConn, 200, Collection(pNode->BuildDevice())); }
        else if(sUri == "/x-nmos/node/v1.3/devices/"+pNode->DeviceId())     { SendJson(pConn, 200, pNode->BuildDevice()); }
        else if(sUri == "/x-nmos/node/v1.3/sources")  { SendJson(pConn, 200, Collection(pNode->BuildSource())); }
        else if(sUri == "/x-nmos/node/v1.3/flows")    { SendJson(pConn, 200, Collection(pNode->BuildFlow())); }
        else if(sUri == "/x-nmos/node/v1.3/senders")  { SendJson(pConn, 200, Collection(pNode->BuildSender())); }
        else if(sUri == "/x-nmos/node/v1.3/senders/"+pNode->SenderId())     { SendJson(pConn, 200, pNode->BuildSender()); }
        else if(sUri == "/x-nmos/node/v1.3/receivers"){ SendJson(pConn, 200, Collection(pNode->BuildReceiver())); }
        else if(sUri == "/x-nmos/node/v1.3/receivers/"+pNode->ReceiverId()) { SendJson(pConn, 200, pNode->BuildReceiver()); }
        else                                          { SendJson(pConn, 404, {{"code", 404}, {"error", "not found"}}); }
        return 200;
    }, this);

    mg_set_request_handler(m_pServer, "/x-nmos$", [](mg_connection* pConn, void*) -> int
    {
        SendJson(pConn, 200, json::array({"node/", "connection/"}));
        return 200;
    }, this);
}

json NmosNode::GetStatusJson() const
{
    std::lock_guard<std::mutex> lg(m_mutex);
    return {
        {"node_id", m_sNodeId},
        {"receiver_id", m_sReceiverId},
        {"sender_id", m_sSenderId},
        {"registry", m_sRegistry},
        {"status", m_sRegistryStatus},
        {"port", m_nPort}
    };
}
