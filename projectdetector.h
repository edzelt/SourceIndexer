// === ProjectDetector.h ===
#pragma once

#include <QString>
#include <QStringList>

struct ProjectInfo {
    QString     projectName;
    QString     jsonPath;
    QString     projectRoot;
    bool        needsCmakeBuild = false;
    QStringList targets;
    QString     error;
};

class ProjectDetector {
public:
    static ProjectInfo detect(const QString& filePath);

private:
    static ProjectInfo fromCmake(const QString& cmakePath);
    static ProjectInfo fromJson(const QString& jsonPath);
    static QString     findUprojectName(const QString& dir);
    static QString     commonRoot(const QStringList& paths);
};