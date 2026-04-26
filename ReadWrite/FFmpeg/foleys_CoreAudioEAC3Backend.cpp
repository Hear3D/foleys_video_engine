#include "foleys_CoreAudioEAC3Backend.h"

#if JUCE_MAC

#include <AudioToolbox/AudioToolbox.h>
#include <cstddef>
#include <cstring>
#include <vector>
#include <array>
#include <cmath>

#define TX_TRACE_THIS_FILE 1
#include "../../../../libraries/DebugTraces/src/DebugTracesLib.h"

namespace foleys
{



// ---------------------------------------------------------------------------
// Label → canonical index (mirrors DecodePrimitives but accessible here)
// ---------------------------------------------------------------------------

static constexpr int kInvalidCh = -1;

static int labelToCanonical (AudioChannelLabel label)
{
    switch (label)
    {
        case kAudioChannelLabel_Left:                return 0;   // L
        case kAudioChannelLabel_Right:               return 1;   // R
        case kAudioChannelLabel_Center:              return 2;   // C
        case kAudioChannelLabel_LFEScreen:           return 3;   // LFE
        // Side surrounds (various label names used by different encoders)
        case kAudioChannelLabel_LeftSurroundDirect:  return 4;   // Lss
        case kAudioChannelLabel_RightSurroundDirect: return 5;   // Rss
        case kAudioChannelLabel_LeftSurround:        return 4;   // Ls → Lss
        case kAudioChannelLabel_RightSurround:       return 5;   // Rs → Rss
        // Rear surrounds
        case kAudioChannelLabel_RearSurroundLeft:    return 6;   // Lrs
        case kAudioChannelLabel_RearSurroundRight:   return 7;   // Rrs
        // Height front
        case kAudioChannelLabel_VerticalHeightLeft:  return 8;   // Ltf
        case kAudioChannelLabel_VerticalHeightRight: return 9;   // Rtf
        // Height rear
        case kAudioChannelLabel_TopBackLeft:         return 10;  // Ltr
        case kAudioChannelLabel_TopBackRight:        return 11;  // Rtr
        default:                                     return kInvalidCh;
    }
}

// Build an AT-output-index → canonical-index remap from a (possibly tag-based) layout.
// Falls back to Dolby order remap for 6ch/12ch when labels are unavailable.
static std::array<int, 12> buildAtToCanonicalRemap (UInt32 layoutTag,
                                                      const AudioChannelLayout* layoutPtr,
                                                      int numCh)
{
    std::array<int, 12> remap;
    for (int i = 0; i < 12; ++i)
        remap[(size_t) i] = i;

    if (numCh <= 0 || numCh > 12)
        return remap;

    // Dolby output order frequently observed from AudioConverter for EAC3/JOC:
    // 6ch:  L R C Ls Rs LFE
    // 12ch: L R C Ls Rs LFE Lrs Rrs Ltf Rtf Ltr Rtr
    // Canonical order we want:
    //       L R C LFE Lss Rss Lrs Rrs Ltf Rtf Ltr Rtr
    // AT-index -> canonical-index
    if (numCh == 6)
    {
        remap = { 0, 1, 2, 4, 5, 3, 6, 7, 8, 9, 10, 11 };
    }
    else if (numCh == 12)
    {
        remap = { 0, 1, 2, 4, 5, 3, 6, 7, 8, 9, 10, 11 };
    }

    // Try to get per-channel descriptions for the layout tag.
    std::vector<uint8_t> expandedBuf;
    const AudioChannelLayout* useLayout = layoutPtr;

    if (layoutTag != kAudioChannelLayoutTag_UseChannelDescriptions
        && layoutTag != kAudioChannelLayoutTag_UseChannelBitmap)
    {
        UInt32 expandedSize = 0;
        if (AudioFormatGetPropertyInfo (kAudioFormatProperty_ChannelLayoutForTag,
                                        sizeof (layoutTag),
                                        &layoutTag,
                                        &expandedSize) == noErr
            && expandedSize >= sizeof (AudioChannelLayout))
        {
            expandedBuf.resize (expandedSize, 0);
            auto* p = reinterpret_cast<AudioChannelLayout*> (expandedBuf.data());
            if (AudioFormatGetProperty (kAudioFormatProperty_ChannelLayoutForTag,
                                         sizeof (layoutTag),
                                         &layoutTag,
                                         &expandedSize,
                                         p) == noErr)
            {
                useLayout = p;
            }
        }
    }

    if (useLayout == nullptr
        || useLayout->mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelDescriptions
        || useLayout->mNumberChannelDescriptions == 0)
    {
        return remap;  // couldn't expand; keep Dolby fallback remap
    }

    const int descCount = juce::jmin ((int) useLayout->mNumberChannelDescriptions, numCh);
    for (int atIdx = 0; atIdx < descCount; ++atIdx)
    {
        const int canonical = labelToCanonical (
            useLayout->mChannelDescriptions[(UInt32) atIdx].mChannelLabel);
        if (canonical != kInvalidCh)
            remap[(size_t) atIdx] = canonical;
    }

    return remap;
}

// ---------------------------------------------------------------------------
// Static callback — routes to instance method provideNextPacket()
// ---------------------------------------------------------------------------

OSStatus CoreAudioEAC3Backend::inputDataCallback (AudioConverterRef,
                                                   UInt32*                        ioNumberDataPackets,
                                                   AudioBufferList*               ioData,
                                                   AudioStreamPacketDescription** outPacketDesc,
                                                   void*                          inUserData)
{
    auto* self = static_cast<CoreAudioEAC3Backend*> (inUserData);
    return self->provideNextPacket (ioNumberDataPackets, ioData, outPacketDesc);
}

// ---------------------------------------------------------------------------
// Instance-method packet provider — serves one queued packet per AT invocation.
// Signals EOS with noErr + zero packets when the queue is empty so AT properly
// flushes its internal decode buffer before returning from FillComplexBuffer.
// The converter is reset at the top of readNextBlock() to clear EOS state.
// ---------------------------------------------------------------------------

OSStatus CoreAudioEAC3Backend::provideNextPacket (UInt32*                        ioNumberDataPackets,
                                                   AudioBufferList*               ioData,
                                                   AudioStreamPacketDescription** outPacketDesc)
{
    if (pendingPackets.empty())
    {
        *ioNumberDataPackets = 0;
        ioData->mBuffers[0].mDataByteSize = 0;
        ioData->mBuffers[0].mData         = nullptr;
        return noErr;  // EOS signal: AT will flush decoded frames and stop
    }

    currentServedPacket = std::move (pendingPackets.front());
    pendingPackets.pop_front();

    *ioNumberDataPackets = 1;
    ioData->mBuffers[0].mNumberChannels = 0;
    ioData->mBuffers[0].mDataByteSize   = (UInt32) currentServedPacket.data.size();
    ioData->mBuffers[0].mData           = currentServedPacket.data.data();

    if (outPacketDesc != nullptr)
    {
        currentPacketDesc.mStartOffset            = 0;
        currentPacketDesc.mVariableFramesInPacket = 0;
        currentPacketDesc.mDataByteSize           = (UInt32) currentServedPacket.data.size();
        *outPacketDesc = &currentPacketDesc;
    }

    return noErr;
}

// ---------------------------------------------------------------------------

CoreAudioEAC3Backend::CoreAudioEAC3Backend (int inChannels, int inSampleRate)
    : inputChannels (inChannels), inputSampleRate (inSampleRate)
{
}

CoreAudioEAC3Backend::~CoreAudioEAC3Backend()
{
    if (converter != nullptr)
    {
        AudioConverterDispose (converter);
        converter = nullptr;
    }
}

bool CoreAudioEAC3Backend::open (const juce::File&, double)
{
    if (converter != nullptr)
    {
        AudioConverterDispose (converter);
        converter = nullptr;
    }

    // Identity until we successfully query the layout.
    for (int i = 0; i < 12; ++i)
        atToCanonical[(size_t) i] = i;

    LOGI("CoreAudioEAC3Backend::open inChannels=%d inSampleRate=%d", inputChannels, inputSampleRate);

    AudioStreamBasicDescription inASBD = {};
    inASBD.mFormatID         = kAudioFormatEnhancedAC3;
    inASBD.mSampleRate       = inputSampleRate;
    inASBD.mChannelsPerFrame = (UInt32) inputChannels;
    inASBD.mFramesPerPacket  = 1536;
    inASBD.mBytesPerPacket   = 0;

    AudioStreamBasicDescription outASBD = {};
    outASBD.mFormatID         = kAudioFormatLinearPCM;
    outASBD.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
                              | kAudioFormatFlagIsNonInterleaved;
    outASBD.mSampleRate       = inputSampleRate;
    outASBD.mChannelsPerFrame = 12;
    outASBD.mBitsPerChannel   = 32;
    outASBD.mBytesPerFrame    = 4;
    outASBD.mFramesPerPacket  = 1;
    outASBD.mBytesPerPacket   = 4;

    OSStatus err = AudioConverterNew (&inASBD, &outASBD, &converter);
    if (err != noErr || converter == nullptr)
        return false;

    // Confirm the channel count the converter resolved (should be 12 for JOC).
    UInt32 sz = sizeof (outASBD);
    AudioConverterGetProperty (converter,
                               kAudioConverterCurrentOutputStreamDescription,
                               &sz,
                               &outASBD);
    outputChannels = (int) outASBD.mChannelsPerFrame;
    LOGI("resolved outputChannels=%d sampleRate=%d", outputChannels, (int) outASBD.mSampleRate);

    // Query the output channel layout and build an AT-index → canonical-index remap.
    // If layout query fails, we still build a deterministic fallback remap.
    UInt32 layoutSize = 0;
    bool gotLayout = false;
    if (AudioConverterGetPropertyInfo (converter,
                                       kAudioConverterOutputChannelLayout,
                                       &layoutSize,
                                       nullptr) == noErr
        && layoutSize >= sizeof (AudioChannelLayout))
    {
        std::vector<uint8_t> layoutBuf (layoutSize, 0);
        auto* layoutPtr = reinterpret_cast<AudioChannelLayout*> (layoutBuf.data());

        if (AudioConverterGetProperty (converter,
                                       kAudioConverterOutputChannelLayout,
                                       &layoutSize,
                                       layoutPtr) == noErr)
        {
            LOGD("layout tag=0x%x bitmap=0x%x descCount=%d",
                 (unsigned) layoutPtr->mChannelLayoutTag,
                 (unsigned) layoutPtr->mChannelBitmap,
                 (int) layoutPtr->mNumberChannelDescriptions);
            atToCanonical = buildAtToCanonicalRemap (layoutPtr->mChannelLayoutTag,
                                                     layoutPtr,
                                                     outputChannels);
            gotLayout = true;
        }
    }

    if (! gotLayout)
    {
        // No layout metadata available from AudioConverter on this system/path.
        // Use deterministic Dolby fallback mapping (e.g. L,R,C,Ls,Rs,LFE,...).
        atToCanonical = buildAtToCanonicalRemap (0, nullptr, outputChannels);
        LOGD("layout unavailable -> using fallback Dolby remap");
    }

    LOGI("atToCanonical: %d->%d %d->%d %d->%d %d->%d %d->%d %d->%d %d->%d %d->%d %d->%d %d->%d %d->%d %d->%d",
         0, atToCanonical[0],  1, atToCanonical[1],  2, atToCanonical[2],
         3, atToCanonical[3],  4, atToCanonical[4],  5, atToCanonical[5],
         6, atToCanonical[6],  7, atToCanonical[7],  8, atToCanonical[8],
         9, atToCanonical[9], 10, atToCanonical[10], 11, atToCanonical[11]);

    return outputChannels >= 6;
}

int CoreAudioEAC3Backend::getNumChannels() const
{
    return outputChannels;
}

bool CoreAudioEAC3Backend::pushPacket (const AVPacket& packet)
{
    if (packet.data == nullptr || packet.size <= 0)
        return false;

    PacketData pd;
    pd.data.assign (packet.data, packet.data + packet.size);
    pendingPackets.push_back (std::move (pd));
    return true;
}

DecodedAudioBlock CoreAudioEAC3Backend::readNextBlock()
{
    if (converter == nullptr || pendingPackets.empty())
        return {};

    const int numCh = outputChannels;

    // Allocate per-channel output buffers
    std::vector<std::vector<float>> chBufs ((size_t) numCh,
                                            std::vector<float> ((size_t) kMaxOutputFrames, 0.0f));

    const size_t ablSize = offsetof (AudioBufferList, mBuffers)
                           + (size_t) numCh * sizeof (AudioBuffer);
    std::vector<uint8_t> ablStore (ablSize, 0);
    auto* abl = reinterpret_cast<AudioBufferList*> (ablStore.data());
    abl->mNumberBuffers = (UInt32) numCh;

    for (int ch = 0; ch < numCh; ++ch)
    {
        abl->mBuffers[ch].mNumberChannels = 1;
        abl->mBuffers[ch].mDataByteSize   = (UInt32) (kMaxOutputFrames * sizeof (float));
        abl->mBuffers[ch].mData           = chBufs[(size_t) ch].data();
    }

    UInt32 frameCount = (UInt32) kMaxOutputFrames;

    // Reset the converter before each packet to clear the EOS state set by the
    // previous call's noErr+0 callback return.  The probe tool uses this same
    // pattern; JOC metadata is re-established from the very first packet after
    // a reset, so this does not degrade height-channel decoding.
    AudioConverterReset (converter);

    const OSStatus err = AudioConverterFillComplexBuffer (converter,
                                                          inputDataCallback,
                                                          this,   // userdata = this
                                                          &frameCount,
                                                          abl,
                                                          nullptr);

    if (frameCount == 0)
        return {};

    // Log err + frameCount once at block 10 for diagnostics.
    if (debugBlocksLogged == 9)
        LOGD("blk10 err=%d frameCount=%u", (int)err, (unsigned)frameCount);

    // Apply the AT-output → canonical remap directly here so that the block
    // arrives at pushDecodedBlock already in canonical channel order.
    // We do NOT set block.layout — the ChannelMapResolver will use identity.
    DecodedAudioBlock block;
    block.buffer.setSize (numCh, (int) frameCount, false, false, true);
    block.buffer.clear();

    for (int atCh = 0; atCh < numCh; ++atCh)
    {
        const int canonical = atToCanonical[(size_t) atCh];
        if (canonical >= 0 && canonical < numCh)
            block.buffer.copyFrom (canonical, 0, chBufs[(size_t) atCh].data(), (int) frameCount);
    }

    ++debugBlocksLogged;
    if (debugBlocksLogged >= 10 && debugBlocksLogged < 30)
    {
        float peaks[12] = {};
        for (int ch = 0; ch < numCh && ch < 12; ++ch)
            for (UInt32 i = 0; i < frameCount; ++i)
                peaks[ch] = juce::jmax (peaks[ch], std::abs (chBufs[(size_t) ch][(size_t) i]));

        LOGD("blk%d raw: L=%.4f R=%.4f C=%.4f Ls=%.4f Rs=%.4f LFE=%.4f Lrs=%.4f Rrs=%.4f Ltf=%.4f Rtf=%.4f Ltr=%.4f Rtr=%.4f",
             debugBlocksLogged,
             peaks[0], peaks[1], peaks[2], peaks[3], peaks[4], peaks[5],
             peaks[6], peaks[7], peaks[8], peaks[9], peaks[10], peaks[11]);
    }

    return block;
}

void CoreAudioEAC3Backend::setPosition (int64_t)
{
    // On seek: flush decoder state and clear the packet queue.
    if (converter != nullptr)
        AudioConverterReset (converter);

    pendingPackets.clear();
    currentServedPacket.data.clear();
}

void CoreAudioEAC3Backend::setOutputSampleRate (double)
{
    // The CoreAudio converter outputs at the stream's native sample rate.
    // Any sample-rate conversion is handled upstream by the FFmpegReader resampler.
}

} // namespace foleys

#endif // JUCE_MAC
