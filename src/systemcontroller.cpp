#include "systemcontroller.h"
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusReply>
#include <QDBusVariant>
#include <QDebug>

// MCE D-Bus constants
static const char *MCE_SERVICE = "com.nokia.mce";
static const char *MCE_REQUEST_PATH = "/com/nokia/mce/request";
static const char *MCE_REQUEST_INTERFACE = "com.nokia.mce.request";

// MCE configuration paths
static const char *MCE_LOW_POWER_MODE_PATH = "/system/osso/dsm/display/use_low_power_mode";
static const char *MCE_TILT_TO_WAKE_PATH = "/system/osso/dsm/display/tilt_to_wake";

// systemd D-Bus constants
static const char *SYSTEMD_SERVICE = "org.freedesktop.systemd1";
static const char *SYSTEMD_PATH = "/org/freedesktop/systemd1";
static const char *SYSTEMD_MANAGER_INTERFACE = "org.freedesktop.systemd1.Manager";

// Background sync services
static const char *BTSYNCD_SERVICE = "asteroid-btsyncd.service";

SystemController::SystemController(QObject *parent)
    : QObject(parent)
    , m_mceRequest(nullptr)
    , m_systemd(nullptr)
    , m_mceAvailable(false)
    , m_systemdAvailable(false)
    , m_radiosEnabled(false)
{
    // Initialize MCE D-Bus interface
    m_mceRequest = new QDBusInterface(
        MCE_SERVICE,
        MCE_REQUEST_PATH,
        MCE_REQUEST_INTERFACE,
        QDBusConnection::systemBus(),
        this
    );

    if (m_mceRequest->isValid()) {
        m_mceAvailable = true;
        qDebug() << "SystemController: MCE interface available";
    } else {
        qWarning() << "SystemController: MCE not available:" << m_mceRequest->lastError().message();
        qWarning() << "SystemController: Display control features will be unavailable";
    }

    // Initialize systemd D-Bus interface
    m_systemd = new QDBusInterface(
        SYSTEMD_SERVICE,
        SYSTEMD_PATH,
        SYSTEMD_MANAGER_INTERFACE,
        QDBusConnection::systemBus(),
        this
    );

    if (m_systemd->isValid()) {
        m_systemdAvailable = true;
        qDebug() << "SystemController: systemd interface available";
    } else {
        qWarning() << "SystemController: systemd not available:" << m_systemd->lastError().message();
        qWarning() << "SystemController: Service control features will be unavailable";
    }

    // Initialize current config to defaults
    m_currentConfig.background_sync = BackgroundSyncMode::Auto;
    m_currentConfig.always_on_display = false;
    m_currentConfig.tilt_to_wake = true;
}

SystemController::~SystemController()
{
    // QObject parent will clean up interfaces
}

bool SystemController::applyConfig(const SystemConfig &config)
{
    qDebug() << "SystemController: Applying system config";
    
    bool success = true;

    // Apply always-on display setting
    if (config.always_on_display != m_currentConfig.always_on_display) {
        if (!setAlwaysOnDisplay(config.always_on_display)) {
            qWarning() << "SystemController: Failed to set always-on display";
            success = false;
        }
    }

    // Apply tilt-to-wake setting
    if (config.tilt_to_wake != m_currentConfig.tilt_to_wake) {
        if (!setTiltToWake(config.tilt_to_wake)) {
            qWarning() << "SystemController: Failed to set tilt-to-wake";
            success = false;
        }
    }

    // Apply background sync setting
    if (config.background_sync != m_currentConfig.background_sync) {
        if (!setBackgroundSync(config.background_sync)) {
            qWarning() << "SystemController: Failed to set background sync mode";
            success = false;
        }
    }

    if (success) {
        m_currentConfig = config;
        emit configApplied(config);
        qDebug() << "SystemController: Config applied successfully";
    }

    return success;
}

SystemConfig SystemController::currentConfig() const
{
    return m_currentConfig;
}

bool SystemController::setAlwaysOnDisplay(bool enabled)
{
    qDebug() << "SystemController: Setting always-on display to" << enabled;

    if (!setMceConfig(MCE_LOW_POWER_MODE_PATH, enabled)) {
        emit systemError(QStringLiteral("always_on_display"), 
                        QStringLiteral("Failed to set MCE low power mode"));
        return false;
    }

    m_currentConfig.always_on_display = enabled;
    return true;
}

bool SystemController::setTiltToWake(bool enabled)
{
    qDebug() << "SystemController: Setting tilt-to-wake to" << enabled;

    if (!setMceConfig(MCE_TILT_TO_WAKE_PATH, enabled)) {
        emit systemError(QStringLiteral("tilt_to_wake"), 
                        QStringLiteral("Failed to set MCE tilt-to-wake"));
        return false;
    }

    m_currentConfig.tilt_to_wake = enabled;
    return true;
}

bool SystemController::setBackgroundSync(BackgroundSyncMode mode)
{
    qDebug() << "SystemController: Setting background sync to" << bgSyncModeToString(mode);

    if (!m_systemdAvailable) {
        qWarning() << "SystemController: Cannot control background sync - systemd unavailable";
        // Don't treat this as fatal - update our state and continue
        m_currentConfig.background_sync = mode;
        return true;
    }

    bool shouldRun = false;

    switch (mode) {
    case BackgroundSyncMode::Auto:
        // Always run in auto mode
        shouldRun = true;
        break;

    case BackgroundSyncMode::WhenRadiosOn:
        // Only run if radios are enabled
        shouldRun = m_radiosEnabled;
        break;

    case BackgroundSyncMode::Off:
        // Never run
        shouldRun = false;
        break;
    }

    // Control the btsyncd service
    if (!controlSystemdService(BTSYNCD_SERVICE, shouldRun)) {
        emit systemError(QStringLiteral("background_sync"),
                        QStringLiteral("Failed to control sync service"));
        return false;
    }

    m_currentConfig.background_sync = mode;
    return true;
}

bool SystemController::isMceAvailable() const
{
    return m_mceAvailable;
}

bool SystemController::isSystemdAvailable() const
{
    return m_systemdAvailable;
}

void SystemController::updateBackgroundSyncServices(BackgroundSyncMode mode, bool radiosEnabled)
{
    // Store radio state for WhenRadiosOn mode
    m_radiosEnabled = radiosEnabled;

    // If we're in WhenRadiosOn mode, update service state based on radio state
    if (mode == BackgroundSyncMode::WhenRadiosOn) {
        setBackgroundSync(mode);
    }
}

bool SystemController::setMceConfig(const QString &path, bool value)
{
    if (!m_mceAvailable) {
        qWarning() << "SystemController: MCE not available, cannot set" << path;
        return false;
    }

    if (!m_mceRequest->isValid()) {
        qWarning() << "SystemController: MCE interface invalid";
        return false;
    }

    // MCE set_config method signature: set_config(string path, variant value)
    QDBusReply<void> reply = m_mceRequest->call(
        QStringLiteral("set_config"),
        path,
        QVariant::fromValue(QDBusVariant(value))
    );

    if (!reply.isValid()) {
        qWarning() << "SystemController: MCE set_config failed for" << path
                   << ":" << reply.error().message();
        return false;
    }

    qDebug() << "SystemController: MCE config" << path << "set to" << value;
    return true;
}

bool SystemController::controlSystemdService(const QString &service, bool start)
{
    if (!m_systemdAvailable) {
        qWarning() << "SystemController: systemd not available, cannot control" << service;
        return false;
    }

    if (!m_systemd->isValid()) {
        qWarning() << "SystemController: systemd interface invalid";
        return false;
    }

    const QString method = start ? QStringLiteral("StartUnit") : QStringLiteral("StopUnit");
    const QString mode = QStringLiteral("replace");

    qDebug() << "SystemController:" << (start ? "Starting" : "Stopping") << service;

    QDBusReply<QDBusObjectPath> reply = m_systemd->call(
        method,
        service,
        mode
    );

    if (!reply.isValid()) {
        // Check if the error is because the service is already in the desired state
        const QString errorName = m_systemd->lastError().name();
        if (errorName == QLatin1String("org.freedesktop.systemd1.NoSuchUnit")) {
            qWarning() << "SystemController: Service" << service << "does not exist";
            // Don't treat as fatal - service might not be installed
            return true;
        }
        
        qWarning() << "SystemController: Failed to" << method << service
                   << ":" << m_systemd->lastError().message();
        return false;
    }

    qDebug() << "SystemController: Service" << service << (start ? "started" : "stopped");
    return true;
}
