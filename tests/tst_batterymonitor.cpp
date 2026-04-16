#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include "batterymonitor.h"
#include "common.h"

class TestBatteryMonitor : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void testInitialization();
    void testRingBufferAddEntries();
    void testRingBufferHistoryRetrieval();
    void testRingBufferTrimOldEntries();
    void testPredictionLinearDrain();
    void testPredictionCharging();
    void testPredictionInsufficientData();
    void testPredictionConfidenceLevels();
    void testChangeBasedRecording();
    void testHeartbeatRecording();
    void testHistoryPersistence();
    void testHistoryPersistenceRoundTrip();
    void testMissingBatterySysfs();
    void testSysfsReading();
    void testHistoryDaysConfiguration();
    void testAdaptivePollInterval();
    void testWorkoutPollSwitch();
    void testPredictionNoVectorCopy();

private:
    void createMockSysfs(const QString &path, int capacity, const QString &status);
    QTemporaryDir *m_tempDir;
    QString m_configDir;
    QString m_mockSysfsDir;
};

void TestBatteryMonitor::initTestCase()
{
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());
}

void TestBatteryMonitor::cleanupTestCase()
{
    delete m_tempDir;
}

void TestBatteryMonitor::init()
{
    m_configDir = m_tempDir->path() + "/config";
    m_mockSysfsDir = m_tempDir->path() + "/sysfs";
    QDir().mkpath(m_configDir);
    QDir().mkpath(m_mockSysfsDir);
}

void TestBatteryMonitor::cleanup()
{
    QDir configDir(m_configDir);
    configDir.removeRecursively();
    QDir sysfsDir(m_mockSysfsDir);
    sysfsDir.removeRecursively();
}

void TestBatteryMonitor::createMockSysfs(const QString &path, int capacity, const QString &status)
{
    QDir().mkpath(path);
    
    QFile typeFile(path + "/type");
    QVERIFY(typeFile.open(QIODevice::WriteOnly | QIODevice::Text));
    typeFile.write("Battery\n");
    typeFile.close();

    QFile capacityFile(path + "/capacity");
    QVERIFY(capacityFile.open(QIODevice::WriteOnly | QIODevice::Text));
    capacityFile.write(QByteArray::number(capacity) + "\n");
    capacityFile.close();

    QFile statusFile(path + "/status");
    QVERIFY(statusFile.open(QIODevice::WriteOnly | QIODevice::Text));
    statusFile.write(status.toLatin1() + "\n");
    statusFile.close();
}

void TestBatteryMonitor::testInitialization()
{
    BatteryMonitor monitor(m_configDir);
    
    QCOMPARE(monitor.level(), 100);
    QCOMPARE(monitor.charging(), false);
    QCOMPARE(monitor.historyDays(), DEFAULT_BATTERY_HISTORY_DAYS);
}

void TestBatteryMonitor::testRingBufferAddEntries()
{
    BatteryMonitor monitor(m_configDir);
    monitor.setActiveProfile("test_profile");

    qint64 baseTime = QDateTime::currentSecsSinceEpoch();
    
    for (int i = 0; i < 10; ++i) {
        BatteryMonitor::BatteryEntry entry(baseTime + i * 300, 90 - i, false, "test_profile", false, false);
        monitor.m_history.append(entry);
    }

    QCOMPARE(monitor.m_history.size(), 10);
    QCOMPARE(monitor.m_history.first().level, 90);
    QCOMPARE(monitor.m_history.last().level, 81);
}

void TestBatteryMonitor::testRingBufferHistoryRetrieval()
{
    BatteryMonitor monitor(m_configDir);
    
    qint64 now = QDateTime::currentSecsSinceEpoch();
    qint64 fiveHoursAgo = now - (5 * 3600);
    qint64 twoHoursAgo = now - (2 * 3600);
    qint64 oneHourAgo = now - (1 * 3600);

    BatteryMonitor::BatteryEntry oldEntry(fiveHoursAgo, 95, false, "profile1", false, false);
    BatteryMonitor::BatteryEntry medEntry(twoHoursAgo, 85, false, "profile1", false, false);
    BatteryMonitor::BatteryEntry recentEntry(oneHourAgo, 75, false, "profile1", false, false);

    monitor.m_history.append(oldEntry);
    monitor.m_history.append(medEntry);
    monitor.m_history.append(recentEntry);

    QVector<BatteryMonitor::BatteryEntry> last3Hours = monitor.history(3);
    QCOMPARE(last3Hours.size(), 2);
    QCOMPARE(last3Hours[0].level, 85);
    QCOMPARE(last3Hours[1].level, 75);

    QVector<BatteryMonitor::BatteryEntry> last10Hours = monitor.history(10);
    QCOMPARE(last10Hours.size(), 3);
}

void TestBatteryMonitor::testRingBufferTrimOldEntries()
{
    BatteryMonitor monitor(m_configDir);
    monitor.setHistoryDays(7);

    qint64 now = QDateTime::currentSecsSinceEpoch();
    qint64 tenDaysAgo = now - (10 * 86400);
    qint64 fiveDaysAgo = now - (5 * 86400);
    qint64 oneDayAgo = now - (1 * 86400);

    BatteryMonitor::BatteryEntry oldEntry(tenDaysAgo, 100, false, "profile1", false, false);
    BatteryMonitor::BatteryEntry medEntry(fiveDaysAgo, 80, false, "profile1", false, false);
    BatteryMonitor::BatteryEntry recentEntry(oneDayAgo, 60, false, "profile1", false, false);

    monitor.m_history.append(oldEntry);
    monitor.m_history.append(medEntry);
    monitor.m_history.append(recentEntry);

    QCOMPARE(monitor.m_history.size(), 3);

    monitor.trimHistory();

    QCOMPARE(monitor.m_history.size(), 2);
    QCOMPARE(monitor.m_history.first().level, 80);
    QCOMPARE(monitor.m_history.last().level, 60);
}

void TestBatteryMonitor::testPredictionLinearDrain()
{
    BatteryMonitor monitor(m_configDir);
    monitor.m_level = 50;
    monitor.m_charging = false;

    qint64 now = QDateTime::currentSecsSinceEpoch();
    
    BatteryMonitor::BatteryEntry entry1(now - 7200, 70, false, "profile1", false, false);
    BatteryMonitor::BatteryEntry entry2(now - 3600, 60, false, "profile1", false, false);
    BatteryMonitor::BatteryEntry entry3(now, 50, false, "profile1", false, false);

    monitor.m_history.append(entry1);
    monitor.m_history.append(entry2);
    monitor.m_history.append(entry3);

    QJsonObject prediction = monitor.prediction();

    QVERIFY(prediction.contains("hours_remaining"));
    QVERIFY(prediction.contains("drain_rate_per_hour"));
    QVERIFY(prediction.contains("confidence"));

    double drainRate = prediction["drain_rate_per_hour"].toDouble();
    QVERIFY(drainRate > 9.0 && drainRate < 11.0);

    double hoursRemaining = prediction["hours_remaining"].toDouble();
    QVERIFY(hoursRemaining > 4.0 && hoursRemaining < 6.0);

    QString confidence = prediction["confidence"].toString();
    QCOMPARE(confidence, QString("medium"));
}

void TestBatteryMonitor::testPredictionCharging()
{
    BatteryMonitor monitor(m_configDir);
    monitor.m_level = 60;
    monitor.m_charging = true;

    qint64 now = QDateTime::currentSecsSinceEpoch();
    
    BatteryMonitor::BatteryEntry entry1(now - 7200, 40, true, "profile1", false, false);
    BatteryMonitor::BatteryEntry entry2(now - 3600, 50, true, "profile1", false, false);
    BatteryMonitor::BatteryEntry entry3(now, 60, true, "profile1", false, false);

    monitor.m_history.append(entry1);
    monitor.m_history.append(entry2);
    monitor.m_history.append(entry3);

    QJsonObject prediction = monitor.prediction();

    QVERIFY(prediction.contains("charging"));
    QCOMPARE(prediction["charging"].toBool(), true);
    QVERIFY(prediction.contains("hours_to_full"));

    double hoursToFull = prediction["hours_to_full"].toDouble();
    QVERIFY(hoursToFull > 3.0 && hoursToFull < 5.0);
}

void TestBatteryMonitor::testPredictionInsufficientData()
{
    BatteryMonitor monitor(m_configDir);
    monitor.m_level = 50;
    monitor.m_charging = false;

    QJsonObject prediction = monitor.prediction();

    QVERIFY(prediction.contains("hours_remaining"));
    QCOMPARE(prediction["hours_remaining"].toInt(), -1);
    QVERIFY(prediction.contains("confidence"));
    QCOMPARE(prediction["confidence"].toString(), QString("low"));
}

void TestBatteryMonitor::testPredictionConfidenceLevels()
{
    BatteryMonitor monitor(m_configDir);
    monitor.m_level = 50;
    monitor.m_charging = false;

    qint64 now = QDateTime::currentSecsSinceEpoch();

    monitor.m_history.clear();
    BatteryMonitor::BatteryEntry entry1(now - 1800, 52, false, "profile1", false, false);
    BatteryMonitor::BatteryEntry entry2(now, 50, false, "profile1", false, false);
    monitor.m_history.append(entry1);
    monitor.m_history.append(entry2);

    QJsonObject predictionLow = monitor.prediction();
    QCOMPARE(predictionLow["confidence"].toString(), QString("low"));

    monitor.m_history.clear();
    BatteryMonitor::BatteryEntry entry3(now - 7200, 58, false, "profile1", false, false);
    BatteryMonitor::BatteryEntry entry4(now - 3600, 54, false, "profile1", false, false);
    BatteryMonitor::BatteryEntry entry5(now, 50, false, "profile1", false, false);
    monitor.m_history.append(entry3);
    monitor.m_history.append(entry4);
    monitor.m_history.append(entry5);

    QJsonObject predictionMed = monitor.prediction();
    QCOMPARE(predictionMed["confidence"].toString(), QString("medium"));

    // Note: prediction() uses a 2-hour window, so max confidence from
    // 2 hours of data is "medium" (1-4h range). "high" requires >4h which
    // exceeds the prediction window. Verify we get the expected "medium" here.
    monitor.m_history.clear();
    for (int i = 0; i <= 5; ++i) {
        BatteryMonitor::BatteryEntry entry(now - (5 - i) * 3600, 70 - i * 4, false, "profile1", false, false);
        monitor.m_history.append(entry);
    }

    QJsonObject predictionHigh = monitor.prediction();
    // Only ~2 hours of data are within the prediction window
    QCOMPARE(predictionHigh["confidence"].toString(), QString("medium"));
}

void TestBatteryMonitor::testChangeBasedRecording()
{
    BatteryMonitor monitor(m_configDir);
    monitor.setActiveProfile("test_profile");

    monitor.m_level = 80;
    monitor.m_lastRecordedLevel = 80;
    monitor.m_history.clear();

    monitor.m_level = 81;
    monitor.recordEntry();
    QCOMPARE(monitor.m_history.size(), 1);

    monitor.m_lastRecordedLevel = 81;
    monitor.m_level = 79;
    monitor.recordEntry();
    QCOMPARE(monitor.m_history.size(), 2);

    monitor.m_lastRecordedLevel = 79;
    monitor.m_level = 80;
    monitor.recordEntry();
    QCOMPARE(monitor.m_history.size(), 3);
}

void TestBatteryMonitor::testHeartbeatRecording()
{
    BatteryMonitor monitor(m_configDir);
    monitor.setActiveProfile("test_profile");

    int initialSize = monitor.m_history.size();

    monitor.heartbeat();

    QCOMPARE(monitor.m_history.size(), initialSize + 1);
    QCOMPARE(monitor.m_history.last().level, monitor.level());
    QCOMPARE(monitor.m_history.last().charging, monitor.charging());
    QCOMPARE(monitor.m_history.last().activeProfile, QString("test_profile"));
}

void TestBatteryMonitor::testHistoryPersistence()
{
    QString historyPath = m_configDir + "/" + QString::fromLatin1(POWERD_BATTERY_FILE);

    {
        BatteryMonitor monitor(m_configDir);
        monitor.setActiveProfile("profile_alpha");
        
        qint64 now = QDateTime::currentSecsSinceEpoch();
        BatteryMonitor::BatteryEntry entry1(now - 3600, 90, false, "profile_alpha", false, false);
        BatteryMonitor::BatteryEntry entry2(now - 1800, 85, false, "profile_beta", false, true);
        BatteryMonitor::BatteryEntry entry3(now, 80, true, "profile_gamma", true, false);

        monitor.m_history.append(entry1);
        monitor.m_history.append(entry2);
        monitor.m_history.append(entry3);

        monitor.saveHistory();
    }

    QVERIFY(QFile::exists(historyPath));

    {
        BatteryMonitor monitor(m_configDir);
        monitor.loadHistory();

        QCOMPARE(monitor.m_history.size(), 3);
        QCOMPARE(monitor.m_history[0].level, 90);
        QCOMPARE(monitor.m_history[0].charging, false);
        QCOMPARE(monitor.m_history[0].activeProfile, QString("profile_alpha"));
        QCOMPARE(monitor.m_history[0].workoutActive, false);

        QCOMPARE(monitor.m_history[1].level, 85);
        QCOMPARE(monitor.m_history[1].charging, false);
        QCOMPARE(monitor.m_history[1].activeProfile, QString("profile_beta"));
        QCOMPARE(monitor.m_history[1].workoutActive, true);

        QCOMPARE(monitor.m_history[2].level, 80);
        QCOMPARE(monitor.m_history[2].charging, true);
        QCOMPARE(monitor.m_history[2].activeProfile, QString("profile_gamma"));
        QCOMPARE(monitor.m_history[2].screenOn, true);
    }
}

void TestBatteryMonitor::testHistoryPersistenceRoundTrip()
{
    BatteryMonitor monitor(m_configDir);
    
    qint64 now = QDateTime::currentSecsSinceEpoch();
    
    for (int i = 0; i < 100; ++i) {
        BatteryMonitor::BatteryEntry entry(
            now - (100 - i) * 600,
            100 - i,
            i % 3 == 0,
            QString("profile_%1").arg(i % 5),
            i % 2 == 0,
            i % 7 == 0
        );
        monitor.m_history.append(entry);
    }

    monitor.saveHistory();

    BatteryMonitor monitor2(m_configDir);
    monitor2.loadHistory();

    QCOMPARE(monitor2.m_history.size(), 100);
    
    for (int i = 0; i < 100; ++i) {
        QCOMPARE(monitor2.m_history[i].timestamp, monitor.m_history[i].timestamp);
        QCOMPARE(monitor2.m_history[i].level, monitor.m_history[i].level);
        QCOMPARE(monitor2.m_history[i].charging, monitor.m_history[i].charging);
        QCOMPARE(monitor2.m_history[i].activeProfile, monitor.m_history[i].activeProfile);
        QCOMPARE(monitor2.m_history[i].screenOn, monitor.m_history[i].screenOn);
        QCOMPARE(monitor2.m_history[i].workoutActive, monitor.m_history[i].workoutActive);
    }
}

void TestBatteryMonitor::testMissingBatterySysfs()
{
    BatteryMonitor monitor(m_configDir);

    QCOMPARE(monitor.level(), 100);
    QCOMPARE(monitor.charging(), false);

    monitor.readBatteryLevel();

    QCOMPARE(monitor.level(), 100);
    QCOMPARE(monitor.charging(), false);
}

void TestBatteryMonitor::testSysfsReading()
{
    QString mockBatteryPath = m_mockSysfsDir + "/battery";
    createMockSysfs(mockBatteryPath, 73, "Discharging");

    BatteryMonitor monitor(m_configDir);
    monitor.m_powerSupplyPath = mockBatteryPath;

    monitor.readBatteryLevel();

    QCOMPARE(monitor.level(), 73);
    QCOMPARE(monitor.charging(), false);

    createMockSysfs(mockBatteryPath, 85, "Charging");
    monitor.readBatteryLevel();

    QCOMPARE(monitor.level(), 85);
    QCOMPARE(monitor.charging(), true);

    createMockSysfs(mockBatteryPath, 100, "Full");
    monitor.readBatteryLevel();

    QCOMPARE(monitor.level(), 100);
    QCOMPARE(monitor.charging(), true);

    createMockSysfs(mockBatteryPath, 42, "Not charging");
    monitor.readBatteryLevel();

    QCOMPARE(monitor.level(), 42);
    QCOMPARE(monitor.charging(), false);
}

void TestBatteryMonitor::testHistoryDaysConfiguration()
{
    BatteryMonitor monitor(m_configDir);

    QCOMPARE(monitor.historyDays(), DEFAULT_BATTERY_HISTORY_DAYS);

    monitor.setHistoryDays(7);
    QCOMPARE(monitor.historyDays(), 7);

    qint64 now = QDateTime::currentSecsSinceEpoch();
    qint64 tenDaysAgo = now - (10 * 86400);
    qint64 fiveDaysAgo = now - (5 * 86400);

    BatteryMonitor::BatteryEntry oldEntry(tenDaysAgo, 100, false, "profile1", false, false);
    BatteryMonitor::BatteryEntry recentEntry(fiveDaysAgo, 80, false, "profile1", false, false);

    monitor.m_history.append(oldEntry);
    monitor.m_history.append(recentEntry);

    QCOMPARE(monitor.m_history.size(), 2);

    // Call trimHistory directly since historyDays is already 7
    monitor.trimHistory();

    QCOMPARE(monitor.m_history.size(), 1);
    QCOMPARE(monitor.m_history.first().level, 80);

    monitor.setHistoryDays(0);
    QCOMPARE(monitor.historyDays(), 7);

    monitor.setHistoryDays(-5);
    QCOMPARE(monitor.historyDays(), 7);
}

void TestBatteryMonitor::testAdaptivePollInterval()
{
    BatteryMonitor monitor(m_configDir);
    
    // Default: not in workout mode, idle poll interval
    monitor.start();
    QCOMPARE(monitor.m_pollTimer->interval(), BATTERY_POLL_IDLE_MS);
    monitor.stop();
    
    // During workout: faster poll interval
    monitor.setWorkoutActive(true);
    monitor.start();
    QCOMPARE(monitor.m_pollTimer->interval(), BATTERY_POLL_ACTIVE_MS);
    monitor.stop();
}

void TestBatteryMonitor::testWorkoutPollSwitch()
{
    BatteryMonitor monitor(m_configDir);
    monitor.start();
    
    // Initially idle
    QCOMPARE(monitor.m_pollTimer->interval(), BATTERY_POLL_IDLE_MS);
    
    // Switch to workout mode - interval should change dynamically
    monitor.setWorkoutActive(true);
    QCOMPARE(monitor.m_pollTimer->interval(), BATTERY_POLL_ACTIVE_MS);
    
    // Switch back to idle
    monitor.setWorkoutActive(false);
    QCOMPARE(monitor.m_pollTimer->interval(), BATTERY_POLL_IDLE_MS);
    
    // Redundant set should not crash
    monitor.setWorkoutActive(false);
    QCOMPARE(monitor.m_pollTimer->interval(), BATTERY_POLL_IDLE_MS);
    
    monitor.stop();
}

void TestBatteryMonitor::testPredictionNoVectorCopy()
{
    // Verify prediction() works correctly with the optimized direct iteration
    BatteryMonitor monitor(m_configDir);
    monitor.m_level = 50;
    monitor.m_charging = false;
    
    qint64 now = QDateTime::currentSecsSinceEpoch();
    
    // Add some old entries that should be outside the 2-hour window
    BatteryMonitor::BatteryEntry oldEntry(now - 10800, 90, false, "p", false, false);
    monitor.m_history.append(oldEntry);
    
    // Add recent entries within the 2-hour window
    BatteryMonitor::BatteryEntry e1(now - 7200, 70, false, "p", false, false);
    BatteryMonitor::BatteryEntry e2(now - 3600, 60, false, "p", false, false);
    BatteryMonitor::BatteryEntry e3(now, 50, false, "p", false, false);
    monitor.m_history.append(e1);
    monitor.m_history.append(e2);
    monitor.m_history.append(e3);
    
    QJsonObject prediction = monitor.prediction();
    
    // Should produce a valid prediction based on the 2-hour window data
    QVERIFY(prediction.contains("hours_remaining"));
    QVERIFY(prediction.contains("drain_rate_per_hour"));
    
    double drainRate = prediction["drain_rate_per_hour"].toDouble();
    QVERIFY(drainRate > 9.0 && drainRate < 11.0); // ~10%/hr
    
    double hoursRemaining = prediction["hours_remaining"].toDouble();
    QVERIFY(hoursRemaining > 4.0 && hoursRemaining < 6.0); // ~5 hours
}

QTEST_MAIN(TestBatteryMonitor)
#include "tst_batterymonitor.moc"
