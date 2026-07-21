#include "rtpsender.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <random>
#include <sys/socket.h>
#include <unistd.h>
#include "config.h"
#include "log.h"
#include "ptp/ptpclient.h"

using namespace pan::rtp;
using json = nlohmann::json;

namespace
{
    constexpr int SAMPLE_RATE = 48000;
    constexpr int CHANNELS = 2;
    constexpr int BYTES_PER_SAMPLE = 3;     //L24

    std::string InterfaceIp(const std::string& sInterface)
    {
        ifaddrs* pList = nullptr;
        if(getifaddrs(&pList) != 0) { return ""; }
        std::string sIp;
        for(auto* p = pList; p; p = p->ifa_next)
        {
            if(p->ifa_addr && p->ifa_addr->sa_family == AF_INET && sInterface == p->ifa_name)
            {
                char buffer[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr, buffer, sizeof(buffer));
                sIp = buffer;
                break;
            }
        }
        freeifaddrs(pList);
        return sIp;
    }
}

RtpSender::~RtpSender()
{
    Stop();
}

void RtpSender::Stop()
{
    m_bRun = false;
    if(m_thread.joinable()) { m_thread.join(); }
    std::lock_guard<std::mutex> lg(m_mutex);
    for(int nSocket : m_vSockets) { close(nSocket); }
    m_vSockets.clear();
    m_bEssenceOk = false;
}

bool RtpSender::Configure(const std::vector<Leg>& vLegs, uint16_t nPort, int nPayloadType,
                          int nPacketTimeUs, std::unique_ptr<audio::Source> pSource)
{
    Stop();
    if(vLegs.empty() || !pSource) { return false; }

    std::lock_guard<std::mutex> lg(m_mutex);
    m_vLegs = vLegs;
    m_nPort = nPort;
    m_nPayloadType = nPayloadType;
    m_nPacketTimeUs = std::max(125, nPacketTimeUs);
    m_pSource = std::move(pSource);
    m_sSourceName = m_pSource->Describe();
    m_vSourceIps.clear();

    for(const auto& leg : m_vLegs)
    {
        int nSocket = socket(AF_INET, SOCK_DGRAM, 0);
        if(nSocket < 0) { return false; }

        int nIfIndex = if_nametoindex(leg.sInterface.c_str());
        ip_mreqn mif{};
        mif.imr_ifindex = nIfIndex;
        if(setsockopt(nSocket, IPPROTO_IP, IP_MULTICAST_IF, &mif, sizeof(mif)) < 0)
        {
            LOG_ERROR("rtptx") << "leg interface " << leg.sInterface << " not usable";
            close(nSocket);
            return false;
        }
        uint8_t nTtl = 64;
        setsockopt(nSocket, IPPROTO_IP, IP_MULTICAST_TTL, &nTtl, sizeof(nTtl));
        int nTos = 0xB8;    //EF - AES67 media
        setsockopt(nSocket, IPPROTO_IP, IP_TOS, &nTos, sizeof(nTos));

        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(m_nPort);
        inet_pton(AF_INET, leg.sMulticast.c_str(), &dest.sin_addr);
        if(connect(nSocket, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) < 0)
        {
            close(nSocket);
            return false;
        }
        m_vSockets.push_back(nSocket);
        m_vSourceIps.push_back(InterfaceIp(leg.sInterface));
    }

    std::random_device rd;
    m_nSsrc = rd();
    m_nPacketsSent = 0;
    m_nSendErrors = 0;
    m_meters.Reset();

    m_bRun = true;
    m_thread = std::thread(&RtpSender::SendLoop, this);
    LOG_INFO("rtptx") << "sending '" << m_sSourceName << "' to "
                      << m_vLegs[0].sMulticast << ":" << m_nPort
                      << (m_vLegs.size() > 1 ? " + "+m_vLegs[1].sMulticast+" (2022-7)" : "")
                      << " ptime " << m_nPacketTimeUs << "us";
    return true;
}

void RtpSender::SendLoop()
{
    const size_t nFramesPerPacket = static_cast<size_t>(SAMPLE_RATE) * m_nPacketTimeUs / 1000000;
    const size_t nPayloadBytes = nFramesPerPacket * CHANNELS * BYTES_PER_SAMPLE;
    std::vector<float> vAudio(nFramesPerPacket * CHANNELS);
    std::vector<uint8_t> vPacket(12 + nPayloadBytes);

    //wait for ptp before anchoring the media clock
    m_bWaitingForPtp = true;
    while(m_bRun && !m_ptp.IsSynced())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    m_bWaitingForPtp = false;
    if(!m_bRun) { return; }

    //anchor: packet n leaves at ptp time nStartNs + n*ptime, rtp timestamp is
    //the ptp media clock (samples since ptp epoch, mediaclk:direct=0)
    uint64_t nStartNs = m_ptp.PtpTimeNs() + 20'000'000;     //begin 20 ms in the future
    uint64_t nPacket = 0;
    uint16_t nSeq = static_cast<uint16_t>(m_nSsrc);

    while(m_bRun)
    {
        uint64_t nDeadlineNs = nStartNs + nPacket * static_cast<uint64_t>(m_nPacketTimeUs) * 1000;

        //ptp time -> realtime via the current mapping, then absolute sleep
        int64_t nAheadNs = static_cast<int64_t>(nDeadlineNs) - static_cast<int64_t>(m_ptp.PtpTimeNs());
        if(nAheadNs > 2'000'000'000 || nAheadNs < -2'000'000'000)
        {
            //mapping jumped (ptp restart) - re-anchor
            nStartNs = m_ptp.PtpTimeNs() + 20'000'000;
            nPacket = 0;
            continue;
        }
        if(nAheadNs > 0)
        {
            timespec ts{static_cast<time_t>(nAheadNs / 1000000000), static_cast<long>(nAheadNs % 1000000000)};
            nanosleep(&ts, nullptr);
        }

        bool bHaveAudio;
        {
            std::lock_guard<std::mutex> lg(m_mutex);
            bHaveAudio = m_pSource && m_pSource->Read(vAudio.data(), nFramesPerPacket);
        }
        m_bEssenceOk = bHaveAudio;
        m_meters.Feed(vAudio.data(), nFramesPerPacket);

        //rtp header
        uint32_t nRtpTs = static_cast<uint32_t>((nDeadlineNs / 1000) * SAMPLE_RATE / 1000000);
        vPacket[0] = 0x80;
        vPacket[1] = static_cast<uint8_t>(m_nPayloadType);
        vPacket[2] = nSeq >> 8;
        vPacket[3] = nSeq & 0xff;
        vPacket[4] = nRtpTs >> 24; vPacket[5] = (nRtpTs >> 16) & 0xff;
        vPacket[6] = (nRtpTs >> 8) & 0xff; vPacket[7] = nRtpTs & 0xff;
        vPacket[8] = m_nSsrc >> 24; vPacket[9] = (m_nSsrc >> 16) & 0xff;
        vPacket[10] = (m_nSsrc >> 8) & 0xff; vPacket[11] = m_nSsrc & 0xff;

        //L24 big endian payload
        for(size_t i = 0; i < nFramesPerPacket * CHANNELS; i++)
        {
            float f = std::clamp(vAudio[i], -1.0f, 1.0f);
            auto nSample = static_cast<int32_t>(f * 8388607.0f);
            vPacket[12 + i*3] = (nSample >> 16) & 0xff;
            vPacket[12 + i*3 + 1] = (nSample >> 8) & 0xff;
            vPacket[12 + i*3 + 2] = nSample & 0xff;
        }

        {
            std::lock_guard<std::mutex> lg(m_mutex);
            for(int nSocket : m_vSockets)
            {
                if(send(nSocket, vPacket.data(), vPacket.size(), 0) < 0)
                {
                    m_nSendErrors++;
                }
            }
        }
        m_nPacketsSent++;
        nSeq++;
        nPacket++;
    }
}

SdpSession RtpSender::DescribeSession() const
{
    std::lock_guard<std::mutex> lg(m_mutex);
    SdpSession session;
    auto sLabel = Config::Get().GetValue<std::string>("sender.label", "");
    session.sName = sLabel.empty() ? "pi-audio-node " + m_sSourceName : sLabel;
    session.nPayloadType = m_nPayloadType;
    session.dPacketTimeMs = m_nPacketTimeUs / 1000.0;
    session.sPtpGrandmaster = m_ptp.GrandmasterId();
    for(size_t nLeg = 0; nLeg < m_vLegs.size(); nLeg++)
    {
        session.vLegs.push_back({m_vLegs.size() > 1 ? (nLeg == 0 ? "PRI" : "SEC") : "",
                                 m_vLegs[nLeg].sMulticast, "", m_nPort});
    }
    return session;
}

std::vector<std::string> RtpSender::GetSourceIps() const
{
    std::lock_guard<std::mutex> lg(m_mutex);
    return m_vSourceIps;
}

json RtpSender::GetStatusJson() const
{
    json js;
    js["running"] = m_bRun.load();
    js["waiting_for_ptp"] = m_bWaitingForPtp.load();
    js["essence_ok"] = m_bEssenceOk.load();
    js["packets_sent"] = m_nPacketsSent.load();
    js["send_errors"] = m_nSendErrors.load();
    js["meters"] = {{"left_db", m_meters.LeftDb()}, {"right_db", m_meters.RightDb()}};
    std::lock_guard<std::mutex> lg(m_mutex);
    js["source"] = m_sSourceName;
    js["legs"] = json::array();
    for(const auto& leg : m_vLegs)
    {
        js["legs"].push_back({{"interface", leg.sInterface}, {"multicast", leg.sMulticast}, {"port", m_nPort}});
    }
    return js;
}
