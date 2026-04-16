#ifndef BATTERYMONITOR_H
#define BATTERYMONITOR_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QTimer>
#include <QJsonObject>

class BatteryMonitor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int level READ level NOTIFY levelChanged)
    Q_PROPERTY(bool charging READ charging NOTIFY chargingChanged)

    friend class TestBatteryMonitor;
    friend class TestDBusInterface;
    friend class TestAutomationEngine;

public:
    explicit BatteryMonitor(const QString &configDir, QObject *parent = nullptr);

    void start();
    void stop();

    int level() const;
    bool charging() const;

    struct BatteryEntry {
        qint64 timestamp;
        int level;
        bool charging;
        QString activeProfile;
        bool screenOn;
        bool workoutActive;

        BatteryEntry()
            : timestamp(0), level(0), charging(false), screenOn(false), workoutActive(false) {}

        BatteryEntry(qint64 ts, int lvl, bool chrg, const QString &profile, bool screen, bool workout)
            : timestamp(ts), level(lvl), charging(chrg), activeProfile(profile),
              screenOn(screen), workoutActive(workout) {}
    };

    QVector<BatteryEntry> history(int hours) const;
    QJsonObject prediction() const;

    int historyDays() const;
    void setHistoryDays(int days);

signals:
    void levelChanged(int level);
    void chargingChanged(bool charging);
    void significantChange(int level, bool charging);

public slots:
    void setActiveProfile(const QString &profileId);
    void setWorkoutActive(bool active);

private slots:
    void pollBattery();
    void heartbeat();

private:
    void readBatteryLevel();
    void recordEntry();
    void loadHistory();
    void saveHistory();
    void trimHistory();
    QString findPowerSupplyPath() const;

    QString m_configDir;
    QString m_powerSupplyPath;
    int m_level;
    bool m_charging;
    int m_lastRecordedLevel;
    QString m_activeProfile;
    bool m_workoutActive;
    int m_historyDays;

    QVector<BatteryEntry> m_history;
    QTimer *m_pollTimer;
    QTimer *m_heartbeatTimer;
};

#endif // BATTERYMONITOR_H
