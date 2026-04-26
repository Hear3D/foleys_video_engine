#include "foleys_AVPlayerEAC3Backend.h"

#if defined(__APPLE__)

namespace foleys
{

AVPlayerEAC3Backend::AVPlayerEAC3Backend (int inSampleRate)
    : outputSampleRate (inSampleRate),
      impl (AVPlayerEAC3BackendImpl_create (inSampleRate))
{
}

AVPlayerEAC3Backend::~AVPlayerEAC3Backend()
{
    AVPlayerEAC3BackendImpl_destroy (impl);
    impl = nullptr;
}

bool AVPlayerEAC3Backend::open (const juce::File& file, double sampleRate)
{
    outputSampleRate = (int) sampleRate;
    return AVPlayerEAC3BackendImpl_open (impl, file, sampleRate);
}

DecodedAudioBlock AVPlayerEAC3Backend::readNextBlock()
{
    return AVPlayerEAC3BackendImpl_readNextBlock (impl);
}

int AVPlayerEAC3Backend::getNumChannels() const
{
    return AVPlayerEAC3BackendImpl_getNumChannels (impl);
}

void AVPlayerEAC3Backend::setPosition (int64_t samplePosition)
{
    AVPlayerEAC3BackendImpl_setPosition (impl, samplePosition, outputSampleRate);
}

} // namespace foleys

#endif // __APPLE__
