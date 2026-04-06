// === IndexerWorker.h ===
#pragma once

#include <QObject>
#include <atomic>
#include "ProjectDetector.h"
#include "CodeIndexer.h"
#include "IndexerConfig.h"

class IndexerWorker : public QObject
{
    Q_OBJECT
public:
    explicit IndexerWorker(const ProjectInfo& info,
                           const QString& dbPath,
                           const IndexerConfig& config,
                           QObject *parent = nullptr);

public slots:
    void run();
    void cancel();

signals:
    void progress(const QString& message);
    void phase(const QString& phaseName, int current, int total);
    void finished(bool success, const QString& dbPath, const IndexStats& stats);

private:
    ProjectInfo          m_info;
    QString              m_dbPath;
    IndexerConfig        m_config;
    std::atomic<bool>    m_cancelled{false};
};