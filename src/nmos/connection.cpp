#include "connection.h"
#include <chrono>
#include "civetweb.h"
#include "config.h"
#include "log.h"
#include "node.h"
#include "rtp/rtpreceiver.h"
#include "rtp/rtpsender.h"

using namespace pan::nmos;
using json = nlohmann::json;

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

    void SendText(mg_connection* pConn, int nStatus, const std::string& sType, const std::string& sBody)
    {
        mg_printf(pConn, "HTTP/1.1 %d OK\r\nContent-Type: %s\r\n"
                         "Access-Control-Allow-Origin: *\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                  nStatus, sType.c_str(), sBody.size());
        mg_write(pConn, sBody.data(), sBody.size());
    }

    std::string ReadBody(mg_connection* pConn)
    {
        std::string sBody;
        char buffer[4096];
        int nRead;
        while((nRead = mg_read(pConn, buffer, sizeof(buffer))) > 0)
        {
            sBody.append(buffer, static_cast<size_t>(nRead));
        }
        return sBody;
    }

    json ActivationDone()
    {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        auto nSec = std::chrono::duration_cast<std::chrono::seconds>(now).count();
        auto nNsec = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() % 1000000000;
        return {{"mode", nullptr}, {"requested_time", nullptr},
                {"activation_time", std::to_string(nSec)+":"+std::to_string(nNsec)}};
    }
}

ConnectionApi::ConnectionApi(NmosNode& node, ReceiverCallback onReceiver, SenderCallback onSender) :
    m_node(node),
    m_onReceiver(std::move(onReceiver)),
    m_onSender(std::move(onSender))
{
    json jsLeg = {{"source_ip", nullptr}, {"destination_ip", nullptr}, {"destination_port", 5004},
                  {"rtp_enabled", true}};
    json jsRxLeg = {{"source_ip", nullptr}, {"interface_ip", "auto"}, {"destination_port", 5004},
                    {"multicast_ip", nullptr}, {"rtp_enabled", true}};

    m_jsSenderStaged = {{"master_enable", false}, {"receiver_id", nullptr},
                        {"activation", {{"mode", nullptr}, {"requested_time", nullptr}, {"activation_time", nullptr}}},
                        {"transport_params", {jsLeg, jsLeg}}};
    m_jsSenderActive = m_jsSenderStaged;
    m_jsReceiverStaged = {{"master_enable", false}, {"sender_id", nullptr},
                          {"transport_file", {{"data", nullptr}, {"type", nullptr}}},
                          {"activation", {{"mode", nullptr}, {"requested_time", nullptr}, {"activation_time", nullptr}}},
                          {"transport_params", {jsRxLeg, jsRxLeg}}};
    m_jsReceiverActive = m_jsReceiverStaged;
}

json ConnectionApi::ReceiverSenderId() const
{
    std::lock_guard<std::mutex> lg(m_mutex);
    //without master_enable there is no subscription - a deactivation that
    //leaves sender_id staged must not report as connected
    if(!m_jsReceiverActive.value("master_enable", false)) { return json(); }
    return m_jsReceiverActive.value("sender_id", json());
}

void ConnectionApi::Register(mg_context* pServer)
{
    mg_set_request_handler(pServer, "/x-nmos/connection", [](mg_connection* pConn, void* pUser) -> int
    {
        return static_cast<ConnectionApi*>(pUser)->Handle(pConn);
    }, this);
}

json ConnectionApi::BuildConstraints(size_t nLegs) const
{
    json js = json::array();
    for(size_t i = 0; i < nLegs; i++) { js.push_back(json::object()); }
    return js;
}

int ConnectionApi::Handle(mg_connection* pConn)
{
    const auto* pInfo = mg_get_request_info(pConn);
    std::string sUri(pInfo->local_uri);
    std::string sMethod(pInfo->request_method);
    if(!sUri.empty() && sUri.back() == '/') { sUri.pop_back(); }

    const std::string sBase = "/x-nmos/connection/v1.1/single";
    const std::string sSender = sBase + "/senders/" + m_node.SenderId();
    const std::string sReceiver = sBase + "/receivers/" + m_node.ReceiverId();

    if(sUri == "/x-nmos/connection")                { SendJson(pConn, 200, json::array({"v1.1/"})); return 200; }
    if(sUri == "/x-nmos/connection/v1.1")           { SendJson(pConn, 200, json::array({"bulk/", "single/"})); return 200; }
    if(sUri == sBase)                               { SendJson(pConn, 200, json::array({"senders/", "receivers/"})); return 200; }
    if(sUri == sBase + "/senders")                  { SendJson(pConn, 200, json::array({m_node.SenderId()+"/"})); return 200; }
    if(sUri == sBase + "/receivers")                { SendJson(pConn, 200, json::array({m_node.ReceiverId()+"/"})); return 200; }

    std::lock_guard<std::mutex> lg(m_mutex);

    if(sUri == sSender)                  { SendJson(pConn, 200, json::array({"constraints/", "staged/", "active/", "transporttype/", "transportfile/"})); }
    else if(sUri == sSender+"/constraints")  { SendJson(pConn, 200, BuildConstraints(m_jsSenderStaged["transport_params"].size())); }
    else if(sUri == sSender+"/transporttype"){ SendJson(pConn, 200, json("urn:x-nmos:transport:rtp.mcast")); }
    else if(sUri == sSender+"/active")       { SendJson(pConn, 200, SenderActiveNow()); }
    else if(sUri == sSender+"/transportfile")
    {
        auto session = m_node.Sender().DescribeSession();
        if(session.vLegs.empty()) { SendJson(pConn, 404, {{"code", 404}, {"error", "sender not configured"}}); }
        else
        {
            auto vIps = m_node.Sender().GetSourceIps();
            SendText(pConn, 200, "application/sdp", rtp::GenerateSdp(session, vIps.empty() ? "0.0.0.0" : vIps[0], vIps));
        }
    }
    else if(sUri == sSender+"/staged")
    {
        if(sMethod == "PATCH")
        {
            try { PatchSender(json::parse(ReadBody(pConn))); SendJson(pConn, 200, m_jsSenderStaged); }
            catch(const std::exception& e) { SendJson(pConn, 400, {{"code", 400}, {"error", e.what()}}); }
        }
        else { SendJson(pConn, 200, m_jsSenderStaged); }
    }
    else if(sUri == sReceiver)               { SendJson(pConn, 200, json::array({"constraints/", "staged/", "active/", "transporttype/"})); }
    else if(sUri == sReceiver+"/constraints"){ SendJson(pConn, 200, BuildConstraints(m_jsReceiverStaged["transport_params"].size())); }
    else if(sUri == sReceiver+"/transporttype"){ SendJson(pConn, 200, json("urn:x-nmos:transport:rtp.mcast")); }
    else if(sUri == sReceiver+"/active")     { SendJson(pConn, 200, m_jsReceiverActive); }
    else if(sUri == sReceiver+"/staged")
    {
        if(sMethod == "PATCH")
        {
            try { PatchReceiver(json::parse(ReadBody(pConn))); SendJson(pConn, 200, m_jsReceiverStaged); }
            catch(const std::exception& e) { SendJson(pConn, 400, {{"code", 400}, {"error", e.what()}}); }
        }
        else { SendJson(pConn, 200, m_jsReceiverStaged); }
    }
    else { SendJson(pConn, 404, {{"code", 404}, {"error", "not found"}}); }
    return 200;
}

json ConnectionApi::SenderActiveNow() const
{
    //caller holds m_mutex. the sender can be driven from the web ui as well, so
    //the is-05 active endpoint reflects the actual engine state, not only what
    //an is-05 activation last wrote
    json js = m_jsSenderActive;
    auto& sender = m_node.Sender();
    js["master_enable"] = sender.IsRunning();

    auto jsStatus = sender.GetStatusJson();
    auto vIps = sender.GetSourceIps();
    const auto& jsLegs = jsStatus["legs"];
    auto& jsParams = js["transport_params"];
    for(size_t i = 0; i < jsParams.size(); i++)
    {
        if(sender.IsRunning() && i < jsLegs.size())
        {
            jsParams[i]["source_ip"] = i < vIps.size() ? json(vIps[i]) : json();
            jsParams[i]["destination_ip"] = jsLegs[i]["multicast"];
            jsParams[i]["destination_port"] = jsLegs[i]["port"];
            jsParams[i]["rtp_enabled"] = sender.LegEnabled(i);
        }
        else
        {
            //not running - show the configured destinations
            jsParams[i]["destination_ip"] = Config::Get().GetValue<std::string>(
                i == 0 ? "sender.multicast_primary" : "sender.multicast_secondary", "");
            jsParams[i]["destination_port"] = Config::Get().GetValue<int>("sender.port", 5004);
            jsParams[i]["rtp_enabled"] = sender.IsRunning();
        }
    }
    return js;
}

void ConnectionApi::PatchSender(const json& jsPatch)
{
    //caller holds m_mutex
    for(const auto& sKey : {"master_enable", "receiver_id"})
    {
        if(jsPatch.contains(sKey)) { m_jsSenderStaged[sKey] = jsPatch[sKey]; }
    }
    if(jsPatch.contains("transport_params") && jsPatch["transport_params"].is_array())
    {
        auto& jsStaged = m_jsSenderStaged["transport_params"];
        for(size_t i = 0; i < jsPatch["transport_params"].size() && i < jsStaged.size(); i++)
        {
            for(const auto& [sKey, jsValue] : jsPatch["transport_params"][i].items())
            {
                jsStaged[i][sKey] = jsValue;
            }
        }
    }
    if(jsPatch.contains("activation") && jsPatch["activation"].value("mode", "") == "activate_immediate")
    {
        ActivateSender();
    }
}

void ConnectionApi::ActivateSender()
{
    //apply staged destination overrides to the config, then fire the callback
    bool bEnable = m_jsSenderStaged.value("master_enable", false);
    const auto& jsParams = m_jsSenderStaged["transport_params"];
    if(jsParams.size() > 0 && jsParams[0].value("destination_ip", json()).is_string())
    {
        Config::Get().SetValue("sender.multicast_primary", jsParams[0]["destination_ip"].get<std::string>());
    }
    if(jsParams.size() > 1 && jsParams[1].value("destination_ip", json()).is_string())
    {
        Config::Get().SetValue("sender.multicast_secondary", jsParams[1]["destination_ip"].get<std::string>());
    }
    if(jsParams.size() > 0 && jsParams[0].contains("destination_port") && jsParams[0]["destination_port"].is_number())
    {
        Config::Get().SetValue("sender.port", jsParams[0]["destination_port"].get<int>());
    }
    for(size_t i = 0; i < jsParams.size() && i < 2; i++)
    {
        if(jsParams[i].contains("rtp_enabled") && jsParams[i]["rtp_enabled"].is_boolean())
        {
            Config::Get().SetValue(i == 0 ? "sender.leg1_enabled" : "sender.leg2_enabled",
                                   jsParams[i]["rtp_enabled"].get<bool>());
            m_node.Sender().SetLegEnabled(i, jsParams[i]["rtp_enabled"].get<bool>());
        }
    }

    m_jsSenderActive = m_jsSenderStaged;
    m_jsSenderActive["activation"] = ActivationDone();
    m_jsSenderStaged["activation"] = {{"mode", nullptr}, {"requested_time", nullptr}, {"activation_time", nullptr}};

    //actualize
    auto vIps = m_node.GetHostIps();
    auto& jsActive = m_jsSenderActive["transport_params"];
    for(size_t i = 0; i < jsActive.size(); i++)
    {
        if(i < vIps.size()) { jsActive[i]["source_ip"] = vIps[i]; }
        if(!jsActive[i]["destination_ip"].is_string())
        {
            jsActive[i]["destination_ip"] = Config::Get().GetValue<std::string>(
                i == 0 ? "sender.multicast_primary" : "sender.multicast_secondary", "");
        }
    }

    LOG_INFO("is05") << "sender activation master_enable=" << bEnable;
    if(m_onSender) { m_onSender(bEnable); }
    m_node.BumpVersion();
}

void ConnectionApi::PatchReceiver(const json& jsPatch)
{
    //caller holds m_mutex
    for(const auto& sKey : {"master_enable", "sender_id"})
    {
        if(jsPatch.contains(sKey)) { m_jsReceiverStaged[sKey] = jsPatch[sKey]; }
    }
    if(jsPatch.contains("transport_file") && jsPatch["transport_file"].is_object())
    {
        for(const auto& [sKey, jsValue] : jsPatch["transport_file"].items())
        {
            m_jsReceiverStaged["transport_file"][sKey] = jsValue;
        }
    }
    if(jsPatch.contains("transport_params") && jsPatch["transport_params"].is_array())
    {
        auto& jsStaged = m_jsReceiverStaged["transport_params"];
        for(size_t i = 0; i < jsPatch["transport_params"].size() && i < jsStaged.size(); i++)
        {
            for(const auto& [sKey, jsValue] : jsPatch["transport_params"][i].items())
            {
                jsStaged[i][sKey] = jsValue;
            }
        }
    }
    if(jsPatch.contains("activation") && jsPatch["activation"].value("mode", "") == "activate_immediate")
    {
        ActivateReceiver();
    }
}

void ConnectionApi::ActivateReceiver()
{
    bool bEnable = m_jsReceiverStaged.value("master_enable", false);
    m_jsReceiverActive = m_jsReceiverStaged;
    m_jsReceiverActive["activation"] = ActivationDone();
    m_jsReceiverStaged["activation"] = {{"mode", nullptr}, {"requested_time", nullptr}, {"activation_time", nullptr}};

    auto vIps = m_node.GetHostIps();
    auto& jsActive = m_jsReceiverActive["transport_params"];
    for(size_t i = 0; i < jsActive.size(); i++)
    {
        if(jsActive[i].value("interface_ip", "auto") == "auto" && i < vIps.size())
        {
            jsActive[i]["interface_ip"] = vIps[i];
        }
    }

    std::optional<rtp::SdpSession> session;
    auto jsFile = m_jsReceiverActive["transport_file"];
    if(bEnable && jsFile.value("data", json()).is_string())
    {
        session = rtp::ParseSdp(jsFile["data"].get<std::string>());
        if(!session) { LOG_WARN("is05") << "receiver activation with unparseable sdp"; }
    }
    else if(bEnable)
    {
        //no transport file - build a session from transport params
        rtp::SdpSession manual;
        manual.sName = "is-05 parameters";
        for(const auto& jsLeg : jsActive)
        {
            if(jsLeg.value("rtp_enabled", true) && jsLeg.value("multicast_ip", json()).is_string())
            {
                manual.vLegs.push_back({"", jsLeg["multicast_ip"].get<std::string>(), "",
                                        static_cast<uint16_t>(jsLeg.value("destination_port", 5004))});
            }
        }
        if(!manual.vLegs.empty()) { session = manual; }
    }

    LOG_INFO("is05") << "receiver activation master_enable=" << bEnable
                     << (session ? " legs="+std::to_string(session->vLegs.size()) : "");
    if(m_onReceiver) { m_onReceiver(bEnable, session); }
    m_node.BumpVersion();
}
