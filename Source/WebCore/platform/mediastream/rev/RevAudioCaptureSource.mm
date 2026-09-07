#import "config.h"

#if ENABLE(MEDIA_STREAM)

#import "CAAudioStreamDescription.h"
#import "CaptureDevice.h"
#import "CaptureDeviceManager.h"
#import "MediaConstraints.h"
#import "RealtimeMediaSource.h"
#import "RealtimeMediaSourceCenter.h"
#import "RealtimeMediaSourceFactory.h"
#import "RealtimeMediaSourceSettings.h"
#import "WebAudioBufferList.h"
#import <AVFoundation/AVFoundation.h>
#import <pal/avfoundation/MediaTimeAVFoundation.h>
#import <pal/cf/CoreMediaSoftLink.h>
#import <wtf/NeverDestroyed.h>
#import <wtf/OSObjectPtr.h>
#import <wtf/RetainPtr.h>
#import <wtf/ThreadSafeWeakPtr.h>

namespace WebCore {
class RevMicSource;
}

@interface RevAudioSampleDelegate : NSObject <AVCaptureAudioDataOutputSampleBufferDelegate> {
    WebCore::RevMicSource* _source;
}
- (id)initWithSource:(WebCore::RevMicSource*)source;
- (void)invalidate;
@end

namespace WebCore {

class RevMicDeviceManager final : public CaptureDeviceManager {
public:
    static RevMicDeviceManager& singleton()
    {
        static NeverDestroyed<RevMicDeviceManager> manager;
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
        for (AVCaptureDevice* device in [AVCaptureDevice devicesWithMediaType:AVMediaTypeAudio])
            m_devices.append(CaptureDevice(String { [device uniqueID] }, CaptureDevice::DeviceType::Microphone, "Microphone"_s, emptyString(), true));
        m_computed = true;
    }

    Vector<CaptureDevice> m_devices;
    bool m_computed { false };
};

class RevMicSource final : public RealtimeMediaSource, public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<RevMicSource, WTF::DestructionThread::MainRunLoop> {
public:
    WTF_ABSTRACT_THREAD_SAFE_REF_COUNTED_AND_CAN_MAKE_WEAK_PTR_IMPL;

    static Ref<RevMicSource> create(const CaptureDevice& device, MediaDeviceHashSalts&& salts, std::optional<PageIdentifier> pageIdentifier)
    {
        return adoptRef(*new RevMicSource(device, WTF::move(salts), pageIdentifier));
    }

    ~RevMicSource();

    void deliverSampleBuffer(CMSampleBufferRef);

private:
    RevMicSource(const CaptureDevice&, MediaDeviceHashSalts&&, std::optional<PageIdentifier>);

    const RealtimeMediaSourceCapabilities& capabilities() final;
    const RealtimeMediaSourceSettings& settings() final;
    void startProducingData() final;
    void stopProducingData() final;
    void settingsDidChange(OptionSet<RealtimeMediaSourceSettings::Flag>) final;

    bool setupSession();

    String m_persistentId;
    RetainPtr<AVCaptureSession> m_session;
    RetainPtr<AVCaptureAudioDataOutput> m_output;
    RetainPtr<RevAudioSampleDelegate> m_delegate;
    OSObjectPtr<dispatch_queue_t> m_queue;
    std::optional<RealtimeMediaSourceCapabilities> m_capabilities;
    std::optional<RealtimeMediaSourceSettings> m_currentSettings;
};

RevMicSource::RevMicSource(const CaptureDevice& device, MediaDeviceHashSalts&& salts, std::optional<PageIdentifier> pageIdentifier)
    : RealtimeMediaSource(device, WTF::move(salts), pageIdentifier)
    , m_persistentId(device.persistentId())
{
    setSampleRate(44100);
}

RevMicSource::~RevMicSource()
{
    stopProducingData();
}

void RevMicSource::settingsDidChange(OptionSet<RealtimeMediaSourceSettings::Flag>)
{
    m_currentSettings = std::nullopt;
}

const RealtimeMediaSourceSettings& RevMicSource::settings()
{
    if (m_currentSettings)
        return *m_currentSettings;

    RealtimeMediaSourceSettings settings;
    settings.setDeviceId(hashedId());
    settings.setLabel(AtomString { name() });
    settings.setVolume(volume());
    settings.setEchoCancellation(echoCancellation());
    settings.setSampleRate(sampleRate());

    RealtimeMediaSourceSupportedConstraints supportedConstraints;
    supportedConstraints.setSupportsDeviceId(true);
    supportedConstraints.setSupportsVolume(true);
    supportedConstraints.setSupportsEchoCancellation(true);
    supportedConstraints.setSupportsSampleRate(true);
    settings.setSupportedConstraints(supportedConstraints);

    m_currentSettings = WTF::move(settings);
    return *m_currentSettings;
}

const RealtimeMediaSourceCapabilities& RevMicSource::capabilities()
{
    if (m_capabilities)
        return *m_capabilities;

    RealtimeMediaSourceCapabilities capabilities(settings().supportedConstraints());
    capabilities.setDeviceId(hashedId());
    capabilities.setVolume({ 0.0, 1.0 });
    capabilities.setEchoCancellation(RealtimeMediaSourceCapabilities::EchoCancellation::OnOrOff);
    capabilities.setSampleRate({ 8000, 48000 });

    m_capabilities = WTF::move(capabilities);
    return *m_capabilities;
}

bool RevMicSource::setupSession()
{
    if (m_session)
        return true;

    AVCaptureDevice* device = [AVCaptureDevice deviceWithUniqueID:m_persistentId.createNSString().get()];
    if (!device)
        device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeAudio];
    if (!device)
        return false;

    NSError* error = nil;
    AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
    if (!input || error)
        return false;

    RetainPtr<AVCaptureSession> session = adoptNS([[AVCaptureSession alloc] init]);
    if (![session canAddInput:input])
        return false;
    [session addInput:input];

    RetainPtr<AVCaptureAudioDataOutput> output = adoptNS([[AVCaptureAudioDataOutput alloc] init]);
    m_queue = adoptOSObject(dispatch_queue_create("org.rev.mic.capture", DISPATCH_QUEUE_SERIAL));
    m_delegate = adoptNS([[RevAudioSampleDelegate alloc] initWithSource:this]);
    [output setSampleBufferDelegate:m_delegate.get() queue:m_queue.get()];

    if (![session canAddOutput:output])
        return false;
    [session addOutput:output];

    m_session = WTF::move(session);
    m_output = WTF::move(output);
    return true;
}

void RevMicSource::startProducingData()
{
    if (!setupSession())
        return;
    RetainPtr<AVCaptureSession> session = m_session;
    dispatch_async(m_queue.get(), ^{
        [session.get() startRunning];
    });
}

void RevMicSource::stopProducingData()
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

void RevMicSource::deliverSampleBuffer(CMSampleBufferRef sampleBuffer)
{
    if (!sampleBuffer)
        return;

    CMFormatDescriptionRef formatDescription = PAL::CMSampleBufferGetFormatDescription(sampleBuffer);
    if (!formatDescription)
        return;
    const AudioStreamBasicDescription* asbd = PAL::CMAudioFormatDescriptionGetStreamBasicDescription(formatDescription);
    if (!asbd)
        return;

    CAAudioStreamDescription description(*asbd);
    auto bufferList = makeUnique<WebAudioBufferList>(description, sampleBuffer);
    if (!bufferList->bufferCount())
        return;

    auto time = PAL::toMediaTime(PAL::CMSampleBufferGetPresentationTimeStamp(sampleBuffer));
    audioSamplesAvailable(time, *bufferList, description, PAL::CMSampleBufferGetNumSamples(sampleBuffer));
}

class RevAudioCaptureFactory final : public AudioCaptureFactory {
public:
    CaptureSourceOrError createAudioCaptureSource(const CaptureDevice& device, MediaDeviceHashSalts&& salts, const MediaConstraints*, std::optional<PageIdentifier> pageIdentifier) final
    {
        return CaptureSourceOrError { RevMicSource::create(device, WTF::move(salts), pageIdentifier) };
    }

    CaptureDeviceManager& audioCaptureDeviceManager() final
    {
        return RevMicDeviceManager::singleton();
    }

    const Vector<CaptureDevice>& speakerDevices() const final
    {
        static NeverDestroyed<Vector<CaptureDevice>> devices;
        return devices.get();
    }
};

AudioCaptureFactory& RealtimeMediaSourceCenter::defaultAudioCaptureFactory()
{
    static NeverDestroyed<RevAudioCaptureFactory> factory;
    return factory.get();
}

} // namespace WebCore

@implementation RevAudioSampleDelegate

- (id)initWithSource:(WebCore::RevMicSource*)source
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
