#pragma once

// Bridge header for AVPlayerEAC3Backend's ObjC++ implementation.
// This header intentionally does NOT include any FFmpeg headers so it can
// be safely included from .mm (ObjC++) translation units.

#if defined(__APPLE__)

#include <cstdint>
#include "foleys_DecodePrimitives.h"  // DecodedAudioBlock (JUCE only, no FFmpeg)

namespace foleys
{

/**
 * Opaque implementation handle — defined in foleys_AVPlayerEAC3BackendImpl.mm.
 * Never include the definition from C++ (.cpp) files.
 */
struct AVPlayerEAC3BackendImpl;

AVPlayerEAC3BackendImpl* AVPlayerEAC3BackendImpl_create (int sampleRate);
void                     AVPlayerEAC3BackendImpl_destroy (AVPlayerEAC3BackendImpl*);
bool                     AVPlayerEAC3BackendImpl_open (AVPlayerEAC3BackendImpl*,
                                                        const juce::File& file,
                                                        double sampleRate);
DecodedAudioBlock        AVPlayerEAC3BackendImpl_readNextBlock (AVPlayerEAC3BackendImpl*);
void                     AVPlayerEAC3BackendImpl_setPosition (AVPlayerEAC3BackendImpl*,
                                                               int64_t samplePosition,
                                                               int sampleRate);
int                      AVPlayerEAC3BackendImpl_getNumChannels (AVPlayerEAC3BackendImpl*);

} // namespace foleys

#endif // __APPLE__
