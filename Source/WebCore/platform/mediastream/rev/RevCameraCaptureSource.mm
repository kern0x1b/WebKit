#import "config.h"

#if ENABLE(MEDIA_STREAM)

#import "CaptureDevice.h"
#import "CaptureDeviceManager.h"
#import "FloatSize.h"
#import "IntSize.h"
#import "MediaConstraints.h"
#import "PlatformVideoColorSpace.h"
#import "RealtimeMediaSourceCapabilities.h"
#import "RealtimeMediaSourceCenter.h"
#import "RealtimeMediaSourceFactory.h"
#import "RealtimeMediaSourceSettings.h"
#import "RealtimeVideoCaptureSource.h"
#import "VideoFrameCV.h"
#import "VideoPreset.h"
#import <AVFoundation/AVFoundation.h>
#import <pal/avfoundation/MediaTimeAVFoundation.h>
#import <pal/cf/CoreMediaSoftLink.h>
#import <wtf/MonotonicTime.h>
#import <wtf/NeverDestroyed.h>
#import <wtf/OSObjectPtr.h>
#import <wtf/RetainPtr.h>

namespace WebCore {
class RevCameraSource;
}

@interface RevCameraSampleDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
    WebCore::RevCameraSource* _source;
}
- (id)initWithSource:(WebCore::RevCameraSource*)source;
- (void)invalidate;
@end

namespace WebCore {

static constexpr int kRevCaptureWidth = 640;
static constexpr int kRevCaptureHeight = 480;
static constexpr double kRevCaptureFrameRate = 30;

class RevCameraDeviceManager final : public CaptureDeviceManager {
public:
    static RevCameraDeviceManager& singleton()
    {
        static NeverDestroyed<RevCameraDeviceManager> manager;
        return manager.get();
    }

    const Vector<CaptureDevice>& captureDevices() final
    {
        if (!m_computed)
            refresh();
        return m_devices;
    }

private:
    void refresh()
    {
        m_devices.clear();
        for (AVCaptureDevice* device in [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo]) {
            String label = [device position] == AVCaptureDevicePositionFront ? "Front Camera"_s : "Back Camera"_s;
            m_devices.append(CaptureDevice(String { [device uniqueID] }, CaptureDevice::DeviceType::Camera, label, emptyString(), true));
        }
        m_computed = true;
    }

    Vector<CaptureDevice> m_devices;
    bool m_computed { false };
};

class RevCameraSource final : public RealtimeVideoCaptureSource {
public:
    static Ref<RevCameraSource> create(const CaptureDevice& device, MediaDeviceHashSalts&& salts, std::optional<PageIdentifier> pageIdentifier)
    {
        return adoptRef(*new RevCameraSource(device, WTF::move(salts), pageIdentifier));
    }

    void deliverSampleBuffer(CMSampleBufferRef);

private:
    RevCameraSource(const CaptureDevice&, MediaDeviceHashSalts&&, std::optional<PageIdentifier>);
    ~RevCameraSource();

    const RealtimeMediaSourceCapabilities& capabilities() final;
    const RealtimeMediaSourceSettings& settings() final;
    void generatePresets() final;
    void startProducingData() final;
    void stopProducingData() final;
    void settingsDidChange(OptionSet<RealtimeMediaSourceSettings::Flag>) final;

    bool setupSession();

    String m_persistentId;
    RetainPtr<AVCaptureSession> m_session;
    RetainPtr<AVCaptureVideoDataOutput> m_output;
    RetainPtr<RevCameraSampleDelegate> m_delegate;
    OSObjectPtr<dispatch_queue_t> m_queue;
    std::optional<RealtimeMediaSourceCapabilities> m_capabilities;
    std::optional<RealtimeMediaSourceSettings> m_currentSettings;
};

RevCameraSource::RevCameraSource(const CaptureDevice& device, MediaDeviceHashSalts&& salts, std::optional<PageIdentifier> pageIdentifier)
    : RealtimeVideoCaptureSource(device, WTF::move(salts), pageIdentifier)
    , m_persistentId(device.persistentId())
{
    setIntrinsicSize({ kRevCaptureWidth, kRevCaptureHeight });
    setSize({ kRevCaptureWidth, kRevCaptureHeight });
    setFrameRate(kRevCaptureFrameRate);
    setFacingMode(VideoFacingMode::Environment);
}

RevCameraSource::~RevCameraSource()
{
    stopProducingData();
}

void RevCameraSource::generatePresets()
{
    Vector<VideoPresetData> presets;
    presets.append(VideoPresetData {
        IntSize { kRevCaptureWidth, kRevCaptureHeight },
        Vector<FrameRateRange> { { 1.0, kRevCaptureFrameRate } },
        1, 1, false
    });
    setSupportedPresets(WTF::move(presets));
}

const RealtimeMediaSourceCapabilities& RevCameraSource::capabilities()
{
    if (m_capabilities)
        return *m_capabilities;

    RealtimeMediaSourceSupportedConstraints supportedConstraints;
    supportedConstraints.setSupportsWidth(true);
    supportedConstraints.setSupportsHeight(true);
    supportedConstraints.setSupportsFrameRate(true);
    supportedConstraints.setSupportsFacingMode(true);
    supportedConstraints.setSupportsDeviceId(true);

    RealtimeMediaSourceCapabilities capabilities(supportedConstraints);
    capabilities.setDeviceId(hashedId());
    capabilities.setWidth({ kRevCaptureWidth, kRevCaptureWidth });
    capabilities.setHeight({ kRevCaptureHeight, kRevCaptureHeight });
    capabilities.setFrameRate({ 1.0, kRevCaptureFrameRate });
    capabilities.addFacingMode(VideoFacingMode::Environment);
    capabilities.setSupportedConstraints(supportedConstraints);

    m_capabilities = WTF::move(capabilities);
    return *m_capabilities;
}

const RealtimeMediaSourceSettings& RevCameraSource::settings()
{
    if (m_currentSettings)
        return *m_currentSettings;

    RealtimeMediaSourceSettings settings;
    settings.setLabel(name());
    settings.setDeviceId(hashedId());
    settings.setFacingMode(facingMode());
    settings.setFrameRate(frameRate());
    auto currentSize = size();
    settings.setWidth(currentSize.width());
    settings.setHeight(currentSize.height());

    RealtimeMediaSourceSupportedConstraints supportedConstraints;
    supportedConstraints.setSupportsWidth(true);
    supportedConstraints.setSupportsHeight(true);
    supportedConstraints.setSupportsFrameRate(true);
    supportedConstraints.setSupportsFacingMode(true);
    supportedConstraints.setSupportsDeviceId(true);
    settings.setSupportedConstraints(supportedConstraints);

    m_currentSettings = WTF::move(settings);
    return *m_currentSettings;
}

void RevCameraSource::settingsDidChange(OptionSet<RealtimeMediaSourceSettings::Flag>)
{
    m_currentSettings = std::nullopt;
}

bool RevCameraSource::setupSession()
{
    if (m_session)
        return true;

    AVCaptureDevice* device = [AVCaptureDevice deviceWithUniqueID:m_persistentId.createNSString().get()];
    if (!device)
        return false;

    NSError* error = nil;
    AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
    if (!input || error)
        return false;

    RetainPtr<AVCaptureSession> session = adoptNS([[AVCaptureSession alloc] init]);
    if ([session canSetSessionPreset:AVCaptureSessionPreset640x480])
        [session setSessionPreset:AVCaptureSessionPreset640x480];

    if (![session canAddInput:input])
        return false;
    [session addInput:input];

    RetainPtr<AVCaptureVideoDataOutput> output = adoptNS([[AVCaptureVideoDataOutput alloc] init]);
    [output setVideoSettings:@{ (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA) }];
    [output setAlwaysDiscardsLateVideoFrames:YES];

    m_queue = adoptOSObject(dispatch_queue_create("org.rev.camera.capture", DISPATCH_QUEUE_SERIAL));
    m_delegate = adoptNS([[RevCameraSampleDelegate alloc] initWithSource:this]);
    [output setSampleBufferDelegate:m_delegate.get() queue:m_queue.get()];

    if (![session canAddOutput:output])
        return false;
    [session addOutput:output];

    AVCaptureConnection* connection = [output connectionWithMediaType:AVMediaTypeVideo];
    if ([connection isVideoOrientationSupported])
        [connection setVideoOrientation:AVCaptureVideoOrientationPortrait];

    m_session = WTF::move(session);
    m_output = WTF::move(output);
    return true;
}

void RevCameraSource::startProducingData()
{
    if (!setupSession())
        return;
    RetainPtr<AVCaptureSession> session = m_session;
    dispatch_async(m_queue.get(), ^{
        [session.get() startRunning];
    });
}

void RevCameraSource::stopProducingData()
{
    if (!m_session)
        return;
    [m_session stopRunning];
    [m_delegate invalidate];
    [m_output setSampleBufferDelegate:nil queue:nil];
    m_session = nullptr;
    m_output = nullptr;
    m_delegate = nullptr;
    m_queue = nullptr;
}

void RevCameraSource::deliverSampleBuffer(CMSampleBufferRef sampleBuffer)
{
    if (!sampleBuffer)
        return;

    RetainPtr<CVPixelBufferRef> pixelBuffer = static_cast<CVPixelBufferRef>(PAL::CMSampleBufferGetImageBuffer(sampleBuffer));
    if (!pixelBuffer)
        return;
    auto timeStamp = PAL::CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    PlatformVideoColorSpace colorSpace;
    auto videoFrame = VideoFrameCV::create(PAL::toMediaTime(timeStamp), false, VideoFrame::Rotation::None, WTF::move(pixelBuffer), WTF::move(colorSpace));
    setIntrinsicSize(expandedIntSize(videoFrame->presentationSize()));

    VideoFrameTimeMetadata metadata;
    metadata.captureTime = MonotonicTime::now().secondsSinceEpoch();
    dispatchVideoFrameToObservers(videoFrame.get(), metadata);
}

class RevVideoCaptureFactory final : public VideoCaptureFactory {
public:
    CaptureSourceOrError createVideoCaptureSource(const CaptureDevice& device, MediaDeviceHashSalts&& salts, const MediaConstraints*, std::optional<PageIdentifier> pageIdentifier) final
    {
        return CaptureSourceOrError { RevCameraSource::create(device, WTF::move(salts), pageIdentifier) };
    }

    CaptureDeviceManager& videoCaptureDeviceManager() final
    {
        return RevCameraDeviceManager::singleton();
    }
};

VideoCaptureFactory& RealtimeMediaSourceCenter::defaultVideoCaptureFactory()
{
    static NeverDestroyed<RevVideoCaptureFactory> factory;
    return factory.get();
}

} // namespace WebCore

@implementation RevCameraSampleDelegate

- (id)initWithSource:(WebCore::RevCameraSource*)source
{
    if ((self = [super init]))
        _source = source;
    return self;
}

- (void)invalidate
{
    _source = nullptr;
}

- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer fromConnection:(AVCaptureConnection*)connection
{
    UNUSED_PARAM(output);
    UNUSED_PARAM(connection);
    if (_source)
        _source->deliverSampleBuffer(sampleBuffer);
}

@end

#endif // ENABLE(MEDIA_STREAM)
