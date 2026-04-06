// === CodeIndexer.cpp ===
#include "CodeIndexer.h"
#include "Storage.h"
#include "IndexerConfig.h"
#include <clang-c/CXCompilationDatabase.h>
#include <QDebug>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QVector>
#include <QThreadPool>
#include <QRunnable>
#include <QSemaphore>
#include <QMutex>
#include <QThread>
#include <QRegularExpression>
#include <atomic>
#include <algorithm>

// ── Проверка доступности API libclang ─────────────────────────────────────
// Не все функции существуют во всех версиях libclang.
// Определяем доступность через прямую проверку наличия объявления.
//
// clang_EnumDecl_isScoped         — LLVM 16+
// clang_CXXMethod_isDeleted       — LLVM 16+
// clang_CXXMethod_isDefaulted     — LLVM 16+
// CXCursor_ConceptDecl            — LLVM 16+
// clang_Cursor_isConstexpr        — НЕ существует в libclang C API
// clang_Cursor_isInlineFunction   — НЕ существует в libclang C API

#ifndef CINDEX_VERSION_MINOR
#define CINDEX_VERSION_MINOR 0
#endif

#if CINDEX_VERSION_MINOR >= 62
#define SB_HAS_SCOPED_ENUM      1
#define SB_HAS_DELETED_DEFAULTED 1
#define SB_HAS_CONCEPT_DECL     1
#else
#define SB_HAS_SCOPED_ENUM      0
#define SB_HAS_DELETED_DEFAULTED 0
#define SB_HAS_CONCEPT_DECL     0
#endif

// constexpr и inline через libclang C API невозможны —
// эти функции не экспортированы в clang-c/Index.h.
// Флаги Flag_Constexpr и Flag_Inline зарезервированы,
// но не заполняются индексатором.
#define SB_HAS_CONSTEXPR        0
#define SB_HAS_INLINE_FUNCTION  0

// ── Флаги и типы ──────────────────────────────────────────────────────────

enum EntityFlags {
    Flag_Const          = 0x0001,
    Flag_Virtual        = 0x0002,
    Flag_Static         = 0x0004,
    Flag_Override       = 0x0008,
    Flag_PureVirtual    = 0x0010,
    Flag_Macro          = 0x0020,
    Flag_FunctionLike   = 0x0040,
    Flag_Template       = 0x0080,
    Flag_Specialization = 0x0100,
    Flag_Constexpr      = 0x0200,   // constexpr / consteval
    Flag_Inline         = 0x0400,   // явный inline
    Flag_Operator       = 0x0800,   // перегрузка оператора
    Flag_ScopedEnum     = 0x1000,   // enum class (scoped enum)
    Flag_Lambda         = 0x2000,   // лямбда-выражение
    Flag_Deleted        = 0x4000,   // = delete
    Flag_Defaulted      = 0x8000    // = default
};

enum RelType {
    Rel_Contains     = 1,
    Rel_Inherits     = 2,
    Rel_Calls        = 3,
    Rel_Reads        = 4,
    Rel_Writes       = 5,
    Rel_Instantiates = 6,
    Rel_Specializes  = 7,
    Rel_Friend       = 8,
    Rel_Overrides    = 9,
    Rel_Uses         = 10   // using directive/declaration
};

// ── Вспомогательные структуры ─────────────────────────────────────────────

struct BodyRef {
    int     offset      = 0;
    int     length      = 0;
    QString usr;
    QString name;
    QString filePath;
    int     kind        = 0;
    int     relType     = 0;
    int     refOffStart = 0;
    int     refOffEnd   = 0;
};

struct VisitorData {
    QString          ownerUsr;  // USR функции-владельца — для фильтрации самосвязей
    QVector<BodyRef> refs;
};

struct DeclVisitorData {
    const QString*     projectRoot;
    QStringList        buildDirFilters;   // значение — безопасно для лямбды
    EntityBatch*       batch;
    std::atomic<bool>* cancelFlag = nullptr;
};

// ── collectDiagnostics ────────────────────────────────────────────────────
// Собирает диагностики libclang severity >= minSeverity.
// Незаглушенные → out, заглушенные → suppressedOut (текст → счётчик).
// Возвращает true если в out что-то попало.

static bool collectDiagnostics(CXTranslationUnit tu,
                               int minSeverity,
                               const QStringList& suppressPatterns,
                               QList<ClangDiagnostic>& out,
                               QHash<QString, int>& suppressedOut)
{
    if (!tu) return false;

    unsigned numDiag = clang_getNumDiagnostics(tu);
    bool found = false;

    for (unsigned i = 0; i < numDiag; ++i) {
        CXDiagnostic diag = clang_getDiagnostic(tu, i);
        int sev = (int)clang_getDiagnosticSeverity(diag);

        if (sev < minSeverity) {
            clang_disposeDiagnostic(diag);
            continue;
        }

        ClangDiagnostic cd;
        cd.severity = sev;

        switch (sev) {
        case CXDiagnostic_Ignored: cd.severityStr = "ignored";  break;
        case CXDiagnostic_Note:    cd.severityStr = "note";     break;
        case CXDiagnostic_Warning: cd.severityStr = "warning";  break;
        case CXDiagnostic_Error:   cd.severityStr = "error";    break;
        case CXDiagnostic_Fatal:   cd.severityStr = "fatal";    break;
        default:                   cd.severityStr = "unknown";  break;
        }

        CXString spell = clang_getDiagnosticSpelling(diag);
        cd.text = QString::fromUtf8(clang_getCString(spell));
        clang_disposeString(spell);

        // Проверяем паттерны подавления
        bool suppressed = false;
        for (const QString& pat : suppressPatterns) {
            if (cd.text.contains(pat, Qt::CaseInsensitive)) {
                suppressed = true;
                break;
            }
        }
        if (suppressed) {
            // Фиксируем подавленную диагностику — укорачиваем текст до
            // первых 120 символов чтобы похожие сообщения группировались
            QString key = cd.text.left(120);
            suppressedOut[key]++;
            clang_disposeDiagnostic(diag);
            continue;
        }

        // Местоположение: файл:строка:столбец
        CXSourceLocation loc = clang_getDiagnosticLocation(diag);
        CXFile file;
        unsigned line = 0, col = 0;
        clang_getSpellingLocation(loc, &file, &line, &col, nullptr);
        if (file) {
            CXString fn = clang_getFileName(file);
            cd.location = QString("%1:%2:%3")
                              .arg(QString::fromLocal8Bit(clang_getCString(fn))
                                       .replace("\\", "/"))
                              .arg(line)
                              .arg(col);
            clang_disposeString(fn);
        }

        // Категория
        CXString cat = clang_getDiagnosticCategoryText(diag);
        cd.category = QString::fromUtf8(clang_getCString(cat));
        clang_disposeString(cat);

        out.append(cd);
        found = true;

        clang_disposeDiagnostic(diag);
    }

    return found;
}

// ── Конвертация CXCursorKind → CXIdxEntityKind ────────────────────────────
// Все сущности в базе хранят kind как CXIdxEntityKind — семантический enum
// из IndexerCallbacks API. Внешние сущности создаются через clang_getCursorKind
// (CXCursorKind) — конвертируем чтобы kind был единым по всей базе.
//
// Специальные значения за пределами CXIdxEntityKind:
//   500 — Macro
//   501 — NamespaceAlias
//   502 — Concept (C++20)

static int cursorKindToIdxKind(CXCursorKind ck)
{
    switch (ck) {
    // Функции
    case CXCursor_FunctionDecl:     return 2;   // CXIdxEntity_Function
    case CXCursor_FunctionTemplate: return 2;
    // Переменные
    case CXCursor_VarDecl:          return 3;   // CXIdxEntity_Variable
    // Поля
    case CXCursor_FieldDecl:        return 4;   // CXIdxEntity_Field
    // Перечисления
    case CXCursor_EnumDecl:         return 5;   // CXIdxEntity_Enum
    case CXCursor_EnumConstantDecl: return 6;   // CXIdxEntity_EnumConstant
    // Классы и структуры
    case CXCursor_StructDecl:                   return 14;  // CXIdxEntity_Struct
    case CXCursor_ClassDecl:                    return 16;  // CXIdxEntity_CXXClass
    case CXCursor_ClassTemplate:                return 16;
    case CXCursor_ClassTemplatePartialSpecialization: return 16;
    // Методы
    case CXCursor_CXXMethod:        return 21;  // CXIdxEntity_CXXInstanceMethod
    case CXCursor_Constructor:      return 22;  // CXIdxEntity_CXXConstructor
    case CXCursor_Destructor:       return 23;  // CXIdxEntity_CXXDestructor
    case CXCursor_ConversionFunction: return 24;
    // Typedef / using
    case CXCursor_TypedefDecl:      return 32;  // CXIdxEntity_Typedef
    case CXCursor_TypeAliasDecl:    return 32;
    case CXCursor_TypeAliasTemplateDecl: return 32;
    // Namespace
    case CXCursor_Namespace:        return 20;  // CXIdxEntity_CXXNamespace
    case CXCursor_NamespaceAlias:   return 501; // Отдельное значение — не 21 (CXXInstanceMethod)
    // Макросы (CXCursorKind специфичные)
    case CXCursor_MacroDefinition:  return 500; // нет аналога в CXIdxEntityKind — сохраняем как есть
#if SB_HAS_CONCEPT_DECL
    // C++20 concept
    case CXCursor_ConceptDecl:      return 502;
#endif
    default:
        // Для всего остального сохраняем CXCursorKind напрямую с флагом смещения
        // чтобы не путать с CXIdxEntityKind — добавляем 1000
        return (int)ck + 1000;
    }
}

// ── Вспомогательные функции ───────────────────────────────────────────────

static void ensureExternalInBatch(EntityBatch& batch,
                                  const QString& usr,
                                  const QString& name,
                                  int kind,
                                  const QString& filePath = {})
{
    if (usr.isEmpty()) return;
    if (batch.hasEntity(usr)) return;

    EntityRec e;
    e.usr        = usr;
    e.name       = name.isEmpty() ? usr : name;
    e.kind       = kind;
    e.filePath   = filePath;
    e.isExternal = true;
    batch.addEntity(e);
}

static void ensureExternalInBatch(EntityBatch& batch,
                                  const QString& usr,
                                  const QString& name,
                                  CXCursor cursor)
{
    if (usr.isEmpty()) return;
    if (batch.hasEntity(usr)) return;

    CXFile file;
    unsigned startOffset = 0, endOffset = 0;
    CXSourceRange range = clang_getCursorExtent(cursor);
    clang_getSpellingLocation(clang_getRangeStart(range),
                              &file, nullptr, nullptr, &startOffset);
    CXFile fileEnd;
    clang_getSpellingLocation(clang_getRangeEnd(range),
                              &fileEnd, nullptr, nullptr, &endOffset);

    QString filePath;
    if (file) {
        CXString fn = clang_getFileName(file);
        filePath = QString::fromLocal8Bit(clang_getCString(fn))
                       .replace("\\", "/").toLower();
        clang_disposeString(fn);
    }
    if (fileEnd != file) endOffset = startOffset;

    EntityRec e;
    e.usr         = usr;
    e.name        = name.isEmpty() ? usr : name;
    e.kind        = cursorKindToIdxKind(clang_getCursorKind(cursor));
    e.filePath    = filePath;
    e.offsetStart = (int)startOffset;
    e.offsetEnd   = (int)endOffset;
    e.isExternal  = true;
    batch.addEntity(e);
}

static int detectRelType(CXCursor cursor, CXCursor parent, CXCursor referenced)
{
    CXCursorKind refKind = clang_getCursorKind(referenced);

    if (refKind == CXCursor_Constructor)
        return Rel_Instantiates;

    if (refKind == CXCursor_FunctionDecl ||
        refKind == CXCursor_CXXMethod    ||
        refKind == CXCursor_Destructor   ||
        refKind == CXCursor_FunctionTemplate ||
        refKind == CXCursor_ConversionFunction)
        return Rel_Calls;

    if (refKind == CXCursor_FieldDecl       ||
        refKind == CXCursor_VarDecl         ||
        refKind == CXCursor_EnumConstantDecl)
    {
        CXCursorKind pk = clang_getCursorKind(parent);
        if (pk == CXCursor_BinaryOperator ||
            pk == CXCursor_CompoundAssignOperator)
        {
            unsigned parentOff = 0, cursorOff = 0;
            clang_getSpellingLocation(
                clang_getRangeStart(clang_getCursorExtent(parent)),
                nullptr, nullptr, nullptr, &parentOff);
            clang_getSpellingLocation(
                clang_getRangeStart(clang_getCursorExtent(cursor)),
                nullptr, nullptr, nullptr, &cursorOff);
            if (cursorOff == parentOff)
                return Rel_Writes;
        }
        if (pk == CXCursor_UnaryOperator)
            return Rel_Writes;

        return Rel_Reads;
    }

    if (refKind == CXCursor_ClassDecl     ||
        refKind == CXCursor_StructDecl    ||
        refKind == CXCursor_ClassTemplate ||
        refKind == CXCursor_TypedefDecl   ||
        refKind == CXCursor_TypeAliasDecl
#if SB_HAS_CONCEPT_DECL
        || refKind == CXCursor_ConceptDecl
#endif
        )
        return Rel_Reads;

    return Rel_Calls;
}

static CXChildVisitResult bodyVisitor(CXCursor cursor,
                                      CXCursor parent,
                                      CXClientData clientData)
{
    CXCursorKind kind = clang_getCursorKind(cursor);

    // Лямбды — отдельные анонимные сущности.
    // Рекурсивно входим внутрь, но ссылки внутри лямбды
    // привязываются к внешней функции (владельцу тела).
    // TODO: в будущем можно создавать отдельную сущность для лямбды.
    if (kind == CXCursor_LambdaExpr)
        return CXChildVisit_Recurse;

    if (kind != CXCursor_DeclRefExpr   &&
        kind != CXCursor_MemberRefExpr &&
        kind != CXCursor_TypeRef       &&
        kind != CXCursor_TemplateRef   &&
        kind != CXCursor_CallExpr)
        return CXChildVisit_Recurse;

    CXCursor referenced = clang_getCursorReferenced(cursor);
    if (clang_Cursor_isNull(referenced)) return CXChildVisit_Recurse;

    CXCursorKind refKind = clang_getCursorKind(referenced);

    // Пропускаем локальные переменные и параметры
    if (refKind == CXCursor_VarDecl || refKind == CXCursor_ParmDecl) {
        CXCursor refParent = clang_getCursorSemanticParent(referenced);
        CXCursorKind pk = clang_getCursorKind(refParent);
        if (pk == CXCursor_FunctionDecl || pk == CXCursor_CXXMethod ||
            pk == CXCursor_Constructor  || pk == CXCursor_Destructor ||
            pk == CXCursor_ConversionFunction ||
            pk == CXCursor_FunctionTemplate)
            return CXChildVisit_Recurse;
    }

    CXString usrStr = clang_getCursorUSR(referenced);
    QString usr = QString::fromUtf8(clang_getCString(usrStr));
    clang_disposeString(usrStr);
    if (usr.isEmpty()) return CXChildVisit_Recurse;

    // Самосвязь — фильтруем в источнике
    auto *vd = static_cast<VisitorData*>(clientData);
    if (usr == vd->ownerUsr) return CXChildVisit_Recurse;

    CXSourceRange range = clang_getCursorExtent(cursor);
    unsigned offset = 0, endOffset = 0;
    clang_getSpellingLocation(clang_getRangeStart(range),
                              nullptr, nullptr, nullptr, &offset);
    clang_getSpellingLocation(clang_getRangeEnd(range),
                              nullptr, nullptr, nullptr, &endOffset);

    CXString nameStr = clang_getCursorSpelling(referenced);
    QString  name    = QString::fromUtf8(clang_getCString(nameStr));
    clang_disposeString(nameStr);

    QString extPath;
    unsigned refOffStart = 0, refOffEnd = 0;
    CXFile extFile;
    CXSourceRange refRange = clang_getCursorExtent(referenced);
    CXFile refFileEnd;
    clang_getSpellingLocation(clang_getRangeStart(refRange),
                              &extFile, nullptr, nullptr, &refOffStart);
    clang_getSpellingLocation(clang_getRangeEnd(refRange),
                              &refFileEnd, nullptr, nullptr, &refOffEnd);
    if (extFile) {
        CXString extFn = clang_getFileName(extFile);
        extPath = QString::fromLocal8Bit(clang_getCString(extFn))
                      .replace("\\", "/").toLower();
        clang_disposeString(extFn);
    }
    if (refFileEnd != extFile) refOffEnd = refOffStart;

    BodyRef ref;
    ref.offset      = (int)offset;
    ref.length      = (int)(endOffset - offset);
    ref.usr         = usr;
    ref.name        = name;
    ref.filePath    = extPath;
    ref.kind        = cursorKindToIdxKind(refKind);  // унифицируем с остальной базой
    ref.relType     = detectRelType(cursor, parent, referenced);
    ref.refOffStart = (int)refOffStart;
    ref.refOffEnd   = (int)refOffEnd;

    vd->refs.append(ref);
    return CXChildVisit_Recurse;
}

// ── Проверка: является ли имя перегрузкой оператора ──────────────────────

static bool isOperatorName(const QString& name)
{
    return name.startsWith("operator") &&
           (name.length() == 8 ||                            // "operator" ровно
            !name[8].isLetterOrNumber());                    // "operator+" и т.д.
}

// ── Проверка: scoped enum (enum class / enum struct) ─────────────────────

#if SB_HAS_SCOPED_ENUM
static bool isScopedEnum(CXCursor cursor)
{
    return clang_EnumDecl_isScoped(cursor) != 0;
}
#endif

// Декларационный визитор — вызывается через IndexerCallbacks
static void onDeclaration(CXClientData clientData, const CXIdxDeclInfo *info)
{
    if (!info->entityInfo || !info->entityInfo->name) return;

    auto *dd = static_cast<DeclVisitorData*>(clientData);

    CXFile file;
    unsigned startOffset = 0, endOffset = 0;
    CXSourceRange range = clang_getCursorExtent(info->cursor);
    clang_getSpellingLocation(clang_getRangeStart(range),
                              &file, nullptr, nullptr, &startOffset);
    CXFile fileEnd;
    clang_getSpellingLocation(clang_getRangeEnd(range),
                              &fileEnd, nullptr, nullptr, &endOffset);
    if (!file) return;
    if (fileEnd != file) endOffset = startOffset;

    CXString fileName = clang_getFileName(file);
    QString path = QString::fromLocal8Bit(clang_getCString(fileName))
                       .replace("\\", "/").toLower();
    clang_disposeString(fileName);

    if (!path.startsWith(*dd->projectRoot)) return;
    for (const QString& f : std::as_const(dd->buildDirFilters))
        if (path.contains(f)) return;
    if (path.contains("moc_")) return;

    QString parentUsr;
    CXCursor parent = clang_getCursorSemanticParent(info->cursor);
    if (!clang_Cursor_isNull(parent)) {
        CXString pUsr = clang_getCursorUSR(parent);
        parentUsr = QString::fromUtf8(clang_getCString(pUsr));
        clang_disposeString(pUsr);
    }

    QString typeUsr;
    CXType cxType = clang_getCursorType(info->cursor);
    if (cxType.kind != CXType_Invalid && cxType.kind != CXType_Unexposed) {
        CXCursor typeDecl = clang_getTypeDeclaration(cxType);
        if (!clang_Cursor_isNull(typeDecl)) {
            CXString tUsr = clang_getCursorUSR(typeDecl);
            typeUsr = QString::fromUtf8(clang_getCString(tUsr));
            clang_disposeString(tUsr);
        }
    }

    int access = 0;
    switch (clang_getCXXAccessSpecifier(info->cursor)) {
    case CX_CXXPublic:    access = 1; break;
    case CX_CXXProtected: access = 2; break;
    case CX_CXXPrivate:   access = 3; break;
    default:              access = 0; break;
    }

    int flags = 0;
    if (clang_CXXMethod_isConst(info->cursor))       flags |= Flag_Const;
    if (clang_CXXMethod_isVirtual(info->cursor))     flags |= Flag_Virtual;
    if (clang_CXXMethod_isStatic(info->cursor))      flags |= Flag_Static;
    if (clang_CXXMethod_isPureVirtual(info->cursor)) flags |= Flag_PureVirtual;

    // C++11: constexpr
#if SB_HAS_CONSTEXPR
    if (clang_Cursor_isConstexpr(info->cursor))      flags |= Flag_Constexpr;
#endif

    // C++17: inline переменные/функции (явный)
#if SB_HAS_INLINE_FUNCTION
    if (clang_Cursor_isInlineFunction(info->cursor)) flags |= Flag_Inline;
#endif

    // Перегрузка оператора — проверяем имя
    QString entityName = QString::fromUtf8(info->entityInfo->name);
    if (isOperatorName(entityName))                  flags |= Flag_Operator;

    // Scoped enum (enum class / enum struct)
#if SB_HAS_SCOPED_ENUM
    if (info->entityInfo->kind == CXIdxEntity_Enum) {
        if (isScopedEnum(info->cursor))              flags |= Flag_ScopedEnum;
    }
#endif

    // = delete / = default
#if SB_HAS_DELETED_DEFAULTED
    if (clang_CXXMethod_isDeleted(info->cursor))     flags |= Flag_Deleted;
    if (clang_CXXMethod_isDefaulted(info->cursor))   flags |= Flag_Defaulted;
#endif

    unsigned numOverridden = 0;
    CXCursor *overridden = nullptr;
    clang_getOverriddenCursors(info->cursor, &overridden, &numOverridden);
    if (numOverridden > 0) {
        flags |= Flag_Override;
        for (unsigned o = 0; o < numOverridden; ++o) {
            CXString oUsr  = clang_getCursorUSR(overridden[o]);
            CXString oName = clang_getCursorSpelling(overridden[o]);
            QString oUsrStr  = QString::fromUtf8(clang_getCString(oUsr));
            QString oNameStr = QString::fromUtf8(clang_getCString(oName));
            clang_disposeString(oUsr);
            clang_disposeString(oName);
            if (!oUsrStr.isEmpty()) {
                ensureExternalInBatch(*dd->batch, oUsrStr, oNameStr,
                                      overridden[o]);
                dd->batch->addRelation(
                    QString::fromUtf8(info->entityInfo->USR),
                    oUsrStr, Rel_Overrides);
            }
        }
        clang_disposeOverriddenCursors(overridden);
    }

    CXCursorKind cursorKind = clang_getCursorKind(info->cursor);
    if (cursorKind == CXCursor_ClassTemplate ||
        cursorKind == CXCursor_FunctionTemplate ||
        cursorKind == CXCursor_ClassTemplatePartialSpecialization)
        flags |= Flag_Template;

    CXCursor primaryTemplate = clang_getSpecializedCursorTemplate(info->cursor);
    if (!clang_Cursor_isNull(primaryTemplate))
        flags |= Flag_Specialization;

    QString comment;
    CXString rawComment = clang_Cursor_getRawCommentText(info->cursor);
    const char *rawStr = clang_getCString(rawComment);
    if (rawStr && rawStr[0] != '\0')
        comment = QString::fromUtf8(rawStr);
    clang_disposeString(rawComment);

    QString usr = QString::fromUtf8(info->entityInfo->USR);

    EntityRec e;
    e.usr         = usr;
    e.parentUsr   = parentUsr;
    e.name        = entityName;
    e.kind        = (int)info->entityInfo->kind;
    e.filePath    = path;
    e.offsetStart = (int)startOffset;
    e.offsetEnd   = (int)endOffset;
    e.isImplicit  = (info->isImplicit != 0);
    e.isExternal  = false;
    e.typeUsr     = typeUsr;
    e.access      = access;
    e.flags       = flags;
    e.comment     = comment;
    dd->batch->addEntity(e);

    // Связь специализации
    if (flags & Flag_Specialization) {
        CXString tmplUsr  = clang_getCursorUSR(primaryTemplate);
        CXString tmplName = clang_getCursorSpelling(primaryTemplate);
        QString tmplUsrStr  = QString::fromUtf8(clang_getCString(tmplUsr));
        QString tmplNameStr = QString::fromUtf8(clang_getCString(tmplName));
        clang_disposeString(tmplUsr);
        clang_disposeString(tmplName);
        if (!tmplUsrStr.isEmpty()) {
            ensureExternalInBatch(*dd->batch, tmplUsrStr, tmplNameStr,
                                  primaryTemplate);
            dd->batch->addRelation(usr, tmplUsrStr, Rel_Specializes);
        }
    }

    // Связь Contains
    if (!parentUsr.isEmpty())
        dd->batch->addRelation(parentUsr, usr, Rel_Contains);

    // Связи наследования
    if (info->entityInfo->kind == CXIdxEntity_CXXClass ||
        info->entityInfo->kind == CXIdxEntity_Struct)
    {
        const CXIdxCXXClassDeclInfo *classInfo =
            clang_index_getCXXClassDeclInfo(info);
        if (classInfo) {
            for (unsigned b = 0; b < classInfo->numBases; ++b) {
                const CXIdxBaseClassInfo *base = classInfo->bases[b];
                if (!base || !base->base) continue;

                // base->base->cursor — курсор использования (TypeRef/TemplateRef).
                // Получаем реальное объявление через clang_getCursorReferenced.
                CXCursor baseCursor = clang_getCursorReferenced(base->base->cursor);
                if (clang_Cursor_isNull(baseCursor))
                    baseCursor = base->base->cursor;  // fallback на исходный

                // Проверяем что referenced — действительно класс или структура
                CXCursorKind baseKind = clang_getCursorKind(baseCursor);
                if (baseKind != CXCursor_ClassDecl            &&
                    baseKind != CXCursor_StructDecl           &&
                    baseKind != CXCursor_ClassTemplate        &&
                    baseKind != CXCursor_ClassTemplatePartialSpecialization)
                    continue;

                CXString baseUsr  = clang_getCursorUSR(baseCursor);
                CXString baseName = clang_getCursorSpelling(baseCursor);
                QString baseUsrStr  = QString::fromUtf8(clang_getCString(baseUsr));
                QString baseNameStr = QString::fromUtf8(clang_getCString(baseName));
                clang_disposeString(baseUsr);
                clang_disposeString(baseName);

                if (baseUsrStr.isEmpty() || baseUsrStr == usr) continue;

                ensureExternalInBatch(*dd->batch, baseUsrStr,
                                      baseNameStr, baseCursor);
                dd->batch->addRelation(usr, baseUsrStr, Rel_Inherits);
            }
        }
    }
}

// Сбор макросов
static void collectMacros(CXTranslationUnit tu,
                          const QString& projectRoot,
                          const QStringList& buildDirFilters,
                          EntityBatch& batch)
{
    CXCursor tuCursor = clang_getTranslationUnitCursor(tu);

    struct MacroData {
        const QString*     projectRoot;
        const QStringList* buildDirFilters;
        EntityBatch*       batch;
    };
    MacroData md { &projectRoot, &buildDirFilters, &batch };

    clang_visitChildren(tuCursor,
                        [](CXCursor cursor, CXCursor, CXClientData cd) -> CXChildVisitResult
                        {
                            if (clang_getCursorKind(cursor) != CXCursor_MacroDefinition)
                                return CXChildVisit_Continue;

                            if (clang_Location_isInSystemHeader(clang_getCursorLocation(cursor)))
                                return CXChildVisit_Continue;

                            CXFile file;
                            unsigned startOffset = 0, endOffset = 0;
                            CXSourceRange range = clang_getCursorExtent(cursor);
                            clang_getSpellingLocation(clang_getRangeStart(range),
                                                      &file, nullptr, nullptr, &startOffset);
                            clang_getSpellingLocation(clang_getRangeEnd(range),
                                                      nullptr, nullptr, nullptr, &endOffset);
                            if (!file) return CXChildVisit_Continue;

                            CXString fn = clang_getFileName(file);
                            QString path = QString::fromLocal8Bit(clang_getCString(fn))
                                               .replace("\\", "/").toLower();
                            clang_disposeString(fn);

                            auto *md = static_cast<MacroData*>(cd);
                            if (!path.startsWith(*md->projectRoot)) return CXChildVisit_Continue;
                            for (const QString& f : std::as_const(*md->buildDirFilters))
                                if (path.contains(f)) return CXChildVisit_Continue;

                            CXString nameStr = clang_getCursorSpelling(cursor);
                            QString name = QString::fromUtf8(clang_getCString(nameStr));
                            clang_disposeString(nameStr);
                            if (name.isEmpty()) return CXChildVisit_Continue;

                            QString usr = QString("macro@%1:%2").arg(path).arg(startOffset);

                            QByteArray bodyBytes;
                            CXToken *tokens = nullptr;
                            unsigned numTokens = 0;
                            clang_tokenize(clang_Cursor_getTranslationUnit(cursor),
                                           range, &tokens, &numTokens);
                            if (tokens && numTokens > 1) {
                                QStringList parts;
                                for (unsigned t = 1; t < numTokens; ++t) {
                                    CXString spell = clang_getTokenSpelling(
                                        clang_Cursor_getTranslationUnit(cursor), tokens[t]);
                                    parts << QString::fromUtf8(clang_getCString(spell));
                                    clang_disposeString(spell);
                                }
                                bodyBytes = parts.join(" ").toUtf8();
                            }
                            if (tokens)
                                clang_disposeTokens(clang_Cursor_getTranslationUnit(cursor),
                                                    tokens, numTokens);

                            int macroFlags = Flag_Macro;
                            if (clang_Cursor_isMacroFunctionLike(cursor))
                                macroFlags |= Flag_FunctionLike;

                            EntityRec e;
                            e.usr         = usr;
                            e.name        = name;
                            e.kind        = cursorKindToIdxKind(CXCursor_MacroDefinition);
                            e.filePath    = path;
                            e.offsetStart = (int)startOffset;
                            e.offsetEnd   = (int)endOffset;
                            e.isExternal  = false;
                            e.flags       = macroFlags;
                            md->batch->addEntity(e);

                            if (!bodyBytes.isEmpty())
                                md->batch->addBody(usr,
                                                   (int)startOffset, (int)endOffset,
                                                   bodyBytes);

                            return CXChildVisit_Continue;
                        }, &md);
}

// ── Сбор using declarations и using directives ───────────────────────────
// Создаёт связь Rel_Uses между контекстом (namespace/функция/файл)
// и целевой сущностью. Важно для анализа зависимостей.

static void collectUsings(CXTranslationUnit tu,
                          const QString& projectRoot,
                          const QStringList& buildDirFilters,
                          EntityBatch& batch)
{
    CXCursor tuCursor = clang_getTranslationUnitCursor(tu);

    struct UsingData {
        const QString*     projectRoot;
        const QStringList* buildDirFilters;
        EntityBatch*       batch;
    };
    UsingData ud { &projectRoot, &buildDirFilters, &batch };

    clang_visitChildren(tuCursor,
                        [](CXCursor cursor, CXCursor, CXClientData cd) -> CXChildVisitResult
                        {
                            CXCursorKind kind = clang_getCursorKind(cursor);
                            if (kind != CXCursor_UsingDeclaration &&
                                kind != CXCursor_UsingDirective)
                                return CXChildVisit_Recurse;

                            CXSourceLocation loc = clang_getCursorLocation(cursor);
                            if (clang_Location_isInSystemHeader(loc))
                                return CXChildVisit_Recurse;

                            CXFile file;
                            clang_getSpellingLocation(loc, &file, nullptr, nullptr, nullptr);
                            if (!file) return CXChildVisit_Recurse;

                            CXString fn = clang_getFileName(file);
                            QString path = QString::fromLocal8Bit(clang_getCString(fn))
                                               .replace("\\", "/").toLower();
                            clang_disposeString(fn);

                            auto *ud = static_cast<UsingData*>(cd);
                            if (!path.startsWith(*ud->projectRoot)) return CXChildVisit_Recurse;
                            for (const QString& f : std::as_const(*ud->buildDirFilters))
                                if (path.contains(f)) return CXChildVisit_Recurse;

                            // Контекст — семантический родитель (namespace, класс, функция)
                            CXCursor ownerCursor = clang_getCursorSemanticParent(cursor);
                            if (clang_Cursor_isNull(ownerCursor)) return CXChildVisit_Recurse;

                            CXString ownerUsrStr = clang_getCursorUSR(ownerCursor);
                            QString ownerUsr = QString::fromUtf8(clang_getCString(ownerUsrStr));
                            clang_disposeString(ownerUsrStr);
                            if (ownerUsr.isEmpty()) return CXChildVisit_Recurse;

                            // Цель using declaration / directive
                            CXCursor referenced = clang_getCursorReferenced(cursor);
                            if (clang_Cursor_isNull(referenced)) return CXChildVisit_Recurse;

                            CXString refUsr  = clang_getCursorUSR(referenced);
                            CXString refName = clang_getCursorSpelling(referenced);
                            QString refUsrStr  = QString::fromUtf8(clang_getCString(refUsr));
                            QString refNameStr = QString::fromUtf8(clang_getCString(refName));
                            clang_disposeString(refUsr);
                            clang_disposeString(refName);

                            if (refUsrStr.isEmpty() || refUsrStr == ownerUsr)
                                return CXChildVisit_Recurse;

                            ensureExternalInBatch(*ud->batch, refUsrStr, refNameStr, referenced);
                            ud->batch->addRelation(ownerUsr, refUsrStr, Rel_Uses);

                            return CXChildVisit_Recurse;
                        }, &ud);
}

// Обход тел функций и определений с телом (включая conversion functions)
static void collectBodies(CXTranslationUnit tu,
                          const QString& currentFile,
                          const QByteArray& sourceBytes,
                          EntityBatch& batch)
{
    CXCursor tuCursor = clang_getTranslationUnitCursor(tu);

    struct TopData {
        const QString*     filePath;
        const QByteArray*  sourceBytes;
        EntityBatch*       batch;
    };
    TopData td { &currentFile, &sourceBytes, &batch };

    clang_visitChildren(tuCursor,
                        [](CXCursor cursor, CXCursor, CXClientData cd) -> CXChildVisitResult
                        {
                            auto *td = static_cast<TopData*>(cd);

                            CXCursorKind kind = clang_getCursorKind(cursor);

                            // Собираем тела для всех определений с фигурными скобками:
                            // обычные функции, методы, конструкторы, деструкторы,
                            // conversion functions (operator Type())
                            if (kind != CXCursor_FunctionDecl &&
                                kind != CXCursor_CXXMethod    &&
                                kind != CXCursor_Constructor  &&
                                kind != CXCursor_Destructor   &&
                                kind != CXCursor_ConversionFunction &&
                                kind != CXCursor_FunctionTemplate)
                                return CXChildVisit_Recurse;

                            if (!clang_isCursorDefinition(cursor))
                                return CXChildVisit_Continue;

                            CXSourceLocation loc = clang_getCursorLocation(cursor);
                            CXFile file;
                            clang_getSpellingLocation(loc, &file, nullptr, nullptr, nullptr);
                            if (!file) return CXChildVisit_Continue;

                            CXString fn = clang_getFileName(file);
                            QString path = QString::fromLocal8Bit(clang_getCString(fn))
                                               .replace("\\", "/").toLower();
                            clang_disposeString(fn);
                            if (path != *td->filePath) return CXChildVisit_Continue;

                            CXString usrStr = clang_getCursorUSR(cursor);
                            QString usr = QString::fromUtf8(clang_getCString(usrStr));
                            clang_disposeString(usrStr);
                            if (usr.isEmpty()) return CXChildVisit_Continue;

                            CXSourceRange range = clang_getCursorExtent(cursor);
                            unsigned bodyStart = 0, bodyEnd = 0;
                            clang_getSpellingLocation(clang_getRangeStart(range),
                                                      nullptr, nullptr, nullptr, &bodyStart);
                            clang_getSpellingLocation(clang_getRangeEnd(range),
                                                      nullptr, nullptr, nullptr, &bodyEnd);

                            if (bodyEnd <= bodyStart ||
                                bodyEnd > (unsigned)td->sourceBytes->size())
                                return CXChildVisit_Continue;

                            VisitorData vd;
                            vd.ownerUsr = usr;
                            clang_visitChildren(cursor, bodyVisitor, &vd);

                            QSet<QPair<QString,int>> savedRels;
                            for (const BodyRef& ref : std::as_const(vd.refs)) {
                                if (ref.name.isEmpty()) continue;

                                if (!td->batch->hasEntity(ref.usr)) {
                                    EntityRec e;
                                    e.usr         = ref.usr;
                                    e.name        = ref.name;
                                    e.kind        = ref.kind;
                                    e.filePath    = ref.filePath;
                                    e.offsetStart = ref.refOffStart;
                                    e.offsetEnd   = ref.refOffEnd;
                                    e.isExternal  = true;
                                    td->batch->addEntity(e);
                                }

                                auto key = qMakePair(ref.usr, ref.relType);
                                if (!savedRels.contains(key)) {
                                    td->batch->addRelation(usr, ref.usr, ref.relType);
                                    savedRels.insert(key);
                                }
                            }

                            QSet<int> seenOffsets;
                            QVector<BodyRef> unique;
                            for (const BodyRef& r : std::as_const(vd.refs)) {
                                if (!seenOffsets.contains(r.offset)) {
                                    seenOffsets.insert(r.offset);
                                    unique.append(r);
                                }
                            }
                            std::sort(unique.begin(), unique.end(),
                                      [](const BodyRef& a, const BodyRef& b) {
                                          return a.offset > b.offset;
                                      });

                            QByteArray blob = td->sourceBytes->mid(
                                (int)bodyStart, (int)(bodyEnd - bodyStart));

                            if (blob.isEmpty()) return CXChildVisit_Continue;

                            int bodyBraceOpen = -1;
                            {
                                int balance = 0;
                                for (int p = blob.size() - 1; p >= 0; --p) {
                                    char c = blob[p];
                                    if (c == '}') ++balance;
                                    else if (c == '{') {
                                        --balance;
                                        if (balance == 0) {
                                            bodyBraceOpen = p;
                                            break;
                                        }
                                    }
                                }
                                if (bodyBraceOpen < 0)
                                    return CXChildVisit_Continue;
                            }

                            int sigStart = 0;
                            if (bodyBraceOpen > 1000) {
                                int searchFrom = qMax(0, bodyBraceOpen - 65536);
                                int p = bodyBraceOpen - 1;

                                while (p >= searchFrom) {
                                    char c = blob[p];

                                    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                                    { --p; continue; }

                                    if (c == '/' && p > 0 && blob[p-1] == '*') {
                                        sigStart = p + 1;
                                        break;
                                    }

                                    if (c == '#') {
                                        sigStart = p;
                                        while (sigStart < bodyBraceOpen && blob[sigStart] != '\n')
                                            ++sigStart;
                                        while (sigStart < bodyBraceOpen &&
                                               (blob[sigStart] == ' ' || blob[sigStart] == '\t' ||
                                                blob[sigStart] == '\n' || blob[sigStart] == '\r'))
                                            ++sigStart;
                                        break;
                                    }

                                    if (c == '}' || c == ';') {
                                        sigStart = p + 1;
                                        while (sigStart < bodyBraceOpen &&
                                               (blob[sigStart] == ' ' || blob[sigStart] == '\t' ||
                                                blob[sigStart] == '\n' || blob[sigStart] == '\r'))
                                            ++sigStart;
                                        break;
                                    }

                                    if (c == ')') {
                                        int balance = 0;
                                        while (p >= searchFrom) {
                                            char x = blob[p];
                                            if (x == ')') ++balance;
                                            else if (x == '(') {
                                                --balance;
                                                if (balance == 0) { --p; break; }
                                            }
                                            --p;
                                        }
                                        continue;
                                    }

                                    --p;
                                }

                                if (sigStart == 0 && bodyBraceOpen > 1000)
                                    sigStart = qMax(0, bodyBraceOpen - 512);
                            }

                            while (sigStart < bodyBraceOpen &&
                                   (blob[sigStart] == ' '  || blob[sigStart] == '\t' ||
                                    blob[sigStart] == '\n' || blob[sigStart] == '\r'))
                                ++sigStart;

                            blob = blob.mid(sigStart);

                            const int realBlobStart = (int)bodyStart + sigStart;
                            const int realBlobEnd   = (int)bodyEnd;

                            for (const BodyRef& ref : std::as_const(unique)) {
                                int relOffset = ref.offset - realBlobStart;
                                if (relOffset < 0 || ref.length <= 0 ||
                                    relOffset + ref.length > blob.size())
                                    continue;

                                int tempId = td->batch->localId(ref.usr);
                                if (tempId < 0) continue;

                                QByteArray marker(5, '\0');
                                marker[0] = 0x01;
                                marker[1] = (char)( tempId        & 0xFF);
                                marker[2] = (char)((tempId >>  8) & 0xFF);
                                marker[3] = (char)((tempId >> 16) & 0xFF);
                                marker[4] = (char)((tempId >> 24) & 0xFF);
                                blob.replace(relOffset, ref.length, marker);
                            }

                            td->batch->addBody(usr, realBlobStart, realBlobEnd, blob);
                            return CXChildVisit_Continue;

                        }, &td);
}

// Сбор friend деклараций
static void collectFriends(CXTranslationUnit tu,
                           const QString& projectRoot,
                           const QStringList& buildDirFilters,
                           EntityBatch& batch)
{
    CXCursor tuCursor = clang_getTranslationUnitCursor(tu);

    struct FriendData {
        const QString*     projectRoot;
        const QStringList* buildDirFilters;
        EntityBatch*       batch;
    };
    FriendData fd { &projectRoot, &buildDirFilters, &batch };

    clang_visitChildren(tuCursor,
                        [](CXCursor cursor, CXCursor, CXClientData cd) -> CXChildVisitResult
                        {
                            auto *fd = static_cast<FriendData*>(cd);

                            if (clang_getCursorKind(cursor) != CXCursor_FriendDecl)
                                return CXChildVisit_Recurse;

                            CXSourceLocation loc = clang_getCursorLocation(cursor);
                            if (clang_Location_isInSystemHeader(loc))
                                return CXChildVisit_Recurse;

                            CXFile file;
                            clang_getSpellingLocation(loc, &file, nullptr, nullptr, nullptr);
                            if (!file) return CXChildVisit_Recurse;

                            CXString fn = clang_getFileName(file);
                            QString path = QString::fromLocal8Bit(clang_getCString(fn))
                                               .replace("\\", "/").toLower();
                            clang_disposeString(fn);

                            if (!path.startsWith(*fd->projectRoot)) return CXChildVisit_Recurse;
                            for (const QString& f : std::as_const(*fd->buildDirFilters))
                                if (path.contains(f)) return CXChildVisit_Recurse;

                            CXCursor ownerCursor = clang_getCursorSemanticParent(cursor);
                            if (clang_Cursor_isNull(ownerCursor)) return CXChildVisit_Recurse;

                            CXString ownerUsr = clang_getCursorUSR(ownerCursor);
                            QString ownerUsrStr = QString::fromUtf8(clang_getCString(ownerUsr));
                            clang_disposeString(ownerUsr);
                            if (ownerUsrStr.isEmpty()) return CXChildVisit_Recurse;

                            CXCursor referenced = clang_getCursorReferenced(cursor);
                            if (clang_Cursor_isNull(referenced)) return CXChildVisit_Recurse;

                            CXString refUsr  = clang_getCursorUSR(referenced);
                            CXString refName = clang_getCursorSpelling(referenced);
                            QString refUsrStr  = QString::fromUtf8(clang_getCString(refUsr));
                            QString refNameStr = QString::fromUtf8(clang_getCString(refName));
                            clang_disposeString(refUsr);
                            clang_disposeString(refName);

                            if (refUsrStr.isEmpty()) return CXChildVisit_Recurse;

                            if (refUsrStr == ownerUsrStr) return CXChildVisit_Recurse;

                            ensureExternalInBatch(*fd->batch, ownerUsrStr, {}, ownerCursor);
                            ensureExternalInBatch(*fd->batch, refUsrStr, refNameStr, referenced);

                            fd->batch->addRelation(ownerUsrStr, refUsrStr, Rel_Friend);

                            return CXChildVisit_Recurse;
                        }, &fd);
}

static bool shouldSkipFile(const QString& fileName,
                           const QVector<QRegularExpression>& compiledPatterns)
{
    QString base = QFileInfo(fileName).fileName();

    for (const QRegularExpression& re : compiledPatterns) {
        if (re.match(base).hasMatch())
            return true;
    }
    return false;
}

// Предкомпиляция паттернов пропуска файлов — O(1) на проверку вместо O(patterns)
static QVector<QRegularExpression> compileSkipPatterns(const QStringList& patterns)
{
    QVector<QRegularExpression> result;
    result.reserve(patterns.size());
    for (const QString& pat : patterns) {
        QString regex = QRegularExpression::wildcardToRegularExpression(
            pat, QRegularExpression::UnanchoredWildcardConversion);
        result.append(QRegularExpression(regex, QRegularExpression::CaseInsensitiveOption));
    }
    return result;
}

static QString cxErrorCodeStr(int code)
{
    switch (code) {
    case CXError_Success:          return "Success";
    case CXError_Failure:          return "Failure";
    case CXError_Crashed:          return "Crashed";
    case CXError_InvalidArguments: return "InvalidArguments";
    case CXError_ASTReadError:     return "ASTReadError";
    default:                       return QString("Unknown(%1)").arg(code);
    }
}

// ── indexProject ──────────────────────────────────────────────────────────

void CodeIndexer::indexProject(const QString& jsonPath,
                               const QString& projectRoot)
{
    m_projectRoot = QString(projectRoot).replace("\\", "/").toLower();
    if (!m_projectRoot.endsWith("/")) m_projectRoot += "/";

    if (m_projectRoot == "/") {
        qWarning() << ">>> Корень проекта не определён — индексация невозможна";
        return;
    }

    qDebug() << ">>> Корень проекта:" << m_projectRoot;
    qDebug() << ">>> Паттернов пропуска файлов:" << m_config.skipFilePatterns.size();
    qDebug() << ">>> Паттернов пропуска директорий:" << m_config.skipDirPatterns.size();

    // Предкомпилируем паттерны пропуска файлов один раз
    QVector<QRegularExpression> compiledSkipPatterns =
        compileSkipPatterns(m_config.skipFilePatterns);

    QString jsonDir = QFileInfo(jsonPath).absolutePath();

    CXCompilationDatabase_Error dbError;
    CXCompilationDatabase db = clang_CompilationDatabase_fromDirectory(
        jsonDir.toUtf8().data(), &dbError);

    if (dbError != CXCompilationDatabase_NoError) {
        qCritical() << "Не удалось открыть CompilationDatabase:" << dbError;
        return;
    }

    CXCompileCommands cmds =
        clang_CompilationDatabase_getAllCompileCommands(db);
    unsigned numCmds = clang_CompileCommands_getSize(cmds);

    struct FileCmd {
        QString              fileName;
        QVector<QByteArray>  argBuffers;
        QVector<const char*> args;
    };
    QVector<FileCmd> fileCmds;
    fileCmds.reserve((int)numCmds);

    static const QStringList srcExts = {"cpp", "c", "cc", "cxx", "c++"};

    for (unsigned i = 0; i < numCmds; ++i) {
        CXCompileCommand cmd = clang_CompileCommands_getCommand(cmds, i);
        CXString file = clang_CompileCommand_getFilename(cmd);
        QString fileName = clang_getCString(file);
        clang_disposeString(file);

        if (shouldSkipFile(fileName, compiledSkipPatterns)) continue;
        if (!QFile::exists(fileName)) continue;

        FileCmd fc;
        fc.fileName = fileName;

        unsigned numArgs = clang_CompileCommand_getNumArgs(cmd);
        unsigned lastArg = numArgs > 0 ? numArgs - 1 : 0;

        bool isCFile = QFileInfo(fileName).suffix().toLower() == "c";

        bool hasDriverMode = false;
        for (unsigned j = 0; j < numArgs; ++j) {
            CXString arg = clang_CompileCommand_getArg(cmd, j);
            QString sArg = clang_getCString(arg);
            clang_disposeString(arg);
            if (sArg.startsWith("--driver-mode")) { hasDriverMode = true; break; }
        }
        if (!hasDriverMode && !isCFile) {
            fc.argBuffers.append("--driver-mode=cl");
            fc.args.append(fc.argBuffers.last().constData());
        }

        for (unsigned j = 1; j < numArgs; ++j) {
            CXString arg = clang_CompileCommand_getArg(cmd, j);
            QString sArg = clang_getCString(arg);
            bool skip =
                sArg.startsWith("@") ||
                (j == lastArg &&
                 srcExts.contains(QFileInfo(sArg).suffix().toLower())) ||
                sArg.startsWith("/Yu") || sArg.startsWith("/Fp")  ||
                sArg.startsWith("/FI") || sArg.startsWith("/FP")  ||
                sArg.startsWith("/RTC") ||
                sArg.startsWith("/Fd") ||
                sArg == "/permissive-" || sArg == "-permissive-";
            if (!skip) {
                fc.argBuffers.append(sArg.toUtf8());
                fc.args.append(fc.argBuffers.last().constData());
            }
            clang_disposeString(arg);
        }

        // Дополнительные include-пути и defines из конфигурации
        for (const QString& inc : std::as_const(m_config.extraIncludePaths)) {
            if (!inc.isEmpty()) {
                fc.argBuffers.append(("-I" + inc).toUtf8());
                fc.args.append(fc.argBuffers.last().constData());
            }
        }
        for (const QString& def : std::as_const(m_config.extraDefines)) {
            if (!def.isEmpty()) {
                fc.argBuffers.append(("-D" + def).toUtf8());
                fc.args.append(fc.argBuffers.last().constData());
            }
        }

        fileCmds.append(std::move(fc));
    }

    clang_CompileCommands_dispose(cmds);
    clang_CompilationDatabase_dispose(db);

    int total = fileCmds.size();
    qDebug() << ">>> Файлов для индексации:" << total;
    qDebug() << ">>> Потоков:" << QThreadPool::globalInstance()->maxThreadCount();

    QMutex flushMutex;
    QMutex errorMutex;

    std::atomic<int> counter{0};
    std::atomic<int> okCount{0};
    std::atomic<int> errFailure{0}, errCrashed{0}, errASTRead{0}, errOther{0};
    QList<ParseError> errorList;
    SuppressedStats   suppressedStats;

    QSemaphore done(0);
    int launched = 0;

    // Копируем projectRoot по значению для безопасности — лямбды могут жить
    // дольше текущего scope (хотя семафор это предотвращает, лучше быть
    // явным чтобы код не сломался при рефакторинге)
    const QString projectRootCopy = m_projectRoot;

    for (int i = 0; i < total; ++i) {
        QString fileName           = fileCmds[i].fileName;
        QVector<QByteArray> argBuffers = fileCmds[i].argBuffers;
        ProgressCallback progressCb    = m_progress;
        QStringList        dirFilters  = m_config.skipDirPatterns;
        QStringList        suppressPat = m_config.suppressDiagPatterns;
        std::atomic<bool>* cancelFlag  = m_cancelFlag;

        auto task = [=, &projectRootCopy, &counter, &done, &flushMutex,
                     &errorMutex, &okCount, &errFailure, &errCrashed,
                     &errASTRead, &errOther, &errorList,
                     &suppressedStats]() mutable
        {
            if (cancelFlag && cancelFlag->load()) {
                done.release();
                return;
            }

            int n = counter.fetch_add(1) + 1;
            QString shortName = QFileInfo(fileName).fileName();
            if (progressCb)
                progressCb(QString("[%1/%2] %3").arg(n).arg(total).arg(shortName),
                           n, total);

            CXIndex localIndex = clang_createIndex(1, 0);

            QVector<const char*> localArgs;
            localArgs.reserve(argBuffers.size());
            for (const QByteArray& b : std::as_const(argBuffers))
                localArgs.append(b.constData());

            QByteArray sourceBytes;
            {
                QFile srcFile(fileName);
                if (srcFile.open(QIODevice::ReadOnly))
                    sourceBytes = srcFile.readAll();
            }

            CXTranslationUnit tu = nullptr;
            CXErrorCode err = clang_parseTranslationUnit2(
                localIndex, fileName.toUtf8().constData(),
                localArgs.data(), localArgs.size(), nullptr, 0,
                CXTranslationUnit_Incomplete |
                    CXTranslationUnit_DetailedPreprocessingRecord,
                &tu);

            if (err == CXError_Success) {
                okCount.fetch_add(1);

                // Проверяем наличие fatal-диагностик даже у успешно распарсенных файлов.
                // Fatal означает что парсер не смог завершить анализ какого-то участка
                // (например не нашёл заголовок) — данные в базе будут неполными.
                QList<ClangDiagnostic> diags;
                QHash<QString, int>    suppCounts;
                bool hasFatal = collectDiagnostics(tu,
                                                   CXDiagnostic_Fatal,
                                                   suppressPat,
                                                   diags,
                                                   suppCounts);

                // Фиксируем подавленные диагностики в общую статистику
                if (!suppCounts.isEmpty()) {
                    QMutexLocker locker(&errorMutex);
                    suppressedStats.merge(suppCounts);
                }

                if (hasFatal) {
                    ParseError pe;
                    pe.fileName = shortName;
                    pe.filePath = fileName;
                    pe.code     = CXError_Success;
                    pe.codeStr  = "OK/partial";

                    for (const ClangDiagnostic& d : std::as_const(diags)) {
                        if (d.severity == CXDiagnostic_Fatal)        ++pe.numFatal;
                        else if (d.severity == CXDiagnostic_Error)   ++pe.numErrors;
                        else if (d.severity == CXDiagnostic_Warning) ++pe.numWarnings;
                    }
                    pe.diagnostics     = diags;
                    pe.suppressedCounts = suppCounts;

                    QMutexLocker locker(&errorMutex);
                    errorList.append(pe);
                }

                EntityBatch batch;

                IndexerCallbacks cb;
                memset(&cb, 0, sizeof(cb));
                cb.indexDeclaration = onDeclaration;

                // abortQuery вызывается libclang периодически во время обхода.
                // Возврат ненулевого значения прерывает clang_indexTranslationUnit
                // немедленно — без ожидания завершения текущего файла целиком.
                cb.abortQuery = [](CXClientData clientData, void*) -> int {
                    auto* dd = static_cast<DeclVisitorData*>(clientData);
                    return (dd->cancelFlag && dd->cancelFlag->load()) ? 1 : 0;
                };

                DeclVisitorData dd;
                dd.projectRoot     = &projectRootCopy;
                dd.buildDirFilters = dirFilters;
                dd.batch           = &batch;
                dd.cancelFlag      = cancelFlag;

                CXIndexAction action = clang_IndexAction_create(localIndex);
                clang_indexTranslationUnit(action, &dd, &cb,
                                           sizeof(cb), CXIndexOpt_None, tu);
                clang_IndexAction_dispose(action);

                // Проверяем флаг отмены после clang_indexTranslationUnit —
                // abortQuery мог прервать обход на середине файла.
                // Пропускаем collect* и flush — частичные данные не нужны.
                if (!cancelFlag || !cancelFlag->load()) {
                    collectMacros(tu, projectRootCopy, dirFilters, batch);
                    collectFriends(tu, projectRootCopy, dirFilters, batch);
                    collectUsings(tu, projectRootCopy, dirFilters, batch);

                    if (!sourceBytes.isEmpty()) {
                        QString currentFile =
                            QString(fileName).replace("\\", "/").toLower();
                        collectBodies(tu, currentFile, sourceBytes, batch);
                    }

                    clang_disposeTranslationUnit(tu);

                    if (!batch.entities.isEmpty() ||
                        !batch.relations.isEmpty() ||
                        !batch.bodies.isEmpty())
                    {
                        QMutexLocker locker(&flushMutex);
                        Storage::instance().flush(batch);
                    }
                } else {
                    clang_disposeTranslationUnit(tu);
                }
            } else {
                // Парсинг полностью провалился — собираем все диагностики
                // начиная с warning, чтобы понять причину
                ParseError pe;
                pe.fileName = shortName;
                pe.filePath = fileName;
                pe.code     = (int)err;
                pe.codeStr  = cxErrorCodeStr((int)err);

                if (tu) {
                    QList<ClangDiagnostic> diags;
                    QHash<QString, int>    suppCounts;
                    collectDiagnostics(tu, CXDiagnostic_Warning,
                                       suppressPat, diags, suppCounts);
                    for (const ClangDiagnostic& d : std::as_const(diags)) {
                        if (d.severity == CXDiagnostic_Fatal)        ++pe.numFatal;
                        else if (d.severity == CXDiagnostic_Error)   ++pe.numErrors;
                        else if (d.severity == CXDiagnostic_Warning) ++pe.numWarnings;
                    }
                    pe.diagnostics      = diags;
                    pe.suppressedCounts = suppCounts;

                    if (!suppCounts.isEmpty()) {
                        QMutexLocker locker(&errorMutex);
                        suppressedStats.merge(suppCounts);
                    }
                    clang_disposeTranslationUnit(tu);
                }

                switch (err) {
                case CXError_Failure:      errFailure.fetch_add(1); break;
                case CXError_Crashed:      errCrashed.fetch_add(1); break;
                case CXError_ASTReadError: errASTRead.fetch_add(1); break;
                default:                   errOther.fetch_add(1);   break;
                }

                qCritical() << "Ошибка парсинга:" << shortName
                            << "код:" << pe.codeStr
                            << "fatal:" << pe.numFatal
                            << "errors:" << pe.numErrors;

                {
                    QMutexLocker locker(&errorMutex);
                    errorList.append(pe);
                }
            }

            clang_disposeIndex(localIndex);
            done.release();
        };

        QThreadPool::globalInstance()->start(
            QRunnable::create(std::move(task)));
        ++launched;
    }

    // Ждём завершения всех запущенных задач.
    // При отмене задачи завершаются быстро (пропускают flush),
    // но всё равно должны дойти до done.release() — ждём их.
    while (launched > 0) {
        // Забираем все семафоры что уже готовы не блокируясь
        while (done.tryAcquire()) {
            --launched;
        }
        if (launched > 0)
            QThread::msleep(50);
    }

    m_stats.filesTotal  = total;
    m_stats.filesOk     = okCount.load();
    m_stats.errFailure  = errFailure.load();
    m_stats.errCrashed  = errCrashed.load();
    m_stats.errASTRead  = errASTRead.load();
    m_stats.errOther    = errOther.load();
    m_stats.errors      = errorList;
    m_stats.suppressed  = suppressedStats;

    qDebug() << ">>> Индексация завершена.";
}