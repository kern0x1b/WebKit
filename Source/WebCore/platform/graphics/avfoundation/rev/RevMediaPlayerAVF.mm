#import "config.h"
#import "RevMediaPlayerAVF.h"

#import "AudioTrackPrivate.h"
#import "DestinationColorSpace.h"
#import "GraphicsContext.h"
#import "MediaPlayerPrivate.h"
#import "NativeImage.h"
#import "PlatformTimeRanges.h"
#import "VideoTrackPrivate.h"
#import <AVFoundation/AVFoundation.h>
#import <pal/avfoundation/MediaTimeAVFoundation.h>
#import <wtf/BlockPtr.h>
#import <wtf/MainThread.h>
#import <wtf/RetainPtr.h>
#import <wtf/ThreadSafeRefCounted.h>
#import <wtf/ThreadSafeWeakPtr.h>

namespace WebCore {
class RevMediaPlayerAVF;
}

@interface RevAVFObserver : NSObject {
    WebCore::RevMediaPlayerAVF* _player;
}
- (id)initWithPlayer:(WebCore::RevMediaPlayerAVF*)player;
- (void)disconnect;
@end

namespace WebCore {

// The upstream AVFObjC track classes read track metadata through soft-linked
// AVFoundation symbols that iOS 6 does not export, and a missing soft-link is a
// release assertion, so they trap on this platform. WebCore only needs a track
// object that reports its identity, so these report exactly that, reading only
// properties AVAssetTrack has had since iOS 4.
class RevVideoTrack final : public VideoTrackPrivate {
public:
    static Ref<RevVideoTrack> create(AVAssetTrack *track) { return adoptRef(*new RevVideoTrack(track)); }

    TrackID id() const final { return m_id; }
    String label() const final { return emptyString(); }
    String language() const final { return m_language; }
    Kind kind() const final { return Kind::Main; }

private:
    explicit RevVideoTrack(AVAssetTrack *track)
        : m_id(static_cast<TrackID>([track trackID]))
        , m_language(String::fromLatin1([[track languageCode] UTF8String]))
    {
    }

    TrackID m_id;
    String m_language;
};

class RevAudioTrack final : public AudioTrackPrivate {
public:
    static Ref<RevAudioTrack> create(AVAssetTrack *track) { return adoptRef(*new RevAudioTrack(track)); }

    TrackID id() const final { return m_id; }
    String label() const final { return emptyString(); }
    String language() const final { return m_language; }
    Kind kind() const final { return Kind::Main; }

private:
    explicit RevAudioTrack(AVAssetTrack *track)
        : m_id(static_cast<TrackID>([track trackID]))
        , m_language(String::fromLatin1([[track languageCode] UTF8String]))
    {
    }

    TrackID m_id;
    String m_language;
};

class RevMediaPlayerAVF final
    : public MediaPlayerPrivateInterface
    , public ThreadSafeRefCounted<RevMediaPlayerAVF, WTF::DestructionThread::Main> {
public:
    explicit RevMediaPlayerAVF(MediaPlayer& player)
        : m_player(player)
    {
        m_observer = adoptNS([[RevAVFObserver alloc] initWithPlayer:this]);
    }

    ~RevMediaPlayerAVF()
    {
        tearDown();
    }

    void ref() const final { ThreadSafeRefCounted::ref(); }
    void deref() const final { ThreadSafeRefCounted::deref(); }

    static void registerMediaEngine(MediaEngineRegistrar);

    void observedStatusChanged()
    {
        Ref protectedThis { *this };
        callOnMainThread([this, protectedThis = WTF::move(protectedThis)] {
            updateStates();
        });
    }

    void observedDidEnd()
    {
        Ref protectedThis { *this };
        callOnMainThread([this, protectedThis = WTF::move(protectedThis)] {
            if (RefPtr player = m_player.get())
                player->timeChanged();
        });
    }

private:
    constexpr MediaPlayerType mediaPlayerType() const final { return MediaPlayerType::AVFObjC; }

    using MediaPlayerPrivateInterface::load;

    void load(const URL& url, const LoadOptions&) final
    {
        m_asset = adoptNS([[AVURLAsset alloc] initWithURL:url.createNSURL().get() options:nil]);
        m_item = adoptNS([[AVPlayerItem alloc] initWithAsset:m_asset.get()]);
        m_avPlayer = adoptNS([[AVPlayer alloc] init]);
        [m_avPlayer.get() replaceCurrentItemWithPlayerItem:m_item.get()];

        // NSKeyValueObservingOptionInitial, not 0: an item served from a fast
        // enough source is already ReadyToPlay by the time observation starts,
        // and a change notification never arrives for a value that has already
        // settled. Without the initial delivery the element kept reporting
        // readyState 0 and a natural size of zero while the movie played, so
        // videoWidth was zero and nothing waiting on canplay ever ran.
        [m_item.get() addObserver:m_observer.get() forKeyPath:@"status" options:NSKeyValueObservingOptionInitial context:nullptr];
        [m_avPlayer.get() addObserver:m_observer.get() forKeyPath:@"rate" options:NSKeyValueObservingOptionInitial context:nullptr];
        [[NSNotificationCenter defaultCenter] addObserver:m_observer.get() selector:@selector(didEnd:) name:AVPlayerItemDidPlayToEndTimeNotification object:m_item.get()];

        m_networkState = MediaPlayer::NetworkState::Loading;
        if (RefPtr player = m_player.get())
            player->networkStateChanged();
    }

    void load(MediaStreamPrivate&) final { }

    void cancelLoad() final { tearDown(); }

    void tearDown()
    {
        if (m_observer) {
            if (m_item)
                [m_item.get() removeObserver:m_observer.get() forKeyPath:@"status"];
            if (m_avPlayer)
                [m_avPlayer.get() removeObserver:m_observer.get() forKeyPath:@"rate"];
            [[NSNotificationCenter defaultCenter] removeObserver:m_observer.get()];
            [m_observer.get() disconnect];
            m_observer = nullptr;
        }
        if (RefPtr player = m_player.get()) {
            for (auto& track : m_videoTracks)
                player->removeVideoTrack(track.get());
            for (auto& track : m_audioTracks)
                player->removeAudioTrack(track.get());
        }
        m_videoTracks.clear();
        m_audioTracks.clear();
        m_tracksReported = false;

        if (m_videoLayer)
            [m_videoLayer.get() setPlayer:nil];
        m_avPlayer = nullptr;
        m_item = nullptr;
        m_asset = nullptr;
    }

    void updateStates()
    {
        if (!m_item)
            return;
        AVPlayerItemStatus status = [m_item.get() status];
        auto oldNetwork = m_networkState;
        auto oldReady = m_readyState;

        if (status == AVPlayerItemStatusReadyToPlay) {
            m_networkState = MediaPlayer::NetworkState::Loaded;
            m_readyState = MediaPlayer::ReadyState::HaveEnoughData;
            if (!m_durationKnown) {
                m_durationKnown = true;
                if (RefPtr player = m_player.get()) {
                    updateTracks();
                    player->durationChanged();
                    player->sizeChanged();
                    player->firstVideoFrameAvailable();
                    player->characteristicChanged();
                }
            }
        } else if (status == AVPlayerItemStatusFailed) {
            m_networkState = MediaPlayer::NetworkState::DecodeError;
            m_readyState = MediaPlayer::ReadyState::HaveNothing;
        }

        m_didProgress = true;
        if (RefPtr player = m_player.get()) {
            if (m_networkState != oldNetwork)
                player->networkStateChanged();
            if (m_readyState != oldReady)
                player->readyStateChanged();
        }
    }

    void updateTracks()
    {
        RefPtr player = m_player.get();
        if (!player || !m_asset || m_tracksReported)
            return;
        m_tracksReported = true;

        for (AVAssetTrack *track in [m_asset.get() tracksWithMediaType:AVMediaTypeVideo]) {
            auto videoTrack = RevVideoTrack::create(track);
            m_videoTracks.append(videoTrack.copyRef());
            player->addVideoTrack(videoTrack.get());
        }

        for (AVAssetTrack *track in [m_asset.get() tracksWithMediaType:AVMediaTypeAudio]) {
            auto audioTrack = RevAudioTrack::create(track);
            m_audioTracks.append(audioTrack.copyRef());
            player->addAudioTrack(audioTrack.get());
        }
    }

    void play() final { if (m_avPlayer) [m_avPlayer.get() play]; }
    void pause() final { if (m_avPlayer) [m_avPlayer.get() pause]; }
    bool paused() const final { return !m_avPlayer || [m_avPlayer.get() rate] == 0; }

    void setVolume(float volume) final
    {
        if (m_avPlayer && [m_avPlayer.get() respondsToSelector:@selector(setVolume:)])
            [m_avPlayer.get() setVolume:volume];
    }
    void setMuted(bool muted) final
    {
        if (m_avPlayer && [m_avPlayer.get() respondsToSelector:@selector(setMuted:)])
            [m_avPlayer.get() setMuted:muted];
    }
    void setRate(float rate) final { m_requestedRate = rate; if (m_avPlayer && [m_avPlayer.get() rate] != 0) [m_avPlayer.get() setRate:rate]; }

    FloatSize naturalSize() const final
    {
        if (m_item) {
            CGSize size = [m_item.get() presentationSize];
            if (size.width > 0 && size.height > 0)
                return FloatSize(size.width, size.height);
        }

        // presentationSize is only populated once the item is ready to play,
        // and the element asks for the size before that to lay the video out.
        // The track carries it from the moment the asset's metadata is loaded,
        // through its own transform, which is where a rotated recording says so.
        if (m_asset) {
            NSArray *videoTracks = [m_asset.get() tracksWithMediaType:AVMediaTypeVideo];
            if ([videoTracks count]) {
                AVAssetTrack *track = [videoTracks objectAtIndex:0];
                CGSize size = CGSizeApplyAffineTransform([track naturalSize], [track preferredTransform]);
                size.width = std::abs(size.width);
                size.height = std::abs(size.height);
                if (size.width > 0 && size.height > 0)
                    return FloatSize(size.width, size.height);
            }
        }
        return FloatSize();
    }

    bool hasVideo() const final
    {
        if (!m_asset)
            return false;
        return [[m_asset.get() tracksWithMediaType:AVMediaTypeVideo] count] > 0;
    }

    bool hasAudio() const final
    {
        if (!m_asset)
            return false;
        return [[m_asset.get() tracksWithMediaType:AVMediaTypeAudio] count] > 0;
    }

    void setPageIsVisible(bool visible) final { m_visible = visible; }

    MediaTime currentTime() const final
    {
        return m_avPlayer ? PAL::toMediaTime([m_avPlayer.get() currentTime]) : MediaTime::zeroTime();
    }

    MediaTime duration() const final
    {
        if (!m_item)
            return MediaTime::zeroTime();
        MediaTime d = PAL::toMediaTime([m_item.get() duration]);
        return d.isValid() ? d : MediaTime::zeroTime();
    }

    Ref<MediaTimePromise> seekToTarget(const SeekTarget& target) final
    {
        if (!m_avPlayer)
            return MediaTimePromise::createAndReject(PlatformMediaError::Cancelled);

        MediaTimePromise::Producer producer;
        Ref promise = producer.promise();

        // A seek is asynchronous, so the promise has to be settled from the
        // completion handler; resolving it up front leaves the element believing
        // the seek is finished while currentTime still reports the old position,
        // which is why replaying a finished video, the skip buttons and dragging
        // the scrubber all appeared to do nothing.
        MediaTime time = target.time;
        [m_avPlayer.get() seekToTime:PAL::toCMTime(time)
            toleranceBefore:PAL::toCMTime(target.negativeThreshold)
            toleranceAfter:PAL::toCMTime(target.positiveThreshold)
            completionHandler:makeBlockPtr([this, protectedThis = Ref { *this }, producer = WTF::move(producer), time] (BOOL finished) mutable {
                callOnMainThread([this, protectedThis = WTF::move(protectedThis), producer = WTF::move(producer), time, finished] () mutable {
                    if (!finished) {
                        producer.reject(PlatformMediaError::Cancelled);
                        return;
                    }
                    if (RefPtr player = m_player.get())
                        player->timeChanged();
                    producer.resolve(time);
                });
            }).get()];

        return promise;
    }

    // Without a seekable range the element rejects every seek before it reaches
    // the player, so nothing can be scrubbed, skipped or replayed. AVPlayerItem
    // publishes the ranges it can seek within; report them.
    const PlatformTimeRanges& seekable() const final
    {
        m_seekableRanges.clear();
        if (m_item) {
            for (NSValue *value in [m_item.get() seekableTimeRanges]) {
                CMTimeRange range = [value CMTimeRangeValue];
                if (CMTIMERANGE_IS_VALID(range) && !CMTIMERANGE_IS_EMPTY(range))
                    m_seekableRanges.add(PAL::toMediaTime(range.start), PAL::toMediaTime(CMTimeRangeGetEnd(range)));
            }
        }
        return m_seekableRanges;
    }

    MediaTime maxTimeSeekable() const final
    {
        const auto& ranges = seekable();
        return ranges.length() ? ranges.maximumBufferedTime() : MediaTime::zeroTime();
    }

    MediaTime minTimeSeekable() const final
    {
        const auto& ranges = seekable();
        return ranges.length() ? ranges.minimumBufferedTime() : MediaTime::zeroTime();
    }

    MediaPlayer::NetworkState networkState() const final { return m_networkState; }
    MediaPlayer::ReadyState readyState() const final { return m_readyState; }

    const PlatformTimeRanges& buffered() const final
    {
        m_buffered.clear();
        if (m_item) {
            for (NSValue *value in [m_item.get() loadedTimeRanges]) {
                CMTimeRange range = [value CMTimeRangeValue];
                if (CMTIMERANGE_IS_VALID(range))
                    m_buffered.add(PAL::toMediaTime(range.start), PAL::toMediaTime(CMTimeRangeGetEnd(range)));
            }
        }
        return m_buffered;
    }
    bool didLoadingProgress() const final { bool p = m_didProgress; m_didProgress = false; return p; }

    PlatformLayer* platformLayer() const final
    {
        if (!m_videoLayer && m_avPlayer) {
            m_videoLayer = adoptNS([[AVPlayerLayer alloc] init]);
            [m_videoLayer.get() setPlayer:m_avPlayer.get()];
            [m_videoLayer.get() setVideoGravity:AVLayerVideoGravityResizeAspect];
        }
        return m_videoLayer.get();
    }
    bool supportsAcceleratedRendering() const final { return true; }
    void acceleratedRenderingStateChanged() final { }
    bool supportsFullscreen() const final { return true; }
    bool supportsPictureInPicture() const final { return false; }

    void paint(GraphicsContext& context, const FloatRect& destRect) final
    {
        if (!m_asset || context.paintingDisabled())
            return;
        if (!m_imageGenerator) {
            m_imageGenerator = adoptNS([[AVAssetImageGenerator alloc] initWithAsset:m_asset.get()]);
            [m_imageGenerator.get() setAppliesPreferredTrackTransform:YES];
        }
        CMTime at = PAL::toCMTime(currentTime());
        RetainPtr<CGImageRef> image = adoptCF([m_imageGenerator.get() copyCGImageAtTime:at actualTime:nullptr error:nullptr]);
        if (!image)
            return;
        auto nativeImage = NativeImage::create(image.get());
        if (!nativeImage)
            return;
        FloatRect imageRect { FloatPoint::zero(), nativeImage->size() };
        context.drawNativeImage(*nativeImage, destRect, imageRect);
    }

    DestinationColorSpace colorSpace() final { return DestinationColorSpace::SRGB(); }

    MediaPlayer::MovieLoadType movieLoadType() const final { return MediaPlayer::MovieLoadType::Download; }

    ThreadSafeWeakPtr<MediaPlayer> m_player;
    RetainPtr<RevAVFObserver> m_observer;
    RetainPtr<AVURLAsset> m_asset;
    RetainPtr<AVPlayerItem> m_item;
    RetainPtr<AVPlayer> m_avPlayer;
    mutable RetainPtr<AVPlayerLayer> m_videoLayer;
    RetainPtr<AVAssetImageGenerator> m_imageGenerator;
    MediaPlayer::NetworkState m_networkState { MediaPlayer::NetworkState::Empty };
    MediaPlayer::ReadyState m_readyState { MediaPlayer::ReadyState::HaveNothing };
    FloatSize m_naturalSize;
    float m_requestedRate { 1 };
    bool m_visible { false };
    bool m_durationKnown { false };
    mutable bool m_didProgress { false };
    mutable PlatformTimeRanges m_buffered;
    mutable PlatformTimeRanges m_seekableRanges;
    Vector<Ref<RevVideoTrack>> m_videoTracks;
    Vector<Ref<RevAudioTrack>> m_audioTracks;
    bool m_tracksReported { false };
};

class RevAVFPlayerFactory final : public MediaPlayerFactory {
private:
    MediaPlayerEnums::MediaEngineIdentifier identifier() const final { return MediaPlayerEnums::MediaEngineIdentifier::AVFoundation; }

    Ref<MediaPlayerPrivateInterface> createMediaEnginePlayer(MediaPlayer& player) const final
    {
        return adoptRef(*new RevMediaPlayerAVF(player));
    }

    void getSupportedTypes(HashSet<String>& types) const final
    {
        types.add("video/mp4"_s);
        types.add("video/quicktime"_s);
        types.add("audio/mpeg"_s);
        types.add("audio/mp4"_s);
        types.add("audio/x-m4a"_s);
    }

    MediaPlayer::SupportsType supportsTypeAndCodecs(const MediaEngineSupportParameters& parameters) const final
    {
        HashSet<String> types;
        getSupportedTypes(types);
        bool ok = types.contains(parameters.type.containerType());
        if (parameters.type.containerType().isEmpty())
            return MediaPlayer::SupportsType::IsNotSupported;
        if (ok)
            return MediaPlayer::SupportsType::IsSupported;
        return MediaPlayer::SupportsType::IsNotSupported;
    }
};

void RevMediaPlayerAVF::registerMediaEngine(MediaEngineRegistrar registrar)
{
    registrar(makeUnique<RevAVFPlayerFactory>());
}

void registerRevAVFPlayer(MediaEngineRegistrar registrar)
{
    RevMediaPlayerAVF::registerMediaEngine(registrar);
}

} // namespace WebCore

@implementation RevAVFObserver
- (id)initWithPlayer:(WebCore::RevMediaPlayerAVF*)player
{
    self = [super init];
    if (self)
        _player = player;
    return self;
}
- (void)disconnect
{
    _player = nullptr;
}
- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change context:(void *)context
{
    UNUSED_PARAM(object);
    UNUSED_PARAM(change);
    UNUSED_PARAM(context);
    if (_player)
        _player->observedStatusChanged();
}
- (void)didEnd:(NSNotification *)notification
{
    UNUSED_PARAM(notification);
    if (_player)
        _player->observedDidEnd();
}
@end
