#include "config.h"
#include "RealtimeMediaSourceCenter.h"

#if ENABLE(MEDIA_STREAM)

#include "CaptureDeviceManager.h"
#include "DisplayCaptureManager.h"
#include "MockRealtimeMediaSourceCenter.h"
#include "RealtimeMediaSource.h"
#include "RealtimeMediaSourceFactory.h"
#include <wtf/NeverDestroyed.h>

namespace WebCore {

static const Vector<CaptureDevice>& emptyCaptureDevices()
{
    static NeverDestroyed<Vector<CaptureDevice>> devices;
    return devices.get();
}

class RevEmptyDisplayCaptureManager final : public DisplayCaptureManager {
public:
    const Vector<CaptureDevice>& captureDevices() final { return emptyCaptureDevices(); }
};

class RevDisplayCaptureFactory final : public DisplayCaptureFactory {
public:
    CaptureSourceOrError createDisplayCaptureSource(const CaptureDevice&, MediaDeviceHashSalts&&, const MediaConstraints*, std::optional<PageIdentifier>) final
    {
        return CaptureSourceOrError({ "Display capture not available"_s, MediaAccessDenialReason::PermissionDenied });
    }
    DisplayCaptureManager& displayCaptureDeviceManager() final
    {
        static NeverDestroyed<RevEmptyDisplayCaptureManager> manager;
        return manager.get();
    }
};

DisplayCaptureFactory& RealtimeMediaSourceCenter::defaultDisplayCaptureFactory()
{
    static NeverDestroyed<RevDisplayCaptureFactory> factory;
    return factory.get();
}

void MockRealtimeMediaSourceCenter::setMockRealtimeMediaSourceCenterEnabled(bool)
{
}

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM)
