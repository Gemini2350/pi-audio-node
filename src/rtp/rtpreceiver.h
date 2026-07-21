#pragma once
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "audio/meters.h"
#include "json.hpp"
#include "sdp.h"

namespace pan::ptp { class PtpClient; }
namespace pan::audio { class AlsaOut; }

namespace pan::rtp
{
    /** AES67 receiver with ST 2022-7 seamless merge.
    *   One socket per leg, each joined on its own interface with
    *   IP_MULTICAST_ALL disabled so a leg only ever sees its own group.
    *   Packets from both legs land in one sequence-indexed jitter buffer -
    *   first arrival wins, the copy is dropped. Playout is anchored to the
    *   PTP media clock and then steered by the ALSA buffer level.
    **/
    class RtpReceiver
    {
        public:
            RtpReceiver(ptp::PtpClient& ptpClient, audio::AlsaOut& alsa) : m_ptp(ptpClient), m_alsa(alsa) {}
            ~RtpReceiver();

            /** legs in session order are joined on vInterfaces in the same order **/
            bool Configure(const SdpSession& session, const std::vector<std::string>& vInterfaces,
                           int nPlayoutDelayMs, std::function<double()> pGainDb);
            void Stop();

            bool IsRunning() const { return m_bRun.load(); }
            bool IsReceiving() const { return m_bReceiving.load(); }
            nlohmann::json GetStatusJson() const;
            audio::Meters& GetMeters() { return m_meters; }

            //bcp-008 counters
            uint64_t LostPackets(size_t nLeg) const { return nLeg < 2 ? m_aLegs[nLeg].nLost.load() : 0; }
            uint64_t LatePackets() const { return m_nConcealed.load(); }

        private:
            static constexpr size_t JITTER_SLOTS = 1024;

            struct Slot
            {
                std::atomic<uint32_t> nGeneration{0};   //0 = empty, else seq+1 marker
                uint16_t nSeq = 0;
                std::vector<float> vSamples;
            };

            struct LegState
            {
                int nSocket = -1;
                std::string sInterface;
                std::string sMulticast;
                std::atomic<uint64_t> nReceived{0};
                std::atomic<uint64_t> nLost{0};
                std::atomic<bool> bActive{false};
                uint16_t nLastSeq = 0;
                bool bHaveSeq = false;
                std::chrono::steady_clock::time_point lastPacket{};
            };

            void RxLoop();
            void PlayoutLoop();
            void HandlePacket(size_t nLeg, const uint8_t* pData, size_t nSize);

            ptp::PtpClient& m_ptp;
            audio::AlsaOut& m_alsa;

            mutable std::mutex m_mutex;
            SdpSession m_session;
            std::array<LegState, 2> m_aLegs;
            size_t m_nLegCount = 0;
            int m_nPlayoutDelayMs = 20;
            std::function<double()> m_pGainDb;

            std::unique_ptr<Slot[]> m_vJitter = std::make_unique<Slot[]>(JITTER_SLOTS);   //atomics are not movable
            std::atomic<bool> m_bHaveFirst{false};
            std::atomic<uint16_t> m_nFirstSeq{0};
            std::atomic<uint32_t> m_nFirstTs{0};
            size_t m_nFramesPerPacket = 48;

            std::thread m_rxThread;
            std::thread m_playThread;
            std::atomic<bool> m_bRun{false};
            std::atomic<bool> m_bReceiving{false};
            std::atomic<uint64_t> m_nPlayed{0};
            std::atomic<uint64_t> m_nConcealed{0};
            std::atomic<uint64_t> m_nDuplicates{0};
            std::atomic<uint64_t> m_nMergedFromSecondary{0};
            audio::Meters m_meters;
    };
}
