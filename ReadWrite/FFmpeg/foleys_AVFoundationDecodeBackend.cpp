#include "foleys_AVFoundationDecodeBackend.h"

namespace foleys
{

bool AVFoundationDecodeBackend::open (const juce::File& file, double sampleRate)
{
#if JUCE_MAC
    if (! reader.open (file, sampleRate, 12))
        return false;

    channels = reader.getNumOutputChannels();
    return true;
#else
    juce::ignoreUnused (file, sampleRate);
    return false;
#endif
}

DecodedAudioBlock AVFoundationDecodeBackend::readNextBlock()
{
#if JUCE_MAC
    return reader.readNextBlock();
#else
    return {};
#endif
}

int AVFoundationDecodeBackend::getNumChannels() const
{
    return channels;
}

void AVFoundationDecodeBackend::setPosition (int64_t samplePosition)
{
#if JUCE_MAC
    reader.setPosition (samplePosition);
#else
    juce::ignoreUnused (samplePosition);
#endif
}

} // namespace foleys
