#ifndef SYSTEMCONTROLLER_H
#define SYSTEMCONTROLLER_H

#include <QObject>
#include <QDBusInterface>
#include "profilemodel.h"

class SystemController : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY(SystemController)

public:
    explicit SystemController(QObject *parent = nullptr);
    ~SystemController() override;

    bool applyConfig(const SystemConfig &config);
    SystemConfig currentConfig() const;

    bool setAlwaysOnDisplay(bool enabled);
    bool setTiltToWake(bool enabled);
    bool setBackgroundSync(BackgroundSyncMode mode);

    // Query availability
    bool isMceAvailable() const;
    bool isSystemdAvailable() const;

    // Called by RadioController when radio state changes
    void updateBackgroundSyncServices(BackgroundSyncMode mode, bool radiosEnabled);

signals:
    void systemError(const QString &component, const QString &error);
    void configApplied(const SystemConfig &config);

private:
    bool setMceConfig(const QString &path, bool value);
    bool controlSystemdService(const QString &service, bool start);

    // MCE D-Bus interface for display control
    QDBusInterface *m_mceRequest;

    // systemd D-Bus for service control
    QDBusInterface *m_systemd;

    SystemConfig m_currentConfig;
    
    // State tracking
    bool m_mceAvailable;
    bool m_systemdAvailable;
    bool m_radiosEnabled;  // Set externally by RadioController state
};

#endif // SYSTEMCONTROLLER_H
