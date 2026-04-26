#pragma once

#if JUCE_MAC

#include <array>
#include <deque>
#include <vector>

#include <AudioToolbox/AudioToolbox.h>

#include "foleys_IAudioDecodeBackend.h"

namespace foleys
{

/**
 * CoreAudioEAC3Backend — decodes E-AC-3 / JOC packets using Apple's
 * AudioConverter (kAudioFormatEnhancedAC3 → 12-ch float PCM).
 *
 * The backend is push-based: FFmpegReader demuxes raw EAC3 packets and
 * pushes them via pushPacket(); readNextBlock() drains decoded PCM.
 */
class CoreAudioEAC3Backend final : public IAudioDecodeBackend
{
public:
    /**
     * @param inChannels     channel count reported by the EAC3 stream (typically 6 for 5.1 bed).
     * @param inSampleRate   sample rate of the EAC3 stream (typically 48000).
     */
    CoreAudioEAC3Backend (int inChannels, int inSampleRate);
    ~CoreAudioEAC3Backend() override;

    bool open (const juce::File& file, double sampleRate) override;
    DecodedAudioBlock readNextBlock() override;
    int getNumChannels() const override;
    bool pushPacket (const AVPacket& packet) override;
    void setPosition (int64_t samplePosition) override;
    void setOutputSampleRate (double sampleRate) override;

private:
    // Instance-method callback: serves one queued packet per AT invocation.
    // Returns a non-zero OSStatus when the queue is empty — this prevents AT
    // from entering EOS state and discarding JOC decoder state.
    static OSStatus inputDataCallback (AudioConverterRef,
                                       UInt32* ioNumberDataPackets,
                                       AudioBufferList* ioData,
                                       AudioStreamPacketDescription** outPacketDesc,
                                       void* inUserData);

    OSStatus provideNextPacket (UInt32* ioNumberDataPackets,
                                AudioBufferList* ioData,
                                AudioStreamPacketDescription** outPacketDesc);

    int inputChannels = 6;
    int inputSampleRate = 48000;
    int outputChannels = 12;

    AudioConverterRef converter = nullptr;

    // Maps AT output channel index → canonical 7.1.4 index (0-11).
    // Built from AudioConverter output layout in open(); -1 means drop.
    std::array<int, 12> atToCanonical {};

    struct PacketData
    {
        std::vector<uint8_t> data;
    };

    // Queue fed by pushPacket(); drained by FillComplexBuffer callback.
    std::deque<PacketData> pendingPackets;

    // Packet currently being served to AT — must remain valid during the callback.
    PacketData currentServedPacket;

    // Used as the packet description handed to AT.
    AudioStreamPacketDescription currentPacketDesc {};

    static constexpr int kMaxOutputFrames = 2048;

    int debugBlocksLogged = 0;
};

} // namespace foleys

#endif // JUCE_MAC
