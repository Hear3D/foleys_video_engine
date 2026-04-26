#pragma once

#include "foleys_IAudioDecodeBackend.h"

#if JUCE_MAC
#include "foleys_AVAssetAudioReader.h"
#endif

namespace foleys
{

class AVFoundationDecodeBackend final : public IAudioDecodeBackend
{
public:
    bool open (const juce::File& file, double sampleRate) override;
    DecodedAudioBlock readNextBlock() override;
    int getNumChannels() const override;
    void setPosition (int64_t samplePosition) override;
    bool isPullBased() const override { return true; }

private:
#if JUCE_MAC
    AVAssetAudioReader reader;
#endif
    int channels = 0;
};

} // namespace foleys
