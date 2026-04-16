#include "batterymonitor.h"
#include "common.h"
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QDateTime>
#include <QDebug>
#include <QDataStream>
#include <QtEndian>
#include <cmath>

BatteryMonitor::BatteryMonitor(const QString &configDir, QObject *parent)
    : QObject(parent)
    , m_configDir(configDir)
    , m_level(100)
    , m_charging(false)
    , m_lastRecordedLevel(100)
    , m_workoutActive(false)
    , m_historyDays(DEFAULT_BATTERY_HISTORY_DAYS)
    , m_pollTimer(new QTimer(this))
    , m_heartbeatTimer(new QTimer(this))
{
    m_powerSupplyPath = findPowerSupplyPath();
    
    if (m_powerSupplyPath.isEmpty()) {
        qWarning() << "BatteryMonitor: No battery power supply found, using defaults";
    }

    connect(m_pollTimer, &QTimer::timeout, this, &BatteryMonitor::pollBattery);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &BatteryMonitor::heartbeat);

    loadHistory();
}

void BatteryMonitor::start()
{
    readBatteryLevel();
    m_lastRecordedLevel = m_level;
    recordEntry();

    // Use adaptive poll interval: slower during idle, faster during workouts
    m_pollTimer->start(m_workoutActive ? BATTERY_POLL_ACTIVE_MS : BATTERY_POLL_IDLE_MS);
    m_heartbeatTimer->start(BATTERY_HEARTBEAT_MINUTES * 60 * 1000);
}

void BatteryMonitor::stop()
{
    m_pollTimer->stop();
    m_heartbeatTimer->stop();
    saveHistory();
}

int BatteryMonitor::level() const
{
    return m_level;
}

bool BatteryMonitor::charging() const
{
    return m_charging;
}

QVector<BatteryMonitor::BatteryEntry> BatteryMonitor::history(int hours) const
{
    qint64 cutoffTime = QDateTime::currentSecsSinceEpoch() - (hours * 3600);
    QVector<BatteryEntry> result;

    for (const BatteryEntry &entry : m_history) {
        if (entry.timestamp >= cutoffTime) {
            result.append(entry);
        }
    }

    return result;
}

QJsonObject BatteryMonitor::prediction() const
{
    QJsonObject result;
    const qint64 cutoff = QDateTime::currentSecsSinceEpoch() - 7200; // last 2 hours

    if (m_charging) {
        int chargeGain = 0;
        qint64 timeSpan = 0;
        int chargingSamples = 0;

        // Iterate m_history directly — avoid copying the vector
        for (int i = m_history.size() - 1; i > 0; --i) {
            if (m_history[i].timestamp < cutoff)
                break;
            if (m_history[i - 1].timestamp < cutoff)
                continue; // pair spans the cutoff boundary, skip it
            if (m_history[i].charging && m_history[i-1].charging) {
                chargeGain += m_history[i].level - m_history[i-1].level;
                timeSpan += m_history[i].timestamp - m_history[i-1].timestamp;
                chargingSamples++;
            }
        }

        if (chargingSamples > 0 && timeSpan > 0 && chargeGain > 0) {
            double chargeRatePerHour = (chargeGain * 3600.0) / timeSpan;
            int remaining = 100 - m_level;
            double hoursToFull = remaining / chargeRatePerHour;
            result["charging"] = true;
            result["hours_to_full"] = qRound(hoursToFull * 10.0) / 10.0;
        } else {
            result["charging"] = true;
            result["hours_to_full"] = -1;
        }

        return result;
    }

    int drainTotal = 0;
    qint64 timeSpan = 0;
    int drainingSamples = 0;

    for (int i = m_history.size() - 1; i > 0; --i) {
        if (m_history[i].timestamp < cutoff)
            break;
        if (m_history[i - 1].timestamp < cutoff)
            continue; // pair spans the cutoff boundary, skip it
        if (!m_history[i].charging && !m_history[i-1].charging) {
            int levelDrop = m_history[i-1].level - m_history[i].level;
            if (levelDrop > 0) {
                drainTotal += levelDrop;
                timeSpan += m_history[i].timestamp - m_history[i-1].timestamp;
                drainingSamples++;
            }
        }
    }

    if (drainingSamples == 0 || timeSpan == 0) {
        result["hours_remaining"] = -1;
        result["drain_rate_per_hour"] = 0.0;
        result["confidence"] = "low";
        return result;
    }

    double drainRatePerHour = (drainTotal * 3600.0) / timeSpan;
    
    if (drainRatePerHour <= 0) {
        result["hours_remaining"] = -1;
        result["drain_rate_per_hour"] = 0.0;
        result["confidence"] = "low";
        return result;
    }

    double hoursRemaining = m_level / drainRatePerHour;

    double dataHours = timeSpan / 3600.0;
    QString confidence;
    if (dataHours < 1.0) {
        confidence = "low";
    } else if (dataHours < 4.0) {
        confidence = "medium";
    } else {
        confidence = "high";
    }

    result["hours_remaining"] = qRound(hoursRemaining * 10.0) / 10.0;
    result["drain_rate_per_hour"] = qRound(drainRatePerHour * 10.0) / 10.0;
    result["confidence"] = confidence;

    return result;
}

int BatteryMonitor::historyDays() const
{
    return m_historyDays;
}

void BatteryMonitor::setHistoryDays(int days)
{
    if (days > 0 && days != m_historyDays) {
        m_historyDays = days;
        trimHistory();
        saveHistory();
    }
}

void BatteryMonitor::setActiveProfile(const QString &profileId)
{
    m_activeProfile = profileId;
}

void BatteryMonitor::setWorkoutActive(bool active)
{
    if (m_workoutActive == active)
        return;

    m_workoutActive = active;

    // Adjust poll frequency: faster during workouts, slower during idle
    if (m_pollTimer->isActive()) {
        m_pollTimer->setInterval(active ? BATTERY_POLL_ACTIVE_MS : BATTERY_POLL_IDLE_MS);
    }
}

void BatteryMonitor::pollBattery()
{
    int prevLevel = m_level;
    bool prevCharging = m_charging;

    readBatteryLevel();

    if (m_level != prevLevel) {
        emit levelChanged(m_level);
    }

    if (m_charging != prevCharging) {
        emit chargingChanged(m_charging);
    }

    if (qAbs(m_level - m_lastRecordedLevel) >= BATTERY_CHANGE_THRESHOLD) {
        recordEntry();
        m_lastRecordedLevel = m_level;
    }

    if (m_level != prevLevel || m_charging != prevCharging) {
        emit significantChange(m_level, m_charging);
    }
}

void BatteryMonitor::heartbeat()
{
    recordEntry();
    m_lastRecordedLevel = m_level;
    saveHistory();
}

void BatteryMonitor::readBatteryLevel()
{
    if (m_powerSupplyPath.isEmpty()) {
        m_level = 100;
        m_charging = false;
        return;
    }

    QFile capacityFile(m_powerSupplyPath + "/capacity");
    if (capacityFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString content = QString::fromLatin1(capacityFile.readAll().trimmed());
        bool ok = false;
        int level = content.toInt(&ok);
        if (ok && level >= 0 && level <= 100) {
            m_level = level;
        }
        capacityFile.close();
    }

    QFile statusFile(m_powerSupplyPath + "/status");
    if (statusFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString status = QString::fromLatin1(statusFile.readAll().trimmed());
        m_charging = (status.compare(QLatin1String("Charging"), Qt::CaseInsensitive) == 0 ||
                      status.compare(QLatin1String("Full"), Qt::CaseInsensitive) == 0);
        statusFile.close();
    }
}

void BatteryMonitor::recordEntry()
{
    BatteryEntry entry(
        QDateTime::currentSecsSinceEpoch(),
        m_level,
        m_charging,
        m_activeProfile,
        false,
        m_workoutActive
    );

    m_history.append(entry);
    trimHistory();
}

void BatteryMonitor::loadHistory()
{
    QString historyPath = m_configDir + "/" + QString::fromLatin1(POWERD_BATTERY_FILE);
    QFile file(historyPath);

    if (!file.exists()) {
        return;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open battery history file:" << historyPath;
        return;
    }

    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);

    char magic[4];
    if (stream.readRawData(magic, 4) != 4 || qstrncmp(magic, "BATT", 4) != 0) {
        qWarning() << "Invalid battery history file magic";
        file.close();
        return;
    }

    quint16 version;
    stream >> version;

    if (version != 1) {
        qWarning() << "Unsupported battery history version:" << version;
        file.close();
        return;
    }

    quint32 entryCount;
    quint32 maxEntries;
    stream >> entryCount >> maxEntries;

    m_history.clear();
    m_history.reserve(entryCount);

    for (quint32 i = 0; i < entryCount; ++i) {
        qint64 timestamp;
        qint32 level;
        quint8 charging;
        quint8 screenOn;
        quint8 workoutActive;
        quint32 profileLen;

        stream >> timestamp >> level >> charging >> screenOn >> workoutActive >> profileLen;

        QByteArray profileData(profileLen, '\0');
        if (profileLen > 0) {
            stream.readRawData(profileData.data(), profileLen);
        }

        BatteryEntry entry;
        entry.timestamp = timestamp;
        entry.level = level;
        entry.charging = (charging != 0);
        entry.screenOn = (screenOn != 0);
        entry.workoutActive = (workoutActive != 0);
        entry.activeProfile = QString::fromUtf8(profileData);

        m_history.append(entry);
    }

    file.close();
    trimHistory();
}

void BatteryMonitor::saveHistory()
{
    QString historyPath = m_configDir + "/" + QString::fromLatin1(POWERD_BATTERY_FILE);
    QSaveFile file(historyPath);

    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Failed to save battery history to:" << historyPath;
        return;
    }

    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);

    stream.writeRawData("BATT", 4);
    stream << quint16(1);
    stream << quint32(m_history.size());
    stream << quint32(m_history.size());

    for (const BatteryEntry &entry : m_history) {
        QByteArray profileUtf8 = entry.activeProfile.toUtf8();

        stream << qint64(entry.timestamp);
        stream << qint32(entry.level);
        stream << quint8(entry.charging ? 1 : 0);
        stream << quint8(entry.screenOn ? 1 : 0);
        stream << quint8(entry.workoutActive ? 1 : 0);
        stream << quint32(profileUtf8.size());

        if (!profileUtf8.isEmpty()) {
            stream.writeRawData(profileUtf8.constData(), profileUtf8.size());
        }
    }

    if (!file.commit()) {
        qWarning() << "Failed to commit battery history file";
    }
}

void BatteryMonitor::trimHistory()
{
    qint64 cutoffTime = QDateTime::currentSecsSinceEpoch() - (m_historyDays * 86400);

    int removeCount = 0;
    while (removeCount < m_history.size() && m_history[removeCount].timestamp < cutoffTime) {
        ++removeCount;
    }

    if (removeCount > 0) {
        m_history.remove(0, removeCount);
    }
}

QString BatteryMonitor::findPowerSupplyPath() const
{
    QDir powerSupplyDir("/sys/class/power_supply");
    if (!powerSupplyDir.exists()) {
        return QString();
    }

    QStringList candidates = powerSupplyDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString &candidate : candidates) {
        QString path = powerSupplyDir.absoluteFilePath(candidate);
        QFile typeFile(path + "/type");

        if (typeFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QString type = QString::fromLatin1(typeFile.readAll().trimmed());
            typeFile.close();

            if (type.compare(QLatin1String("Battery"), Qt::CaseInsensitive) == 0) {
                return path;
            }
        }
    }

    return QString();
}
