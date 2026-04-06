// === Storage.cpp ===
#include "Storage.h"
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QDebug>
#include <QFile>
#include <QThread>
#include <QDateTime>

Storage& Storage::instance()
{
    static Storage inst;
    return inst;
}

// ── getConnection ──────────────────────────────────────────────────────────
// Возвращает соединение для текущего потока, создавая его при необходимости.
// Двухступенчатая схема: мьютекс защищает хеш, WaitCondition — ожидание
// завершения создания соединения другим потоком с тем же tid (невозможно
// в реальности, но защита от edge case с переиспользованием tid).

QSqlDatabase Storage::getConnection()
{
    Qt::HANDLE tid = QThread::currentThreadId();

    QMutexLocker locker(&m_connMutex);

    // Ждём если другой поток в процессе создания соединения для нашего tid
    // (теоретически невозможно, но защита от edge case)
    while (m_connCreating.contains(tid))
        m_connReady.wait(&m_connMutex);

    // Соединение уже есть для этого потока?
    auto it = m_connNames.find(tid);
    if (it != m_connNames.end()) {
        QSqlDatabase db = QSqlDatabase::database(it.value(), false);
        if (db.isValid() && db.isOpen())
            return db;
        // Инвалидировано (после close()) — убираем и пересоздаём
        m_connNames.erase(it);
    }

    // Помечаем что создаём соединение — другие потоки не будут обращаться
    // к connName до завершения
    m_connCreating.insert(tid);

    // Генерируем уникальное имя соединения
    QString connName = QString("sb_%1")
                           .arg((quintptr)tid, 16, 16, QChar('0'));

    // Отпускаем мьютекс на время open() — это может занять время,
    // но connName ещё не в m_connNames, поэтому другие потоки не найдут
    // незавершённое соединение
    locker.unlock();

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
    db.setDatabaseName(m_dbPath);
    if (!db.open())
        qCritical() << "Storage: не удалось открыть соединение:"
                    << db.lastError().text();

    QSqlQuery q(db);
    q.exec("PRAGMA journal_mode = WAL;");
    q.exec("PRAGMA synchronous  = NORMAL;");
    q.exec("PRAGMA foreign_keys = OFF;");
    q.exec("PRAGMA cache_size   = 100000;");
    q.exec("PRAGMA busy_timeout = 5000;");

    // Регистрируем готовое соединение и снимаем флаг создания
    locker.relock();
    m_connNames[tid] = connName;
    m_connCreating.remove(tid);
    m_connReady.wakeAll();

    return db;
}

bool Storage::init(const QString& dbPath)
{
    {
        QMutexLocker locker(&m_connMutex);
        m_connNames.clear();
        m_connCreating.clear();
    }
    {
        QWriteLocker locker(&m_cacheLock);
        m_idCache.clear();
    }

    m_dbPath = dbPath;
    if (QFile::exists(dbPath)) QFile::remove(dbPath);

    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "sb_init");
        db.setDatabaseName(dbPath);

        if (!db.open()) {
            qCritical() << "Storage: не удалось создать БД:"
                        << db.lastError().text();
        } else {
            QSqlQuery q(db);
            q.exec("PRAGMA journal_mode = WAL;");
            q.exec("PRAGMA foreign_keys = OFF;");
            q.exec("PRAGMA cache_size   = 100000;");
            // NORMAL вместо OFF — защита от повреждения базы при падении процесса
            q.exec("PRAGMA synchronous  = NORMAL;");

            q.exec(R"(
                CREATE TABLE entities (
                    id           INTEGER PRIMARY KEY,
                    usr          TEXT UNIQUE NOT NULL,
                    parent_usr   TEXT,
                    name         TEXT NOT NULL,
                    kind         INTEGER,
                    file_path    TEXT,
                    offset_start INTEGER,
                    offset_end   INTEGER,
                    is_implicit  BOOLEAN DEFAULT 0,
                    is_external  BOOLEAN DEFAULT 0,
                    type_usr     TEXT,
                    access       INTEGER DEFAULT 0,
                    flags        INTEGER DEFAULT 0,
                    comment      TEXT,
                    alias        TEXT,
                    short_note   TEXT,
                    description  TEXT,
                    status       INTEGER DEFAULT 0
                );
            )");

            q.exec(R"(
                CREATE TABLE relations (
                    from_id     INTEGER NOT NULL REFERENCES entities(id),
                    to_id       INTEGER NOT NULL REFERENCES entities(id),
                    rel_type    INTEGER NOT NULL,
                    short_note  TEXT,
                    description TEXT,
                    PRIMARY KEY (from_id, to_id, rel_type)
                );
            )");

            q.exec(R"(
                CREATE TABLE entity_bodies (
                    entity_id   INTEGER PRIMARY KEY REFERENCES entities(id),
                    body_start  INTEGER,
                    body_end    INTEGER,
                    body_blob   BLOB
                );
            )");

            q.exec(R"(
                CREATE TABLE db_info (
                    key    TEXT PRIMARY KEY,
                    value  TEXT
                );
            )");

            q.exec("INSERT INTO db_info (key, value) VALUES "
                   "('indexing_complete', '0');");
            q.exec(QString("INSERT INTO db_info (key, value) VALUES "
                           "('created_at', '%1');")
                   .arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
            q.exec("INSERT INTO db_info (key, value) VALUES "
                   "('project_root', '');");
            q.exec("INSERT INTO db_info (key, value) VALUES "
                   "('manually_edited', '0');");
            q.exec("INSERT INTO db_info (key, value) VALUES "
                   "('imported_from', '');");

            q.exec(R"(
                CREATE TABLE local_entities (
                    id          INTEGER PRIMARY KEY,
                    owner_id    INTEGER NOT NULL REFERENCES entities(id),
                    name        TEXT,
                    kind        INTEGER,
                    alias       TEXT,
                    short_note  TEXT,
                    description TEXT
                );
            )");

            q.exec(R"(
                CREATE TABLE groups (
                    id          INTEGER PRIMARY KEY,
                    name        TEXT,
                    short_note  TEXT,
                    description TEXT
                );
            )");

            q.exec(R"(
                CREATE TABLE group_members (
                    group_id    INTEGER NOT NULL REFERENCES groups(id),
                    member_id   INTEGER NOT NULL,
                    member_type INTEGER NOT NULL,
                    PRIMARY KEY (group_id, member_id)
                );
            )");

            db.close();
        }
    }
    QSqlDatabase::removeDatabase("sb_init");
    return true;
}

// ── createIndexes ─────────────────────────────────────────────────────────
// Создаёт индексы после завершения всей индексации.
// Вызывать до setIndexingComplete() — так INSERT'ы не замедляются индексами.

void Storage::createIndexes()
{
    QSqlDatabase db = getConnection();
    QSqlQuery q(db);

    qDebug() << ">>> Создание индексов...";

    q.exec("CREATE INDEX IF NOT EXISTS idx_entities_name      ON entities(name);");
    q.exec("CREATE INDEX IF NOT EXISTS idx_entities_kind      ON entities(kind);");
    q.exec("CREATE INDEX IF NOT EXISTS idx_entities_parent    ON entities(parent_usr);");
    q.exec("CREATE INDEX IF NOT EXISTS idx_entities_file      ON entities(file_path);");
    q.exec("CREATE INDEX IF NOT EXISTS idx_entities_external  ON entities(is_external);");
    q.exec("CREATE INDEX IF NOT EXISTS idx_relations_from     ON relations(from_id);");
    q.exec("CREATE INDEX IF NOT EXISTS idx_relations_to       ON relations(to_id);");
    q.exec("CREATE INDEX IF NOT EXISTS idx_relations_type     ON relations(rel_type);");
    q.exec("CREATE INDEX IF NOT EXISTS idx_bodies_entity      ON entity_bodies(entity_id);");

    qDebug() << ">>> Индексы созданы.";
}

// ── walCheckpoint ─────────────────────────────────────────────────────────
// Сбрасывает WAL в основной файл — уменьшает размер -wal файла
// и гарантирует что база читается без replay.

void Storage::walCheckpoint()
{
    QSqlDatabase db = getConnection();
    QSqlQuery q(db);
    q.exec("PRAGMA wal_checkpoint(TRUNCATE);");
    qDebug() << ">>> WAL checkpoint выполнен.";
}

// ── flushImpl ─────────────────────────────────────────────────────────────
// Приватный метод — весь SQL одного батча в одной транзакции.
// Все QSqlQuery объявлены внутри — уничтожаются при выходе из метода,
// до того как flush() вызовет closeCurrentThread().

void Storage::flushImpl(EntityBatch& batch, QSqlDatabase db)
{
    db.transaction();

    // ── 1. Сущности ────────────────────────────────────────────────────────
    QSqlQuery qEnt(db);
    qEnt.prepare(R"(
        INSERT INTO entities
            (usr, parent_usr, name, kind, file_path,
             offset_start, offset_end, is_implicit, is_external,
             type_usr, access, flags, comment)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(usr) DO UPDATE SET
            parent_usr   = COALESCE(NULLIF(excluded.parent_usr, ''),
                                    entities.parent_usr),
            name         = COALESCE(NULLIF(excluded.name, ''), entities.name),
            kind         = excluded.kind,
            file_path    = CASE WHEN excluded.is_external = 0
                           THEN excluded.file_path
                           ELSE COALESCE(entities.file_path,
                                         excluded.file_path) END,
            offset_start = CASE WHEN excluded.is_external = 0
                           THEN excluded.offset_start
                           ELSE COALESCE(entities.offset_start,
                                         excluded.offset_start) END,
            offset_end   = CASE WHEN excluded.is_external = 0
                           THEN excluded.offset_end
                           ELSE COALESCE(entities.offset_end,
                                         excluded.offset_end) END,
            is_implicit  = excluded.is_implicit,
            is_external  = MIN(entities.is_external, excluded.is_external),
            type_usr     = COALESCE(NULLIF(excluded.type_usr, ''),
                                    entities.type_usr),
            access       = excluded.access,
            flags        = excluded.flags,
            comment      = COALESCE(excluded.comment, entities.comment);
    )");

    auto nullIfEmpty = [](const QString& s) -> QVariant {
        return s.isEmpty() ? QVariant(QMetaType::fromType<QString>())
                           : QVariant(s);
    };

    QHash<int, int> tempToReal;  // временный индекс в батче → реальный DB id

    // Подготовка запроса для получения id после UPSERT
    QSqlQuery qGetId(db);
    qGetId.prepare("SELECT id FROM entities WHERE usr = ?;");

    for (int i = 0; i < batch.entities.size(); ++i) {
        const EntityRec& e = batch.entities[i];

        qEnt.addBindValue(e.usr);
        qEnt.addBindValue(nullIfEmpty(e.parentUsr));
        qEnt.addBindValue(e.name);
        qEnt.addBindValue(e.kind);
        qEnt.addBindValue(e.filePath);
        qEnt.addBindValue(e.offsetStart);
        qEnt.addBindValue(e.offsetEnd);
        qEnt.addBindValue(e.isImplicit);
        qEnt.addBindValue(e.isExternal);
        qEnt.addBindValue(nullIfEmpty(e.typeUsr));
        qEnt.addBindValue(e.access);
        qEnt.addBindValue(e.flags);
        qEnt.addBindValue(nullIfEmpty(e.comment));

        if (!qEnt.exec()) {
            qWarning() << "flush entity failed:"
                       << qEnt.lastError().text() << e.usr;
            continue;
        }

        // Безопасное получение id: всегда через SELECT.
        // lastInsertId() при ON CONFLICT DO UPDATE может быть ненадёжен
        // на разных версиях SQLite.
        int realId = -1;
        {
            QReadLocker rl(&m_cacheLock);
            auto cacheIt = m_idCache.find(e.usr);
            if (cacheIt != m_idCache.end())
                realId = cacheIt.value();
        }

        if (realId < 0) {
            qGetId.addBindValue(e.usr);
            if (qGetId.exec() && qGetId.next()) {
                realId = qGetId.value(0).toInt();
                QWriteLocker wl(&m_cacheLock);
                m_idCache[e.usr] = realId;
            }
        }

        if (realId >= 0)
            tempToReal[i] = realId;
    }

    // ── 2. Связи ───────────────────────────────────────────────────────────

    // Допустимые kind для to_id в Rel_Inherits — только CXIdxEntityKind.
    // После унификации через cursorKindToIdxKind все сущности хранят
    // одинаковый семантический kind независимо от того как они попали в базу.
    // CXIdxEntity_Struct=14, CXIdxEntity_CXXClass=16
    // (ClassTemplate и ClassTemplatePartialSpecialization тоже → 16)
    static const QSet<int> validBaseKinds = { 14, 16 };

    QSqlQuery qRel(db);
    qRel.prepare(R"(
        INSERT OR IGNORE INTO relations (from_id, to_id, rel_type)
        VALUES (?, ?, ?);
    )");

    // Кэш kind для сущностей батча — избегаем SELECT для каждой связи
    QHash<int, int> idToKind;
    for (const EntityRec& e : std::as_const(batch.entities)) {
        int id = getEntityId(e.usr);
        if (id >= 0) idToKind[id] = e.kind;
    }

    for (const RelationRec& r : std::as_const(batch.relations)) {
        int fromId = getEntityId(r.fromUsr);
        int toId   = getEntityId(r.toUsr);
        if (fromId < 0 || toId < 0) continue;

        // Самосвязи молча пропускаем
        if (fromId == toId) continue;

        // Для Rel_Inherits проверяем что to (базовый класс) — действительно класс
        if (r.relType == 2 /* Rel_Inherits */) {
            auto it = idToKind.find(toId);
            if (it != idToKind.end() && !validBaseKinds.contains(it.value()))
                continue;  // молча пропускаем — не класс
        }

        qRel.addBindValue(fromId);
        qRel.addBindValue(toId);
        qRel.addBindValue(r.relType);
        if (!qRel.exec())
            qWarning() << "flush relation failed:" << qRel.lastError().text();
    }

    // ── 3. Тела функций ────────────────────────────────────────────────────
    QSqlQuery qBody(db);
    qBody.prepare(R"(
        INSERT OR REPLACE INTO entity_bodies (entity_id, body_start, body_end, body_blob)
        VALUES (?, ?, ?, ?);
    )");

    for (const BodyRec& b : std::as_const(batch.bodies)) {
        int entityId = getEntityId(b.entityUsr);
        if (entityId < 0) continue;

        // Перезаписываем маркеры в blob: временный индекс → реальный DB id
        QByteArray blob = b.blob;
        for (int pos = 0; pos + 5 <= blob.size(); ) {
            if ((unsigned char)blob[pos] == 0x01) {
                int tempId = (unsigned char)blob[pos+1]        |
                             ((unsigned char)blob[pos+2] << 8)  |
                             ((unsigned char)blob[pos+3] << 16) |
                             ((unsigned char)blob[pos+4] << 24);
                auto it = tempToReal.find(tempId);
                if (it != tempToReal.end()) {
                    int realId = it.value();
                    blob[pos+1] = (char)( realId        & 0xFF);
                    blob[pos+2] = (char)((realId >>  8) & 0xFF);
                    blob[pos+3] = (char)((realId >> 16) & 0xFF);
                    blob[pos+4] = (char)((realId >> 24) & 0xFF);
                }
                pos += 5;
            } else {
                ++pos;
            }
        }

        qBody.addBindValue(entityId);
        qBody.addBindValue(b.bodyStart);
        qBody.addBindValue(b.bodyEnd);
        qBody.addBindValue(blob);
        if (!qBody.exec())
            qWarning() << "flush body failed:" << qBody.lastError().text();
    }

    db.commit();

    // Все QSqlQuery (qEnt, qGetId, qRel, qBody) уничтожаются здесь при выходе из метода
}

// ── flush ──────────────────────────────────────────────────────────────────

void Storage::flush(EntityBatch& batch)
{
    if (batch.entities.isEmpty() &&
        batch.relations.isEmpty() &&
        batch.bodies.isEmpty())
        return;

    // flushImpl завершается — все QSqlQuery уничтожены
    flushImpl(batch, getConnection());

    // Теперь безопасно закрыть соединение этого потока
    closeCurrentThread();
}

// ── getEntityId ───────────────────────────────────────────────────────────

int Storage::getEntityId(const QString& usr)
{
    {
        QReadLocker rl(&m_cacheLock);
        auto it = m_idCache.find(usr);
        if (it != m_idCache.end()) return it.value();
    }

    QSqlDatabase db = getConnection();
    QSqlQuery q(db);
    q.prepare("SELECT id FROM entities WHERE usr = ?;");
    q.addBindValue(usr);
    if (q.exec() && q.next()) {
        int id = q.value(0).toInt();
        QWriteLocker wl(&m_cacheLock);
        m_idCache[usr] = id;
        return id;
    }
    return -1;
}

bool Storage::entityExists(const QString& usr)
{
    QReadLocker rl(&m_cacheLock);
    if (m_idCache.contains(usr)) return true;
    rl.unlock();
    return getEntityId(usr) >= 0;
}

bool Storage::open(const QString& dbPath)
{
    if (!QFile::exists(dbPath)) {
        qCritical() << "Storage::open: файл не найден:" << dbPath;
        return false;
    }

    if (!m_dbPath.isEmpty())
        close();

    {
        QMutexLocker locker(&m_connMutex);
        m_connNames.clear();
        m_connCreating.clear();
    }
    {
        QWriteLocker locker(&m_cacheLock);
        m_idCache.clear();
    }

    m_dbPath = dbPath;
    qDebug() << ">>> База открыта:" << dbPath;
    return true;
}

void Storage::setIndexingComplete(const QString& projectRoot)
{
    QSqlDatabase db = getConnection();
    QSqlQuery q(db);

    q.prepare("INSERT OR REPLACE INTO db_info (key, value) VALUES ('indexing_complete', '1');");
    q.exec();

    q.prepare("INSERT OR REPLACE INTO db_info (key, value) VALUES ('completed_at', ?);");
    q.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    q.exec();

    q.prepare("INSERT OR REPLACE INTO db_info (key, value) VALUES ('project_root', ?);");
    q.addBindValue(projectRoot);
    q.exec();
}

void Storage::printStats()
{
    QSqlDatabase db = getConnection();
    QSqlQuery q(db);

    if (q.exec("SELECT COUNT(*) FROM entities WHERE is_external=0") && q.next())
        qDebug() << ">>> Сущностей проекта:" << q.value(0).toInt();
    if (q.exec("SELECT COUNT(*) FROM entities WHERE is_external=1") && q.next())
        qDebug() << ">>> Внешних сущностей:" << q.value(0).toInt();
    if (q.exec("SELECT COUNT(*) FROM relations") && q.next())
        qDebug() << ">>> Связей:"             << q.value(0).toInt();
    if (q.exec("SELECT COUNT(*) FROM entity_bodies") && q.next())
        qDebug() << ">>> Тел определений:"    << q.value(0).toInt();

    QReadLocker rl(&m_cacheLock);
    qDebug() << ">>> Кэш сущностей:" << m_idCache.size();
}

// ── closeCurrentThread ─────────────────────────────────────────────────────
// Закрывает соединение текущего потока.
// Вызывать только из того потока который его создал.
// К моменту вызова не должно быть живых QSqlQuery на стеке.

void Storage::closeCurrentThread()
{
    Qt::HANDLE tid = QThread::currentThreadId();

    QString connName;
    {
        QMutexLocker locker(&m_connMutex);
        auto it = m_connNames.find(tid);
        if (it == m_connNames.end()) return;
        connName = it.value();
        m_connNames.erase(it);
    }

    // Закрываем в отдельном scope: объект db уничтожается до removeDatabase
    {
        QSqlDatabase db = QSqlDatabase::database(connName, false);
        if (db.isValid() && db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

// ── close ──────────────────────────────────────────────────────────────────
// Вызывается из главного потока. К этому моменту все рабочие потоки
// уже закрыли свои соединения через closeCurrentThread().

void Storage::close()
{
    if (m_dbPath.isEmpty()) return;

    closeCurrentThread();

    {
        QMutexLocker locker(&m_connMutex);
        if (!m_connNames.isEmpty()) {
            qWarning() << "Storage::close: остались незакрытые соединения:"
                       << m_connNames.size();
            m_connNames.clear();
        }
        m_connCreating.clear();
    }

    {
        QWriteLocker wl(&m_cacheLock);
        m_idCache.clear();
    }

    m_dbPath.clear();
}