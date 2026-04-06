// === IndexerConfig.cpp ===
#include "IndexerConfig.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>

// ── Путь к файлу конфигурации ─────────────────────────────────────────────

QString IndexerConfig::configPath()
{
    return QFileInfo(QCoreApplication::applicationFilePath()).absolutePath()
    + "/indexer_config.json";
}

// ── Значения по умолчанию ─────────────────────────────────────────────────

IndexerConfig IndexerConfig::defaults()
{
    IndexerConfig cfg;

    // Файлы которые libclang не умеет парсить или которые генерируются автоматически
    cfg.skipFilePatterns = {
        "moc_*",            // Qt meta-object compiler output
        "qrc_*",            // Qt resource compiler output
        "ui_*",             // Qt Designer generated headers
        "*.rc",             // Windows resource scripts
        "cmake_pch.*",      // CMake precompiled headers
        "empty_pch.*",      // пустые PCH-заглушки
        "*mocs_compilation*" // сводный moc-файл Qt
    };

    // Директории сборки и служебные директории
    cfg.skipDirPatterns = {
        "/build/",
        "/out/",
        "/cmake-build-debug/",
        "/cmake-build-release/",
        "/cmake-build-relwithdebinfo/",
        "/.git/",
        "/.cmake/"
    };

    cfg.extraIncludePaths = {};
    cfg.extraDefines      = {};

    // Заведомо известные неустранимые диагностики — не засоряют лог.
    // .moc файлы генерируются Qt в папке сборки и отсутствуют при индексации
    // из исходников — это нормально, парсинг файла при этом не нарушается
    // (Qt добавляет #include "Foo.moc" в конце .cpp, после всех объявлений).
    cfg.suppressDiagPatterns = {
        ".moc' file not found",
        ".moc\" file not found"
    };

    return cfg;
}

// ── Загрузка ──────────────────────────────────────────────────────────────

static QStringList jsonArrayToStringList(const QJsonArray& arr)
{
    QStringList result;
    result.reserve(arr.size());
    for (const QJsonValue& v : arr)
        if (v.isString()) result.append(v.toString());
    return result;
}

IndexerConfig IndexerConfig::load()
{
    QString path = configPath();

    if (!QFile::exists(path)) {
        // Первый запуск — создаём файл с умолчаниями
        IndexerConfig cfg = defaults();
        cfg.save();
        qDebug() << ">>> Создан файл конфигурации:" << path;
        return cfg;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << ">>> Не удалось открыть конфигурацию:" << path
                   << "— используются значения по умолчанию";
        return defaults();
    }

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();

    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << ">>> Ошибка разбора конфигурации:" << err.errorString()
                   << "— используются значения по умолчанию";
        return defaults();
    }

    QJsonObject root = doc.object();
    IndexerConfig cfg;

    cfg.skipFilePatterns     = jsonArrayToStringList(root["skipFilePatterns"].toArray());
    cfg.skipDirPatterns      = jsonArrayToStringList(root["skipDirPatterns"].toArray());
    cfg.extraIncludePaths    = jsonArrayToStringList(root["extraIncludePaths"].toArray());
    cfg.extraDefines         = jsonArrayToStringList(root["extraDefines"].toArray());
    cfg.suppressDiagPatterns = jsonArrayToStringList(root["suppressDiagPatterns"].toArray());

    // Если какой-то раздел отсутствует в файле — берём умолчания для него
    IndexerConfig def = defaults();
    if (cfg.skipFilePatterns.isEmpty())      cfg.skipFilePatterns     = def.skipFilePatterns;
    if (cfg.skipDirPatterns.isEmpty())       cfg.skipDirPatterns      = def.skipDirPatterns;
    if (cfg.suppressDiagPatterns.isEmpty())  cfg.suppressDiagPatterns = def.suppressDiagPatterns;

    return cfg;
}

// ── Сохранение ────────────────────────────────────────────────────────────

static QJsonArray stringListToJsonArray(const QStringList& list)
{
    QJsonArray arr;
    for (const QString& s : list)
        arr.append(s);
    return arr;
}

void IndexerConfig::save() const
{
    QJsonObject root;

    root["skipFilePatterns"]     = stringListToJsonArray(skipFilePatterns);
    root["skipDirPatterns"]      = stringListToJsonArray(skipDirPatterns);
    root["extraIncludePaths"]    = stringListToJsonArray(extraIncludePaths);
    root["extraDefines"]         = stringListToJsonArray(extraDefines);
    root["suppressDiagPatterns"] = stringListToJsonArray(suppressDiagPatterns);

    // Комментарий в JSON (через специальное поле — JSON не поддерживает комментарии)
    root["_readme"] = "Паттерны файлов поддерживают wildcards: * и ?  "
                      "Паттерны директорий — подстроки полного пути. "
                      "Изменения применяются при следующем запуске индексации.";

    QJsonDocument doc(root);

    QString path = configPath();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << ">>> Не удалось сохранить конфигурацию:" << path;
        return;
    }
    f.write(doc.toJson(QJsonDocument::Indented));
    f.close();

    qDebug() << ">>> Конфигурация сохранена:" << path;
}