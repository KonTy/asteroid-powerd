#ifndef AUTOMATIONENGINE_H
#define AUTOMATIONENGINE_H

#include <QObject>
#include <QString>
#include <QTimer>
#include "profilemanager.h"
#include "batterymonitor.h"
#include "profilemodel.h"

class AutomationEngine : public QObject
{
    Q_OBJECT

public:
    explicit AutomationEngine(ProfileManager *pm, BatteryMonitor *bm, QObject *parent = nullptr);

    void start();
    void stop();

    // State
    bool isWorkoutActive() const;
    QString activeWorkoutType() const;

    // Workout control (called from D-Bus interface)
    bool startWorkout(const QString &workoutType);
    bool stopWorkout();

    // Sleep mode (used by RadioController)
    bool isSleepTime() const;

signals:
    void profileSwitchRequested(const QString &profileId, const QString &reason);
    void workoutStarted(const QString &workoutType, const QString &profileId);
    void workoutStopped();
    void sleepModeChanged(bool sleeping);

private slots:
    void onBatteryLevelChanged(int level);
    void onTimeCheck();
    void evaluateRules();

private:
    // Rule evaluation
    void evaluateBatteryRules(int batteryLevel);
    void evaluateTimeRules();
    QString findBatteryRuleProfile(int level, const QVector<BatteryRule> &rules) const;
    bool isInTimeWindow(const QString &start, const QString &end) const;
    bool isInSleepWindow() const;

    ProfileManager *m_profileManager;
    BatteryMonitor *m_batteryMonitor;

    // Timers
    QTimer *m_timeCheckTimer;   // Check time rules every 60 seconds

    // Workout state
    bool m_workoutActive;
    QString m_workoutType;
    QString m_preWorkoutProfileId;  // Profile to restore after workout

    // Rule state tracking (prevent re-triggering)
    QString m_lastBatteryTriggeredProfile;  // Last profile triggered by battery rule
    QString m_lastTimeTriggeredProfile;     // Last profile triggered by time rule
    int m_lastBatteryLevel;

    // Sleep mode tracking
    bool m_inSleepWindow;

    // Priority system for rule conflicts
    enum RuleSource {
        SourceManual = 0,       // User selected
        SourceTimeRule = 1,     // Time-of-day rule
        SourceBatteryRule = 2,  // Battery threshold rule
        SourceWorkout = 3       // Workout assignment (highest priority)
    };
    RuleSource m_currentSource;
};

#endif // AUTOMATIONENGINE_H
