#if defined(__APPLE__)

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <cmath>
#include <vector>

#include "foleys_AVAssetAudioReader.h"

namespace foleys
{

namespace
{
constexpr AudioChannelLayoutTag kAtmos714Tag = (AudioChannelLayoutTag) ((190u << 16) | 12u);

AudioChannelLayout copyLayoutFrom (CMAudioFormatDescriptionRef fmt, bool& ok)
{
    AudioChannelLayout copied {};
    ok = false;

    if (fmt == nullptr)
        return copied;

    size_t layoutSize = 0;
    const AudioChannelLayout* layout = CMAudioFormatDescriptionGetChannelLayout (fmt, &layoutSize);
    if (layout == nullptr || layoutSize < sizeof (AudioChannelLayout))
        return copied;

    copied = *layout;
    ok = true;
    return copied;
}

NSDictionary* makeOutputSettings (double sampleRate, int requestedChannels, NSData* channelLayoutData)
{
    NSMutableDictionary* settings = [@{
        AVFormatIDKey: @(kAudioFormatLinearPCM),
        AVSampleRateKey: @(sampleRate),
        AVLinearPCMBitDepthKey: @(32),
        AVLinearPCMIsFloatKey: @YES,
        AVLinearPCMIsNonInterleaved: @YES
    } mutableCopy];

    if (requestedChannels > 0)
        settings[AVNumberOfChannelsKey] = @(requestedChannels);

    if (channelLayoutData != nil)
        settings[AVChannelLayoutKey] = channelLayoutData;

    return [settings autorelease];
}

int getChannelCountFromTrack (AVAssetTrack* track)
{
    NSArray* trackFormats = track.formatDescriptions;
    if (trackFormats.count == 0)
        return 0;

    CMAudioFormatDescriptionRef fmt = (__bridge CMAudioFormatDescriptionRef) trackFormats[0];
    const AudioStreamBasicDescription* asbd = CMAudioFormatDescriptionGetStreamBasicDescription (fmt);
    return asbd != nullptr ? (int) asbd->mChannelsPerFrame : 0;
}

} // namespace

struct AVAssetAudioReader::Pimpl
{
    AVAssetReader* reader = nil;
    AVAssetReaderTrackOutput* trackOutput = nil;

    juce::File file;
    double sampleRate = 48000.0;
    int requestedChannels = 12;

    std::atomic<int64_t> pendingSeekSample { -1 };
    int64_t currentSamplePosition = 0;

    void releaseReader()
    {
        if (reader != nil)
        {
            [reader cancelReading];
            [reader release];
            reader = nil;
        }

        if (trackOutput != nil)
        {
            [trackOutput release];
            trackOutput = nil;
        }
    }

    ~Pimpl()
    {
        releaseReader();
    }

    int createReader (double fromSeconds)
    {
        releaseReader();

        @autoreleasepool
        {
            NSString* path = [NSString stringWithUTF8String:file.getFullPathName().toRawUTF8()];
            NSURL* url = [NSURL fileURLWithPath:path];
            AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
            if (! asset)
                return 0;

            NSArray<AVAssetTrack*>* audioTracks = [asset tracksWithMediaType:AVMediaTypeAudio];
            if (! audioTracks || audioTracks.count == 0)
                return 0;

            AVAssetTrack* track = audioTracks[0];
            NSError* error = nil;
            AVAssetReader* newReader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
            if (! newReader)
                return 0;

            if (fromSeconds > 0.0)
            {
                CMTime startTime = CMTimeMakeWithSeconds (fromSeconds, (int32_t) sampleRate);
                newReader.timeRange = CMTimeRangeMake (startTime, kCMTimePositiveInfinity);
            }

            const auto startReader = ^int (NSDictionary* settings, int fallbackChannels)
            {
                AVAssetReaderTrackOutput* output = [[AVAssetReaderTrackOutput alloc] initWithTrack:track outputSettings:settings];
                output.alwaysCopiesSampleData = NO;

                if (! [newReader canAddOutput:output])
                {
                    [output release];
                    return 0;
                }

                [newReader addOutput:output];
                if (! [newReader startReading])
                {
                    [output release];
                    return 0;
                }

                reader = newReader;
                trackOutput = output;
                currentSamplePosition = (int64_t) std::llround (fromSeconds * sampleRate);
                return fallbackChannels;
            };

            AudioChannelLayout atmosLayout {};
            atmosLayout.mChannelLayoutTag = kAtmos714Tag;
            NSData* atmosLayoutData = [NSData dataWithBytes:&atmosLayout length:offsetof (AudioChannelLayout, mChannelDescriptions)];

            int actualChannels = startReader (makeOutputSettings (sampleRate, requestedChannels, atmosLayoutData), requestedChannels);

            if (actualChannels <= 0)
            {
                [newReader release];
                newReader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
                if (! newReader)
                    return 0;

                if (fromSeconds > 0.0)
                {
                    CMTime startTime = CMTimeMakeWithSeconds (fromSeconds, (int32_t) sampleRate);
                    newReader.timeRange = CMTimeRangeMake (startTime, kCMTimePositiveInfinity);
                }

                actualChannels = startReader (makeOutputSettings (sampleRate, 0, nil), getChannelCountFromTrack (track));
            }

            if (actualChannels <= 0)
                return 0;

            if (actualChannels <= 0)
                actualChannels = requestedChannels;

            return actualChannels;
        }
    }
};

AVAssetAudioReader::AVAssetAudioReader()
    : pimpl (std::make_unique<Pimpl>())
{
}

AVAssetAudioReader::~AVAssetAudioReader() = default;

bool AVAssetAudioReader::open (const juce::File& file, double sr, int requestedChannels)
{
    close();

    pimpl->file = file;
    pimpl->sampleRate = sr;
    pimpl->requestedChannels = requestedChannels;

    const int ch = pimpl->createReader (0.0);
    if (ch <= 0)
        return false;

    numChannels = ch;
    sampleRate = sr;
    opened = true;
    pimpl->pendingSeekSample.store (-1, std::memory_order_release);
    return true;
}

void AVAssetAudioReader::close()
{
    pimpl->releaseReader();
    opened = false;
    numChannels = 0;
    sampleRate = 0.0;
    pimpl->currentSamplePosition = 0;
}

bool AVAssetAudioReader::isAtEnd() const
{
    if (! opened || pimpl->reader == nil)
        return true;

    return pimpl->reader.status != AVAssetReaderStatusReading;
}

void AVAssetAudioReader::setPosition (int64_t samplePosition)
{
    if (! opened)
        return;

    pimpl->pendingSeekSample.store (samplePosition, std::memory_order_release);
}

DecodedAudioBlock AVAssetAudioReader::readNextBlock()
{
    DecodedAudioBlock block;

    if (! opened)
        return block;

    const int64_t pending = pimpl->pendingSeekSample.exchange (-1, std::memory_order_acq_rel);
    if (pending >= 0)
    {
        const double secs = sampleRate > 0.0 ? (double) pending / sampleRate : 0.0;
        pimpl->createReader (secs);
    }

    if (pimpl->reader == nil)
        return block;

    @autoreleasepool
    {
        if (pimpl->reader.status != AVAssetReaderStatusReading)
        {
            const double secs = sampleRate > 0.0 ? (double) pimpl->currentSamplePosition / sampleRate : 0.0;
            if (pimpl->createReader (secs) <= 0)
                return block;
        }

        CMSampleBufferRef sampleBuffer = [pimpl->trackOutput copyNextSampleBuffer];
        if (! sampleBuffer)
            return block;

        const CMItemCount numSamples = CMSampleBufferGetNumSamples (sampleBuffer);
        if (numSamples <= 0)
        {
            CFRelease (sampleBuffer);
            return block;
        }

        CMAudioFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription (sampleBuffer);
        bool hasLayout = false;
        AudioChannelLayout layout = copyLayoutFrom (fmt, hasLayout);
        if (hasLayout)
            block.layout = layout;

        size_t ablSize = 0;
        CMBlockBufferRef blockBuffer = nil;

        CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer (sampleBuffer,
                                                                  &ablSize,
                                                                  nullptr,
                                                                  0,
                                                                  kCFAllocatorDefault,
                                                                  kCFAllocatorDefault,
                                                                  0,
                                                                  &blockBuffer);
        if (blockBuffer)
        {
            CFRelease (blockBuffer);
            blockBuffer = nil;
        }

        if (ablSize == 0)
        {
            CFRelease (sampleBuffer);
            return block;
        }

        std::vector<uint8_t> storage (ablSize, 0);
        auto* abl = reinterpret_cast<AudioBufferList*> (storage.data());

        const OSStatus err = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer (sampleBuffer,
                                                                                        nullptr,
                                                                                        abl,
                                                                                        ablSize,
                                                                                        kCFAllocatorDefault,
                                                                                        kCFAllocatorDefault,
                                                                                        0,
                                                                                        &blockBuffer);

        if (err == noErr && blockBuffer != nil)
        {
            const int numCh = (int) abl->mNumberBuffers;
            block.buffer.setSize (numCh, (int) numSamples, false, false, true);

            for (int ch = 0; ch < numCh; ++ch)
            {
                const auto* src = static_cast<const float*> (abl->mBuffers[ch].mData);
                juce::FloatVectorOperations::copy (block.buffer.getWritePointer (ch), src, (int) numSamples);
            }

            pimpl->currentSamplePosition += (int64_t) numSamples;
            CFRelease (blockBuffer);
        }

        CFRelease (sampleBuffer);
        return block;
    }
}

} // namespace foleys

#endif
