// === IndexProgressDialog.h ===
#pragma once

#include <QMainWindow>
#include <QProgressBar>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include "CodeIndexer.h"
#include "IndexerConfig.h"

class IndexProgressDialog : public QMainWindow
{
    Q_OBJECT
public:
    explicit IndexProgressDialog(QWidget *parent = nullptr);

    void appendMessage(const QString& message);

public slots:
    void onProgress(const QString& message);
    void onPhase(const QString& phaseName, int current, int total);
    void onFinished(bool success, const QString& message, const IndexStats& stats);
    void onSelectProject();
    void onSettings();

signals:
    void cancelRequested();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void removeIncompleteDb();

    QLabel         *m_phaseLabel;
    QProgressBar   *m_progressBar;
    QPlainTextEdit *m_log;
    QPushButton    *m_selectButton;
    QPushButton    *m_settingsButton;
    QPushButton    *m_cancelButton;
    QPushButton    *m_closeButton;

    bool     m_indexingActive = false;
    bool     m_indexingDone   = false;
    QString  m_currentDbPath;

    IndexerConfig m_config;

    QThread* m_workerThread = nullptr;
};