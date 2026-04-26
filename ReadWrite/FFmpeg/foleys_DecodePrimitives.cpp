#include "foleys_DecodePrimitives.h"

namespace foleys
{

namespace
{
constexpr int kInvalid = -1;

enum CanonicalIndex
{
    L = 0,
    R,
    C,
    LFE,
    Lss,
    Rss,
    Lrs,
    Rrs,
    Ltf,
    Rtf,
    Ltr,
    Rtr,
    NumCanonicalChannels
};

void setIdentityMapping (std::array<int, 12>& mapping)
{
    for (int i = 0; i < NumCanonicalChannels; ++i)
        mapping[(size_t) i] = i;
}

#if JUCE_MAC
int labelToCanonicalIndex (AudioChannelLabel label)
{
    switch (label)
    {
        case kAudioChannelLabel_Left:                return L;
        case kAudioChannelLabel_Right:               return R;
        case kAudioChannelLabel_Center:              return C;
        case kAudioChannelLabel_LFEScreen:           return LFE;
        case kAudioChannelLabel_LeftSurroundDirect:  return Lss;
        case kAudioChannelLabel_RightSurroundDirect: return Rss;
        case kAudioChannelLabel_RearSurroundLeft:    return Lrs;
        case kAudioChannelLabel_RearSurroundRight:   return Rrs;
        case kAudioChannelLabel_VerticalHeightLeft:  return Ltf;
        case kAudioChannelLabel_VerticalHeightRight: return Rtf;
        case kAudioChannelLabel_TopBackLeft:         return Ltr;
        case kAudioChannelLabel_TopBackRight:        return Rtr;
        default:                                     return kInvalid;
    }
}
#endif

} // namespace

ChannelMapResolver::ChannelMapResolver()
{
    setIdentityMapping (mapping);
}

bool ChannelMapResolver::configureFromLayout (const AudioChannelLayout* layout)
{
    setIdentityMapping (mapping);

#if JUCE_MAC
    if (layout == nullptr)
        return false;

    if (layout->mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelDescriptions
        || layout->mNumberChannelDescriptions == 0)
        return false;

    mapping.fill (kInvalid);

    bool mappedAny = false;
    const auto count = juce::jmin ((int) layout->mNumberChannelDescriptions, (int) NumCanonicalChannels);

    for (int inputIdx = 0; inputIdx < count; ++inputIdx)
    {
        const auto label = layout->mChannelDescriptions[(UInt32) inputIdx].mChannelLabel;
        const int canonical = labelToCanonicalIndex (label);

        if (canonical >= 0 && canonical < NumCanonicalChannels)
        {
            mapping[(size_t) inputIdx] = canonical;
            mappedAny = true;
        }
    }

    return mappedAny;
#else
    juce::ignoreUnused (layout);
    return false;
#endif
}

int ChannelMapResolver::mapToCanonical (int inputChannelIndex) const
{
    if (! juce::isPositiveAndBelow (inputChannelIndex, (int) mapping.size()))
        return kInvalid;

    return mapping[(size_t) inputChannelIndex];
}

Atmos714Buffer normalizeToAtmos714 (const DecodedAudioBlock& src, const ChannelMapResolver* resolver)
{
    Atmos714Buffer result;

    const int numSamples = src.buffer.getNumSamples();
    result.buffer.setSize (NumCanonicalChannels, numSamples, false, false, true);
    result.buffer.clear();

    if (numSamples == 0)
        return result;

    const int srcChannels = src.buffer.getNumChannels();
    for (int inputCh = 0; inputCh < srcChannels; ++inputCh)
    {
        int canonical = inputCh;
        if (resolver != nullptr)
            canonical = resolver->mapToCanonical (inputCh);

        if (! juce::isPositiveAndBelow (canonical, NumCanonicalChannels))
            continue;

        result.buffer.copyFrom (canonical, 0, src.buffer, inputCh, 0, numSamples);
    }

    return result;
}

void HostBusMapper::mapToHost (const Atmos714Buffer& src,
                               juce::AudioBuffer<float>& dst,
                               const juce::AudioChannelSet& layout)
{
    juce::ignoreUnused (layout);

    const int samples = juce::jmin (src.buffer.getNumSamples(), dst.getNumSamples());
    if (samples <= 0)
        return;

    dst.clear();

    const int dstChannels = dst.getNumChannels();

    if (dstChannels >= 12)
    {
        for (int ch = 0; ch < 12; ++ch)
            dst.copyFrom (ch, 0, src.buffer, ch, 0, samples);
        return;
    }

    if (dstChannels == 6)
    {
        dst.copyFrom (0, 0, src.buffer, L, 0, samples);
        dst.copyFrom (1, 0, src.buffer, R, 0, samples);
        dst.copyFrom (2, 0, src.buffer, C, 0, samples);
        dst.copyFrom (3, 0, src.buffer, LFE, 0, samples);
        dst.copyFrom (4, 0, src.buffer, Lss, 0, samples);
        dst.copyFrom (5, 0, src.buffer, Rss, 0, samples);
        return;
    }

    if (dstChannels >= 2)
    {
        auto* left  = dst.getWritePointer (0);
        auto* right = dst.getWritePointer (1);

        const auto* Lp   = src.buffer.getReadPointer (L);
        const auto* Rp   = src.buffer.getReadPointer (R);
        const auto* Cp   = src.buffer.getReadPointer (C);
        const auto* LFEp = src.buffer.getReadPointer (LFE);
        const auto* Lssp = src.buffer.getReadPointer (Lss);
        const auto* Rssp = src.buffer.getReadPointer (Rss);
        const auto* Lrsp = src.buffer.getReadPointer (Lrs);
        const auto* Rrsp = src.buffer.getReadPointer (Rrs);
        const auto* Ltfp = src.buffer.getReadPointer (Ltf);
        const auto* Rtfp = src.buffer.getReadPointer (Rtf);
        const auto* Ltrp = src.buffer.getReadPointer (Ltr);
        const auto* Rtrp = src.buffer.getReadPointer (Rtr);

        for (int i = 0; i < samples; ++i)
        {
            left[i] = Lp[i] + 0.707f * Cp[i] + 0.5f * Lssp[i] + 0.5f * Lrsp[i]
                    + 0.35f * Ltfp[i] + 0.35f * Ltrp[i] + 0.2f * LFEp[i];
            right[i] = Rp[i] + 0.707f * Cp[i] + 0.5f * Rssp[i] + 0.5f * Rrsp[i]
                     + 0.35f * Rtfp[i] + 0.35f * Rtrp[i] + 0.2f * LFEp[i];
        }

        for (int ch = 2; ch < dstChannels; ++ch)
            dst.clear (ch, 0, samples);
        return;
    }

    if (dstChannels == 1)
    {
        dst.copyFrom (0, 0, src.buffer, C, 0, samples);
        dst.addFrom (0, 0, src.buffer, L, 0, samples, 0.5f);
        dst.addFrom (0, 0, src.buffer, R, 0, samples, 0.5f);
    }
}

} // namespace foleys
