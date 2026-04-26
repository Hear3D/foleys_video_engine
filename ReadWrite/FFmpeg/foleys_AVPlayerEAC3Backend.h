#pragma once

#if defined(__APPLE__)

#include <memory>

#include "foleys_IAudioDecodeBackend.h"
#include "foleys_AVPlayerEAC3BackendImpl.h"  // declares AVPlayerEAC3BackendImpl + bridge fns

namespace foleys
{

/**
 * AVPlayerEAC3Backend — decodes Dolby Atmos / E-AC-3 JOC using AVPlayer
 * with an MTAudioProcessingTap.
 *
 * Unlike CoreAudioEAC3Backend (which uses a raw AudioConverter and only
 * delivers the 5.1 base layer), this backend routes decoding through
 * AVPlayer's render graph, which fully resolves JOC objects into 12
 * discrete channels (7.1.4: L R C LFE Lss Rss Lrs Rrs Ltf Rtf Ltr Rtr).
 *
 * Architecture
 * ────────────
 * • AVPlayer plays the file internally at real-time speed.
 * • MTAudioProcessingTap (PostEffects) intercepts the rendered 12-ch PCM
 *   and writes it into a lock-free JUCE ring buffer (AbstractFifo).
 * • readNextBlock() is pull-based: it spins briefly then drains the ring.
 * • setPosition() seeks AVPlayer and flushes the ring.
 * • isPullBased() returns true so FFmpegReader bypasses the demux/push path.
 *
 * Channel order
 * ─────────────
 * The tap receives Apple's speaker-mix order:
 *   0  L     1  R     2  C     3  LFE
 *   4  Ls    5  Rs    6  Lrs   7  Rrs
 *   8  Ltf   9  Rtf  10  Ltr  11  Rtr
 * This already matches the canonical 7.1.4 order used by normalizeToAtmos714(),
 * so no channel remapping is required.
 *
 * Implementation note
 * ───────────────────
 * All Objective-C / AVFoundation / MediaToolbox code lives in
 * foleys_AVPlayerEAC3BackendImpl.mm via an opaque AVPlayerEAC3BackendImpl
 * handle.  This file and its .cpp are pure C++ with no Apple SDK includes,
 * which avoids the AVMediaType typedef clash between FFmpeg and AVFoundation.
 */
class AVPlayerEAC3Backend final : public IAudioDecodeBackend
{
public:
    explicit AVPlayerEAC3Backend (int inSampleRate);
    ~AVPlayerEAC3Backend() override;

    bool open (const juce::File& file, double sampleRate) override;
    DecodedAudioBlock readNextBlock() override;
    int getNumChannels() const override;
    void setPosition (int64_t samplePosition) override;
    void setOutputSampleRate (double) override {}
    bool isPullBased() const override { return true; }

private:
    AVPlayerEAC3BackendImpl* impl = nullptr;  // defined in foleys_AVPlayerEAC3BackendImpl.mm
    int outputSampleRate = 48000;
};

} // namespace foleys

#endif // __APPLE__
