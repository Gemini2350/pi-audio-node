#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include "audio/alsaout.h"
#include "audio/sources.h"
#include "config.h"
#include "log.h"
#include "nmos/connection.h"
#include "nmos/is12.h"
#include "nmos/node.h"
#include "ptp/ptpclient.h"
#include "rtp/rtpreceiver.h"
#include "rtp/rtpsender.h"
#include "web/server.h"

using namespace pan;
using json = nlohmann::json;

namespace
{
    volatile sig_atomic_t g_bStop = 0;
    void OnSignal(int) { g_bStop = 1; }

    std::vector<std::string> Interfaces()
    {
        std::vector<std::string> vInterfaces{Config::Get().GetValue<std::string>("network.interface_primary", "eth0")};
        auto sSecondary = Config::Get().GetValue<std::string>("network.interface_secondary", "");
        if(!sSecondary.empty() && sSecondary != vInterfaces[0]) { vInterfaces.push_back(sSecondary); }
        return vInterfaces;
    }

    std::string RunCommand(const std::string& sCmd)
    {
        std::string sOut;
        FILE* pPipe = popen((sCmd + " 2>&1").c_str(), "r");
        if(!pPipe) { return sOut; }
        char buffer[512];
        while(fgets(buffer, sizeof(buffer), pPipe)) { sOut += buffer; }
        pclose(pPipe);
        return sOut;
    }

    //nmcli terse output is KEY:value per line (IP4.ADDRESS shows as IP4.ADDRESS[1])
    std::string NmcliField(const std::string& sOutput, const std::string& sKey)
    {
        std::istringstream stream(sOutput);
        std::string sLine;
        while(std::getline(stream, sLine))
        {
            if(sLine.rfind(sKey, 0) == 0)
            {
                auto nColon = sLine.find(':');
                if(nColon != std::string::npos) { return sLine.substr(nColon+1); }
            }
        }
        return "";
    }

    bool ValidIp4(const std::string& s, bool bWithPrefix)
    {
        int nA, nB, nC, nD, nP;
        char cEnd;
        if(bWithPrefix)
        {
            if(sscanf(s.c_str(), "%d.%d.%d.%d/%d%c", &nA, &nB, &nC, &nD, &nP, &cEnd) != 5) { return false; }
            if(nP < 1 || nP > 32) { return false; }
        }
        else if(sscanf(s.c_str(), "%d.%d.%d.%d%c", &nA, &nB, &nC, &nD, &cEnd) != 4) { return false; }
        return nA >= 0 && nA <= 255 && nB >= 0 && nB <= 255 && nC >= 0 && nC <= 255 && nD >= 0 && nD <= 255;
    }

    bool SafeShellArg(const std::string& s)
    {
        return !s.empty() && s.find('\'') == std::string::npos && s.find('\n') == std::string::npos;
    }

    void ApplySender(rtp::RtpSender& sender, const std::string& sFilesDir)
    {
        auto js = Config::Get().GetJson()["sender"];
        if(!js.value("enabled", false))
        {
            sender.Stop();
            return;
        }
        auto vInterfaces = Interfaces();
        std::vector<rtp::RtpSender::Leg> vLegs{{vInterfaces[0], js.value("multicast_primary", "239.69.145.10"),
                                                js.value("leg1_enabled", true)}};
        if(vInterfaces.size() > 1)
        {
            vLegs.push_back({vInterfaces[1], js.value("multicast_secondary", "239.69.146.10"),
                             js.value("leg2_enabled", true)});
        }
        sender.Configure(vLegs, static_cast<uint16_t>(js.value("port", 5004)), js.value("payload_type", 96),
                         js.value("packet_time_us", 1000), audio::CreateFromConfig(js, sFilesDir));
    }
}

int main(int argc, char** argv)
{
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);
    signal(SIGPIPE, SIG_IGN);

    std::string sConfig = argc > 1 ? argv[1] : std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/pi-audio-node.json";
    Config::Get().Load(sConfig);
    LOG_INFO("main") << "pi-audio-node " << APP_VERSION << " config " << sConfig;

    auto sFilesDir = Config::Get().GetValue<std::string>("web.audio_files_path", "");
    if(sFilesDir.empty())
    {
        sFilesDir = std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/audio-files";
    }
    mkdir(sFilesDir.c_str(), 0755);

    //---- engine ----
    ptp::PtpClient ptpClient;
    ptpClient.Run(Interfaces(), Config::Get().GetValue<int>("ptp.domain", 0));

    audio::AlsaOut alsa;
    alsa.Open(Config::Get().GetValue<std::string>("receiver.alsa_device", "default"));

    rtp::RtpSender sender(ptpClient);
    rtp::RtpReceiver receiver(ptpClient, alsa);
    ApplySender(sender, sFilesDir);

    //---- nmos ----
    nmos::NmosNode node(ptpClient, sender, receiver);
    nmos::NmosNode::Callbacks callbacks;
    callbacks.onReceiver = [&](bool bEnable, const std::optional<rtp::SdpSession>& session)
    {
        if(bEnable && session)
        {
            receiver.Configure(*session, Interfaces(),
                               Config::Get().GetValue<int>("receiver.playout_delay_ms", 20),
                               [](){ return Config::Get().GetValue<double>("receiver.gain_db", 0.0); });
        }
        else
        {
            receiver.Stop();
        }
    };
    callbacks.onSender = [&](bool bEnable)
    {
        Config::Get().SetValue("sender.enabled", bEnable);
        ApplySender(sender, sFilesDir);
        node.BumpVersion();
    };
    if(Config::Get().GetValue<bool>("nmos.enabled", true))
    {
        node.Start(Config::Get().GetValue<int>("nmos.http_port", 8080), Interfaces(), callbacks);
    }

    //---- web ui ----
    auto sWebRoot = Config::Get().GetValue<std::string>("web.webui_path", "webui");
    web::WebServer web;
    auto StatusJson = [&]() -> json
    {
        json js;
        js["version"] = APP_VERSION;
        js["build"] = __DATE__ " " __TIME__;    //ui reloads itself when this changes
        js["ptp"] = ptpClient.GetStatusJson();
        js["sender"] = sender.GetStatusJson();
        js["receiver"] = receiver.GetStatusJson();
        js["receiver"]["gain_db"] = Config::Get().GetValue<double>("receiver.gain_db", 0.0);
        js["config"] = Config::Get().GetJson();     //ui forms follow is-05/config changes
        js["nmos"] = node.GetStatusJson();
        js["alsa"] = {{"underruns", alsa.Underruns()}, {"delay_frames", alsa.DelayFrames()}};
        if(auto* pIs12 = node.GetIs12()) { js["monitors"] = pIs12->GetMonitorJson(); }
        return js;
    };
    auto Action = [&](const std::string& sAction, const json& jsBody) -> json
    {
        int nPort = Config::Get().GetValue<int>("nmos.http_port", 8080);
        auto sBase = "http://127.0.0.1:" + std::to_string(nPort) +
                     "/x-nmos/connection/v1.1/single/receivers/" + node.ReceiverId() + "/staged";
        int nStatus = 0;
        if(sAction == "receiver-connect")
        {
            json jsPatch = {{"master_enable", true}, {"activation", {{"mode", "activate_immediate"}}},
                            {"transport_file", {{"type", "application/sdp"}, {"data", jsBody.value("sdp", "")}}}};
            nmos::HttpJson("PATCH", sBase, &jsPatch, nStatus);
        }
        else if(sAction == "receiver-disconnect")
        {
            json jsPatch = {{"master_enable", false}, {"activation", {{"mode", "activate_immediate"}}}};
            nmos::HttpJson("PATCH", sBase, &jsPatch, nStatus);
        }
        else if(sAction == "sender-sdp")
        {
            auto session = sender.DescribeSession();
            if(session.vLegs.empty()) { return {{"error", "sender is not running"}}; }
            auto vIps = sender.GetSourceIps();
            return {{"sdp", rtp::GenerateSdp(session, vIps.empty() ? "0.0.0.0" : vIps[0], vIps)}};
        }
        else if(sAction == "nmos-senders")
        {
            //browse every audio sender the registry knows via the is-04 query api
            auto sRegistry = node.GetStatusJson().value("registry", "");
            if(sRegistry.empty()) { return {{"error", "not registered with a registry"}}; }

            auto jsSenders = nmos::HttpJson("GET", sRegistry + "/x-nmos/query/v1.3/senders", nullptr, nStatus);
            if(!jsSenders || !jsSenders->is_array())
            {
                return {{"error", "query api not reachable (status " + std::to_string(nStatus) + ")"}};
            }
            auto jsFlows = nmos::HttpJson("GET", sRegistry + "/x-nmos/query/v1.3/flows", nullptr, nStatus);
            auto jsDevices = nmos::HttpJson("GET", sRegistry + "/x-nmos/query/v1.3/devices", nullptr, nStatus);

            auto Find = [](const std::optional<json>& jsList, const std::string& sId) -> json
            {
                if(!jsList || !jsList->is_array() || sId.empty()) { return json::object(); }
                for(const auto& js : *jsList)
                {
                    if(js.value("id", "") == sId) { return js; }
                }
                return json::object();
            };
            auto StringOr = [](const json& js, const char* sKey) -> std::string
            {
                return js.contains(sKey) && js[sKey].is_string() ? js[sKey].get<std::string>() : "";
            };

            json jsList = json::array();
            for(const auto& jsSender : *jsSenders)
            {
                auto jsFlow = Find(jsFlows, StringOr(jsSender, "flow_id"));
                auto sFormat = jsFlow.value("format", "");
                if(!sFormat.empty() && sFormat != "urn:x-nmos:format:audio") { continue; }
                jsList.push_back({
                    {"id", jsSender.value("id", "")},
                    {"label", jsSender.value("label", "")},
                    {"device", Find(jsDevices, StringOr(jsSender, "device_id")).value("label", "")},
                    {"media_type", jsFlow.value("media_type", "")},
                    {"manifest_href", StringOr(jsSender, "manifest_href")},
                    {"is_self", jsSender.value("id", "") == node.SenderId()}
                });
            }
            return {{"senders", jsList}};
        }
        else if(sAction == "nmos-connect")
        {
            auto sSenderId = jsBody.value("sender_id", "");
            auto sManifest = jsBody.value("manifest_href", "");
            if(sManifest.rfind("http://", 0) != 0) { return {{"error", "sender has no usable manifest url"}}; }

            auto sSdp = nmos::HttpText("GET", sManifest, nullptr, nStatus);
            if(!sSdp || nStatus != 200 || sSdp->find("m=audio") == std::string::npos)
            {
                return {{"error", "could not fetch sdp from sender (status " + std::to_string(nStatus) + ")"}};
            }
            json jsPatch = {{"master_enable", true}, {"sender_id", sSenderId},
                            {"activation", {{"mode", "activate_immediate"}}},
                            {"transport_file", {{"type", "application/sdp"}, {"data", *sSdp}}}};
            nmos::HttpJson("PATCH", sBase, &jsPatch, nStatus);
            return {{"status", nStatus}};
        }
        else if(sAction == "network-status")
        {
            json jsIfs = json::array();
            auto vInterfaces = Interfaces();
            const char* ROLES[] = {"amber", "blue"};
            for(size_t nRole = 0; nRole < vInterfaces.size() && nRole < 2; nRole++)
            {
                const auto& sIf = vInterfaces[nRole];
                if(!SafeShellArg(sIf)) { continue; }
                auto sInfo = RunCommand("nmcli -t -f GENERAL.CONNECTION,IP4.ADDRESS,IP4.GATEWAY device show '" + sIf + "'");
                auto sCon = NmcliField(sInfo, "GENERAL.CONNECTION");
                std::string sMethod;
                if(SafeShellArg(sCon))
                {
                    sMethod = NmcliField(RunCommand("nmcli -t -f ipv4.method con show '" + sCon + "'"), "ipv4.method");
                }
                int nCarrier = 0;
                std::ifstream ifs("/sys/class/net/" + sIf + "/carrier");
                ifs >> nCarrier;
                jsIfs.push_back({{"role", ROLES[nRole]}, {"interface", sIf}, {"link", nCarrier == 1},
                                 {"connection", sCon}, {"method", sMethod},
                                 {"address", NmcliField(sInfo, "IP4.ADDRESS")},
                                 {"gateway", NmcliField(sInfo, "IP4.GATEWAY")}});
            }
            return {{"interfaces", jsIfs}};
        }
        else if(sAction == "network-apply")
        {
            auto vInterfaces = Interfaces();
            auto sIf = jsBody.value("interface", "");
            auto sMethod = jsBody.value("method", "");
            auto sAddress = jsBody.value("address", "");
            auto sGateway = jsBody.value("gateway", "");

            size_t nRole = 0;
            while(nRole < vInterfaces.size() && vInterfaces[nRole] != sIf) { nRole++; }
            if(nRole >= vInterfaces.size() || nRole >= 2 || !SafeShellArg(sIf)) { return {{"error", "unknown interface"}}; }
            if(sMethod != "dhcp" && sMethod != "static") { return {{"error", "mode must be dhcp or static"}}; }
            if(sMethod == "static" && !ValidIp4(sAddress, true)) { return {{"error", "address must be a.b.c.d/prefix"}}; }
            if(!sGateway.empty() && !ValidIp4(sGateway, false)) { return {{"error", "bad gateway"}}; }

            //reuse the profile currently on the device, else create a role-named one
            auto sCon = NmcliField(RunCommand("nmcli -t -f GENERAL.CONNECTION device show '" + sIf + "'"),
                                   "GENERAL.CONNECTION");
            if(!SafeShellArg(sCon))
            {
                sCon = nRole == 0 ? "amber" : "blue";
                RunCommand("sudo -n nmcli con add type ethernet ifname '" + sIf + "' con-name '" + sCon + "'");
            }
            std::string sMod = "sudo -n nmcli con mod '" + sCon + "' ";
            if(sMethod == "dhcp") { sMod += "ipv4.method auto ipv4.addresses '' ipv4.gateway ''"; }
            else                  { sMod += "ipv4.method manual ipv4.addresses '" + sAddress + "' ipv4.gateway '" + sGateway + "'"; }
            auto sResult = RunCommand(sMod);
            if(sResult.find("Error") != std::string::npos) { return {{"error", sResult}}; }

            //activate detached - if this is the requester's own subnet the reply
            //may not make it back, the ui reloads the state afterwards anyway
            LOG_INFO("web") << "network " << sIf << " (" << sCon << ") -> " << sMethod
                            << (sMethod == "static" ? " " + sAddress : "");
            (void)!system(("sudo -n nmcli con up '" + sCon + "' >/dev/null 2>&1 &").c_str());
            return {{"ok", true}};
        }
        return {{"status", nStatus}};
    };

    web.Start(Config::Get().GetValue<int>("web.port", 80), sWebRoot, sFilesDir, StatusJson,
              [&](const json& jsChanged)
              {
                  //leg toggles apply live - everything else restarts the sender
                  bool bOnlyLegToggle = !jsChanged.empty();
                  for(const auto& [sKey, jsValue] : jsChanged.items())
                  {
                      if(sKey != "sender.leg1_enabled" && sKey != "sender.leg2_enabled") { bOnlyLegToggle = false; }
                  }
                  if(bOnlyLegToggle)
                  {
                      sender.SetLegEnabled(0, Config::Get().GetValue<bool>("sender.leg1_enabled", true));
                      sender.SetLegEnabled(1, Config::Get().GetValue<bool>("sender.leg2_enabled", true));
                      node.BumpVersion();
                      return;
                  }
                  for(const auto& [sKey, jsValue] : jsChanged.items())
                  {
                      if(sKey.rfind("sender.", 0) == 0) { ApplySender(sender, sFilesDir); node.BumpVersion(); break; }
                  }
                  for(const auto& [sKey, jsValue] : jsChanged.items())
                  {
                      if(sKey == "ptp.domain") { ptpClient.SetDomain(jsValue.get<int>()); }
                      if(sKey == "receiver.alsa_device") { alsa.Open(jsValue.get<std::string>()); }
                  }
              }, Action);

    //---- bcp-008 status feed ----
    uint64_t nLastLost[2] = {0, 0};
    uint64_t nLastLate = 0;
    uint64_t nLastPlayed = 0, nLastSent = 0;
    while(!g_bStop)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto* pIs12 = node.GetIs12();
        if(!pIs12) { continue; }

        //sync domain (both monitors)
        bool bSynced = ptpClient.IsSynced();
        auto sGm = ptpClient.GrandmasterId();
        pIs12->SetSyncSource(sGm.empty() ? std::nullopt : std::optional(sGm));
        int nSync = sGm.empty() ? 0 : (bSynced ? 1 : 3);
        pIs12->UpdateReceiverDomain(nmos::Is12Server::SYNC, nSync, bSynced || sGm.empty() ? "" : "not locked to ptp");
        pIs12->UpdateSenderDomain(nmos::Is12Server::SYNC, nSync, bSynced || sGm.empty() ? "" : "not locked to ptp");

        //receiver path + stream
        auto jsRx = receiver.GetStatusJson();
        if(!receiver.IsRunning())
        {
            pIs12->UpdateReceiverDomain(nmos::Is12Server::PATH, 0, "");
            pIs12->UpdateReceiverDomain(nmos::Is12Server::STREAM, 0, "");
        }
        else if(!receiver.IsReceiving())
        {
            pIs12->UpdateReceiverDomain(nmos::Is12Server::PATH, 3, "no rtp packets arriving");
        }
        else
        {
            auto nPlayed = jsRx.value("played", 0ULL);
            if(nPlayed < nLastPlayed) { nLastPlayed = 0; }      //receiver was reconfigured
            pIs12->UpdateReceiverDomain(nmos::Is12Server::PATH, 1, "");
            pIs12->UpdateReceiverDomain(nmos::Is12Server::STREAM,
                nPlayed > nLastPlayed ? 1 : 2, nPlayed > nLastPlayed ? "" : "no audio decoded");
            nLastPlayed = nPlayed;

            //leg loss counters + link status from leg activity
            bool bPrimary = false, bSecondary = true;
            const auto& jsLegs = jsRx["legs"];
            for(size_t nLeg = 0; nLeg < jsLegs.size() && nLeg < 2; nLeg++)
            {
                auto nLost = receiver.LostPackets(nLeg);
                if(nLost < nLastLost[nLeg]) { nLastLost[nLeg] = 0; }
                if(nLost > nLastLost[nLeg])
                {
                    pIs12->AddReceiverLost(jsLegs[nLeg].value("interface", "leg"+std::to_string(nLeg)), nLost - nLastLost[nLeg]);
                    nLastLost[nLeg] = nLost;
                }
                if(nLeg == 0) { bPrimary = jsLegs[nLeg].value("active", false); }
                else          { bSecondary = jsLegs[nLeg].value("active", false); }
            }
            int nLink = (bPrimary && bSecondary) ? 1 : ((bPrimary || bSecondary) ? 2 : 3);
            pIs12->UpdateReceiverDomain(nmos::Is12Server::LINK, nLink,
                nLink == 1 ? "" : (nLink == 2 ? "one leg down" : "all legs down"));

            auto nLate = receiver.LatePackets();
            if(nLate < nLastLate) { nLastLate = 0; }
            if(nLate > nLastLate)
            {
                pIs12->AddReceiverLate(nLate - nLastLate);
                nLastLate = nLate;
            }
        }

        //sender path + essence
        auto jsTx = sender.GetStatusJson();
        if(!sender.IsRunning())
        {
            pIs12->UpdateSenderDomain(nmos::Is12Server::PATH, 0, "");
            pIs12->UpdateSenderDomain(nmos::Is12Server::STREAM, 0, "");
        }
        else if(jsTx.value("waiting_for_ptp", false))
        {
            pIs12->UpdateSenderDomain(nmos::Is12Server::PATH, 3, "waiting for ptp lock");
        }
        else
        {
            auto nSent = jsTx.value("packets_sent", 0ULL);
            if(nSent < nLastSent) { nLastSent = 0; }            //sender was reconfigured
            pIs12->UpdateSenderDomain(nmos::Is12Server::PATH, nSent > nLastSent ? 1 : 3,
                                      nSent > nLastSent ? "" : "transmission stalled");
            nLastSent = nSent;
            pIs12->UpdateSenderDomain(nmos::Is12Server::STREAM, sender.IsSendingAudio() ? 1 : 2,
                                      sender.IsSendingAudio() ? "" : "source exhausted");
            auto nErrors = jsTx.value("send_errors", 0ULL);
            static uint64_t nLastErrors = 0;
            if(nErrors < nLastErrors) { nLastErrors = 0; }
            if(nErrors > nLastErrors)
            {
                pIs12->AddSenderErrors("socket_errors", nErrors - nLastErrors);
                nLastErrors = nErrors;
            }
        }
    }

    LOG_INFO("main") << "shutting down";
    web.Stop();
    node.Stop();
    sender.Stop();
    receiver.Stop();
    ptpClient.Stop();
    return 0;
}
