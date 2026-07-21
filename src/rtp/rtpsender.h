#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "audio/meters.h"
#include "audio/sources.h"
#include "json.hpp"
#include "sdp.h"

namespace pan::ptp { class PtpClient; }

namespace pan::rtp
{
    /** AES67 L24 sender paced by the PTP media clock. With two legs configured
    *   the identical packet (same sequence number and timestamp) leaves on both
    *   interfaces - ST 2022-7 duplication.
    **/
    class RtpSender
    {
        public:
            explicit RtpSender(ptp::PtpClient& ptpClient) : m_ptp(ptpClient) {}
            ~RtpSender();

            struct Leg
            {
                std::string sInterface;
                std::string sMulticast;
            };

            /** (re)start with the given settings; empty legs stops the sender **/
            bool Configure(const std::vector<Leg>& vLegs, uint16_t nPort, int nPayloadType,
                           int nPacketTimeUs, std::unique_ptr<audio::Source> pSource);
            void Stop();

            bool IsRunning() const { return m_bRun.load(); }
            bool IsSendingAudio() const { return m_bEssenceOk.load(); }
            SdpSession DescribeSession() const;
            std::vector<std::string> GetSourceIps() const;
            nlohmann::json GetStatusJson() const;
            audio::Meters& GetMeters() { return m_meters; }

        private:
            void SendLoop();

            ptp::PtpClient& m_ptp;
            mutable std::mutex m_mutex;
            std::vector<Leg> m_vLegs;
            std::vector<int> m_vSockets;
            std::vector<std::string> m_vSourceIps;
            uint16_t m_nPort = 5004;
            int m_nPayloadType = 96;
            int m_nPacketTimeUs = 1000;
            std::unique_ptr<audio::Source> m_pSource;
            std::string m_sSourceName;

            std::thread m_thread;
            std::atomic<bool> m_bRun{false};
            std::atomic<bool> m_bEssenceOk{false};
            std::atomic<bool> m_bWaitingForPtp{false};
            std::atomic<uint64_t> m_nPacketsSent{0};
            std::atomic<uint64_t> m_nSendErrors{0};
            uint32_t m_nSsrc = 0;
            audio::Meters m_meters;
    };
}
