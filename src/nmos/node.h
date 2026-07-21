#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "json.hpp"
#include "rtp/sdp.h"

struct mg_context;

namespace pan::rtp { class RtpSender; class RtpReceiver; }
namespace pan::ptp { class PtpClient; }

namespace pan::nmos
{
    class ConnectionApi;
    class Is12Server;

    /** http request helper (civetweb client) - used by the registration client **/
    std::optional<nlohmann::json> HttpJson(const std::string& sMethod, const std::string& sUrl,
                                           const nlohmann::json* pBody, int& nStatus);

    std::string MakeUuid(const std::string& sSeed);

    /** IS-04 node: resources, node api, registration + heartbeat. Owns the
    *   nmos http server (default port 8080) that also carries IS-05 and IS-12.
    **/
    class NmosNode
    {
        public:
            struct Callbacks
            {
                //receiver: enable with parsed sdp, or disable
                std::function<void(bool, const std::optional<rtp::SdpSession>&)> onReceiver;
                //sender: master_enable
                std::function<void(bool)> onSender;
            };

            NmosNode(ptp::PtpClient& ptpClient, rtp::RtpSender& sender, rtp::RtpReceiver& receiver);
            ~NmosNode();

            bool Start(int nPort, const std::vector<std::string>& vInterfaces, Callbacks callbacks);
            void Stop();

            const std::string& NodeId() const     { return m_sNodeId; }
            const std::string& DeviceId() const   { return m_sDeviceId; }
            const std::string& SenderId() const   { return m_sSenderId; }
            const std::string& ReceiverId() const { return m_sReceiverId; }

            nlohmann::json GetStatusJson() const;
            Is12Server* GetIs12() { return m_pIs12.get(); }
            ConnectionApi* GetConnection() { return m_pConnection.get(); }
            void BumpVersion();     //resource change -> re-register

            //resource builders (also served by the node api)
            nlohmann::json BuildSelf() const;
            nlohmann::json BuildDevice() const;
            nlohmann::json BuildSource() const;
            nlohmann::json BuildFlow() const;
            nlohmann::json BuildSender() const;
            nlohmann::json BuildReceiver() const;

            std::vector<std::string> GetHostIps() const;
            int Port() const { return m_nPort; }

            ptp::PtpClient& Ptp() { return m_ptp; }
            rtp::RtpSender& Sender() { return m_sender; }
            rtp::RtpReceiver& Receiver() { return m_receiver; }

        private:
            void RegistrationLoop();
            bool RegisterAll(const std::string& sRegistryUrl);
            void RegisterNodeApi();
            std::string Version() const;

            ptp::PtpClient& m_ptp;
            rtp::RtpSender& m_sender;
            rtp::RtpReceiver& m_receiver;

            mg_context* m_pServer = nullptr;
            std::unique_ptr<ConnectionApi> m_pConnection;
            std::unique_ptr<Is12Server> m_pIs12;

            int m_nPort = 8080;
            std::vector<std::string> m_vInterfaces;
            std::string m_sHostname;
            std::string m_sNodeId, m_sDeviceId, m_sSourceId, m_sFlowId, m_sSenderId, m_sReceiverId;

            std::thread m_regThread;
            std::atomic<bool> m_bRun{false};
            std::atomic<uint64_t> m_nVersion{0};
            std::atomic<uint64_t> m_nRegisteredVersion{0};
            mutable std::mutex m_mutex;
            std::string m_sRegistry;        //current registry url or empty
            std::string m_sRegistryStatus;  //for the ui
    };
}
