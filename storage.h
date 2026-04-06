// === Storage.h ===
#pragma once

#include <QString>
#include <QByteArray>
#include <QVector>
#include <QHash>
#include <QSet>
#include <QSqlDatabase>
#include <QMutex>
#include <QReadWriteLock>
#include <QWaitCondition>

// ── Записи батча ───────────────────────────────────────────────────────────
// ВАЖНО: поле kind хранит CXIdxEntityKind — семантический enum из IndexerCallbacks.
// Все пути записи kind обязаны конвертировать CXCursorKind через cursorKindToIdxKind().
// Значения: Function=2, Variable=3, Field=4, Enum=5, EnumConstant=6,
//           Struct=14, CXXClass=16, CXXMethod=21, Constructor=22, Destructor=23,
//           Typedef=32, CXXNamespace=20, CXXNamespaceAlias=501, Macro=500,
//           Concept=502

struct EntityRec {
    QString usr;
    QString parentUsr;
    QString name;
    int     kind        = 0;   // CXIdxEntityKind (см. комментарий выше)
    QString filePath;
    int     offsetStart = 0;
    int     offsetEnd   = 0;
    bool    isImplicit  = false;
    bool    isExternal  = false;
    QString typeUsr;
    int     access      = 0;
    int     flags       = 0;
    QString comment;
};

struct RelationRec {
    QString fromUsr;
    QString toUsr;
    int     relType = 0;
};

struct BodyRec {
    QString    entityUsr;
    int        bodyStart = 0;
    int        bodyEnd   = 0;
    QByteArray blob;
};

// ── Батч одного потока ─────────────────────────────────────────────────────

struct EntityBatch {
    QVector<EntityRec>   entities;
    QVector<RelationRec> relations;
    QVector<BodyRec>     bodies;

    QHash<QString, int>  localIdx;  // USR → индекс в entities

    // Дедупликация связей внутри батча — избегаем лишних INSERT OR IGNORE
    QSet<quint64>        relSeen;

    void addEntity(const EntityRec& e) {
        auto it = localIdx.find(e.usr);
        if (it == localIdx.end()) {
            localIdx[e.usr] = entities.size();
            entities.append(e);
        } else {
            EntityRec& ex = entities[it.value()];
            if (!e.isExternal) {
                // Реальная декларация побеждает заглушку по всем полям
                ex.filePath    = e.filePath;
                ex.offsetStart = e.offsetStart;
                ex.offsetEnd   = e.offsetEnd;
                ex.isExternal  = false;
                ex.kind        = e.kind;  // kind от реальной декларации всегда точнее
            }
            if (!e.name.isEmpty())    ex.name    = e.name;
            if (!e.typeUsr.isEmpty()) ex.typeUsr = e.typeUsr;
            if (!e.comment.isEmpty()) ex.comment = e.comment;
            if (e.access != 0) ex.access = e.access;
            if (e.flags  != 0) ex.flags  = e.flags;
        }
    }

    void addRelation(const QString& fromUsr,
                     const QString& toUsr, int relType) {
        // Дедупликация: хешируем пару (from, to, type) через
        // комбинацию хешей строк и типа
        quint64 h = qHash(fromUsr) ^ (quint64(qHash(toUsr)) << 32) ^
                    quint64(relType) * 2654435761ULL;
        if (relSeen.contains(h)) return;
        relSeen.insert(h);
        relations.append({fromUsr, toUsr, relType});
    }

    void addBody(const QString& usr,
                 int bodyStart, int bodyEnd,
                 const QByteArray& blob) {
        bodies.append({usr, bodyStart, bodyEnd, blob});
    }

    int localId(const QString& usr) const {
        return localIdx.value(usr, -1);
    }

    bool hasEntity(const QString& usr) const {
        return localIdx.contains(usr);
    }
};

// ── Storage ────────────────────────────────────────────────────────────────

class Storage {
public:
    static Storage& instance();

    bool init(const QString& dbPath);
    bool open(const QString& dbPath);
    void close();
    void closeCurrentThread();

    // Создаёт индексы после завершения всей индексации — не замедляет INSERT'ы
    void createIndexes();

    // WAL checkpoint — сбрасывает WAL в основной файл
    void walCheckpoint();

    QString currentPath() const { return m_dbPath; }
    bool    isOpen()      const { return !m_dbPath.isEmpty(); }

    void flush(EntityBatch& batch);

    void setIndexingComplete(const QString& projectRoot);

    int  getEntityId(const QString& usr);
    bool entityExists(const QString& usr);

    void printStats();

private:
    Storage() = default;

    QSqlDatabase getConnection();

    // Весь SQL одного батча — все QSqlQuery уничтожаются внутри,
    // до того как flush() вызовет closeCurrentThread()
    void flushImpl(EntityBatch& batch, QSqlDatabase db);

    QString                    m_dbPath;
    QMutex                     m_connMutex;
    QHash<Qt::HANDLE, QString> m_connNames;

    // Защита от гонки: потоки ожидающие завершения создания соединения
    QSet<Qt::HANDLE>           m_connCreating;
    QWaitCondition             m_connReady;

    QReadWriteLock             m_cacheLock;
    QHash<QString, int>        m_idCache;
};