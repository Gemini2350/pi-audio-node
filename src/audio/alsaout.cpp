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

    //explicit hw params: small periods keep snd_pcm_delay fine grained enough
    //to steer low playout delays (5-10 ms); the buffer size is only headroom
    snd_pcm_hw_params_t* pHw;
    snd_pcm_hw_params_alloca(&pHw);
    snd_pcm_hw_params_any(m_pPcm, pHw);
    snd_pcm_hw_params_set_rate_resample(m_pPcm, pHw, 1);
    snd_pcm_hw_params_set_access(m_pPcm, pHw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(m_pPcm, pHw, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(m_pPcm, pHw, 2);
    unsigned int nRate = 48000;
    snd_pcm_uframes_t nPeriod = 96;         //2 ms
    snd_pcm_uframes_t nBuffer = 4800;       //100 ms headroom
    snd_pcm_hw_params_set_rate_near(m_pPcm, pHw, &nRate, nullptr);
    snd_pcm_hw_params_set_period_size_near(m_pPcm, pHw, &nPeriod, nullptr);
    snd_pcm_hw_params_set_buffer_size_near(m_pPcm, pHw, &nBuffer);
    nError = snd_pcm_hw_params(m_pPcm, pHw);
    if(nError < 0)
    {
        //fall back to the generic setup for devices that reject the layout
        LOG_WARN("alsa") << "explicit hw params failed (" << snd_strerror(nError) << ") - using defaults";
        nError = snd_pcm_set_params(m_pPcm, SND_PCM_FORMAT_S32_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                    2, 48000, 1 /*allow resample*/, 100000 /*100 ms buffer*/);
        if(nError < 0)
        {
            LOG_ERROR("alsa") << "set_params: " << snd_strerror(nError);
            snd_pcm_close(m_pPcm);
            m_pPcm = nullptr;
            return false;
        }
    }
    else
    {
        //start as soon as the anchor burst is queued, not when the buffer fills
        snd_pcm_sw_params_t* pSw;
        snd_pcm_sw_params_alloca(&pSw);
        snd_pcm_sw_params_current(m_pPcm, pSw);
        snd_pcm_sw_params_set_start_threshold(m_pPcm, pSw, nPeriod * 2);
        snd_pcm_sw_params(m_pPcm, pSw);
    }
    LOG_INFO("alsa") << "playback open on '" << sDevice << "' period " << nPeriod
                     << " buffer " << nBuffer << " frames";
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
