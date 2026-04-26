#pragma once

#include <deque>

#include "foleys_IAudioDecodeBackend.h"
#include "foleys_FFmpegHelpers.h"

namespace foleys
{

class FFmpegDecodeBackend final : public IAudioDecodeBackend
{
public:
    FFmpegDecodeBackend (AVCodecContext* contextToUse, const FoleysChannelLayout& channelLayoutToUse);
    ~FFmpegDecodeBackend() override;

    bool open (const juce::File& file, double sampleRate) override;
    DecodedAudioBlock readNextBlock() override;
    int getNumChannels() const override;
    bool pushPacket (const AVPacket& packet) override;
    void setOutputSampleRate (double sampleRate) override;
    void setPosition (int64_t samplePosition) override;

private:
    bool configureResampler();

    AVCodecContext* audioContext = nullptr;
    FoleysChannelLayout channelLayout {};
    SwrContext* audioConverterContext = nullptr;

    double outputSampleRate = 0.0;
    int numChannels = 0;

    juce::AudioBuffer<float> audioConvertBuffer;
    AVFrame* frame = nullptr;

    std::deque<DecodedAudioBlock> pendingBlocks;
};

} // namespace foleys
