#pragma once
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include "json.hpp"
#include "rtp/sdp.h"

struct mg_context;
struct mg_connection;

namespace pan::nmos
{
    class NmosNode;

    /** IS-05 v1.1 connection api: /single sender + receiver with immediate
    *   activation and up to two transport param legs (ST 2022-7).
    **/
    class ConnectionApi
    {
        public:
            using ReceiverCallback = std::function<void(bool, const std::optional<rtp::SdpSession>&)>;
            using SenderCallback = std::function<void(bool)>;

            ConnectionApi(NmosNode& node, ReceiverCallback onReceiver, SenderCallback onSender);
            void Register(mg_context* pServer);

            nlohmann::json ReceiverSenderId() const;    //active subscription for IS-04

        private:
            int Handle(mg_connection* pConn);
            nlohmann::json BuildStaged(bool bSender) const;
            nlohmann::json BuildActive(bool bSender) const;
            nlohmann::json BuildConstraints(size_t nLegs) const;
            void PatchSender(const nlohmann::json& jsPatch);
            void PatchReceiver(const nlohmann::json& jsPatch);
            void ActivateSender();
            void ActivateReceiver();

            NmosNode& m_node;
            ReceiverCallback m_onReceiver;
            SenderCallback m_onSender;

            mutable std::mutex m_mutex;
            nlohmann::json m_jsSenderStaged;
            nlohmann::json m_jsSenderActive;
            nlohmann::json m_jsReceiverStaged;
            nlohmann::json m_jsReceiverActive;
    };
}
