// === IndexerConfigDialog.h ===
#pragma once

#include <QDialog>
#include <QListWidget>
#include <QTabWidget>
#include <QPushButton>
#include "IndexerConfig.h"

// Диалог редактирования конфигурации индексатора.
// Открывается из главного окна кнопкой "Настройки".
// Изменения применяются сразу при нажатии OK.

class IndexerConfigDialog : public QDialog
{
    Q_OBJECT
public:
    explicit IndexerConfigDialog(const IndexerConfig& config,
                                 QWidget* parent = nullptr);

    // Возвращает отредактированную конфигурацию
    IndexerConfig config() const;

private slots:
    void addItem(QListWidget* list);
    void removeItem(QListWidget* list);
    void resetTab(QListWidget* list, const QStringList& defaults);

private:
    // Создаёт вкладку с редактируемым списком строк
    QWidget* makeListTab(const QString& description,
                         const QStringList& items,
                         QListWidget*& outList,
                         const QStringList& defaultItems);

    QListWidget* m_skipFiles    = nullptr;
    QListWidget* m_skipDirs     = nullptr;
    QListWidget* m_includePaths = nullptr;
    QListWidget* m_defines      = nullptr;
    QListWidget* m_suppressDiag = nullptr;

    IndexerConfig m_defaults;
};