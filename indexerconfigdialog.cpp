// === IndexerConfigDialog.cpp ===
#include "IndexerConfigDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QTabWidget>
#include <QPushButton>
#include <QInputDialog>
#include <QFileDialog>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QFont>

// ── makeListTab ───────────────────────────────────────────────────────────
// Создаёт вкладку: описание сверху, список посередине, кнопки снизу.

QWidget* IndexerConfigDialog::makeListTab(const QString& description,
                                          const QStringList& items,
                                          QListWidget*& outList,
                                          const QStringList& defaultItems)
{
    QWidget* tab = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    QLabel* desc = new QLabel(description, tab);
    desc->setWordWrap(true);
    layout->addWidget(desc);

    outList = new QListWidget(tab);
    outList->setAlternatingRowColors(true);

    // Элементы редактируются двойным кликом
    outList->setEditTriggers(QAbstractItemView::DoubleClicked |
                             QAbstractItemView::SelectedClicked);

    QFont mono("Consolas");
    mono.setPointSize(9);
    outList->setFont(mono);

    for (const QString& s : items) {
        QListWidgetItem* item = new QListWidgetItem(s, outList);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
    }
    layout->addWidget(outList);

    // Кнопки управления списком
    QHBoxLayout* btnRow = new QHBoxLayout;
    btnRow->setContentsMargins(0, 0, 0, 0);

    QPushButton* btnAdd    = new QPushButton("+ Добавить", tab);
    QPushButton* btnRemove = new QPushButton("− Удалить", tab);
    QPushButton* btnReset  = new QPushButton("По умолчанию", tab);

    connect(btnAdd,    &QPushButton::clicked, this,
            [this, outList]() { addItem(outList); });
    connect(btnRemove, &QPushButton::clicked, this,
            [this, outList]() { removeItem(outList); });
    connect(btnReset,  &QPushButton::clicked, this,
            [this, outList, defaultItems]() { resetTab(outList, defaultItems); });

    btnRow->addWidget(btnAdd);
    btnRow->addWidget(btnRemove);
    btnRow->addStretch();
    btnRow->addWidget(btnReset);
    layout->addLayout(btnRow);

    return tab;
}

// ── Конструктор ───────────────────────────────────────────────────────────

IndexerConfigDialog::IndexerConfigDialog(const IndexerConfig& config,
                                         QWidget* parent)
    : QDialog(parent)
    , m_defaults(IndexerConfig::defaults())
{
    setWindowTitle("Настройки индексатора");
    resize(580, 440);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(8);

    QTabWidget* tabs = new QTabWidget(this);

    // ── Вкладка 1: Пропуск файлов ─────────────────────────────────────────
    tabs->addTab(
        makeListTab(
            "Файлы совпадающие с паттерном пропускаются без парсинга.\n"
            "Поддерживаются wildcards: * (любая строка) и ? (любой символ).\n"
            "Проверяется только имя файла без пути. Двойной клик — редактировать.",
            config.skipFilePatterns,
            m_skipFiles,
            m_defaults.skipFilePatterns),
        "Пропуск файлов");

    // ── Вкладка 2: Пропуск директорий ────────────────────────────────────
    tabs->addTab(
        makeListTab(
            "Файл пропускается если его полный путь содержит один из этих подстрок.\n"
            "Регистр не важен. Используйте слеши как разделители: /build/\n"
            "Двойной клик — редактировать.",
            config.skipDirPatterns,
            m_skipDirs,
            m_defaults.skipDirPatterns),
        "Пропуск директорий");

    // ── Вкладка 3: Include-пути ───────────────────────────────────────────
    tabs->addTab(
        makeListTab(
            "Дополнительные пути к заголовочным файлам.\n"
            "Добавляются как -I ко всем файлам. Помогает исправить ошибки\n"
            "'file not found' для заголовков не попавших в compile_commands.json.",
            config.extraIncludePaths,
            m_includePaths,
            {}),
        "Include-пути");

    // ── Вкладка 4: Defines ────────────────────────────────────────────────
    tabs->addTab(
        makeListTab(
            "Дополнительные макросы препроцессора.\n"
            "Добавляются как -D ко всем файлам.\n"
            "Пример: MY_DEFINE  или  MY_DEFINE=value",
            config.extraDefines,
            m_defines,
            {}),
        "Defines");

    // ── Вкладка 5: Подавление диагностик ─────────────────────────────────
    tabs->addTab(
        makeListTab(
            "Диагностики libclang не попадают в лог если их текст содержит\n"
            "одну из этих подстрок (регистронезависимо).\n"
            "Используется для заведомо известных неустранимых предупреждений.\n"
            "Пример: \".moc' file not found\" — Qt генерирует эти файлы\n"
            "в папке сборки, при индексации из исходников их нет (это норма).",
            config.suppressDiagPatterns,
            m_suppressDiag,
            IndexerConfig::defaults().suppressDiagPatterns),
        "Подавление диагностик");

    mainLayout->addWidget(tabs);

    // ── Кнопки OK / Отмена ───────────────────────────────────────────────
    QDialogButtonBox* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttons);
}

// ── Слоты управления списком ──────────────────────────────────────────────

void IndexerConfigDialog::addItem(QListWidget* list)
{
    bool ok = false;
    QString text = QInputDialog::getText(
        this, "Добавить", "Значение:", QLineEdit::Normal, {}, &ok);
    if (!ok || text.trimmed().isEmpty()) return;

    QListWidgetItem* item = new QListWidgetItem(text.trimmed(), list);
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    list->setCurrentItem(item);
}

void IndexerConfigDialog::removeItem(QListWidget* list)
{
    QListWidgetItem* item = list->currentItem();
    if (!item) return;
    delete item;
}

void IndexerConfigDialog::resetTab(QListWidget* list,
                                   const QStringList& defaults)
{
    if (defaults.isEmpty()) {
        // Для include-путей и defines — просто очищаем
        list->clear();
        return;
    }

    auto btn = QMessageBox::question(
        this, "Сброс",
        "Восстановить значения по умолчанию?\nВсе изменения будут потеряны.",
        QMessageBox::Yes | QMessageBox::No);

    if (btn != QMessageBox::Yes) return;

    list->clear();
    for (const QString& s : defaults) {
        QListWidgetItem* item = new QListWidgetItem(s, list);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
    }
}

// ── config() — считываем отредактированные значения ─────────────────────

static QStringList listWidgetToStringList(QListWidget* list)
{
    QStringList result;
    result.reserve(list->count());
    for (int i = 0; i < list->count(); ++i) {
        QString text = list->item(i)->text().trimmed();
        if (!text.isEmpty())
            result.append(text);
    }
    return result;
}

IndexerConfig IndexerConfigDialog::config() const
{
    IndexerConfig cfg;
    cfg.skipFilePatterns     = listWidgetToStringList(m_skipFiles);
    cfg.skipDirPatterns      = listWidgetToStringList(m_skipDirs);
    cfg.extraIncludePaths    = listWidgetToStringList(m_includePaths);
    cfg.extraDefines         = listWidgetToStringList(m_defines);
    cfg.suppressDiagPatterns = listWidgetToStringList(m_suppressDiag);
    return cfg;
}