#include "ptpclient.h"
#include <arpa/inet.h>
#include <cstring>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include "log.h"

using namespace pan::ptp;
using json = nlohmann::json;

namespace
{
    const char* PTP_GROUP = "224.0.1.129";
    const uint16_t PORT_EVENT = 319;
    const uint16_t PORT_GENERAL = 320;

    enum MsgType { SYNC=0x0, DELAY_REQ=0x1, FOLLOW_UP=0x8, DELAY_RESP=0x9, ANNOUNCE=0xB };

    uint16_t Get16(const uint8_t* p) { return static_cast<uint16_t>((p[0]<<8)|p[1]); }
    uint64_t Get48(const uint8_t* p)
    {
        uint64_t n = 0;
        for(int i = 0; i < 6; i++) { n = (n<<8)|p[i]; }
        return n;
    }
    uint32_t Get32(const uint8_t* p) { return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3]; }

    //ptp timestamp: 48 bit seconds + 32 bit nanoseconds
    uint64_t GetTimestampNs(const uint8_t* p) { return Get48(p)*1000000000ULL + Get32(p+6); }

    uint64_t RealtimeNs()
    {
        timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return uint64_t(ts.tv_sec)*1000000000ULL + ts.tv_nsec;
    }

    int OpenPtpSocket(uint16_t nPort, const std::string& sInterface, int nIfIndex)
    {
        int nSocket = socket(AF_INET, SOCK_DGRAM, 0);
        if(nSocket < 0) { return -1; }
        int nOn = 1;
        setsockopt(nSocket, SOL_SOCKET, SO_REUSEADDR, &nOn, sizeof(nOn));
        setsockopt(nSocket, SOL_SOCKET, SO_REUSEPORT, &nOn, sizeof(nOn));
        setsockopt(nSocket, SOL_SOCKET, SO_TIMESTAMPNS, &nOn, sizeof(nOn));
        //both sockets share the port and group, so reuseport does not deliver
        //strictly per interface - the real ingress comes from IP_PKTINFO
        setsockopt(nSocket, IPPROTO_IP, IP_PKTINFO, &nOn, sizeof(nOn));
        #ifdef IP_MULTICAST_ALL
        int nOff = 0;
        setsockopt(nSocket, IPPROTO_IP, IP_MULTICAST_ALL, &nOff, sizeof(nOff));
        #endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(nPort);
        addr.sin_addr.s_addr = INADDR_ANY;
        if(bind(nSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            close(nSocket);
            return -1;
        }

        ip_mreqn mreq{};
        inet_pton(AF_INET, PTP_GROUP, &mreq.imr_multiaddr);
        mreq.imr_ifindex = nIfIndex;
        setsockopt(nSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

        //outgoing multicast (delay_req) leaves on the ptp interface
        ip_mreqn mif{};
        mif.imr_ifindex = nIfIndex;
        setsockopt(nSocket, IPPROTO_IP, IP_MULTICAST_IF, &mif, sizeof(mif));
        uint8_t nTtl = 1;
        setsockopt(nSocket, IPPROTO_IP, IP_MULTICAST_TTL, &nTtl, sizeof(nTtl));
        return nSocket;
    }
}

PtpClient::~PtpClient()
{
    Stop();
}

bool PtpClient::Run(const std::vector<std::string>& vInterfaces, int nDomain)
{
    Stop();
    if(vInterfaces.empty()) { return false; }
    m_vInterfaces = vInterfaces;
    if(m_vInterfaces.size() > 2) { m_vInterfaces.resize(2); }
    m_nDomain = nDomain;

    if(!OpenSockets())
    {
        LOG_ERROR("ptp") << "could not open sockets on " << m_vInterfaces[0];
        return false;
    }

    m_bRun = true;
    m_rxThread = std::thread(&PtpClient::RxLoop, this);
    m_timerThread = std::thread(&PtpClient::TimerLoop, this);
    LOG_INFO("ptp") << "running on " << m_vInterfaces[0]
                    << (m_vInterfaces.size() > 1 ? " (announce monitor on " + m_vInterfaces[1] + ")" : "")
                    << " domain " << nDomain << " identity " << IdentityToString(m_ownIdentity);
    return true;
}

void PtpClient::Stop()
{
    m_bRun = false;
    if(m_rxThread.joinable()) { m_rxThread.join(); }
    if(m_timerThread.joinable()) { m_timerThread.join(); }
    CloseSockets();
}

void PtpClient::SetDomain(int nDomain)
{
    if(nDomain == m_nDomain.load()) { return; }
    m_nDomain = nDomain;
    std::lock_guard<std::mutex> lg(m_mutex);
    m_mForeign.clear();
    m_selectedMaster.reset();
    m_bSynced = false;
    m_bHaveDelay = false;
    m_qOffsetWindow.clear();
    LOG_INFO("ptp") << "domain changed to " << nDomain;
}

bool PtpClient::OpenSockets()
{
    //clock identity from the primary interface mac (EUI-64 style)
    ifreq ifr{};
    strncpy(ifr.ifr_name, m_vInterfaces[0].c_str(), IFNAMSIZ-1);
    int nTemp = socket(AF_INET, SOCK_DGRAM, 0);
    if(nTemp >= 0 && ioctl(nTemp, SIOCGIFHWADDR, &ifr) == 0)
    {
        const auto* mac = reinterpret_cast<uint8_t*>(ifr.ifr_hwaddr.sa_data);
        m_ownIdentity = {mac[0], mac[1], mac[2], 0xff, 0xfe, mac[3], mac[4], mac[5]};
    }
    if(nTemp >= 0) { close(nTemp); }

    for(size_t nIf = 0; nIf < m_vInterfaces.size(); nIf++)
    {
        int nIfIndex = if_nametoindex(m_vInterfaces[nIf].c_str());
        if(nIfIndex == 0)
        {
            if(nIf == 0) { return false; }
            LOG_WARN("ptp") << "secondary interface " << m_vInterfaces[nIf] << " not found - amber only";
            continue;
        }
        m_anIfIndex[nIf] = nIfIndex;
        m_anEventSocket[nIf] = OpenPtpSocket(PORT_EVENT, m_vInterfaces[nIf], nIfIndex);
        m_anGeneralSocket[nIf] = OpenPtpSocket(PORT_GENERAL, m_vInterfaces[nIf], nIfIndex);
        if(nIf == 0 && (m_anEventSocket[0] < 0 || m_anGeneralSocket[0] < 0)) { return false; }
    }
    return true;
}

void PtpClient::CloseSockets()
{
    for(auto* pSockets : {&m_anEventSocket, &m_anGeneralSocket})
    {
        for(int& nSocket : *pSockets)
        {
            if(nSocket >= 0) { close(nSocket); nSocket = -1; }
        }
    }
}

void PtpClient::RxLoop()
{
    //socket -> owning interface for the per-net announce bookkeeping
    std::vector<std::pair<int, size_t>> vSockets;
    for(size_t nIf = 0; nIf < 2; nIf++)
    {
        if(m_anEventSocket[nIf] >= 0) { vSockets.emplace_back(m_anEventSocket[nIf], nIf); }
        if(m_anGeneralSocket[nIf] >= 0) { vSockets.emplace_back(m_anGeneralSocket[nIf], nIf); }
    }

    while(m_bRun)
    {
        fd_set fds;
        FD_ZERO(&fds);
        int nMax = -1;
        for(const auto& [nSocket, nIf] : vSockets)
        {
            FD_SET(nSocket, &fds);
            nMax = std::max(nMax, nSocket);
        }
        timeval tv{0, 200000};
        int nReady = select(nMax+1, &fds, nullptr, nullptr, &tv);
        if(nReady <= 0) { continue; }

        for(const auto& [nSocket, nInterface] : vSockets)
        {
            if(!FD_ISSET(nSocket, &fds)) { continue; }

            uint8_t buffer[512];
            uint8_t control[256];
            sockaddr_in from{};
            iovec iov{buffer, sizeof(buffer)};
            msghdr msg{};
            msg.msg_name = &from;
            msg.msg_namelen = sizeof(from);
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = control;
            msg.msg_controllen = sizeof(control);

            ssize_t nSize = recvmsg(nSocket, &msg, 0);
            if(nSize < 34) { continue; }

            uint64_t nRxNs = RealtimeNs();
            size_t nIngress = nInterface;
            for(cmsghdr* pCmsg = CMSG_FIRSTHDR(&msg); pCmsg; pCmsg = CMSG_NXTHDR(&msg, pCmsg))
            {
                if(pCmsg->cmsg_level == SOL_SOCKET && pCmsg->cmsg_type == SCM_TIMESTAMPNS)
                {
                    timespec ts;
                    memcpy(&ts, CMSG_DATA(pCmsg), sizeof(ts));
                    nRxNs = uint64_t(ts.tv_sec)*1000000000ULL + ts.tv_nsec;
                }
                else if(pCmsg->cmsg_level == IPPROTO_IP && pCmsg->cmsg_type == IP_PKTINFO)
                {
                    in_pktinfo info;
                    memcpy(&info, CMSG_DATA(pCmsg), sizeof(info));
                    for(size_t nIf = 0; nIf < 2; nIf++)
                    {
                        if(m_anIfIndex[nIf] == info.ipi_ifindex) { nIngress = nIf; }
                    }
                }
            }

            char sFrom[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &from.sin_addr, sFrom, sizeof(sFrom));
            HandleMessage(buffer, static_cast<size_t>(nSize), nRxNs, sFrom, nIngress);
        }
    }
}

void PtpClient::HandleMessage(const uint8_t* pData, size_t nSize, uint64_t nRxNs,
                              const std::string& sFrom, size_t nInterface)
{
    uint8_t nType = pData[0] & 0x0f;
    uint8_t nVersion = pData[1] & 0x0f;
    uint8_t nDomain = pData[4];
    if(nVersion != 2 || nDomain != m_nDomain.load()) { return; }

    //ignore our own transmissions
    if(memcmp(pData+20, m_ownIdentity.data(), 8) == 0) { return; }

    //announces feed the per-net comparison; the clock itself only follows
    //messages on the primary interface
    if(nType == ANNOUNCE)
    {
        HandleAnnounce(pData, nSize, sFrom, nInterface);
        return;
    }
    if(nInterface != 0) { return; }
    switch(nType)
    {
        case SYNC:       HandleSync(pData, nSize, nRxNs); break;
        case FOLLOW_UP:  HandleFollowUp(pData, nSize); break;
        case DELAY_RESP: HandleDelayResp(pData, nSize); break;
        default: break;
    }
}

void PtpClient::HandleAnnounce(const uint8_t* pData, size_t nSize, const std::string& sFrom, size_t nInterface)
{
    if(nSize < 64) { return; }

    AnnounceDataset ds;
    memcpy(ds.source.clockIdentity.data(), pData+20, 8);
    ds.source.nPort = Get16(pData+28);
    uint16_t nFlags = Get16(pData+6);
    ds.bLeap61 = nFlags & 0x0001;
    ds.bLeap59 = nFlags & 0x0002;
    ds.bUtcValid = nFlags & 0x0004;
    ds.bTimeTraceable = nFlags & 0x0010;
    ds.bFreqTraceable = nFlags & 0x0020;
    ds.nUtcOffset = static_cast<int16_t>(Get16(pData+44));
    ds.nPriority1 = pData[47];
    ds.nClockClass = pData[48];
    ds.nClockAccuracy = pData[49];
    ds.nOffsetScaledLogVariance = Get16(pData+50);
    ds.nPriority2 = pData[52];
    memcpy(ds.grandmasterIdentity.data(), pData+53, 8);
    ds.nStepsRemoved = Get16(pData+61);
    ds.nTimeSource = pData[63];

    std::lock_guard<std::mutex> lg(m_mutex);
    auto& foreign = m_mForeign[ds.source];
    if(foreign.aAnnounceCount[0] + foreign.aAnnounceCount[1] == 0)
    {
        foreign.firstSeen = std::chrono::steady_clock::now();
        LOG_INFO("ptp") << "new announce source " << IdentityToString(ds.source.clockIdentity)
                        << " gm " << IdentityToString(ds.grandmasterIdentity) << " from " << sFrom
                        << " (" << (nInterface == 0 ? "amber" : "blue") << ")";
    }
    foreign.aAnnounceCount[nInterface]++;
    foreign.aDataset[nInterface] = ds;
    foreign.lastSeen = std::chrono::steady_clock::now();
    if(nInterface == 0)
    {
        foreign.dataset = ds;
        foreign.nAnnounceCount++;
        foreign.sAddress = sFrom;
    }
    else if(foreign.nAnnounceCount == 0)
    {
        //only seen on blue so far - show its data in the table anyway
        foreign.dataset = ds;
        foreign.sAddress = sFrom;
    }
    RunBmca();
}

void PtpClient::RunBmca()
{
    //caller holds m_mutex. qualified = seen at least twice
    const PortIdentity* pBest = nullptr;
    for(auto& [id, foreign] : m_mForeign)
    {
        if(foreign.nAnnounceCount < 2) { continue; }
        if(pBest == nullptr || CompareDatasets(foreign.dataset, m_mForeign[*pBest].dataset).nResult < 0)
        {
            pBest = &id;
        }
    }

    //annotate every master with why it lost (analyzer data)
    for(auto& [id, foreign] : m_mForeign)
    {
        foreign.bSelected = (pBest && id == *pBest);
        if(foreign.bSelected)
        {
            foreign.sBmcaInfo = "selected";
        }
        else if(foreign.nAnnounceCount == 0 && foreign.aAnnounceCount[1] > 0)
        {
            foreign.sBmcaInfo = "only on blue - not eligible";
        }
        else if(foreign.nAnnounceCount < 2)
        {
            foreign.sBmcaInfo = "not yet qualified";
        }
        else if(pBest)
        {
            auto cmp = CompareDatasets(m_mForeign[*pBest].dataset, foreign.dataset);
            foreign.sBmcaInfo = "loses to " + IdentityToString(pBest->clockIdentity) + " on " + cmp.sDecidingField;
        }
    }

    std::optional<PortIdentity> newMaster = pBest ? std::optional(*pBest) : std::nullopt;
    if(newMaster != m_selectedMaster)
    {
        m_selectedMaster = newMaster;
        m_bAwaitFollowUp = false;
        m_bHaveDelay = false;
        m_qOffsetWindow.clear();
        if(m_selectedMaster)
        {
            LOG_INFO("ptp") << "bmca selected master " << IdentityToString(m_selectedMaster->clockIdentity);
        }
        else
        {
            LOG_WARN("ptp") << "no ptp master on the domain";
            m_bSynced = false;
        }
    }
}

void PtpClient::HandleSync(const uint8_t* pData, size_t nSize, uint64_t nRxNs)
{
    if(nSize < 44) { return; }
    std::lock_guard<std::mutex> lg(m_mutex);
    if(!m_selectedMaster || memcmp(pData+20, m_selectedMaster->clockIdentity.data(), 8) != 0) { return; }

    m_lastSyncSeen = std::chrono::steady_clock::now();
    uint16_t nFlags = Get16(pData+6);
    m_nSyncSeq = Get16(pData+30);
    m_nSyncRxNs = nRxNs;
    if(nFlags & 0x0200)     //two step - wait for follow up
    {
        m_bAwaitFollowUp = true;
    }
    else
    {
        CompleteMeasurement(GetTimestampNs(pData+34), nRxNs);
    }
}

void PtpClient::HandleFollowUp(const uint8_t* pData, size_t nSize)
{
    if(nSize < 44) { return; }
    std::lock_guard<std::mutex> lg(m_mutex);
    if(!m_bAwaitFollowUp || !m_selectedMaster) { return; }
    if(memcmp(pData+20, m_selectedMaster->clockIdentity.data(), 8) != 0) { return; }
    if(Get16(pData+30) != m_nSyncSeq) { return; }

    m_bAwaitFollowUp = false;
    CompleteMeasurement(GetTimestampNs(pData+34), m_nSyncRxNs);
}

void PtpClient::CompleteMeasurement(uint64_t nT1, uint64_t nT2)
{
    //caller holds m_mutex. t1 = master tx (ptp), t2 = our rx (realtime)
    //master-to-slave difference includes the path delay
    double dM2S = double(int64_t(nT1 - nT2));      //ptp - realtime, still missing path delay
    double dOffset = dM2S + (m_bHaveDelay ? m_dMeanPathDelayNs : 0.0);
    AddOffsetSample(dOffset);
    m_nSyncCount++;
}

void PtpClient::AddOffsetSample(double dOffsetNs)
{
    //caller holds m_mutex. reject outliers once settled, then low-pass
    if(m_qOffsetWindow.size() >= 8)
    {
        if(std::abs(dOffsetNs - m_dSmoothedOffsetNs) > 50e6)    //50ms jump - restart mapping
        {
            m_qOffsetWindow.clear();
        }
    }
    m_qOffsetWindow.push_back(dOffsetNs);
    if(m_qOffsetWindow.size() > 16) { m_qOffsetWindow.pop_front(); }

    if(m_qOffsetWindow.size() >= 4)
    {
        //median of the window - robust against delayed packets
        auto sorted = std::vector<double>(m_qOffsetWindow.begin(), m_qOffsetWindow.end());
        std::sort(sorted.begin(), sorted.end());
        double dMedian = sorted[sorted.size()/2];
        m_dSmoothedOffsetNs = m_bSynced ? (m_dSmoothedOffsetNs*0.9 + dMedian*0.1) : dMedian;
        m_nMappingNs = int64_t(m_dSmoothedOffsetNs);
        m_bSynced = true;
    }

    uint64_t nNowMs = (RealtimeNs() + uint64_t(m_nMappingNs.load())) / 1000000ULL;
    m_qOffsetHistory.emplace_back(nNowMs, dOffsetNs - m_dSmoothedOffsetNs);
    if(m_qOffsetHistory.size() > 300) { m_qOffsetHistory.pop_front(); }
}

void PtpClient::SendDelayReq()
{
    uint8_t buffer[44] = {0};
    buffer[0] = DELAY_REQ;
    buffer[1] = 2;                              //version
    buffer[2] = 0; buffer[3] = 44;              //length
    buffer[4] = static_cast<uint8_t>(m_nDomain.load());
    memcpy(buffer+20, m_ownIdentity.data(), 8);
    buffer[28] = 0; buffer[29] = 1;             //port 1
    {
        std::lock_guard<std::mutex> lg(m_mutex);
        m_nDelaySeq++;
        buffer[30] = m_nDelaySeq >> 8;
        buffer[31] = m_nDelaySeq & 0xff;
        m_nDelayReqTxNs = RealtimeNs();
    }
    buffer[32] = 0x7f;                          //logMessageInterval

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(PORT_EVENT);
    inet_pton(AF_INET, PTP_GROUP, &dest.sin_addr);
    sendto(m_anEventSocket[0], buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
}

void PtpClient::HandleDelayResp(const uint8_t* pData, size_t nSize)
{
    if(nSize < 54) { return; }
    if(memcmp(pData+44, m_ownIdentity.data(), 8) != 0) { return; }  //not answering us

    std::lock_guard<std::mutex> lg(m_mutex);
    if(Get16(pData+30) != m_nDelaySeq) { return; }

    //t3 = our tx (realtime), t4 = master rx (ptp)
    //slave-to-master raw difference = pathDelay + clockOffset, so remove the offset
    uint64_t nT4 = GetTimestampNs(pData+34);
    double dS2M = double(int64_t(nT4 - m_nDelayReqTxNs));
    double dDelay = dS2M - m_dSmoothedOffsetNs;
    if(dDelay >= 0 && dDelay < 100e6)
    {
        m_dMeanPathDelayNs = m_bHaveDelay ? (m_dMeanPathDelayNs*0.9 + dDelay*0.1) : dDelay;
        m_bHaveDelay = true;
    }
}

void PtpClient::TimerLoop()
{
    int nTick = 0;
    while(m_bRun)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        nTick++;

        bool bSendDelay = false;
        {
            std::lock_guard<std::mutex> lg(m_mutex);
            auto now = std::chrono::steady_clock::now();

            //age out silent masters
            for(auto it = m_mForeign.begin(); it != m_mForeign.end();)
            {
                if(now - it->second.lastSeen > std::chrono::milliseconds(6000))
                {
                    LOG_WARN("ptp") << "announce timeout for " << IdentityToString(it->first.clockIdentity);
                    it = m_mForeign.erase(it);
                    RunBmca();
                }
                else
                {
                    ++it;
                }
            }

            //sync silence - drop synced state
            if(m_bSynced && now - m_lastSyncSeen > std::chrono::milliseconds(5000))
            {
                LOG_WARN("ptp") << "sync messages stopped";
                m_bSynced = false;
                m_qOffsetWindow.clear();
            }

            bSendDelay = m_selectedMaster.has_value() && (nTick % 4 == 0);
        }

        if(bSendDelay)
        {
            SendDelayReq();
        }
    }
}

uint64_t PtpClient::PtpTimeNs() const
{
    if(!m_bSynced) { return 0; }
    return RealtimeNs() + uint64_t(m_nMappingNs.load());
}

std::string PtpClient::GrandmasterId() const
{
    std::lock_guard<std::mutex> lg(m_mutex);
    if(m_selectedMaster)
    {
        auto it = m_mForeign.find(*m_selectedMaster);
        if(it != m_mForeign.end())
        {
            return IdentityToString(it->second.dataset.grandmasterIdentity);
        }
    }
    return "";
}

json PtpClient::GetStatusJson() const
{
    std::lock_guard<std::mutex> lg(m_mutex);
    json js;
    js["domain"] = m_nDomain.load();
    js["interface"] = m_vInterfaces.empty() ? "" : m_vInterfaces[0];
    js["identity"] = IdentityToString(m_ownIdentity);
    js["synced"] = m_bSynced.load();
    js["sync_count"] = m_nSyncCount;
    //the system clock is not disciplined to ptp, so an absolute ptp-vs-system
    //comparison is meaningless - report only the servo correction: how far the
    //latest measurement deviates from the established mapping
    js["correction_ns"] = m_qOffsetHistory.empty() ? 0.0 : m_qOffsetHistory.back().second;
    js["mean_path_delay_ns"] = m_bHaveDelay ? json(m_dMeanPathDelayNs) : json();
    js["masters"] = json::array();

    for(const auto& [id, foreign] : m_mForeign)
    {
        const auto& ds = foreign.dataset;
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - foreign.firstSeen).count();
        js["masters"].push_back({
            {"port_identity", IdentityToString(id.clockIdentity)+"-"+std::to_string(id.nPort)},
            {"address", foreign.sAddress},
            {"grandmaster", IdentityToString(ds.grandmasterIdentity)},
            {"priority1", ds.nPriority1},
            {"clock_class", ds.nClockClass},
            {"clock_accuracy", ds.nClockAccuracy},
            {"variance", ds.nOffsetScaledLogVariance},
            {"priority2", ds.nPriority2},
            {"steps_removed", ds.nStepsRemoved},
            {"time_source", ds.nTimeSource},
            {"utc_offset", ds.nUtcOffset},
            {"time_traceable", ds.bTimeTraceable},
            {"freq_traceable", ds.bFreqTraceable},
            {"announces", foreign.nAnnounceCount},
            {"seen_seconds", age},
            {"selected", foreign.bSelected},
            {"bmca", foreign.sBmcaInfo},
            {"nets", {{"amber", foreign.aAnnounceCount[0]}, {"blue", foreign.aAnnounceCount[1]}}},
            //datasets from both nets should describe the same clock identically
            {"net_match", foreign.aAnnounceCount[0] == 0 || foreign.aAnnounceCount[1] == 0 ? json()
                : json(CompareDatasets(foreign.aDataset[0], foreign.aDataset[1]).nResult == 0
                       && foreign.aDataset[0].nUtcOffset == foreign.aDataset[1].nUtcOffset)}
        });
    }

    //meta bmca: best qualified master per net - both nets should agree on the gm
    if(m_vInterfaces.size() > 1)
    {
        const AnnounceDataset* apBest[2] = {nullptr, nullptr};
        for(size_t nIf = 0; nIf < 2; nIf++)
        {
            for(const auto& [id, foreign] : m_mForeign)
            {
                if(foreign.aAnnounceCount[nIf] < 2) { continue; }
                if(!apBest[nIf] || CompareDatasets(foreign.aDataset[nIf], *apBest[nIf]).nResult < 0)
                {
                    apBest[nIf] = &foreign.aDataset[nIf];
                }
            }
        }
        json jsMeta;
        jsMeta["amber_gm"] = apBest[0] ? IdentityToString(apBest[0]->grandmasterIdentity) : "";
        jsMeta["blue_gm"] = apBest[1] ? IdentityToString(apBest[1]->grandmasterIdentity) : "";
        if(apBest[0] && apBest[1])
        {
            auto cmp = CompareDatasets(*apBest[0], *apBest[1]);
            jsMeta["match"] = apBest[0]->grandmasterIdentity == apBest[1]->grandmasterIdentity;
            jsMeta["detail"] = cmp.nResult == 0 ? "" : "differs on " + cmp.sDecidingField;
        }
        else
        {
            jsMeta["match"] = json();
            jsMeta["detail"] = apBest[0] ? "no ptp on blue" : (apBest[1] ? "no ptp on amber" : "no ptp on either net");
        }
        js["meta"] = jsMeta;
    }

    js["offset_history"] = json::array();
    for(const auto& [nMs, dOffset] : m_qOffsetHistory)
    {
        js["offset_history"].push_back({nMs, dOffset});
    }
    return js;
}
