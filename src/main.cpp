#include <csignal>
#include <cstdlib>
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
    ptpClient.Run(Config::Get().GetValue<std::string>("network.interface_primary", "eth0"),
                  Config::Get().GetValue<int>("ptp.domain", 0));

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
        js["ptp"] = ptpClient.GetStatusJson();
        js["sender"] = sender.GetStatusJson();
        js["receiver"] = receiver.GetStatusJson();
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
            pIs12->UpdateSenderDomain(nmos::Is12Server::PATH, nSent > nLastSent ? 1 : 3,
                                      nSent > nLastSent ? "" : "transmission stalled");
            nLastSent = nSent;
            pIs12->UpdateSenderDomain(nmos::Is12Server::STREAM, sender.IsSendingAudio() ? 1 : 2,
                                      sender.IsSendingAudio() ? "" : "source exhausted");
            auto nErrors = jsTx.value("send_errors", 0ULL);
            static uint64_t nLastErrors = 0;
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
