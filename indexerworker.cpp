// === IndexerWorker.cpp ===
#include "IndexerWorker.h"
#include "Storage.h"
#include "CodeIndexer.h"
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>
#include <QDateTime>
#include <QThreadPool>
#include <algorithm>

IndexerWorker::IndexerWorker(const ProjectInfo& info,
                             const QString& dbPath,
                             const IndexerConfig& config,
                             QObject *parent)
    : QObject(parent)
    , m_info(info)
    , m_dbPath(dbPath)
    , m_config(config)
{}

void IndexerWorker::cancel()
{
    m_cancelled.store(true);

    // Убираем из очереди все задачи которые ещё не начали выполняться.
    // Уже запущенные задачи увидят флаг через abortQuery и остановятся
    // на ближайшей точке проверки внутри clang_indexTranslationUnit.
    QThreadPool::globalInstance()->clear();
}

// ── writeLog ──────────────────────────────────────────────────────────────
// Записывает подробный лог индексации рядом с базой данных.
// Формат рассчитан на ручной анализ и поиск решений.
//
// Структура лога:
//   [ЗАГОЛОВОК]  дата, проект, база, статистика
//   [ИТОГ]       сводная таблица по типам ошибок
//   Для каждого проблемного файла:
//     [FAIL] или [WARN] — тип проблемы
//     Полный путь к файлу
//     Список диагностик libclang с локацией и текстом
//     Подсказка по устранению (если диагностика известного типа)

static void writeLog(const QString& dbPath,
                     const ProjectInfo& info,
                     const IndexStats& stats)
{
    // Лог пишем рядом с базой, имя совпадает с именем базы
    QString logPath = dbPath;
    if (logPath.endsWith(".db", Qt::CaseInsensitive))
        logPath.chop(3);
    logPath += "_indexer.log";

    QFile f(logPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        qWarning() << ">>> Не удалось записать лог:" << logPath;
        return;
    }

    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);

    const QString sep1 = QString("=").repeated(72);
    const QString sep2 = QString("-").repeated(72);

    // ── Заголовок ─────────────────────────────────────────────────────────
    out << sep1 << "\n";
    out << "  SOURCEBREAKER INDEXER LOG\n";
    out << sep1 << "\n";
    out << "  Дата:    " << QDateTime::currentDateTime()
                                .toString("yyyy-MM-dd HH:mm:ss") << "\n";
    out << "  Проект:  " << info.projectName << "\n";
    out << "  Корень:  " << info.projectRoot << "\n";
    out << "  База:    " << dbPath << "\n";
    out << sep2 << "\n\n";

    // ── Итоговая статистика ────────────────────────────────────────────────
    out << "ИТОГ ПАРСИНГА\n";
    out << sep2 << "\n";
    out << QString("  Всего файлов:  %1\n").arg(stats.filesTotal);
    out << QString("  Успешно:       %1\n").arg(stats.filesOk);

    int totalErrors = stats.errFailure + stats.errCrashed +
                      stats.errASTRead + stats.errOther;

    // Считаем отдельно частичные (OK/partial — распарсены но с fatal)
    int partialCount = 0;
    int failCount    = 0;
    for (const ParseError& e : std::as_const(stats.errors)) {
        if (e.codeStr == "OK/partial") ++partialCount;
        else                           ++failCount;
    }

    if (totalErrors > 0) {
        out << QString("  Провал парсинга: %1"
                       "  (Failure=%2  Crashed=%3  ASTRead=%4  Other=%5)\n")
                   .arg(totalErrors)
                   .arg(stats.errFailure)
                   .arg(stats.errCrashed)
                   .arg(stats.errASTRead)
                   .arg(stats.errOther);
    } else {
        out << "  Провал парсинга: 0\n";
    }

    if (partialCount > 0) {
        out << QString("  Частичный парсинг (есть fatal-диагностики): %1\n")
                   .arg(partialCount);
        out << "  ⚠ Данные по этим файлам могут быть неполными!\n";
    }

    out << "\n";

    if (stats.errors.isEmpty()) {
        out << "  ✓ Все файлы разобраны без ошибок и предупреждений fatal-уровня.\n\n";
        qDebug() << ">>> Лог записан:" << logPath;
        return;
    }

    // ── Детали по каждому проблемному файлу ───────────────────────────────
    out << sep1 << "\n";
    out << "  ДЕТАЛИ ПРОБЛЕМНЫХ ФАЙЛОВ\n";
    out << sep1 << "\n\n";

    for (const ParseError& pe : std::as_const(stats.errors)) {

        bool isPartial = (pe.codeStr == "OK/partial");

        if (isPartial) {
            out << "[PARTIAL] " << pe.fileName << "\n";
            out << "  Статус:  распарсен, но с fatal-диагностиками\n";
            out << "  Данные в базе могут быть неполными\n";
        } else {
            out << "[FAIL] " << pe.fileName << "\n";
            out << "  Код ошибки libclang: " << pe.codeStr << "\n";
        }

        out << "  Путь:    " << pe.filePath << "\n";

        if (pe.numFatal + pe.numErrors + pe.numWarnings > 0) {
            out << QString("  Диагностики: fatal=%1  error=%2  warning=%3\n")
                       .arg(pe.numFatal).arg(pe.numErrors).arg(pe.numWarnings);
        }

        if (!pe.diagnostics.isEmpty()) {
            out << "\n  Диагностики libclang:\n";
            out << "  " << sep2.left(66) << "\n";

            for (const ClangDiagnostic& d : std::as_const(pe.diagnostics)) {
                // Форматируем: [FATAL] path/file.h:42:7 — текст
                out << QString("  [%1] ").arg(d.severityStr.toUpper(), -7);

                if (!d.location.isEmpty())
                    out << d.location << "\n";
                else
                    out << "(нет локации)\n";

                out << "         " << d.text << "\n";

                if (!d.category.isEmpty())
                    out << "         Категория: " << d.category << "\n";

                // ── Подсказки по известным причинам ───────────────────────
                // Анализируем текст диагностики и предлагаем решение.
                // Это ключевая часть — позволяет устранять ошибки без ручного
                // разбора вывода clang.

                QString hint;
                const QString& txt = d.text;

                // Исправлен приоритет операторов: скобки расставлены явно
                if (txt.contains("file not found"))
                {
                    hint = "Заголовочный файл не найден.\n"
                           "         Решения:\n"
                           "         1. Добавьте путь к заголовкам в compile_commands.json\n"
                           "            или в CMakeLists.txt (target_include_directories)\n"
                           "         2. Проверьте что зависимости проекта установлены\n"
                           "         3. Для UE5: убедитесь что ModuleRules.h и Engine\\Public\n"
                           "            включены в include path";
                }
                else if (txt.contains("unknown argument") ||
                         txt.contains("unsupported option"))
                {
                    hint = "Неизвестный флаг компилятора.\n"
                           "         Решения:\n"
                           "         1. Этот флаг поддерживается MSVC но не clang — это норма\n"
                           "            для некоторых флагов, обычно не влияет на анализ\n"
                           "         2. Если влияет: добавьте флаг в список пропускаемых\n"
                           "            в CodeIndexer.cpp (секция фильтрации аргументов)";
                }
                else if (txt.contains("too many errors emitted"))
                {
                    hint = "Слишком много ошибок — clang прервал анализ.\n"
                           "         Обычно вторичный симптом, причина в первой ошибке выше.";
                }
                else if (txt.contains("cannot open source file") ||
                         txt.contains("no such file or directory"))
                {
                    hint = "Исходный файл не найден на диске.\n"
                           "         Решения:\n"
                           "         1. Файл был удалён или перемещён после генерации\n"
                           "            compile_commands.json — перегенерируйте его\n"
                           "         2. Проверьте что путь в compile_commands.json корректен";
                }
                else if (txt.contains("use of undeclared identifier"))
                {
                    hint = "Неизвестный идентификатор — скорее всего отсутствует заголовок\n"
                           "         или не определён необходимый макрос препроцессора.\n"
                           "         Проверьте include path и определения (-D флаги).";
                }
                else if (d.severity == CXDiagnostic_Fatal &&
                         txt.contains("PCH"))
                {
                    hint = "Проблема с precompiled header (PCH).\n"
                           "         Решения:\n"
                           "         1. PCH уже отфильтрованы из аргументов (/Yu, /Fp)\n"
                           "         2. Если ошибка остаётся — удалите PCH-файлы и\n"
                           "            перегенерируйте compile_commands.json";
                }

                if (!hint.isEmpty())
                    out << "         💡 " << hint << "\n";

                out << "\n";
            }
            out << "  " << sep2.left(66) << "\n";
        }

        out << "\n";
    }

    // ── Подавленные диагностики — сводка ─────────────────────────────────
    // Показываем всегда если что-то было подавлено — это важно для понимания
    // полноты базы. Даже если "ошибок нет" — подавленные могут скрывать
    // реальные проблемы вроде отсутствия заголовков Qt.
    if (!stats.suppressed.isEmpty()) {
        out << sep1 << "\n";
        out << "  ПОДАВЛЕННЫЕ ДИАГНОСТИКИ\n";
        out << sep1 << "\n";
        out << QString("  Затронуто файлов: %1  |  Уникальных сообщений: %2\n")
                   .arg(stats.suppressed.totalFiles)
                   .arg(stats.suppressed.counts.size());
        out << "  Эти диагностики не попали в раздел проблемных файлов выше.\n";
        out << "  ⚠ Если среди них есть 'file not found' для системных заголовков\n";
        out << "    (Qt, stdlib и т.д.) — данные о наследовании и вызовах могут\n";
        out << "    быть неполными. Добавьте нужные пути в extraIncludePaths.\n\n";

        // Сортируем по убыванию числа вхождений
        QList<QPair<int, QString>> sorted;
        for (auto it = stats.suppressed.counts.constBegin();
             it != stats.suppressed.counts.constEnd(); ++it)
            sorted.append({it.value(), it.key()});
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });

        out << "  Топ подавленных сообщений (по частоте):\n";
        out << "  " << sep2.left(66) << "\n";
        int shown = 0;
        for (const auto& p : std::as_const(sorted)) {
            out << QString("  [×%1]  %2\n")
                       .arg(p.first, 5)
                       .arg(p.second);
            if (++shown >= 30) {
                if (sorted.size() > 30)
                    out << QString("  ... и ещё %1 уникальных сообщений\n")
                               .arg(sorted.size() - 30);
                break;
            }
        }
        out << "  " << sep2.left(66) << "\n\n";
    }

    // ── Общие рекомендации если есть ошибки ───────────────────────────────
    if (totalErrors > 0 || partialCount > 0) {
        out << sep1 << "\n";
        out << "  ОБЩИЕ РЕКОМЕНДАЦИИ\n";
        out << sep1 << "\n";
        out << "  1. Самая частая причина ошибок парсинга — отсутствующие заголовки.\n";
        out << "     Убедитесь что все зависимости проекта доступны по путям\n";
        out << "     указанным в compile_commands.json.\n\n";
        out << "  2. Для UE5 и других крупных проектов: заголовки движка должны быть\n";
        out << "     доступны. Убедитесь что ENGINE_DIR корректно определён.\n\n";
        out << "  3. Файлы с кодом OK/partial попали в базу, но их данные неполны.\n";
        out << "     После устранения ошибок пересоберите базу заново.\n\n";
        out << "  4. Файлы с кодом Crashed могут указывать на баги libclang\n";
        out << "     с конкретными конструкциями языка. Попробуйте обновить LLVM.\n";
    }

    out.flush();
    f.close();

    qDebug() << ">>> Лог записан:" << logPath;
}

// ── run ───────────────────────────────────────────────────────────────────

void IndexerWorker::run()
{
    emit progress("Открываю базу данных...");

    if (!Storage::instance().init(m_dbPath)) {
        emit finished(false, QString("Не удалось создать базу:\n%1").arg(m_dbPath),
                      IndexStats{});
        return;
    }

    emit progress("Запускаю индексацию...");

    CodeIndexer indexer;
    indexer.setProgressCallback([this](const QString& msg, int cur, int total) {
        emit progress(msg);
        emit phase(msg, cur, total);
    });
    indexer.setCancelFlag(&m_cancelled);
    indexer.setConfig(m_config);

    indexer.indexProject(m_info.jsonPath, m_info.projectRoot);

    const IndexStats& s = indexer.stats();
    qDebug() << s.summary();

    bool cancelled = m_cancelled.load();

    if (cancelled) {
        Storage::instance().closeCurrentThread();
        QFile::remove(m_dbPath);
        qDebug() << ">>> Индексация прервана, база удалена";
        emit finished(false, "", IndexStats{});
    } else {
        // Создаём индексы после завершения всей индексации — до этого момента
        // INSERT'ы не замедляются индексами
        emit progress("Создаю индексы...");
        Storage::instance().createIndexes();

        Storage::instance().setIndexingComplete(m_info.projectRoot);
        Storage::instance().printStats();

        // WAL checkpoint — сбрасываем WAL в основной файл,
        // уменьшаем размер -wal и гарантируем чтение без replay
        Storage::instance().walCheckpoint();

        Storage::instance().closeCurrentThread();

        // Записываем лог всегда — и при ошибках, и при успехе с предупреждениями,
        // и при полностью чистом прогоне (для истории)
        emit progress("Записываю лог...");
        writeLog(m_dbPath, m_info, s);

        emit finished(true, m_dbPath, s);
    }
}