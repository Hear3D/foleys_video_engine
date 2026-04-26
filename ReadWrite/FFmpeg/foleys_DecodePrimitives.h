#pragma once

#include <array>
#include <optional>

#include <juce_audio_basics/juce_audio_basics.h>

#if JUCE_MAC
#include <AudioToolbox/AudioToolbox.h>
#else
struct AudioChannelDescription
{
    unsigned int mChannelLabel = 0;
};

struct AudioChannelLayout
{
    unsigned int mChannelLayoutTag = 0;
    unsigned int mChannelBitmap = 0;
    unsigned int mNumberChannelDescriptions = 0;
    AudioChannelDescription mChannelDescriptions[1] {};
};
#endif

namespace foleys
{

struct DecodedAudioBlock
{
    juce::AudioBuffer<float> buffer;
    std::optional<AudioChannelLayout> layout;
};

class ChannelMapResolver
{
public:
    ChannelMapResolver();

    bool configureFromLayout (const AudioChannelLayout* layout);
    int mapToCanonical (int inputChannelIndex) const;

private:
    std::array<int, 12> mapping;
};

struct Atmos714Buffer
{
    juce::AudioBuffer<float> buffer;
};

Atmos714Buffer normalizeToAtmos714 (const DecodedAudioBlock& src, const ChannelMapResolver* resolver);

class HostBusMapper
{
public:
    void mapToHost (const Atmos714Buffer& src,
                    juce::AudioBuffer<float>& dst,
                    const juce::AudioChannelSet& layout);
};

} // namespace foleys
