// === ProjectDetector.cpp ===
#include "ProjectDetector.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>

ProjectInfo ProjectDetector::detect(const QString& filePath)
{
    QFileInfo fi(filePath);
    QString name = fi.fileName().toLower();

    if (name == "cmakelists.txt")
        return fromCmake(filePath);

    if (name == "compile_commands.json")
        return fromJson(filePath);

    ProjectInfo err;
    err.error = QString("Неизвестный тип файла: %1").arg(fi.fileName());
    return err;
}

ProjectInfo ProjectDetector::fromCmake(const QString& cmakePath)
{
    ProjectInfo info;
    info.needsCmakeBuild = true;
    info.projectRoot = QFileInfo(cmakePath).absolutePath();

    QFile f(cmakePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        info.error = "Не удалось открыть CMakeLists.txt";
        return info;
    }

    QTextStream in(&f);
    QString content = in.readAll();
    f.close();

    QRegularExpression reProject(R"(project\s*\(\s*(\w+))",
                                 QRegularExpression::CaseInsensitiveOption);
    auto mProject = reProject.match(content);
    if (mProject.hasMatch())
        info.projectName = mProject.captured(1);

    QRegularExpression reExe(R"(add_executable\s*\(\s*(\w+))",
                             QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = reExe.globalMatch(content);
    while (it.hasNext()) {
        auto m = it.next();
        QString target = m.captured(1);
        if (!target.startsWith("test", Qt::CaseInsensitive) &&
            !target.startsWith("example", Qt::CaseInsensitive))
            info.targets << target;
    }

    if (info.projectName.isEmpty())
        info.projectName = QDir(info.projectRoot).dirName();

    QDir root(info.projectRoot);
    QStringList buildDirs = {"build", "Build", "out",
                             "cmake-build-debug", "cmake-build-release"};
    for (const QString& bd : buildDirs) {
        QString candidate = root.filePath(bd + "/compile_commands.json");
        if (QFile::exists(candidate)) {
            info.jsonPath = candidate;
            info.needsCmakeBuild = false;
            break;
        }
    }

    return info;
}

ProjectInfo ProjectDetector::fromJson(const QString& jsonPath)
{
    ProjectInfo info;
    info.needsCmakeBuild = false;
    info.jsonPath = jsonPath;

    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        info.error = "Не удалось открыть compile_commands.json";
        return info;
    }

    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();

    if (!doc.isArray() || doc.array().isEmpty()) {
        info.error = "compile_commands.json пуст или имеет неверный формат";
        return info;
    }

    QStringList filePaths;
    for (const QJsonValue& val : doc.array()) {
        QJsonObject obj = val.toObject();
        QString fp = obj.value("file").toString();
        if (!fp.isEmpty())
            filePaths << QDir::fromNativeSeparators(fp);
    }

    info.projectRoot = commonRoot(filePaths);

    QString uname = findUprojectName(info.projectRoot);
    if (!uname.isEmpty()) {
        info.projectName = uname;
        return info;
    }

    QDir dir(info.projectRoot);
    int maxUp = 4;
    while (maxUp-- > 0) {
        QString cmakePath = dir.filePath("CMakeLists.txt");
        if (QFile::exists(cmakePath)) {
            ProjectInfo cmakeInfo = fromCmake(cmakePath);
            if (!cmakeInfo.projectName.isEmpty()) {
                info.projectName = cmakeInfo.projectName;
                return info;
            }
        }
        if (!dir.cdUp()) break;
    }

    info.projectName = QDir(info.projectRoot).dirName();
    return info;
}

QString ProjectDetector::findUprojectName(const QString& dir)
{
    QDir d(dir);
    for (int i = 0; i < 2; ++i) {
        QStringList uprojects = d.entryList({"*.uproject"}, QDir::Files);
        if (!uprojects.isEmpty())
            return QFileInfo(uprojects.first()).completeBaseName();
        if (!d.cdUp()) break;
    }
    return {};
}

QString ProjectDetector::commonRoot(const QStringList& paths)
{
    if (paths.isEmpty()) return {};
    if (paths.size() == 1) return QFileInfo(paths.first()).absolutePath();

    QStringList normalized;
    for (const QString& p : std::as_const(paths))
        normalized << QDir::cleanPath(p);

    // Фильтруем временные пути (Temp) — они не часть проекта
    QString tempPath = QDir::cleanPath(QDir::tempPath()).toLower();
    QStringList projectPaths;
    for (const QString& p : std::as_const(normalized)) {
        if (!p.toLower().startsWith(tempPath))
            projectPaths << p;
    }
    // Если все пути временные — берём все
    if (projectPaths.isEmpty())
        projectPaths = normalized;

    QString base = QFileInfo(projectPaths.first()).absolutePath();
    for (const QString& p : std::as_const(projectPaths)) {
        QString dir = QFileInfo(p).absolutePath();
        while (!dir.startsWith(base) && !base.isEmpty()) {
            int slash = base.lastIndexOf('/');
            if (slash < 0) { base = {}; break; }
            base = base.left(slash);
        }
    }

    return base;
}