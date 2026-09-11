#import "config.h"
#import "WebUserMediaClient.h"

#if ENABLE(MEDIA_STREAM)

#import <WebCore/CaptureDevice.h>
#import <WebCore/CaptureDeviceManager.h>
#import <WebCore/CaptureDeviceWithCapabilities.h>
#import <WebCore/Document.h>
#import <WebCore/MediaConstraints.h>
#import <WebCore/MediaDeviceHashSalts.h>
#import <WebCore/MediaStreamRequest.h>
#import <WebCore/RealtimeMediaSourceCenter.h>
#import <WebCore/RealtimeMediaSourceFactory.h>
#import <WebCore/UserMediaRequest.h>
#import <wtf/CompletionHandler.h>
#import <wtf/UUID.h>

using namespace WebCore;

static MediaDeviceHashSalts deviceHashSalts()
{
    static NeverDestroyed<String> persistent(createVersion4UUIDString());
    static NeverDestroyed<String> ephemeral(createVersion4UUIDString());
    return { persistent.get(), ephemeral.get() };
}

WebUserMediaClient::WebUserMediaClient(WebView *webView)
    : m_webView(webView)
{
}

void WebUserMediaClient::requestUserMediaAccess(UserMediaRequest& request)
{
    auto& streamRequest = request.request();
    auto& center = RealtimeMediaSourceCenter::singleton();

    CaptureDevice audioDevice;
    if (streamRequest.audioConstraints.isValid) {
        auto& devices = center.audioCaptureFactory().audioCaptureDeviceManager().captureDevices();
        if (!devices.isEmpty())
            audioDevice = devices.first();
    }

    CaptureDevice videoDevice;
    if (streamRequest.videoConstraints.isValid) {
        auto& devices = center.videoCaptureFactory().videoCaptureDeviceManager().captureDevices();
        if (!devices.isEmpty())
            videoDevice = devices.first();
    }

    if (!audioDevice && !videoDevice) {
        request.deny(MediaAccessDenialReason::PermissionDenied);
        return;
    }

    request.allow(WTF::move(audioDevice), WTF::move(videoDevice), deviceHashSalts(), [] { });
}

void WebUserMediaClient::cancelUserMediaAccessRequest(UserMediaRequest&)
{
}

void WebUserMediaClient::enumerateMediaDevices(Document&, EnumerateDevicesCallback&& completionHandler)
{
    auto& center = RealtimeMediaSourceCenter::singleton();
    Vector<CaptureDeviceWithCapabilities> devices;
    for (auto& device : center.videoCaptureFactory().videoCaptureDeviceManager().captureDevices())
        devices.append({ device, { } });
    for (auto& device : center.audioCaptureFactory().audioCaptureDeviceManager().captureDevices())
        devices.append({ device, { } });
    completionHandler(WTF::move(devices), deviceHashSalts());
}

WebUserMediaClient::DeviceChangeObserverToken WebUserMediaClient::addDeviceChangeObserver(WTF::Function<void()>&&)
{
    return DeviceChangeObserverToken { 0 };
}

void WebUserMediaClient::removeDeviceChangeObserver(DeviceChangeObserverToken)
{
}

void WebUserMediaClient::updateCaptureState(const Document&, bool, MediaProducerMediaCaptureKind, CompletionHandler<void(std::optional<Exception>&&)>&& completionHandler)
{
    completionHandler({ });
}

void WebUserMediaClient::setShouldListenToVoiceActivity(bool)
{
}

#endif
