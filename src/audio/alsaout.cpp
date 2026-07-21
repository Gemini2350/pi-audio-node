#include "alsaout.h"
#include <alsa/asoundlib.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "log.h"

using namespace pan::audio;

AlsaOut::~AlsaOut()
{
    Close();
}

bool AlsaOut::Open(const std::string& sDevice)
{
    std::lock_guard<std::mutex> lg(m_mutex);
    if(m_pPcm) { snd_pcm_close(m_pPcm); m_pPcm = nullptr; }

    int nError = snd_pcm_open(&m_pPcm, sDevice.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if(nError < 0)
    {
        LOG_ERROR("alsa") << "open '" << sDevice << "': " << snd_strerror(nError);
        m_pPcm = nullptr;
        return false;
    }

    nError = snd_pcm_set_params(m_pPcm, SND_PCM_FORMAT_S32_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                2, 48000, 1 /*allow resample*/, 100000 /*100 ms buffer*/);
    if(nError < 0)
    {
        LOG_ERROR("alsa") << "set_params: " << snd_strerror(nError);
        snd_pcm_close(m_pPcm);
        m_pPcm = nullptr;
        return false;
    }
    LOG_INFO("alsa") << "playback open on '" << sDevice << "'";
    return true;
}

void AlsaOut::Close()
{
    std::lock_guard<std::mutex> lg(m_mutex);
    if(m_pPcm) { snd_pcm_close(m_pPcm); m_pPcm = nullptr; }
}

bool AlsaOut::Write(const float* pInterleaved, size_t nFrames, double dGainDb)
{
    std::lock_guard<std::mutex> lg(m_mutex);
    if(!m_pPcm) { return false; }

    float fGain = static_cast<float>(pow(10.0, dGainDb/20.0));
    std::vector<int32_t> vSamples(nFrames*2);
    for(size_t i = 0; i < nFrames*2; i++)
    {
        float f = pInterleaved[i] * fGain;
        f = std::clamp(f, -1.0f, 1.0f);
        vSamples[i] = static_cast<int32_t>(f * 2147483392.0f);
    }

    size_t nWritten = 0;
    while(nWritten < nFrames)
    {
        auto nResult = snd_pcm_writei(m_pPcm, vSamples.data()+nWritten*2, nFrames-nWritten);
        if(nResult < 0)
        {
            m_nUnderruns++;
            nResult = snd_pcm_recover(m_pPcm, static_cast<int>(nResult), 1);
            if(nResult < 0)
            {
                LOG_ERROR("alsa") << "write: " << snd_strerror(static_cast<int>(nResult));
                return false;
            }
            continue;
        }
        nWritten += static_cast<size_t>(nResult);
    }
    return true;
}

long AlsaOut::DelayFrames()
{
    std::lock_guard<std::mutex> lg(m_mutex);
    if(!m_pPcm) { return 0; }
    snd_pcm_sframes_t nDelay = 0;
    if(snd_pcm_delay(m_pPcm, &nDelay) < 0) { return 0; }
    return nDelay;
}
