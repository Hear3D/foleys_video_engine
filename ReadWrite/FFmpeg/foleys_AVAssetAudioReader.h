#pragma once

#if defined(__APPLE__)

#include <memory>

#include "foleys_DecodePrimitives.h"

namespace foleys
{

class AVAssetAudioReader
{
public:
    AVAssetAudioReader();
    ~AVAssetAudioReader();

    bool open (const juce::File& file, double sampleRate, int requestedChannels);
    void close();

    int getNumOutputChannels() const  { return numChannels; }
    double getSampleRate() const      { return sampleRate; }
    bool isOpenedOk() const           { return opened; }

    static bool probeFile (const juce::File& file,
                           double sampleRate,
                           int requestedChannels,
                           int& outChannels,
                           double& outDurationSeconds);

    bool isAtEnd() const;
    void setPosition (int64_t samplePosition);

    DecodedAudioBlock readNextBlock();

private:
    struct Pimpl;
    std::unique_ptr<Pimpl> pimpl;

    int numChannels = 0;
    double sampleRate = 0.0;
    bool opened = false;
};

} // namespace foleys

#endif
