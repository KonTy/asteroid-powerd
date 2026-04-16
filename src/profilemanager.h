#ifndef PROFILEMANAGER_H
#define PROFILEMANAGER_H

#include <QObject>
#include <QList>
#include <QMap>
#include <QString>
#include "profilemodel.h"

class ProfileManager : public QObject
{
    Q_OBJECT

public:
    explicit ProfileManager(const QString &configDir, QObject *parent = nullptr);

    // Profile CRUD
    QList<PowerProfile> profiles() const;
    PowerProfile profile(const QString &id) const;
    bool hasProfile(const QString &id) const;
    bool addProfile(const PowerProfile &profile);
    bool updateProfile(const PowerProfile &profile);
    bool deleteProfile(const QString &id);

    // Active profile
    QString activeProfileId() const;
    PowerProfile activeProfile() const;
    bool setActiveProfile(const QString &id);

    // Workout profiles
    QMap<QString, QString> workoutProfiles() const;
    bool setWorkoutProfile(const QString &workoutType, const QString &profileId);
    QString workoutProfileId(const QString &workoutType) const;

    // Persistence
    bool loadProfiles();
    bool saveProfiles();
    void loadDefaultProfiles();

signals:
    void activeProfileChanged(const QString &id, const QString &name);
    void profilesChanged();

private:
    QString generateProfileId() const;
    QString profilesFilePath() const;
    void ensureConfigDir();

    QString m_configDir;
    QList<PowerProfile> m_profiles;
    QString m_activeProfileId;
    QMap<QString, QString> m_workoutProfiles;
};

#endif // PROFILEMANAGER_H
