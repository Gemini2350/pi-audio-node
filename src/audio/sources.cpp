#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"
#include <samplerate.h>

#include "sources.h"
#include <cmath>
#include <cstring>
#include "log.h"

using namespace pan::audio;

namespace
{
    constexpr double SAMPLE_RATE = 48000.0;

    class ToneSource : public Source
    {
        public:
            ToneSource(double dFrequency, double dLevelDb) :
                m_dFrequency(dFrequency),
                m_dAmplitude(pow(10.0, dLevelDb/20.0))
            {}

            bool Read(float* pOut, size_t nFrames) override
            {
                for(size_t i = 0; i < nFrames; i++)
                {
                    auto fSample = static_cast<float>(m_dAmplitude * sin(m_dPhase));
                    pOut[i*2] = fSample;
                    pOut[i*2+1] = fSample;
                    m_dPhase += 2.0*M_PI*m_dFrequency/SAMPLE_RATE;
                    if(m_dPhase > 2.0*M_PI) { m_dPhase -= 2.0*M_PI; }
                }
                return true;
            }

            std::string Describe() const override
            {
                return "tone " + std::to_string(static_cast<int>(m_dFrequency)) + " Hz";
            }

        private:
            double m_dFrequency;
            double m_dAmplitude;
            double m_dPhase = 0.0;
    };

    class SweepSource : public Source
    {
        public:
            explicit SweepSource(double dLevelDb) : m_dAmplitude(pow(10.0, dLevelDb/20.0)) {}

            bool Read(float* pOut, size_t nFrames) override
            {
                constexpr double dDuration = 10.0;
                for(size_t i = 0; i < nFrames; i++)
                {
                    double dT = m_nPosition / SAMPLE_RATE;
                    double dFreq = 20.0 * pow(1000.0, dT/dDuration);    //20 Hz .. 20 kHz log
                    m_dPhase += 2.0*M_PI*dFreq/SAMPLE_RATE;
                    if(m_dPhase > 2.0*M_PI) { m_dPhase -= 2.0*M_PI; }
                    auto fSample = static_cast<float>(m_dAmplitude * sin(m_dPhase));
                    pOut[i*2] = fSample;
                    pOut[i*2+1] = fSample;
                    if(++m_nPosition >= static_cast<size_t>(dDuration*SAMPLE_RATE)) { m_nPosition = 0; }
                }
                return true;
            }

            std::string Describe() const override { return "sweep 20 Hz - 20 kHz"; }

        private:
            double m_dAmplitude;
            double m_dPhase = 0.0;
            size_t m_nPosition = 0;
    };

    class MemorySource : public Source
    {
        public:
            MemorySource(std::vector<float>&& vSamples, std::string sDescription, bool bLoop) :
                m_vSamples(std::move(vSamples)),
                m_sDescription(std::move(sDescription)),
                m_bLoop(bLoop)
            {}

            bool Read(float* pOut, size_t nFrames) override
            {
                size_t nWanted = nFrames*2;
                size_t nWritten = 0;
                while(nWritten < nWanted)
                {
                    if(m_nPosition >= m_vSamples.size())
                    {
                        if(!m_bLoop || m_vSamples.empty())
                        {
                            memset(pOut+nWritten, 0, (nWanted-nWritten)*sizeof(float));
                            return false;
                        }
                        m_nPosition = 0;
                    }
                    size_t nChunk = std::min(nWanted-nWritten, m_vSamples.size()-m_nPosition);
                    memcpy(pOut+nWritten, m_vSamples.data()+m_nPosition, nChunk*sizeof(float));
                    m_nPosition += nChunk;
                    nWritten += nChunk;
                }
                return true;
            }

            std::string Describe() const override { return m_sDescription; }

        private:
            std::vector<float> m_vSamples;
            std::string m_sDescription;
            bool m_bLoop;
            size_t m_nPosition = 0;
    };

    std::vector<float> Resample(const std::vector<float>& vIn, int nInRate)
    {
        if(nInRate == static_cast<int>(SAMPLE_RATE)) { return vIn; }
        double dRatio = SAMPLE_RATE / nInRate;
        std::vector<float> vOut(static_cast<size_t>(vIn.size()*dRatio) + 16);
        SRC_DATA data{};
        data.data_in = vIn.data();
        data.input_frames = static_cast<long>(vIn.size()/2);
        data.data_out = vOut.data();
        data.output_frames = static_cast<long>(vOut.size()/2);
        data.src_ratio = dRatio;
        if(src_simple(&data, SRC_SINC_FASTEST, 2) != 0) { return vIn; }
        vOut.resize(static_cast<size_t>(data.output_frames_gen)*2);
        return vOut;
    }

    std::vector<float> ToStereo(const float* pIn, size_t nFrames, int nChannels)
    {
        std::vector<float> vOut(nFrames*2);
        for(size_t i = 0; i < nFrames; i++)
        {
            if(nChannels == 1)
            {
                vOut[i*2] = vOut[i*2+1] = pIn[i];
            }
            else
            {
                vOut[i*2] = pIn[i*nChannels];
                vOut[i*2+1] = pIn[i*nChannels+1];
            }
        }
        return vOut;
    }
}

std::unique_ptr<Source> pan::audio::CreateTone(double dFrequencyHz, double dLevelDb)
{
    return std::make_unique<ToneSource>(dFrequencyHz, dLevelDb);
}

std::unique_ptr<Source> pan::audio::CreateSweep(double dLevelDb)
{
    return std::make_unique<SweepSource>(dLevelDb);
}

std::unique_ptr<Source> pan::audio::CreateFile(const std::string& sPath, bool bLoop)
{
    auto sName = sPath.substr(sPath.rfind('/')+1);

    if(sPath.size() > 4 && sPath.substr(sPath.size()-4) == ".mp3")
    {
        mp3dec_ex_t dec;
        if(mp3dec_ex_open(&dec, sPath.c_str(), MP3D_SEEK_TO_SAMPLE) != 0)
        {
            LOG_ERROR("audio") << "cannot open " << sPath;
            return nullptr;
        }
        std::vector<mp3d_sample_t> vPcm(dec.samples);
        size_t nRead = mp3dec_ex_read(&dec, vPcm.data(), dec.samples);
        int nChannels = dec.info.channels;
        int nRate = dec.info.hz;
        mp3dec_ex_close(&dec);

        std::vector<float> vFloat(nRead);
        for(size_t i = 0; i < nRead; i++) { vFloat[i] = vPcm[i] / 32768.0f; }
        auto vStereo = ToStereo(vFloat.data(), nRead/nChannels, nChannels);
        LOG_INFO("audio") << "loaded " << sName << " (" << nRate << " Hz, " << nChannels << "ch, "
                          << nRead/nChannels/nRate << "s)";
        return std::make_unique<MemorySource>(Resample(vStereo, nRate), sName, bLoop);
    }

    drwav wav;
    if(!drwav_init_file(&wav, sPath.c_str(), nullptr))
    {
        LOG_ERROR("audio") << "cannot open " << sPath;
        return nullptr;
    }
    std::vector<float> vFrames(wav.totalPCMFrameCount * wav.channels);
    auto nFrames = drwav_read_pcm_frames_f32(&wav, wav.totalPCMFrameCount, vFrames.data());
    int nChannels = wav.channels;
    int nRate = static_cast<int>(wav.sampleRate);
    drwav_uninit(&wav);

    auto vStereo = ToStereo(vFrames.data(), nFrames, nChannels);
    LOG_INFO("audio") << "loaded " << sName << " (" << nRate << " Hz, " << nChannels << "ch)";
    return std::make_unique<MemorySource>(Resample(vStereo, nRate), sName, bLoop);
}

std::unique_ptr<Source> pan::audio::CreateFromConfig(const nlohmann::json& jsSender, const std::string& sFilesDir)
{
    auto sSource = jsSender.value("source", "tone");
    auto dLevel = jsSender.value("tone_level_db", -18.0);
    if(sSource == "sweep")
    {
        return CreateSweep(dLevel);
    }
    if(sSource == "file")
    {
        auto sFile = jsSender.value("file", "");
        if(!sFile.empty() && sFile.find('/') == std::string::npos)
        {
            sFile = sFilesDir + "/" + sFile;
        }
        auto pSource = CreateFile(sFile, jsSender.value("loop", true));
        if(pSource) { return pSource; }
        LOG_WARN("audio") << "file source failed - falling back to tone";
    }
    return CreateTone(jsSender.value("tone_hz", 440.0), dLevel);
}
