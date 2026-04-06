// === CodeIndexer.h ===
#pragma once

#include <QString>
#include <QStringList>
#include <QList>
#include <QHash>
#include <functional>
#include <atomic>
#include <clang-c/Index.h>
#include "IndexerConfig.h"

using ProgressCallback = std::function<void(const QString&, int, int)>;

// ── Одна диагностика libclang ──────────────────────────────────────────────
// Соответствует одному CXDiagnostic из translation unit.
// Severity: 0=ignored 1=note 2=warning 3=error 4=fatal

struct ClangDiagnostic {
    int     severity   = 0;
    QString severityStr;     // "note" / "warning" / "error" / "fatal"
    QString text;            // текст сообщения
    QString location;        // "path/file.h:42:7"
    QString category;        // категория диагностики (опционально)
};

// ── Описание одной ошибки парсинга ────────────────────────────────────────

struct ParseError {
    QString fileName;        // короткое имя файла для отображения
    QString filePath;        // полный путь
    int     code     = 0;   // CXErrorCode
    QString codeStr;         // "Failure" / "Crashed" / "ASTReadError" / ...

    // Диагностики libclang которые не были подавлены паттернами
    QList<ClangDiagnostic> diagnostics;

    // Флаги для быстрой фильтрации при записи лога
    int numFatal    = 0;
    int numErrors   = 0;
    int numWarnings = 0;

    // Подавленные диагностики: уникальный текст → число вхождений.
    // Не попали в основной список из-за suppressDiagPatterns,
    // но фиксируются для анализа — чтобы не пропустить реальные проблемы.
    QHash<QString, int> suppressedCounts;
};

// ── Глобальная статистика подавленных диагностик ──────────────────────────
// Агрегируется по всем файлам — показывает масштаб проблем
// которые были заглушены паттернами подавления.

struct SuppressedStats {
    QHash<QString, int> counts;  // текст диагностики → суммарное число по всем файлам
    int totalFiles = 0;          // сколько файлов имели подавленные диагностики

    void merge(const QHash<QString, int>& fileCounts) {
        if (fileCounts.isEmpty()) return;
        ++totalFiles;
        for (auto it = fileCounts.constBegin(); it != fileCounts.constEnd(); ++it)
            counts[it.key()] += it.value();
    }

    bool isEmpty() const { return counts.isEmpty(); }
};

// ── Статистика индексации ─────────────────────────────────────────────────

struct IndexStats {
    int filesTotal   = 0;
    int filesOk      = 0;
    int errFailure   = 0;
    int errCrashed   = 0;
    int errASTRead   = 0;
    int errOther     = 0;

    QList<ParseError>  errors;           // файлы с незаглушенными проблемами
    SuppressedStats    suppressed;       // сводка по заглушенным диагностикам

    bool hasErrors() const {
        return errFailure + errCrashed + errASTRead + errOther > 0;
    }

    QString summary() const {
        if (!hasErrors())
            return QString("Парсинг завершён: %1/%2 файлов OK")
                .arg(filesOk).arg(filesTotal);
        return QString("Парсинг: %1/%2 OK  |  Ошибок: %3"
                       " (Failure=%4  Crashed=%5  ASTRead=%6  Other=%7)")
            .arg(filesOk).arg(filesTotal)
            .arg(errFailure + errCrashed + errASTRead + errOther)
            .arg(errFailure).arg(errCrashed)
            .arg(errASTRead).arg(errOther);
    }
};

// ── CodeIndexer ───────────────────────────────────────────────────────────

class CodeIndexer {
public:
    void setProgressCallback(ProgressCallback cb) { m_progress = cb; }
    void setCancelFlag(std::atomic<bool>* flag) { m_cancelFlag = flag; }
    void setConfig(const IndexerConfig& cfg) { m_config = cfg; }
    void indexProject(const QString& jsonPath, const QString& projectRoot);

    IndexStats stats() const { return m_stats; }

private:
    ProgressCallback      m_progress;
    QString               m_projectRoot;
    IndexerConfig         m_config;
    IndexStats            m_stats;
    std::atomic<bool>*    m_cancelFlag = nullptr;
};