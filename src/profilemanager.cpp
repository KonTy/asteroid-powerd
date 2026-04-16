#include "profilemanager.h"
#include "common.h"
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QDebug>

ProfileManager::ProfileManager(const QString &configDir, QObject *parent)
    : QObject(parent)
    , m_configDir(configDir)
{
}

QList<PowerProfile> ProfileManager::profiles() const
{
    return m_profiles;
}

PowerProfile ProfileManager::profile(const QString &id) const
{
    for (const auto &p : m_profiles) {
        if (p.id.compare(id, Qt::CaseInsensitive) == 0) {
            return p;
        }
    }
    return PowerProfile();
}

bool ProfileManager::hasProfile(const QString &id) const
{
    for (const auto &p : m_profiles) {
        if (p.id.compare(id, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

bool ProfileManager::addProfile(const PowerProfile &profile)
{
    PowerProfile p = profile;
    
    // Auto-generate ID if empty
    if (p.id.isEmpty()) {
        p.id = generateProfileId();
    }
    
    // Check if profile is valid
    if (!p.isValid()) {
        qWarning() << "Cannot add invalid profile";
        return false;
    }
    
    // Check for duplicate ID
    if (hasProfile(p.id)) {
        qWarning() << "Profile with id" << p.id << "already exists";
        return false;
    }
    
    m_profiles.append(p);
    emit profilesChanged();
    return true;
}

bool ProfileManager::updateProfile(const PowerProfile &profile)
{
    if (!profile.isValid()) {
        qWarning() << "Cannot update with invalid profile";
        return false;
    }
    
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].id.compare(profile.id, Qt::CaseInsensitive) == 0) {
            m_profiles[i] = profile;
            emit profilesChanged();
            return true;
        }
    }
    
    qWarning() << "Profile" << profile.id << "not found for update";
    return false;
}

bool ProfileManager::deleteProfile(const QString &id)
{
    // Cannot delete active profile
    if (m_activeProfileId.compare(id, Qt::CaseInsensitive) == 0) {
        qWarning() << "Cannot delete active profile" << id;
        return false;
    }
    
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].id.compare(id, Qt::CaseInsensitive) == 0) {
            if (m_profiles[i].builtin) {
                qWarning() << "Cannot delete built-in profile" << id;
                return false;
            }
            m_profiles.removeAt(i);
            emit profilesChanged();
            return true;
        }
    }
    
    qWarning() << "Profile" << id << "not found for deletion";
    return false;
}

QString ProfileManager::activeProfileId() const
{
    return m_activeProfileId;
}

PowerProfile ProfileManager::activeProfile() const
{
    return profile(m_activeProfileId);
}

bool ProfileManager::setActiveProfile(const QString &id)
{
    if (!hasProfile(id)) {
        qWarning() << "Cannot set active profile to non-existent" << id;
        return false;
    }
    
    if (m_activeProfileId.compare(id, Qt::CaseInsensitive) != 0) {
        m_activeProfileId = id;
        PowerProfile p = profile(id);
        emit activeProfileChanged(id, p.name);
    }
    
    return true;
}

QMap<QString, QString> ProfileManager::workoutProfiles() const
{
    return m_workoutProfiles;
}

bool ProfileManager::setWorkoutProfile(const QString &workoutType, const QString &profileId)
{
    if (!profileId.isEmpty() && !hasProfile(profileId)) {
        qWarning() << "Cannot assign non-existent profile" << profileId << "to workout" << workoutType;
        return false;
    }
    
    if (profileId.isEmpty()) {
        m_workoutProfiles.remove(workoutType);
    } else {
        m_workoutProfiles[workoutType] = profileId;
    }
    
    return true;
}

QString ProfileManager::workoutProfileId(const QString &workoutType) const
{
    return m_workoutProfiles.value(workoutType, QString());
}

bool ProfileManager::loadProfiles()
{
    QString filePath = profilesFilePath();
    QFile file(filePath);
    
    if (!file.exists()) {
        qInfo() << "Profiles file does not exist, loading defaults";
        loadDefaultProfiles();
        return true;
    }
    
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open profiles file:" << filePath;
        loadDefaultProfiles();
        return false;
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    
    if (parseError.error != QJsonParseError::NoError) {
        qWarning() << "Failed to parse profiles JSON:" << parseError.errorString();
        loadDefaultProfiles();
        return false;
    }
    
    if (!doc.isObject()) {
        qWarning() << "Profiles JSON is not an object";
        loadDefaultProfiles();
        return false;
    }
    
    QJsonObject root = doc.object();
    
    // Load profiles array
    m_profiles.clear();
    QJsonArray profilesArray = root.value(QStringLiteral("profiles")).toArray();
    for (const auto &v : profilesArray) {
        PowerProfile p = PowerProfile::fromJson(v.toObject());
        if (p.isValid()) {
            m_profiles.append(p);
        } else {
            qWarning() << "Skipping invalid profile in JSON";
        }
    }
    
    // Load active profile
    m_activeProfileId = root.value(QStringLiteral("active_profile")).toString();
    if (m_activeProfileId.isEmpty() || !hasProfile(m_activeProfileId)) {
        qWarning() << "Invalid active profile, defaulting to smartwatch_indoor";
        m_activeProfileId = QStringLiteral("smartwatch_indoor");
        if (!hasProfile(m_activeProfileId) && !m_profiles.isEmpty()) {
            m_activeProfileId = m_profiles.first().id;
        }
    }
    
    // Load workout profiles
    m_workoutProfiles.clear();
    QJsonObject workoutObj = root.value(QStringLiteral("workout_profiles")).toObject();
    for (auto it = workoutObj.constBegin(); it != workoutObj.constEnd(); ++it) {
        m_workoutProfiles[it.key()] = it.value().toString();
    }
    
    qInfo() << "Loaded" << m_profiles.size() << "profiles from" << filePath;
    return true;
}

bool ProfileManager::saveProfiles()
{
    ensureConfigDir();
    
    QJsonObject root;
    
    // Save active profile
    root[QStringLiteral("active_profile")] = m_activeProfileId;
    
    // Save workout profiles
    QJsonObject workoutObj;
    for (auto it = m_workoutProfiles.constBegin(); it != m_workoutProfiles.constEnd(); ++it) {
        workoutObj[it.key()] = it.value();
    }
    root[QStringLiteral("workout_profiles")] = workoutObj;
    
    // Save profiles array
    QJsonArray profilesArray;
    for (const auto &p : m_profiles) {
        profilesArray.append(p.toJson());
    }
    root[QStringLiteral("profiles")] = profilesArray;
    
    QJsonDocument doc(root);
    
    QString filePath = profilesFilePath();
    QSaveFile file(filePath);
    
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Failed to open profiles file for writing:" << filePath;
        return false;
    }
    
    file.write(doc.toJson(QJsonDocument::Indented));
    
    if (!file.commit()) {
        qWarning() << "Failed to write profiles file";
        return false;
    }
    
    qInfo() << "Saved" << m_profiles.size() << "profiles to" << filePath;
    return true;
}

void ProfileManager::loadDefaultProfiles()
{
    m_profiles.clear();
    m_workoutProfiles.clear();

    // Allow override via environment variable (for testing)
    QString defaultsPath = QString::fromLocal8Bit(qgetenv("POWERD_DEFAULT_PROFILES"));
    if (defaultsPath.isEmpty()) {
        defaultsPath = QLatin1String(POWERD_DEFAULT_PROFILES_PATH);
    }
    QFile file(defaultsPath);

    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Cannot open default profiles:" << defaultsPath;
        // Absolute minimum fallback — single usable profile
        PowerProfile p;
        p.id = QStringLiteral("smartwatch_indoor");
        p.name = QStringLiteral("Smartwatch – Indoor");
        p.icon = QStringLiteral("ios-watch");
        p.color = QStringLiteral("#2196F3");
        p.builtin = true;
        p.system.always_on_display = true;
        p.system.tilt_to_wake = true;
        m_profiles.append(p);
        m_activeProfileId = p.id;
        qInfo() << "Using minimal fallback profile";
        return;
    }

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    file.close();

    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "Failed to parse default profiles:" << err.errorString();
        return;
    }

    QJsonObject root = doc.object();

    // Load profiles
    QJsonArray arr = root.value(QStringLiteral("profiles")).toArray();
    for (const auto &v : arr) {
        PowerProfile p = PowerProfile::fromJson(v.toObject());
        if (p.isValid()) {
            m_profiles.append(p);
        }
    }

    // Active profile
    m_activeProfileId = root.value(QStringLiteral("active_profile")).toString();
    if (m_activeProfileId.isEmpty() && !m_profiles.isEmpty()) {
        m_activeProfileId = m_profiles.first().id;
    }

    // Workout profiles
    QJsonObject wpObj = root.value(QStringLiteral("workout_profiles")).toObject();
    for (auto it = wpObj.constBegin(); it != wpObj.constEnd(); ++it) {
        m_workoutProfiles[it.key()] = it.value().toString();
    }

    qInfo() << "Loaded" << m_profiles.size() << "default profiles from" << defaultsPath;
}

QString ProfileManager::generateProfileId() const
{
    QString base = QStringLiteral("custom_profile_");
    qint64 timestamp = QDateTime::currentMSecsSinceEpoch();
    return base + QString::number(timestamp);
}

QString ProfileManager::profilesFilePath() const
{
    return m_configDir + QLatin1Char('/') + QLatin1String(POWERD_PROFILES_FILE);
}

void ProfileManager::ensureConfigDir()
{
    QDir dir(m_configDir);
    if (!dir.exists()) {
        if (dir.mkpath(QStringLiteral("."))) {
            qInfo() << "Created config directory:" << m_configDir;
        } else {
            qWarning() << "Failed to create config directory:" << m_configDir;
        }
    }
}
