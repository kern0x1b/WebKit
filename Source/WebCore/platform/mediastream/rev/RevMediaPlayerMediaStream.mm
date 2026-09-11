#import "config.h"
#import "RevMediaPlayerMediaStream.h"

#if ENABLE(MEDIA_STREAM)

#import "DestinationColorSpace.h"
#import "GraphicsContext.h"
#import "MediaPlayerPrivate.h"
#import "MediaStreamPrivate.h"
#import "MediaStreamTrackPrivate.h"
#import "NativeImage.h"
#import "PlatformTimeRanges.h"
#import "RealtimeMediaSource.h"
#import "VideoFrame.h"
#import <QuartzCore/QuartzCore.h>
#import <wtf/MainThread.h>
#import <wtf/RetainPtr.h>
#import <wtf/ThreadSafeRefCounted.h>

namespace WebCore {

class RevMediaPlayerMediaStream final
    : public MediaPlayerPrivateInterface
    , public RealtimeMediaSource::VideoFrameObserver
    , public ThreadSafeRefCounted<RevMediaPlayerMediaStream, WTF::DestructionThread::Main> {
public:
    explicit RevMediaPlayerMediaStream(MediaPlayer& player)
        : m_player(player)
    {
    }

    ~RevMediaPlayerMediaStream()
    {
        stopObserving();
    }

    void ref() const final { ThreadSafeRefCounted::ref(); }
    void deref() const final { ThreadSafeRefCounted::deref(); }

    static void registerMediaEngine(MediaEngineRegistrar);

private:
    constexpr MediaPlayerType mediaPlayerType() const final { return MediaPlayerType::AVFObjCMediaStream; }

    using MediaPlayerPrivateInterface::load;

    void load(MediaStreamPrivate& stream) final
    {
        m_stream = &stream;
        auto tracks = stream.tracks();
        for (auto& track : tracks) {
            if (track->isVideo()) {
                m_videoSource = const_cast<RealtimeMediaSource*>(&track->source());
                break;
            }
        }
        ensureLayer();
        if (m_videoSource)
            m_videoSource->addVideoFrameObserver(*this);

        m_networkState = MediaPlayer::NetworkState::Loaded;
        m_readyState = MediaPlayer::ReadyState::HaveMetadata;
        if (RefPtr player = m_player.get()) {
            player->networkStateChanged();
            player->readyStateChanged();
        }
    }

    void cancelLoad() final { stopObserving(); }

    void stopObserving()
    {
        if (m_videoSource) {
            m_videoSource->removeVideoFrameObserver(*this);
            m_videoSource = nullptr;
        }
    }

    void videoFrameAvailable(VideoFrame& videoFrame, VideoFrameTimeMetadata) final
    {
        Ref<VideoFrame> frame = videoFrame;
        Ref protectedThis { *this };
        callOnMainThread([this, protectedThis = WTF::move(protectedThis), frame = WTF::move(frame)]() mutable {
            m_currentFrame = frame.copyRef();
            m_currentImage = nullptr;

            auto size = frame->presentationSize();
            bool sizeChanged = m_naturalSize != size;
            m_naturalSize = size;

            updateCurrentImage();

            bool firstFrame = !m_hasFrame;
            m_hasFrame = true;
            if (firstFrame) {
                m_readyState = MediaPlayer::ReadyState::HaveEnoughData;
                if (RefPtr player = m_player.get()) {
                    player->firstVideoFrameAvailable();
                    player->readyStateChanged();
                }
            }
            if (sizeChanged) {
                if (RefPtr player = m_player.get())
                    player->sizeChanged();
            }
            m_didProgress = true;
        });
    }

    void updateCurrentImage()
    {
        if (!m_currentFrame)
            return;
        auto pixelBuffer = m_currentFrame->pixelBuffer();
        if (!pixelBuffer)
            return;
        CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
        void* base = CVPixelBufferGetBaseAddress(pixelBuffer);
        size_t width = CVPixelBufferGetWidth(pixelBuffer);
        size_t height = CVPixelBufferGetHeight(pixelBuffer);
        size_t bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);
        RetainPtr<CGColorSpaceRef> colorSpace = adoptCF(CGColorSpaceCreateDeviceRGB());
        RetainPtr<CGContextRef> context = base ? adoptCF(CGBitmapContextCreate(base, width, height, 8, bytesPerRow, colorSpace.get(), kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little)) : nullptr;
        RetainPtr<CGImageRef> cgImage = context ? adoptCF(CGBitmapContextCreateImage(context.get())) : nullptr;
        CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
        if (!cgImage)
            return;
        m_currentImage = NativeImage::create(cgImage.get());
        if (m_layer)
            [m_layer setContents:(__bridge id)cgImage.get()];
    }

    PlatformLayer* platformLayer() const final { return m_layer.get(); }
    bool supportsAcceleratedRendering() const final { return true; }
    void acceleratedRenderingStateChanged() final { }
    bool supportsFullscreen() const final { return false; }
    bool supportsPictureInPicture() const final { return false; }

    void ensureLayer()
    {
        if (m_layer)
            return;
        m_layer = adoptNS([[CALayer alloc] init]);
        [m_layer setContentsGravity:kCAGravityResizeAspectFill];
        [m_layer setOpaque:YES];
        [m_layer setMasksToBounds:YES];
    }

    void play() final { m_playing = true; }
    void pause() final { m_playing = false; }
    bool paused() const final { return !m_playing; }

    FloatSize naturalSize() const final { return m_naturalSize; }
    bool hasVideo() const final { return !!m_videoSource; }
    bool hasAudio() const final { return false; }
    void setPageIsVisible(bool visible) final { m_visible = visible; }

    MediaTime currentTime() const final { return MediaTime::zeroTime(); }
    MediaTime duration() const final { return MediaTime::positiveInfiniteTime(); }

    Ref<MediaTimePromise> seekToTarget(const SeekTarget&) final { return MediaTimePromise::createAndReject(PlatformMediaError::Cancelled); }

    MediaPlayer::NetworkState networkState() const final { return m_networkState; }
    MediaPlayer::ReadyState readyState() const final { return m_readyState; }

    const PlatformTimeRanges& buffered() const final { return PlatformTimeRanges::emptyRanges(); }
    bool didLoadingProgress() const final { return m_didProgress; }

    void paint(GraphicsContext& context, const FloatRect& destRect) final
    {
        if (!m_currentImage)
            return;
        FloatRect imageRect { FloatPoint::zero(), m_currentImage->size() };
        context.drawNativeImage(*m_currentImage, destRect, imageRect);
    }

    DestinationColorSpace colorSpace() final { return DestinationColorSpace::SRGB(); }

    MediaPlayer::MovieLoadType movieLoadType() const final { return MediaPlayer::MovieLoadType::LiveStream; }

    ThreadSafeWeakPtr<MediaPlayer> m_player;
    RefPtr<MediaStreamPrivate> m_stream;
    RefPtr<RealtimeMediaSource> m_videoSource;
    RetainPtr<CALayer> m_layer;
    RefPtr<VideoFrame> m_currentFrame;
    RefPtr<NativeImage> m_currentImage;
    FloatSize m_naturalSize;
    MediaPlayer::NetworkState m_networkState { MediaPlayer::NetworkState::Empty };
    MediaPlayer::ReadyState m_readyState { MediaPlayer::ReadyState::HaveNothing };
    bool m_playing { false };
    bool m_visible { false };
    bool m_hasFrame { false };
    bool m_didProgress { false };
};

class RevMediaStreamPlayerFactory final : public MediaPlayerFactory {
private:
    MediaPlayerEnums::MediaEngineIdentifier identifier() const final { return MediaPlayerEnums::MediaEngineIdentifier::AVFoundationMediaStream; }

    Ref<MediaPlayerPrivateInterface> createMediaEnginePlayer(MediaPlayer& player) const final
    {
        return adoptRef(*new RevMediaPlayerMediaStream(player));
    }

    void getSupportedTypes(HashSet<String>& types) const final { types.clear(); }

    MediaPlayer::SupportsType supportsTypeAndCodecs(const MediaEngineSupportParameters& parameters) const final
    {
        return (parameters.platformType == PlatformMediaDecodingType::MediaStream && !parameters.requiresRemotePlayback)
            ? MediaPlayer::SupportsType::IsSupported : MediaPlayer::SupportsType::IsNotSupported;
    }
};

void RevMediaPlayerMediaStream::registerMediaEngine(MediaEngineRegistrar registrar)
{
    registrar(makeUnique<RevMediaStreamPlayerFactory>());
}

void registerRevMediaStreamPlayer(MediaEngineRegistrar registrar)
{
    RevMediaPlayerMediaStream::registerMediaEngine(registrar);
}

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM)
