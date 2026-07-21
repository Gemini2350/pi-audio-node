#include "config.h"
#include <fstream>
#include "log.h"

using namespace pan;
using json = nlohmann::json;

namespace
{
    json Defaults()
    {
        return json{
            {"device", {
                {"label", "pi-audio-node"},
                {"description", "Raspberry Pi AES67 sender/receiver"}
            }},
            {"network", {
                {"interface_primary", "eth0"},
                {"interface_secondary", "eth1"}     //empty = single leg operation
            }},
            {"ptp", {
                {"domain", 0},
                {"announce_timeout_ms", 6000},
                {"delay_req_interval_ms", 1000}
            }},
            {"receiver", {
                {"enabled", true},
                {"playout_delay_ms", 20},
                {"gain_db", 0.0},
                {"alsa_device", "default:CARD=sndrpihifiberry"}
            }},
            {"sender", {
                {"label", ""},                      //empty = "<device label> sender"
                {"enabled", false},
                {"source", "tone"},                 //tone | sweep | file
                {"tone_hz", 440.0},
                {"tone_level_db", -18.0},
                {"file", ""},
                {"loop", true},
                {"multicast_primary", "239.69.145.10"},
                {"multicast_secondary", "239.69.146.10"},
                {"leg1_enabled", true},
                {"leg2_enabled", true},
                {"port", 5004},
                {"packet_time_us", 1000},
                {"payload_type", 96},
                {"channels", 2}
            }},
            {"nmos", {
                {"enabled", true},
                {"http_port", 8080},
                {"registry_override", ""}           //empty = discover via dns-sd
            }},
            {"web", {
                {"port", 80},
                {"webui_path", "/usr/local/share/pi-audio-node/webui"},
                {"audio_files_path", ""}            //empty = ~/audio-files
            }}
        };
    }

    void MergeDefaults(json& jsTarget, const json& jsDefaults)
    {
        for(const auto& [sKey, jsValue] : jsDefaults.items())
        {
            if(!jsTarget.contains(sKey))
            {
                jsTarget[sKey] = jsValue;
            }
            else if(jsValue.is_object() && jsTarget[sKey].is_object())
            {
                MergeDefaults(jsTarget[sKey], jsValue);
            }
        }
    }
}

Config& Config::Get()
{
    static Config config;
    return config;
}

std::string Config::Dotted(const std::string& sPath)
{
    std::string s(sPath);
    for(auto& c : s)
    {
        if(c == '.') { c = '/'; }
    }
    return s;
}

bool Config::Load(const std::string& sPath)
{
    std::lock_guard<std::mutex> lg(m_mutex);
    m_sPath = sPath;
    m_json = json::object();
    std::ifstream ifs(sPath);
    if(ifs)
    {
        try { m_json = json::parse(ifs); }
        catch(const std::exception& e) { LOG_ERROR("config") << "parse failed: " << e.what(); }
    }
    MergeDefaults(m_json, Defaults());
    return true;
}

void Config::Save()
{
    std::lock_guard<std::mutex> lg(m_mutex);
    if(m_sPath.empty()) { return; }
    std::ofstream ofs(m_sPath);
    if(ofs) { ofs << m_json.dump(2) << "\n"; }
    else    { LOG_ERROR("config") << "cannot write " << m_sPath; }
}

json Config::GetJson() const
{
    std::lock_guard<std::mutex> lg(m_mutex);
    return m_json;
}

void Config::Update(const json& jsUpdates)
{
    {
        std::lock_guard<std::mutex> lg(m_mutex);
        for(const auto& [sKey, jsValue] : jsUpdates.items())
        {
            m_json[json::json_pointer("/"+Dotted(sKey))] = jsValue;
        }
    }
    Save();
}
