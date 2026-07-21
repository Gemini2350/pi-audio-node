#pragma once
#include <atomic>
#include <mutex>
#include <string>

typedef struct _snd_pcm snd_pcm_t;

namespace pan::audio
{
    /** ALSA playback, 48 kHz stereo. Write() blocks until the frames are in the
    *   device buffer; DelayFrames() reports how much audio is queued so the
    *   receiver can steer its playout point.
    **/
    class AlsaOut
    {
        public:
            ~AlsaOut();

            bool Open(const std::string& sDevice);
            void Close();
            bool IsOpen() const { return m_pPcm != nullptr; }

            bool Write(const float* pInterleaved, size_t nFrames, double dGainDb);
            long DelayFrames();
            uint64_t Underruns() const { return m_nUnderruns.load(); }

        private:
            std::mutex m_mutex;
            snd_pcm_t* m_pPcm = nullptr;
            std::atomic<uint64_t> m_nUnderruns{0};
    };
}
