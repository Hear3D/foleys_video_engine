#include "foleys_FFmpegDecodeBackend.h"

namespace foleys
{

FFmpegDecodeBackend::FFmpegDecodeBackend (AVCodecContext* contextToUse, const FoleysChannelLayout& channelLayoutToUse)
    : audioContext (contextToUse), channelLayout (channelLayoutToUse)
{
    frame = av_frame_alloc();
}

FFmpegDecodeBackend::~FFmpegDecodeBackend()
{
    if (audioConverterContext != nullptr)
        swr_free (&audioConverterContext);

    if (frame != nullptr)
        av_frame_free (&frame);
}

bool FFmpegDecodeBackend::open (const juce::File& file, double sampleRate)
{
    juce::ignoreUnused (file);

    if (audioContext == nullptr)
        return false;

    outputSampleRate = sampleRate > 0.0 ? sampleRate : audioContext->sample_rate;
    numChannels = getCodecContextChannelCount (audioContext);

    return configureResampler();
}

void FFmpegDecodeBackend::setOutputSampleRate (double sampleRate)
{
    if (sampleRate <= 0.0)
        return;

    outputSampleRate = sampleRate;
    configureResampler();
}

bool FFmpegDecodeBackend::configureResampler()
{
    if (audioContext == nullptr)
        return false;

    if (allocateResampler (&audioConverterContext,
                           channelLayout,
                           AV_SAMPLE_FMT_FLTP,
                           juce::roundToInt (outputSampleRate),
                           channelLayout,
                           audioContext->sample_fmt,
                           audioContext->sample_rate) < 0)
        return false;

    return swr_init (audioConverterContext) >= 0;
}

int FFmpegDecodeBackend::getNumChannels() const
{
    return numChannels;
}

bool FFmpegDecodeBackend::pushPacket (const AVPacket& packet)
{
    if (audioContext == nullptr || frame == nullptr || audioConverterContext == nullptr)
        return false;

    AVPacket temp = packet;
    int response = avcodec_send_packet (audioContext, &temp);

    while (response >= 0)
    {
        response = avcodec_receive_frame (audioContext, frame);

        if (response == AVERROR (EAGAIN) || response == AVERROR_EOF)
            break;

        if (response < 0)
            return false;

        if (frame->extended_data == nullptr || frame->nb_samples <= 0)
            continue;

        const int channels = getFrameChannelCount (frame);
        const int numSamples = frame->nb_samples;
        const int numProduced = int (numSamples * outputSampleRate / juce::jmax (1.0, (double) audioContext->sample_rate));

        if (audioConvertBuffer.getNumChannels() != channels || audioConvertBuffer.getNumSamples() < numProduced)
            audioConvertBuffer.setSize (channels, numProduced, false, false, true);

        swr_convert (audioConverterContext,
                     (uint8_t**) audioConvertBuffer.getArrayOfWritePointers(), numProduced,
                     (const uint8_t**) frame->extended_data, numSamples);

        DecodedAudioBlock block;
        block.buffer.setSize (channels, numProduced, false, false, true);
        for (int ch = 0; ch < channels; ++ch)
            block.buffer.copyFrom (ch, 0, audioConvertBuffer, ch, 0, numProduced);

        pendingBlocks.emplace_back (std::move (block));
    }

    return true;
}

DecodedAudioBlock FFmpegDecodeBackend::readNextBlock()
{
    if (pendingBlocks.empty())
        return {};

    auto block = std::move (pendingBlocks.front());
    pendingBlocks.pop_front();
    return block;
}

void FFmpegDecodeBackend::setPosition (int64_t samplePosition)
{
    juce::ignoreUnused (samplePosition);
    pendingBlocks.clear();
}

} // namespace foleys
