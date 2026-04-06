// === IndexProgressDialog.cpp ===
#include "IndexProgressDialog.h"
#include "ProjectDetector.h"
#include "IndexerWorker.h"
#include "IndexerConfigDialog.h"
#include "Storage.h"

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QCloseEvent>
#include <QApplication>
#include <QFileInfo>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTextStream>
#include <QThread>
#include <QThreadPool>
#include <QDebug>

// ── Конструктор ───────────────────────────────────────────────────────────

IndexProgressDialog::IndexProgressDialog(QWidget *parent)
    : QMainWindow(parent)
    , m_config(IndexerConfig::load())
{
    setWindowTitle("Sourcebreaker Indexer");
    resize(640, 420);

    QWidget *central = new QWidget(this);
    setCentralWidget(central);

    QVBoxLayout *layout = new QVBoxLayout(central);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    m_phaseLabel = new QLabel("Выберите проект для индексации.", central);
    layout->addWidget(m_phaseLabel);

    m_progressBar = new QProgressBar(central);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    layout->addWidget(m_progressBar);

    m_log = new QPlainTextEdit(central);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    m_log->setMinimumHeight(200);
    layout->addWidget(m_log);

    QHBoxLayout *btnLayout = new QHBoxLayout;
    btnLayout->setContentsMargins(0, 0, 0, 0);

    m_selectButton = new QPushButton("Выбрать проект...", central);
    connect(m_selectButton, &QPushButton::clicked,
            this, &IndexProgressDialog::onSelectProject);

    m_settingsButton = new QPushButton("Настройки", central);
    connect(m_settingsButton, &QPushButton::clicked,
            this, &IndexProgressDialog::onSettings);

    m_cancelButton = new QPushButton("Прервать", central);
    m_cancelButton->setEnabled(false);
    connect(m_cancelButton, &QPushButton::clicked, this, [this]() {
        m_cancelButton->setEnabled(false);
        m_cancelButton->setText("Прерывание...");
        m_phaseLabel->setText(
            "Прерывание — завершаем текущие задачи...");
        appendMessage("\n>>> Прерывание запрошено — ожидаем остановки потоков...");
        emit cancelRequested();
    });

    m_closeButton = new QPushButton("Закрыть", central);
    connect(m_closeButton, &QPushButton::clicked, this, &QMainWindow::close);

    btnLayout->addWidget(m_selectButton);
    btnLayout->addWidget(m_settingsButton);
    btnLayout->addStretch();
    btnLayout->addWidget(m_cancelButton);
    btnLayout->addWidget(m_closeButton);
    layout->addLayout(btnLayout);

    // ── Защита от экстренного завершения ─────────────────────────────────
    // aboutToQuit срабатывает при нормальном завершении Qt (закрытие окна,
    // QApplication::quit(), завершение сессии Windows через WM_ENDSESSION).
    // При жёстком kill процесса не срабатывает — но там уже ничего не сделать.
    // Второй рубеж защиты — поле indexing_complete = '0' в самой базе,
    // которое редактор должен проверять при открытии.
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        if (m_indexingActive && !m_indexingDone) {
            qDebug() << ">>> aboutToQuit: индексация не завершена, удаляем базу";
            removeIncompleteDb();
        }
    });
}

// ── removeIncompleteDb ────────────────────────────────────────────────────
// Закрывает Storage и удаляет файлы базы (включая WAL/SHM).
// Безопасно вызывать несколько раз — повторные вызовы ничего не делают.

void IndexProgressDialog::removeIncompleteDb()
{
    if (m_currentDbPath.isEmpty()) return;

    Storage::instance().close();

    const QStringList suffixes = {"", "-wal", "-shm"};
    for (const QString& sfx : suffixes) {
        QString path = m_currentDbPath + sfx;
        if (QFile::exists(path)) {
            QFile::remove(path);
            qDebug() << ">>> Удалён файл неполной базы:" << path;
        }
    }

    m_currentDbPath.clear();  // защита от повторного вызова
}

// ── onSettings ────────────────────────────────────────────────────────────

void IndexProgressDialog::onSettings()
{
    IndexerConfigDialog dlg(m_config, this);
    if (dlg.exec() != QDialog::Accepted) return;

    m_config = dlg.config();
    m_config.save();
    appendMessage(">>> Настройки сохранены.");
}

// ── Вспомогательные функции ───────────────────────────────────────────────

void IndexProgressDialog::appendMessage(const QString& message)
{
    m_log->appendPlainText(message);
    m_log->verticalScrollBar()->setValue(
        m_log->verticalScrollBar()->maximum());
}

// ── cmake ─────────────────────────────────────────────────────────────────

static QString runCmake(const QString& projectRoot,
                        const QString& destDir,
                        IndexProgressDialog* wnd)
{
    wnd->onProgress("Создание временной папки для cmake...");
    qApp->processEvents();

    QString tempPath = QDir::tempPath() + "/sb_cmake_work";
    QDir(tempPath).removeRecursively();
    QDir().mkpath(tempPath);

    QStringList vcPaths = {
        "C:/Program Files/Microsoft Visual Studio/2022/Community"
        "/VC/Auxiliary/Build/vcvarsall.bat",
        "C:/Program Files/Microsoft Visual Studio/2022/Professional"
        "/VC/Auxiliary/Build/vcvarsall.bat",
        "C:/Program Files/Microsoft Visual Studio/2022/Enterprise"
        "/VC/Auxiliary/Build/vcvarsall.bat",
    };
    QString vcvarsall;
    for (const QString& p : vcPaths)
        if (QFile::exists(p)) { vcvarsall = p; break; }

    if (vcvarsall.isEmpty()) {
        QMessageBox::critical(wnd, "Ошибка", "Не найден vcvarsall.bat.");
        return {};
    }

    QString batPath = tempPath + "/run_cmake.bat";
    QFile bat(batPath);
    if (!bat.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(wnd, "Ошибка", "Не удалось создать bat-файл.");
        return {};
    }

    QTextStream out(&bat);
    out << "@echo off\r\n";
    out << "chcp 65001 >nul\r\n";
    out << "call \"" << QDir::toNativeSeparators(vcvarsall) << "\" x64\r\n";
    out << "cmake"
        << " -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
        << " -G Ninja"
        << " \"-DCMAKE_PREFIX_PATH=C:\\Qt\\6.10.2\\msvc2022_64\""
        << " \"" << QDir::toNativeSeparators(projectRoot) << "\""
        << "\r\n";
    bat.close();

    wnd->onProgress("Запуск cmake — генерация compile_commands.json...");
    wnd->onProgress(QString("Проект: %1").arg(projectRoot));
    qApp->processEvents();

    QProcess proc;
    proc.setWorkingDirectory(tempPath);
    proc.start("cmd", QStringList() << "/c" << QDir::toNativeSeparators(batPath));

    if (!proc.waitForStarted(5000)) {
        QMessageBox::critical(wnd, "Ошибка", "Не удалось запустить cmd.");
        return {};
    }

    while (!proc.waitForFinished(200)) {
        qApp->processEvents();
        if (proc.state() == QProcess::NotRunning) break;
    }

    QString logPath = destDir + "/sb_cmake_log.txt";
    QFile log(logPath);
    if (log.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream s(&log);
        s << "=== EXIT CODE: " << proc.exitCode() << " ===\n\n";
        s << "=== STDOUT ===\n" << QString::fromUtf8(proc.readAllStandardOutput()) << "\n\n";
        s << "=== STDERR ===\n" << QString::fromUtf8(proc.readAllStandardError()) << "\n";
    }

    if (proc.exitCode() != 0) {
        QProcess::startDetached("notepad",
                                QStringList() << QDir::toNativeSeparators(logPath));
        QMessageBox::critical(wnd, "cmake ошибка",
                              QString("Код: %1\nПодробности в блокноте.")
                                  .arg(proc.exitCode()));
        return {};
    }

    QString jsonPath = tempPath + "/compile_commands.json";
    if (!QFile::exists(jsonPath)) {
        QStringList files = QDir(tempPath).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
        QProcess::startDetached("notepad",
                                QStringList() << QDir::toNativeSeparators(logPath));
        QMessageBox::critical(wnd, "JSON не найден",
                              QString("Файлы в рабочей папке:\n%1").arg(files.join("\n")));
        return {};
    }

    wnd->onProgress("cmake завершён, копируем compile_commands.json...");
    qApp->processEvents();

    QString dest = QDir(destDir).filePath("compile_commands.json");
    QFile::remove(dest);
    if (!QFile::copy(jsonPath, dest)) {
        qWarning() << ">>> JSON остался в:" << jsonPath;
        return jsonPath;
    }

    QDir(tempPath).removeRecursively();

    wnd->onProgress(QString("compile_commands.json готов: %1").arg(dest));
    qApp->processEvents();

    return dest;
}

// ── onSelectProject ───────────────────────────────────────────────────────

void IndexProgressDialog::onSelectProject()
{
    QString filePath = QFileDialog::getOpenFileName(
        this,
        "Выберите файл проекта",
        QDir::homePath(),
        "Файлы проекта (CMakeLists.txt compile_commands.json);;"
        "CMakeLists.txt (CMakeLists.txt);;"
        "Compile commands (compile_commands.json)");

    if (filePath.isEmpty()) return;

    ProjectInfo info = ProjectDetector::detect(filePath);
    if (!info.error.isEmpty()) {
        QMessageBox::warning(this, "Ошибка", info.error);
        return;
    }

    QString chosenTarget;
    if (info.targets.size() > 1) {
        bool ok = false;
        chosenTarget = QInputDialog::getItem(
            this, "Выбор цели",
            "В проекте несколько целей. Выберите:",
            info.targets, 0, false, &ok);
        if (!ok) return;
    } else if (info.targets.size() == 1) {
        chosenTarget = info.targets.first();
    }

    QString suggestedName = chosenTarget.isEmpty() ? info.projectName : chosenTarget;
    QString defaultPath   = info.projectRoot + "/" + suggestedName + ".db";
    QString dbPath = QFileDialog::getSaveFileName(
        this, "Сохранить базу данных как...",
        defaultPath, "Sourcebreaker база (*.db)");

    if (dbPath.isEmpty()) return;
    if (!dbPath.endsWith(".db", Qt::CaseInsensitive))
        dbPath += ".db";

    // Запоминаем путь к базе — нужен для удаления при незавершённом выходе
    m_currentDbPath  = dbPath;
    m_indexingActive = false;
    m_indexingDone   = false;

    m_log->clear();
    m_progressBar->setRange(0, 0);
    m_progressBar->setValue(0);
    m_selectButton->setEnabled(false);
    m_cancelButton->setEnabled(true);
    m_cancelButton->setText("Прервать");

    appendMessage(QString("Проект: %1").arg(info.projectName));
    appendMessage(QString("Корень: %1").arg(info.projectRoot));
    appendMessage(QString("База:   %1").arg(dbPath));

    if (info.needsCmakeBuild) {
        QString dbDir    = QFileInfo(dbPath).absolutePath();
        QString jsonPath = runCmake(info.projectRoot, dbDir, this);
        if (jsonPath.isEmpty()) {
            m_currentDbPath.clear();
            m_selectButton->setEnabled(true);
            m_cancelButton->setEnabled(false);
            return;
        }
        info.jsonPath = jsonPath;
    }

    appendMessage("Запускаем индексацию...");
    qApp->processEvents();

    m_indexingActive = true;

    QThread*       thread = new QThread(this);
    IndexerWorker* worker = new IndexerWorker(info, dbPath, m_config);
    m_workerThread = thread;

    worker->moveToThread(thread);

    connect(thread, &QThread::started,  worker, &IndexerWorker::run);

    connect(worker, &IndexerWorker::progress,
            this,   &IndexProgressDialog::onProgress,
            Qt::QueuedConnection);

    connect(worker, &IndexerWorker::phase,
            this,   &IndexProgressDialog::onPhase,
            Qt::QueuedConnection);

    connect(worker, &IndexerWorker::finished,
            this,
            [this](bool success, const QString& msg, const IndexStats& stats) {
                onFinished(success, msg, stats);
            },
            Qt::QueuedConnection);

    connect(worker, &IndexerWorker::finished,
            thread,
            [thread](bool, const QString&, const IndexStats&) {
                thread->quit();
            },
            Qt::QueuedConnection);

    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_workerThread = nullptr;
    });

    connect(this,   &IndexProgressDialog::cancelRequested,
            worker, &IndexerWorker::cancel,
            Qt::QueuedConnection);

    thread->start();
}

// ── onProgress / onPhase ─────────────────────────────────────────────────

void IndexProgressDialog::onProgress(const QString& message)
{
    m_phaseLabel->setText(message);
    appendMessage(message);
}

void IndexProgressDialog::onPhase(const QString& /*phaseName*/,
                                  int current, int total)
{
    if (total <= 0) return;
    m_progressBar->setRange(0, total);
    m_progressBar->setValue(current);
}

// ── onFinished ────────────────────────────────────────────────────────────

void IndexProgressDialog::onFinished(bool success,
                                     const QString& message,
                                     const IndexStats& stats)
{
    m_indexingActive = false;
    m_indexingDone   = true;

    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(success ? 100 : 0);
    m_cancelButton->setEnabled(false);

    if (!success) {
        // Индексация прервана или ошибка — база неполная, удаляем
        removeIncompleteDb();

        if (message.isEmpty()) {
            m_phaseLabel->setText("Индексация прервана");
            appendMessage("\nИндексация прервана. Неполная база удалена.");
        } else {
            m_phaseLabel->setText("Ошибка индексации");
            appendMessage(QString("\nОШИБКА: %1\nНеполная база удалена.").arg(message));
        }
        m_selectButton->setEnabled(true);
        QApplication::beep();
        return;
    }

    // Успех — база завершена, m_currentDbPath больше не нужен для защиты
    m_currentDbPath.clear();

    QString logPath = message;
    if (logPath.endsWith(".db", Qt::CaseInsensitive))
        logPath.chop(3);
    logPath += "_indexer.log";

    appendMessage("");

    if (stats.hasErrors()) {
        int partialCount = 0;
        for (const ParseError& e : std::as_const(stats.errors))
            if (e.codeStr == "OK/partial") ++partialCount;
        int failCount = stats.errors.size() - partialCount;

        m_phaseLabel->setText(
            QString("Завершено — %1/%2 файлов OK")
                .arg(stats.filesOk).arg(stats.filesTotal));

        appendMessage("╔══ ИТОГ ПАРСИНГА ══════════════════════════════╗");
        appendMessage(QString("  Успешно:         %1 из %2")
                          .arg(stats.filesOk).arg(stats.filesTotal));
        if (failCount > 0)
            appendMessage(
                QString("  Ошибки парсинга: %1"
                        "  (Failure=%2  Crashed=%3  ASTRead=%4  Other=%5)")
                    .arg(failCount)
                    .arg(stats.errFailure).arg(stats.errCrashed)
                    .arg(stats.errASTRead).arg(stats.errOther));
        if (partialCount > 0)
            appendMessage(
                QString("  Частичный разбор: %1  (данные неполны!)")
                    .arg(partialCount));

        appendMessage("╠══ ПРОБЛЕМНЫЕ ФАЙЛЫ ═══════════════════════════╣");
        for (const ParseError& e : std::as_const(stats.errors)) {
            QString tag    = (e.codeStr == "OK/partial") ? "PARTIAL" : "FAIL   ";
            QString detail;
            if (e.numFatal  > 0) detail += QString("  fatal=%1").arg(e.numFatal);
            if (e.numErrors > 0) detail += QString("  error=%1").arg(e.numErrors);
            appendMessage(
                QString("  [%1]  %2%3").arg(tag).arg(e.fileName).arg(detail));
        }
        appendMessage("╚═══════════════════════════════════════════════╝");
        appendMessage(
            "\n⚠ Данные в базе могут быть неполными.\n"
            "  Подробности и рекомендации — в лог-файле.");
    } else {
        m_phaseLabel->setText("Индексация завершена успешно!");
        appendMessage(
            QString("✓ Все %1 файлов разобраны без ошибок.").arg(stats.filesTotal));
    }

    appendMessage(QString("\nБаза данных:  %1").arg(message));
    appendMessage(QString("Лог-файл:     %1").arg(logPath));

    m_selectButton->setEnabled(true);
    QApplication::beep();
}

// ── closeEvent ────────────────────────────────────────────────────────────

void IndexProgressDialog::closeEvent(QCloseEvent* event)
{
    if (!m_indexingActive) {
        event->accept();
        return;
    }

    if (m_cancelButton->isEnabled()) {
        // Первое закрытие — запрашиваем мягкую отмену
        m_cancelButton->setEnabled(false);
        m_cancelButton->setText("Прерывание...");
        m_phaseLabel->setText("Прерывание — завершаем текущие задачи...");
        appendMessage("\n>>> Прерывание запрошено — ожидаем остановки потоков...");
        emit cancelRequested();
        event->ignore();
        return;
    }

    // Второе закрытие — принудительный выход.
    // Удаляем базу здесь явно, не полагаясь на aboutToQuit.
    m_phaseLabel->setText("Принудительное завершение...");
    qApp->processEvents();

    if (m_workerThread && m_workerThread->isRunning()) {
        m_workerThread->quit();
        m_workerThread->wait(3000);
    }

    removeIncompleteDb();
    event->accept();
}