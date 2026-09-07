#import <WebCore/UserMediaClient.h>
#import <wtf/Forward.h>
#import <wtf/Ref.h>
#import <wtf/RefCounted.h>

#if ENABLE(MEDIA_STREAM)

namespace WebCore {
class Document;
class UserMediaRequest;
}

@class WebView;

class WebUserMediaClient final : public WebCore::UserMediaClient, public RefCounted<WebUserMediaClient> {
public:
    static Ref<WebUserMediaClient> create(WebView *webView) { return adoptRef(*new WebUserMediaClient(webView)); }

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

private:
    explicit WebUserMediaClient(WebView *);

    void requestUserMediaAccess(WebCore::UserMediaRequest&) final;
    void cancelUserMediaAccessRequest(WebCore::UserMediaRequest&) final;
    void enumerateMediaDevices(WebCore::Document&, EnumerateDevicesCallback&&) final;
    DeviceChangeObserverToken addDeviceChangeObserver(WTF::Function<void()>&&) final;
    void removeDeviceChangeObserver(DeviceChangeObserverToken) final;
    void updateCaptureState(const WebCore::Document&, bool isActive, WebCore::MediaProducerMediaCaptureKind, CompletionHandler<void(std::optional<WebCore::Exception>&&)>&&) final;
    void setShouldListenToVoiceActivity(bool) final;

    WebView *m_webView;
};

#endif
