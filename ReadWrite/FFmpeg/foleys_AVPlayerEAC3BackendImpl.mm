// ObjC++ implementation of AVPlayerEAC3Backend.
// This file MUST NOT include any FFmpeg headers to avoid the AVMediaType
// typedef clash between libavutil and <AVFoundation/AVFoundation.h>.

#if defined(__APPLE__)

// Include the bridge header FIRST (JUCE + DecodePrimitives, no FFmpeg).
// This pulls in <juce_audio_basics/juce_audio_basics.h> which defines JUCE_MAC.
#include "foleys_AVPlayerEAC3BackendImpl.h"

// After JUCE (and its AudioToolbox include via DecodePrimitives), include the
// Apple multimedia frameworks.  FFmpeg is never included here, so AVMediaType
// from AVFoundation is the first and only definition → no conflict.
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <MediaToolbox/MediaToolbox.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#define TX_TRACE_THIS_FILE 1
#include "../../../../libraries/DebugTraces/src/DebugTracesLib.h"

namespace foleys
{

// ============================================================================
// AVPlayerEAC3BackendImpl — holds all Obj-C objects + ring buffer
// ============================================================================

struct AVPlayerEAC3BackendImpl
{
    // ---- Obj-C objects ----
    // NOTE: ARC does NOT manage __strong on C++ struct members — they are always
    // __unsafe_unretained.  We use explicit CFRetain/CFRelease in openFile() /
    // releaseAll() to keep these alive for the lifetime of the struct.
    AVPlayer*      player  = nil;
    AVPlayerItem*  item    = nil;
    AVURLAsset*    asset   = nil;
    MTAudioProcessingTapRef tap     = nullptr;

    // ---- Ring buffer ----
    static constexpr int kMaxCh        = 12;
    static constexpr int kRingSeconds  = 4;

    juce::AbstractFifo              fifo    { 1 };  // resized in initRing
    std::vector<std::vector<float>> ring;           // ring[ch][sample]
    int                             ringSamples   = 0;
    int                             outputChannels = kMaxCh;
    int                             outputSampleRate = 48000;
    std::atomic<bool>               flushing { false };
    bool                            hasStartedPlaying { false };

    // ---- Init / teardown ----

    void releaseAll()
    {
        AVPlayer* localPlayer = player;
        AVPlayerItem* localItem = item;
        AVURLAsset* localAsset = asset;
        MTAudioProcessingTapRef localTap = tap;

        player = nil;
        item = nil;
        asset = nil;
        tap = nullptr;
        hasStartedPlaying = false;

        auto releaseBlock = ^
        {
            if (localPlayer != nil)
            {
                [localPlayer pause];
                [localPlayer replaceCurrentItemWithPlayerItem:nil];
            }

            if (localItem != nil)
            {
                [localItem cancelPendingSeeks];
                localItem.audioMix = nil;
            }

            if (localTap != nullptr)
                CFRelease (localTap);

            if (localItem != nil)
                CFRelease ((__bridge CFTypeRef) localItem);

            if (localAsset != nil)
                CFRelease ((__bridge CFTypeRef) localAsset);

            if (localPlayer != nil)
                CFRelease ((__bridge CFTypeRef) localPlayer);
        };

        if ([NSThread isMainThread])
            releaseBlock();
        else
            dispatch_sync (dispatch_get_main_queue(), releaseBlock);
    }

    ~AVPlayerEAC3BackendImpl() { releaseAll(); }

    void initRing (int numCh, int sampleRate)
    {
        const int samples    = sampleRate * kRingSeconds;
        outputChannels       = numCh;
        outputSampleRate     = sampleRate;
        ringSamples          = samples;
        ring.assign ((size_t) numCh, std::vector<float> ((size_t) samples, 0.0f));
        fifo.setTotalSize (samples);
        fifo.reset();
    }

    void flushRing()
    {
        flushing.store (true,  std::memory_order_release);
        fifo.reset();
        flushing.store (false, std::memory_order_release);
    }

    // ---- Tap callbacks ----

    void tapPrepare (int numChannels, double rate)
    {
        LOGI ("AVPlayerEAC3Backend tapPrepare numCh=%d rate=%.0f", numChannels, rate);
        initRing (numChannels, (int) rate);
    }

    void tapUnprepare()
    {
        LOGI ("AVPlayerEAC3Backend tapUnprepare");
        flushRing();
    }

    void tapProcess (const float* const* channelPtrs, int numFrames)
    {
        if (flushing.load (std::memory_order_acquire) || ringSamples == 0)
            return;

        const int nch = outputChannels;
        if (fifo.getFreeSpace() < numFrames)
            return;  // ring full — drop to avoid blocking

        int start1, size1, start2, size2;
        fifo.prepareToWrite (numFrames, start1, size1, start2, size2);
        if (size1 + size2 == 0) return;

        for (int c = 0; c < std::min (nch, kMaxCh); ++c)
        {
            if (channelPtrs[c] == nullptr) continue;
            auto* dst = ring[(size_t) c].data();
            if (size1 > 0)
                std::memcpy (dst + start1, channelPtrs[c],        (size_t) size1 * sizeof (float));
            if (size2 > 0)
                std::memcpy (dst,          channelPtrs[c] + size1, (size_t) size2 * sizeof (float));
        }

        fifo.finishedWrite (size1 + size2);
    }

    // ---- PCM pull ----

    DecodedAudioBlock pullBlock()
    {
        static constexpr int kBlockFrames = 1024;

        // Start playback the first time audio is actually requested.
        // AVPlayer::play must be called on the main thread; dispatch async so
        // we don't block the reading thread.
        if (player != nil && ! hasStartedPlaying)
        {
            hasStartedPlaying = true;  // set BEFORE dispatch to prevent double-call
            __strong AVPlayer* p = player;
            dispatch_async (dispatch_get_main_queue(), ^{
                if (p.currentItem != nil)
                    [p play];
            });
            LOGI ("AVPlayerEAC3Backend starting playback (dispatched to main thread)");
        }

        // Return immediately — do NOT sleep here.  pullBlock() is called from
        // the same TimeSlice thread that decodes video packets; any blocking
        // stalls video rendering.  The AVPlayer tap fills the ring buffer
        // asynchronously, so we just return whatever is ready right now.
        const int available = fifo.getNumReady();
        if (available < kBlockFrames / 4)
            return {};

        const int toRead = std::min (available, kBlockFrames);
        const int nch    = outputChannels;

        int start1, size1, start2, size2;
        fifo.prepareToRead (toRead, start1, size1, start2, size2);
        const int total = size1 + size2;

        DecodedAudioBlock out;
        out.buffer.setSize (nch, total, false, false, true);
        out.buffer.clear();

        for (int c = 0; c < std::min (nch, kMaxCh); ++c)
        {
            const auto* src = ring[(size_t) c].data();
            auto*       dst = out.buffer.getWritePointer (c);
            if (size1 > 0)
                juce::FloatVectorOperations::copy (dst,         src + start1, size1);
            if (size2 > 0)
                juce::FloatVectorOperations::copy (dst + size1, src,          size2);
        }

        fifo.finishedRead (total);
        return out;
    }

    // ---- Open (creates AVPlayer + tap) ----

    bool openFile (const juce::File& file, double sampleRate)
    {
        @autoreleasepool
        {
            releaseAll();

            NSString* pathNS = [NSString stringWithUTF8String:file.getFullPathName().toRawUTF8()];
            NSURL* url = [NSURL fileURLWithPath:pathNS];

            asset = [AVURLAsset URLAssetWithURL:url options:nil];
            if (! asset) return false;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            NSArray<AVAssetTrack*>* tracks = [asset tracksWithMediaType:AVMediaTypeAudio];
#pragma clang diagnostic pop

            if (! tracks || tracks.count == 0) return false;
            AVAssetTrack* track = tracks.firstObject;

            // ------ Create processing tap ------
            MTAudioProcessingTapCallbacks cbs;
            cbs.version    = kMTAudioProcessingTapCallbacksVersion_0;
            cbs.clientInfo = this;  // AVPlayerEAC3BackendImpl*
            cbs.init       = [] (MTAudioProcessingTapRef t, void* ci, void** out) { *out = ci; };
            cbs.finalize   = [] (MTAudioProcessingTapRef) {};
            cbs.prepare    = [] (MTAudioProcessingTapRef t, CMItemCount,
                                  const AudioStreamBasicDescription* fmt)
            {
                auto* s = static_cast<AVPlayerEAC3BackendImpl*> (MTAudioProcessingTapGetStorage (t));
                s->tapPrepare ((int) fmt->mChannelsPerFrame, (double) fmt->mSampleRate);
            };
            cbs.unprepare  = [] (MTAudioProcessingTapRef t)
            {
                auto* s = static_cast<AVPlayerEAC3BackendImpl*> (MTAudioProcessingTapGetStorage (t));
                s->tapUnprepare();
            };
            cbs.process    = [] (MTAudioProcessingTapRef t,
                                  CMItemCount            numFrames,
                                  MTAudioProcessingTapFlags,
                                  AudioBufferList*       bufList,
                                  CMItemCount*           framesOut,
                                  MTAudioProcessingTapFlags* flagsOut)
            {
                CMItemCount rendered = 0;
                MTAudioProcessingTapGetSourceAudio (t, numFrames, bufList,
                                                    flagsOut, nullptr, &rendered);
                if (rendered <= 0) return;

                auto* s   = static_cast<AVPlayerEAC3BackendImpl*> (MTAudioProcessingTapGetStorage (t));
                const int nch = (int) bufList->mNumberBuffers;
                const float* ptrs[kMaxCh] = {};
                for (int c = 0; c < std::min (nch, kMaxCh); ++c)
                    ptrs[c] = reinterpret_cast<const float*> (bufList->mBuffers[c].mData);
                s->tapProcess (ptrs, (int) rendered);
                *framesOut = rendered;
            };

            OSStatus st = MTAudioProcessingTapCreate (kCFAllocatorDefault,
                                                       &cbs,
                                                       kMTAudioProcessingTapCreationFlag_PostEffects,
                                                       &tap);
            if (st != noErr || ! tap)
            {
                LOGE ("MTAudioProcessingTapCreate failed: %d", (int) st);
                return false;
            }

            // ------ Wire tap into audio mix ------
            AVMutableAudioMixInputParameters* params =
                [AVMutableAudioMixInputParameters audioMixInputParametersWithTrack:track];
            params.audioTapProcessor = tap;
            AVMutableAudioMix* mix = [AVMutableAudioMix audioMix];
            mix.inputParameters = @[params];

            // ------ Create player item & player ------
            item = [AVPlayerItem playerItemWithAsset:asset];
            if (! item) return false;

            item.audioMix = mix;
            player = [AVPlayer playerWithPlayerItem:item];
            if (! player) return false;

            // Manually retain all three ObjC objects.  ARC does NOT honour
            // __strong on C++ struct members, so without this they would be
            // released when the @autoreleasepool below exits.
            CFRetain ((__bridge CFTypeRef) asset);
            CFRetain ((__bridge CFTypeRef) item);
            CFRetain ((__bridge CFTypeRef) player);

            player.volume = 0.0f;  // audio goes to tap only

            // Wait up to 5 s for the item to become ready.
            NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:5.0];
            while (item.status == AVPlayerItemStatusUnknown
                   && [deadline timeIntervalSinceNow] > 0.0)
                [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];

            if (item.status == AVPlayerItemStatusFailed)
            {
                LOGE ("AVPlayerItem failed: %s",
                      item.error.localizedDescription.UTF8String ?: "<nil>");
                releaseAll();
                return false;
            }

            // Pre-size ring with conservative defaults; tapPrepare() will resize.
            initRing (kMaxCh, sampleRate > 0 ? (int) sampleRate : 48000);

            // NOTE: Do NOT call [player play] here. Playback is started lazily
            // in pullBlock() to avoid crashing when the backend is used only for
            // format probing (getFileDurationAndMask) and destroyed immediately.
            LOGI ("AVPlayerEAC3Backend ready (playback deferred)");
            return true;
        }
    }

    // ---- Seek ----

    void seekTo (int64_t samplePosition, int sampleRate)
    {
        flushRing();
        if (player == nil) return;

        const double secs = sampleRate > 0 ? (double) samplePosition / (double) sampleRate : 0.0;
        CMTime seekTime = CMTimeMakeWithSeconds (secs, 48000);

        // All AVPlayer operations must be on the main thread.
        __strong AVPlayer* p = player;
        hasStartedPlaying = true;  // set before dispatch
        auto seekBlock = ^{
            if (p.currentItem == nil)
                return;

            [p seekToTime:seekTime
               toleranceBefore:kCMTimeZero
                toleranceAfter:kCMTimeZero
             completionHandler:^(BOOL) {
                 if (p.currentItem != nil)
                     [p play];
             }];
        };

        if ([NSThread isMainThread])
            seekBlock();
        else
            dispatch_async (dispatch_get_main_queue(), seekBlock);
    }
};

// ============================================================================
// Bridge C functions (declared in foleys_AVPlayerEAC3BackendImpl.h)
// ============================================================================

AVPlayerEAC3BackendImpl* AVPlayerEAC3BackendImpl_create (int sampleRate)
{
    auto* p = new AVPlayerEAC3BackendImpl();
    p->outputSampleRate = sampleRate;
    return p;
}

void AVPlayerEAC3BackendImpl_destroy (AVPlayerEAC3BackendImpl* p)
{
    delete p;
}

bool AVPlayerEAC3BackendImpl_open (AVPlayerEAC3BackendImpl* p,
                                    const juce::File& file,
                                    double sampleRate)
{
    if (! p) return false;
    return p->openFile (file, sampleRate);
}

DecodedAudioBlock AVPlayerEAC3BackendImpl_readNextBlock (AVPlayerEAC3BackendImpl* p)
{
    if (! p) return {};
    return p->pullBlock();
}

void AVPlayerEAC3BackendImpl_setPosition (AVPlayerEAC3BackendImpl* p,
                                           int64_t samplePosition,
                                           int sampleRate)
{
    if (p) p->seekTo (samplePosition, sampleRate);
}

int AVPlayerEAC3BackendImpl_getNumChannels (AVPlayerEAC3BackendImpl* p)
{
    return p ? p->outputChannels : 12;
}

} // namespace foleys

#endif // __APPLE__
