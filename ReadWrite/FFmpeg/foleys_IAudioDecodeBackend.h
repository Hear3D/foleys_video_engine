#pragma once

#include "foleys_DecodePrimitives.h"

extern "C"
{
#include <libavcodec/avcodec.h>
}

namespace foleys
{

class IAudioDecodeBackend
{
public:
    virtual ~IAudioDecodeBackend() = default;

    virtual bool open (const juce::File& file, double sampleRate) = 0;
    virtual DecodedAudioBlock readNextBlock() = 0;
    virtual int getNumChannels() const = 0;

    virtual bool pushPacket (const AVPacket& packet) { juce::ignoreUnused (packet); return false; }
    virtual void setPosition (int64_t samplePosition) { juce::ignoreUnused (samplePosition); }
    virtual void setOutputSampleRate (double sampleRate) { juce::ignoreUnused (sampleRate); }
    virtual bool isPullBased() const { return false; }
};

} // namespace foleys
