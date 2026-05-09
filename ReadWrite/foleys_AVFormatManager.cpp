/*
 ==============================================================================

 Copyright (c) 2019 - 2021, Foleys Finest Audio - Daniel Walz
 All rights reserved.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 OF THE POSSIBILITY OF SUCH DAMAGE.

 ==============================================================================
 */

namespace foleys
{

AVFormatManager::AVFormatManager()
{
    audioFormatManager.registerBasicFormats();
}

CreatedClipResult AVFormatManager::createClipFromFile (VideoEngine& engine, juce::URL url, StreamTypes type)
{
    const auto& factory = factories.find (url.getScheme());
    if (factory != factories.end())
    {
        CreatedClipResult result;
        result.clip = std::shared_ptr<foleys::AVClip> (factory->second (engine, url, type));
        return result;
    }

    if (url.isLocalFile())
    {
        const auto file = url.getLocalFile();
        auto image = juce::ImageFileFormat::loadFrom (file);
        if (image.isValid())
        {
            auto clip = std::make_shared<ImageClip> (engine);
            clip->setImage (image);
            clip->setMediaFile (url);
            CreatedClipResult result;
            result.clip = clip;
            return result;
        }

        // findFormatForFileExtension would consume some video formats as well
        // if (audioFormatManager.findFormatForFileExtension (file.getFileExtension()) != nullptr)
        if (file.hasFileExtension ("wav;aif;aiff;mp3;wma;m4a"))
        {
            if (auto* audio = audioFormatManager.createReaderFor (file))
            {
                auto clip = std::make_shared<AudioClip> (engine);
                CreatedClipResult result;
                result.clip = clip;
                result.playbackNumChannels = audio->numChannels;
                result.playbackSampleRate = audio->sampleRate;
                result.durationSeconds = audio->sampleRate > 0.0 ? audio->lengthInSamples / audio->sampleRate : 0.0;
                result.audioSettings.numChannels = audio->numChannels;
                result.audioSettings.sourceNumChannels = audio->numChannels;
                result.audioSettings.timebase = int (audio->sampleRate);
                result.audioSettings.bitsPerSample = audio->bitsPerSample;
                result.audioSettings.defaultNumSamples = 1024;
                clip->setAudioFormatReader (audio);
                clip->setMediaFile (url);
                return result;
            }
        }

        auto reader = AVFormatManager::createReaderFor (file, type);
        if (reader && reader->isOpenedOk())
        {
            auto clip = std::make_shared<MovieClip> (engine);
            CreatedClipResult result;
            result.clip = clip;
            result.playbackSampleRate = reader->sampleRate;
            result.playbackNumChannels = reader->numChannels;
            result.durationSeconds = reader->getLengthInSeconds();

            if (reader->getNumAudioStreams() > 0)
                result.audioSettings = reader->getAudioSettings (0);

            if (result.audioSettings.numChannels <= 0)
                result.audioSettings.numChannels = reader->numChannels;

            if (result.audioSettings.sourceNumChannels <= 0)
                result.audioSettings.sourceNumChannels = reader->numChannels;

            if (result.audioSettings.timebase <= 0)
                result.audioSettings.timebase = int (reader->sampleRate);

            if (reader->hasVideo())
                clip->setThumbnailReader (AVFormatManager::createReaderFor (file, StreamTypes::video()));

            clip->setReader (std::move (reader));
            return result;
        }
    }

    return {};
}


std::unique_ptr<AVReader> AVFormatManager::createReaderFor (juce::File file, StreamTypes type)
{
    // If you hit this jassert you didn't add an AVFormat to read video files.
    // use @see registerFormat() to add a reader backend, e.g.
    // videoEngine.getFormatManager().registerFormat (std::make_unique<foleys::FFmpegFormat>());
    jassert(videoFormats.empty() == false);

    for (auto& format : videoFormats)
    {
        if (format->canRead (file))
            return format->createReaderFor (file, type);
    }

    return {};
}

std::unique_ptr<AVWriter> AVFormatManager::createClipWriter (juce::File file)
{

#if FOLEYS_USE_FFMPEG
    auto writer = std::make_unique<FFmpegWriter>(file, "");
    return writer;
#else
    juce::ignoreUnused (file);
    return {};
#endif
}

void AVFormatManager::registerFormat(std::unique_ptr<AVFormat> format)
{
    videoFormats.push_back (std::move (format));
}

void AVFormatManager::registerFactory (const juce::String& schema, std::function<std::shared_ptr<AVClip>(foleys::VideoEngine& videoEngine, juce::URL url, StreamTypes type)> factory)
{
    factories [schema] = factory;
}


} // foleys
