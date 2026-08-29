#include "services/fileoperations.h"
#include "services/giotransferworker.h"
#include "services/xdgtrash.h"
#include <QBuffer>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QThread>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QPixmap>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTemporaryFile>
#include <QUuid>
#include <QUrl>
#include <algorithm>
#include <unistd.h>

#undef signals
#include <gio/gio.h>
#define signals Q_SIGNALS

namespace {

// True when this binary is running inside a Flatpak sandbox. Defined here
// (rather than further down) so it's visible to trash/restore/empty
// helpers above their use site.
bool runningInFlatpak()
{
    static const bool inSandbox = QFile::exists(QStringLiteral("/.flatpak-info"));
    return inSandbox;
}

// Locate a .desktop file from its desktop ID ("mpv.desktop"). Under Flatpak
// the host's applications are only visible through /run/host, so search there
// too — the caller strips the prefix back off before handing the path to the
// host process.
QString desktopEntryPath(const QString &desktopId)
{
    if (desktopId.isEmpty())
        return {};
    if (QFileInfo(desktopId).isAbsolute())
        return QFile::exists(desktopId) ? desktopId : QString();

    QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
    if (runningInFlatpak()) {
        for (const QString &dir : std::as_const(dirs)) {
            if (dir.startsWith(QLatin1Char('/')))
                dirs.append(QStringLiteral("/run/host") + dir);
        }
    }

    for (const QString &dir : std::as_const(dirs)) {
        const QString candidate = QDir(dir).filePath(desktopId);
        if (QFile::exists(candidate))
            return candidate;
    }
    return {};
}

bool isTrashUriPath(const QString &path)
{
    return QUrl(path).scheme() == "trash";
}

// A name typed into New Folder / New File / Rename has to stay inside the
// folder it was typed in: no separators, no "." or "..". The QML dialogs
// validate too, but the filesystem boundary is here.
QString entryNameError(const QString &name)
{
    if (name.isEmpty())
        return QStringLiteral("Name cannot be empty");
    if (name == QLatin1String(".") || name == QLatin1String(".."))
        return QStringLiteral("'%1' is not a valid name").arg(name);
    if (name.contains(QLatin1Char('/')) || name.contains(QChar(0)))
        return QStringLiteral("Names cannot contain '/'");
    return {};
}

bool isUriPath(const QString &path)
{
    const QUrl url(path);
    return url.isValid() && !url.scheme().isEmpty();
}

bool isRemoteUriPath(const QString &path)
{
    const QUrl url(path);
    return url.isValid() && !url.scheme().isEmpty()
        && url.scheme() != QStringLiteral("file")
        && url.scheme() != QStringLiteral("trash");
}

QString remoteAuthority(const QString &uri)
{
    const int schemeSep = uri.indexOf(QStringLiteral("://"));
    if (schemeSep < 0)
        return {};

    const int authorityStart = schemeSep + 3;
    int authorityEnd = uri.size();
    const int pathStart = uri.indexOf(QLatin1Char('/'), authorityStart);
    const int queryStart = uri.indexOf(QLatin1Char('?'), authorityStart);
    const int fragmentStart = uri.indexOf(QLatin1Char('#'), authorityStart);
    for (const int marker : {pathStart, queryStart, fragmentStart}) {
        if (marker >= 0)
            authorityEnd = std::min(authorityEnd, marker);
    }

    return uri.mid(authorityStart, authorityEnd - authorityStart);
}

QString normalizeRemoteUri(const QString &path)
{
    const QUrl url(path);
    if (!url.isValid() || url.scheme().isEmpty())
        return path;

    const QUrl normalizedUrl = url.adjusted(QUrl::NormalizePathSegments);
    QString encodedPath = normalizedUrl.path(QUrl::FullyEncoded);
    if (encodedPath.isEmpty())
        encodedPath = QStringLiteral("/");
    if (encodedPath.size() > 1 && encodedPath.endsWith(QLatin1Char('/')))
        encodedPath.chop(1);

    QString normalized = url.scheme().toLower() + QStringLiteral("://")
        + remoteAuthority(path)
        + encodedPath;

    const QString query = normalizedUrl.query(QUrl::FullyEncoded);
    if (!query.isEmpty())
        normalized += QLatin1Char('?') + query;

    const QString fragment = normalizedUrl.fragment(QUrl::FullyEncoded);
    if (!fragment.isEmpty())
        normalized += QLatin1Char('#') + fragment;

    return normalized;
}

QString normalizeLocation(const QString &path)
{
    if (path.isEmpty())
        return {};

    const QUrl url(path);
    if (url.isValid() && url.scheme() == QStringLiteral("file"))
        return QDir::cleanPath(url.toLocalFile());

    if (url.isValid() && !url.scheme().isEmpty())
        return normalizeRemoteUri(path);

    return QDir::cleanPath(path);
}

QImage clipboardImage(const QClipboard *clipboard)
{
    if (!clipboard)
        return {};

    const QMimeData *mime = clipboard->mimeData();
    if (!mime || !mime->hasImage())
        return {};

    QImage image = clipboard->image();
    if (!image.isNull())
        return image;

    const QVariant imageData = mime->imageData();
    if (imageData.canConvert<QImage>())
        return qvariant_cast<QImage>(imageData);
    if (imageData.canConvert<QPixmap>())
        return qvariant_cast<QPixmap>(imageData).toImage();
    return {};
}

QString gioLocationArg(const QString &path)
{
    const QString normalized = normalizeLocation(path);
    if (isUriPath(normalized))
        return normalized;
    return normalized;
}

QString locationFileName(const QString &path)
{
    const QString normalized = normalizeLocation(path);
    if (isUriPath(normalized)) {
        const QUrl url(normalized);
        QString fileName = QUrl::fromPercentEncoding(url.fileName().toUtf8());
        if (!fileName.isEmpty())
            return fileName;
        const QString authority = remoteAuthority(normalized);
        if (!authority.isEmpty())
            return QUrl::fromPercentEncoding(authority.toUtf8());
        return url.scheme().toUpper();
    }

    if (normalized == QStringLiteral("/"))
        return normalized;

    const QFileInfo info(normalized);
    return info.fileName().isEmpty() ? normalized : info.fileName();
}

void appendUniqueLocation(QStringList *paths, const QString &path)
{
    const QString normalized = normalizeLocation(path);
    if (normalized.isEmpty() || paths->contains(normalized))
        return;

    paths->append(normalized);
}

QStringList uniqueLocations(const QStringList &paths)
{
    QStringList result;
    for (const QString &path : paths)
        appendUniqueLocation(&result, path);
    return result;
}

QString parentLocation(const QString &path)
{
    const QString normalized = normalizeLocation(path);

    if (isTrashUriPath(normalized)) {
        QString current = normalized;
        if (current.size() > 9 && current.endsWith('/'))
            current.chop(1);
        if (current == QStringLiteral("trash://"))
            current = QStringLiteral("trash:///");
        if (current == QStringLiteral("trash:///") || current == QStringLiteral("trash://"))
            return QStringLiteral("trash:///");

        const int slashIndex = current.lastIndexOf('/');
        return slashIndex <= 8 ? QStringLiteral("trash:///") : current.left(slashIndex);
    }

    if (isRemoteUriPath(normalized)) {
        QUrl url(normalized);
        QString urlPath = url.path(QUrl::FullyEncoded);
        const QString base = url.scheme().toLower() + QStringLiteral("://")
            + remoteAuthority(normalized);
        if (urlPath.isEmpty() || urlPath == QStringLiteral("/"))
            return base + QStringLiteral("/");

        if (urlPath.endsWith('/'))
            urlPath.chop(1);

        const int slashIndex = urlPath.lastIndexOf('/');
        return base + (slashIndex <= 0 ? QStringLiteral("/") : urlPath.left(slashIndex));
    }

    const QFileInfo info(normalized);
    return info.absolutePath();
}

QString joinLocation(const QString &parentPath, const QString &name)
{
    const QString normalizedParent = normalizeLocation(parentPath);
    if (isUriPath(normalizedParent)) {
        const QUrl url(normalizedParent);
        QString urlPath = url.path(QUrl::FullyEncoded);
        if (!urlPath.endsWith('/'))
            urlPath += '/';

        QString joined = url.scheme().toLower() + QStringLiteral("://")
            + remoteAuthority(normalizedParent)
            + urlPath
            + QString::fromUtf8(QUrl::toPercentEncoding(name, "/"));

        const QString query = url.query(QUrl::FullyEncoded);
        if (!query.isEmpty())
            joined += QLatin1Char('?') + query;

        const QString fragment = url.fragment(QUrl::FullyEncoded);
        if (!fragment.isEmpty())
            joined += QLatin1Char('#') + fragment;

        return normalizeLocation(joined);
    }

    return QDir(normalizedParent).filePath(name);
}

GFile *gFileForLocation(const QString &path)
{
    const QByteArray utf8 = path.toUtf8();
    if (isUriPath(path))
        return g_file_new_for_uri(utf8.constData());
    return g_file_new_for_path(utf8.constData());
}

bool gioPathExists(const QString &path)
{
    GFile *file = gFileForLocation(path);
    GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                        G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    const bool exists = info != nullptr;
    if (info) g_object_unref(info);
    g_object_unref(file);
    return exists;
}

bool deleteGFileRecursive(GFile *file, QString *error, GCancellable *cancellable = nullptr)
{
    const GFileType type = g_file_query_file_type(
        file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable);
    if (type != G_FILE_TYPE_DIRECTORY) {
        GError *delErr = nullptr;
        const bool ok = g_file_delete(file, cancellable, &delErr);
        if (!ok && error)
            *error = delErr ? QString::fromUtf8(delErr->message)
                            : QStringLiteral("Failed to delete item");
        if (delErr)
            g_error_free(delErr);
        return ok;
    }

    // Some backends own the recursion and refuse per-child deletes: the gvfs
    // trash backend answers "Items in the trash may not be modified" for
    // anything below a top-level entry, so walking into a trashed folder can
    // never empty it. Try the whole-directory delete first; local filesystems
    // just report G_IO_ERROR_NOT_EMPTY and we fall through to the walk.
    {
        GError *directErr = nullptr;
        if (g_file_delete(file, cancellable, &directErr))
            return true;
        if (directErr)
            g_error_free(directErr);
    }

    GError *enumErr = nullptr;
    GFileEnumerator *enumerator = g_file_enumerate_children(
        file,
        G_FILE_ATTRIBUTE_STANDARD_NAME,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
        cancellable,
        &enumErr);
    if (!enumerator) {
        if (error)
            *error = enumErr ? QString::fromUtf8(enumErr->message)
                             : QStringLiteral("Failed to enumerate directory");
        if (enumErr)
            g_error_free(enumErr);
        return false;
    }

    GFileInfo *childInfo = nullptr;
    while ((childInfo = g_file_enumerator_next_file(enumerator, cancellable, nullptr)) != nullptr) {
        GFile *child = g_file_get_child(file, g_file_info_get_name(childInfo));
        g_object_unref(childInfo);

        const bool ok = deleteGFileRecursive(child, error, cancellable);
        g_object_unref(child);
        if (!ok) {
            g_file_enumerator_close(enumerator, nullptr, nullptr);
            g_object_unref(enumerator);
            return false;
        }
    }

    g_file_enumerator_close(enumerator, nullptr, nullptr);
    g_object_unref(enumerator);

    GError *delErr = nullptr;
    const bool ok = g_file_delete(file, cancellable, &delErr);
    if (!ok && error)
        *error = delErr ? QString::fromUtf8(delErr->message)
                        : QStringLiteral("Failed to delete directory");
    if (delErr)
        g_error_free(delErr);
    return ok;
}

QVariantMap remotePathInfo(const QString &path)
{
    QVariantMap result;
    const QString normalized = normalizeLocation(path);

    GFile *file = gFileForLocation(normalized);
    GFileInfo *info = g_file_query_info(file,
        G_FILE_ATTRIBUTE_STANDARD_TYPE ","
        G_FILE_ATTRIBUTE_STANDARD_SIZE ","
        G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);

    if (!info) {
        g_object_unref(file);
        return result;
    }

    result[QStringLiteral("exists")] = true;
    result[QStringLiteral("fileName")] = locationFileName(normalized);
    result[QStringLiteral("path")] = normalized;
    result[QStringLiteral("isDir")] = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    result[QStringLiteral("size")] = static_cast<qint64>(g_file_info_get_size(info));
    result[QStringLiteral("isSymlink")] = g_file_info_has_attribute(
        info, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK)
        ? static_cast<bool>(g_file_info_get_is_symlink(info))
        : false;

    g_object_unref(info);
    g_object_unref(file);
    return result;
}

QVariantMap sourceInfoForPath(const QString &path)
{
    const QString normalized = normalizeLocation(path);
    if (isRemoteUriPath(normalized))
        return remotePathInfo(normalized);

    QVariantMap result;
    QFileInfo info(normalized);
    result[QStringLiteral("exists")] = info.exists();
    result[QStringLiteral("fileName")] = info.fileName();
    result[QStringLiteral("path")] = info.absoluteFilePath();
    result[QStringLiteral("isDir")] = info.isDir();
    result[QStringLiteral("size")] = info.size();
    result[QStringLiteral("isSymlink")] = info.isSymLink();
    return result;
}

bool moveLocationSync(const QString &sourcePath, const QString &targetPath, QString *error = nullptr)
{
    const QString normalizedSource = normalizeLocation(sourcePath);
    const QString normalizedTarget = normalizeLocation(targetPath);

    if (!isUriPath(normalizedSource) && !isUriPath(normalizedTarget)) {
        const bool ok = QFile::rename(normalizedSource, normalizedTarget);
        if (!ok && error)
            *error = QStringLiteral("Could not rename %1").arg(locationFileName(normalizedSource));
        return ok;
    }

    GFile *src = gFileForLocation(normalizedSource);
    GFile *dst = gFileForLocation(normalizedTarget);
    GError *gErr = nullptr;
    const bool ok = g_file_move(src, dst, G_FILE_COPY_NONE, nullptr, nullptr, nullptr, &gErr);
    if (!ok && error)
        *error = gErr ? QString::fromUtf8(gErr->message) : QStringLiteral("Move failed");
    if (gErr) g_error_free(gErr);
    g_object_unref(src);
    g_object_unref(dst);
    return ok;
}

bool makeDirectorySync(const QString &path, QString *error = nullptr)
{
    const QString normalized = normalizeLocation(path);
    if (!isUriPath(normalized)) {
        const bool ok = QDir().mkpath(normalized);
        if (!ok && error)
            *error = QStringLiteral("Could not create folder");
        return ok;
    }

    GFile *file = gFileForLocation(normalized);
    GError *gErr = nullptr;
    const bool ok = g_file_make_directory_with_parents(file, nullptr, &gErr);
    const bool alreadyExists = gErr && g_error_matches(gErr, G_IO_ERROR, G_IO_ERROR_EXISTS);
    if (!ok && !alreadyExists && error)
        *error = gErr ? QString::fromUtf8(gErr->message) : QStringLiteral("Could not create folder");
    if (gErr) g_error_free(gErr);
    g_object_unref(file);
    return ok || alreadyExists;
}

bool createEmptyFileSync(const QString &path, QString *error = nullptr)
{
    const QString normalized = normalizeLocation(path);
    if (!isUriPath(normalized)) {
        QFile file(normalized);
        // NewOnly: O_EXCL semantics. WriteOnly would truncate a file another
        // process created between the UI's existence check and this call.
        const bool ok = file.open(QIODevice::NewOnly);
        file.close();
        if (!ok && error)
            *error = file.exists() ? QStringLiteral("'%1' already exists").arg(locationFileName(normalized))
                                   : QStringLiteral("Could not create file");
        return ok;
    }

    GFile *file = gFileForLocation(normalized);
    GError *gErr = nullptr;
    GFileOutputStream *stream = g_file_create(file, G_FILE_CREATE_NONE, nullptr, &gErr);
    if (stream) {
        g_output_stream_close(G_OUTPUT_STREAM(stream), nullptr, nullptr);
        g_object_unref(stream);
    } else {
        if (error)
            *error = gErr ? QString::fromUtf8(gErr->message) : QStringLiteral("Could not create file");
        if (gErr) g_error_free(gErr);
        g_object_unref(file);
        return false;
    }
    g_object_unref(file);
    return true;
}

bool pathExistsSync(const QString &path)
{
    const QString normalized = normalizeLocation(path);
    if (isUriPath(normalized))
        return gioPathExists(normalized);
    return QFileInfo::exists(normalized);
}

QVariantList buildBreadcrumbs(const QString &path)
{
    QVariantList segments;
    const QString normalized = normalizeLocation(path);

    if (normalized.isEmpty())
        return segments;

    if (isTrashUriPath(normalized)) {
        if (normalized == QStringLiteral("trash:///") || normalized == QStringLiteral("trash://")) {
            segments.append(QVariantMap{{QStringLiteral("label"), QStringLiteral("Trash")},
                                        {QStringLiteral("fullPath"), QStringLiteral("trash:///")}});
            return segments;
        }

        QString current = normalized;
        if (current.endsWith('/'))
            current.chop(1);
        QString remainder = current.mid(QStringLiteral("trash:///").size());
        segments.append(QVariantMap{{QStringLiteral("label"), QStringLiteral("Trash")},
                                    {QStringLiteral("fullPath"), QStringLiteral("trash:///")}});

        QString accumulated = QStringLiteral("trash:///");
        const QStringList parts = remainder.split('/', Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            accumulated = joinLocation(accumulated, QUrl::fromPercentEncoding(part.toUtf8()));
            segments.append(QVariantMap{{QStringLiteral("label"), QUrl::fromPercentEncoding(part.toUtf8())},
                                        {QStringLiteral("fullPath"), accumulated}});
        }
        return segments;
    }

    if (isRemoteUriPath(normalized)) {
        const QUrl url(normalized);
        const QString authority = !remoteAuthority(normalized).isEmpty()
            ? QUrl::fromPercentEncoding(remoteAuthority(normalized).toUtf8())
            : url.scheme().toUpper();
        const QString rootPath = url.scheme().toLower() + QStringLiteral("://")
            + remoteAuthority(normalized) + QStringLiteral("/");
        segments.append(QVariantMap{{QStringLiteral("label"), authority},
                                    {QStringLiteral("fullPath"), rootPath}});

        QString accumulatedPath;
        const QStringList parts = url.path(QUrl::FullyEncoded).split('/', Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            accumulatedPath += QStringLiteral("/") + part;
            segments.append(QVariantMap{{QStringLiteral("label"), QUrl::fromPercentEncoding(part.toUtf8())},
                                        {QStringLiteral("fullPath"), rootPath.left(rootPath.size() - 1) + accumulatedPath}});
        }
        return segments;
    }

    if (normalized == QStringLiteral("/"))
        return segments;

    QString accumulated;
    QStringList parts = normalized.split('/', Qt::SkipEmptyParts);

    // Anything inside the user's home starts at a single "Home" crumb. Walking
    // the real ancestors instead would render "/home" and "/home/<user>" side
    // by side, which reads as "home / Home".
    const QString home = QDir::homePath();
    if (!home.isEmpty() && home != QStringLiteral("/")
        && (normalized == home || normalized.startsWith(home + QLatin1Char('/')))) {
        segments.append(QVariantMap{{QStringLiteral("label"), QStringLiteral("Home")},
                                    {QStringLiteral("fullPath"), home}});
        accumulated = home;
        parts = normalized.mid(home.size()).split('/', Qt::SkipEmptyParts);
    }

    for (const QString &part : parts) {
        accumulated += QStringLiteral("/") + part;
        segments.append(QVariantMap{{QStringLiteral("label"), part},
                                    {QStringLiteral("fullPath"), accumulated}});
    }

    return segments;
}

QString currentUidString()
{
    return QString::number(geteuid());
}

QString trashUriRootPath()
{
    return QStringLiteral("trash:///");
}

QString homeTrashFilesPath()
{
    return XdgTrash::homeRoot() + "/files";
}

QString existingLookupPathFor(const QString &path)
{
    QString candidate = QDir::cleanPath(path);
    if (candidate.isEmpty())
        return QDir::homePath();

    const QFileInfo info(candidate);
    candidate = info.isDir() ? info.absoluteFilePath() : info.absolutePath();

    while (!candidate.isEmpty() && !QFileInfo::exists(candidate)) {
        const QString parent = QFileInfo(candidate).absolutePath();
        if (parent == candidate)
            break;
        candidate = parent;
    }

    return QFileInfo::exists(candidate) ? candidate : QDir::homePath();
}

QStringList trashRootCandidatesForPath(const QString &path)
{
    QStringList roots;

    const QString cleanPath = QDir::cleanPath(path);
    if (!cleanPath.isEmpty()) {
        const QString lookupPath = existingLookupPathFor(cleanPath);
        const QString storageRoot = QDir::cleanPath(QStorageInfo(lookupPath).rootPath());
        if (!storageRoot.isEmpty() && storageRoot != "/") {
            const QString uid = currentUidString();
            roots.append(QDir(storageRoot).filePath(".Trash-" + uid));
            roots.append(QDir(storageRoot).filePath(".Trash/" + uid));
        }
    }

    roots.append(XdgTrash::homeRoot());
    roots.removeDuplicates();
    return roots;
}

QString matchingTrashFilesRoot(const QString &path)
{
    if (isTrashUriPath(path))
        return trashUriRootPath();

    const QString cleanPath = QDir::cleanPath(path);
    if (cleanPath.isEmpty())
        return {};

    for (const QString &trashRoot : trashRootCandidatesForPath(cleanPath)) {
        const QString filesRoot = QDir(trashRoot).filePath("files");
        if (cleanPath == filesRoot || cleanPath.startsWith(filesRoot + "/"))
            return filesRoot;
    }

    return {};
}

// Resolves whatever the UI hands us to a real path under <trash>/files. The
// trash view uses those directly now; a trash:// URI from a saved session or
// an older caller is matched by name against every trash root. Returns empty
// for anything not inside a trash directory, so callers can't be tricked into
// deleting outside one.
QString localTrashPathFor(const QString &path)
{
    const QString normalized = QDir::cleanPath(path);
    if (!isTrashUriPath(normalized))
        return matchingTrashFilesRoot(normalized).isEmpty() ? QString() : normalized;

    QString relative = QUrl(normalized).path();
    while (relative.startsWith(QLatin1Char('/')))
        relative.remove(0, 1);
    if (relative.isEmpty())
        return {};

    const QStringList roots = XdgTrash::roots();
    for (const QString &root : roots) {
        const QString filesRoot = QDir(root).filePath(QStringLiteral("files"));
        const QString candidate = QDir::cleanPath(filesRoot + QLatin1Char('/') + relative);
        // cleanPath resolves ".." segments, so re-check containment before
        // handing the path to anything that deletes recursively.
        if (candidate != filesRoot && !candidate.startsWith(filesRoot + QLatin1Char('/')))
            continue;
        if (QFileInfo::exists(candidate))
            return candidate;
    }
    return {};
}

enum class ArchiveKind {
    None,
    Zip,
    Tar,
    TarGz,
    TarXz,
    TarBz2,
    Gz,
    Xz,
    Bz2,
    SevenZip,
    Rar,
};

ArchiveKind archiveKindForPath(const QString &path)
{
    const QString lower = path.toLower();
    if (lower.endsWith(QStringLiteral(".tar.gz")) || lower.endsWith(QStringLiteral(".tgz")))
        return ArchiveKind::TarGz;
    if (lower.endsWith(QStringLiteral(".tar.xz")) || lower.endsWith(QStringLiteral(".txz")))
        return ArchiveKind::TarXz;
    if (lower.endsWith(QStringLiteral(".tar.bz2")) || lower.endsWith(QStringLiteral(".tbz2")))
        return ArchiveKind::TarBz2;
    if (lower.endsWith(QStringLiteral(".tar")))
        return ArchiveKind::Tar;
    if (lower.endsWith(QStringLiteral(".zip")))
        return ArchiveKind::Zip;
    if (lower.endsWith(QStringLiteral(".7z")))
        return ArchiveKind::SevenZip;
    if (lower.endsWith(QStringLiteral(".rar")))
        return ArchiveKind::Rar;
    if (lower.endsWith(QStringLiteral(".gz")))
        return ArchiveKind::Gz;
    if (lower.endsWith(QStringLiteral(".xz")))
        return ArchiveKind::Xz;
    if (lower.endsWith(QStringLiteral(".bz2")))
        return ArchiveKind::Bz2;
    return ArchiveKind::None;
}

bool archiveExtractCommand(const QString &archivePath, const QString &destination,
                           QString *program, QStringList *args)
{
    switch (archiveKindForPath(archivePath)) {
    case ArchiveKind::Zip:
        *program = QStringLiteral("unzip");
        *args = {QStringLiteral("-o"), archivePath, QStringLiteral("-d"), destination};
        return true;
    case ArchiveKind::TarGz:
        *program = QStringLiteral("tar");
        *args = {QStringLiteral("-xzf"), archivePath, QStringLiteral("-C"), destination};
        return true;
    case ArchiveKind::TarXz:
        *program = QStringLiteral("tar");
        *args = {QStringLiteral("-xJf"), archivePath, QStringLiteral("-C"), destination};
        return true;
    case ArchiveKind::TarBz2:
        *program = QStringLiteral("tar");
        *args = {QStringLiteral("-xjf"), archivePath, QStringLiteral("-C"), destination};
        return true;
    case ArchiveKind::Tar:
        *program = QStringLiteral("tar");
        *args = {QStringLiteral("-xf"), archivePath, QStringLiteral("-C"), destination};
        return true;
    case ArchiveKind::Gz:
        *program = QStringLiteral("gunzip");
        *args = {QStringLiteral("-k"), archivePath};
        return true;
    case ArchiveKind::Xz:
        *program = QStringLiteral("unxz");
        *args = {QStringLiteral("-k"), archivePath};
        return true;
    case ArchiveKind::Bz2:
        *program = QStringLiteral("bunzip2");
        *args = {QStringLiteral("-k"), archivePath};
        return true;
    case ArchiveKind::SevenZip:
    case ArchiveKind::Rar:
        if (!QStandardPaths::findExecutable(QStringLiteral("7z")).isEmpty()) {
            *program = QStringLiteral("7z");
            *args = {QStringLiteral("x"), QStringLiteral("-aoa"),
                     QStringLiteral("-o%1").arg(destination), archivePath};
            return true;
        }
        if (!QStandardPaths::findExecutable(QStringLiteral("bsdtar")).isEmpty()) {
            *program = QStringLiteral("bsdtar");
            *args = {QStringLiteral("-xf"), archivePath, QStringLiteral("-C"), destination};
            return true;
        }
        return false;
    case ArchiveKind::None:
        return false;
    }

    return false;
}

bool archiveListCommand(const QString &archivePath, QString *program, QStringList *args)
{
    switch (archiveKindForPath(archivePath)) {
    case ArchiveKind::Zip:
        *program = QStringLiteral("unzip");
        *args = {QStringLiteral("-Z1"), archivePath};
        return true;
    case ArchiveKind::TarGz:
        *program = QStringLiteral("tar");
        *args = {QStringLiteral("-tzf"), archivePath};
        return true;
    case ArchiveKind::TarXz:
        *program = QStringLiteral("tar");
        *args = {QStringLiteral("-tJf"), archivePath};
        return true;
    case ArchiveKind::TarBz2:
        *program = QStringLiteral("tar");
        *args = {QStringLiteral("-tjf"), archivePath};
        return true;
    case ArchiveKind::Tar:
        *program = QStringLiteral("tar");
        *args = {QStringLiteral("-tf"), archivePath};
        return true;
    case ArchiveKind::SevenZip:
    case ArchiveKind::Rar:
        if (!QStandardPaths::findExecutable(QStringLiteral("bsdtar")).isEmpty()) {
            *program = QStringLiteral("bsdtar");
            *args = {QStringLiteral("-tf"), archivePath};
            return true;
        }
        if (!QStandardPaths::findExecutable(QStringLiteral("7z")).isEmpty()) {
            *program = QStringLiteral("7z");
            *args = {QStringLiteral("l"), QStringLiteral("-ba"), QStringLiteral("-slt"), archivePath};
            return true;
        }
        return false;
    case ArchiveKind::Gz:
    case ArchiveKind::Xz:
    case ArchiveKind::Bz2:
    case ArchiveKind::None:
        return false;
    }

    return false;
}

QStringList archiveEntriesFromOutput(const QString &program, const QString &output)
{
    QStringList entries;

    if (program == QStringLiteral("7z")) {
        // "-slt" prints an archive-info block ("Path = <archive>") followed
        // by a "----------" line and then the entries. "-ba" drops that
        // header block, separator included, so only wait for the separator
        // when there is one; otherwise every "Path =" line is an entry.
        const QStringList lines = output.split('\n');
        bool inEntries = !lines.contains(QStringLiteral("----------"));
        for (const QString &line : lines) {
            const QString trimmed = line.trimmed();
            if (trimmed == QStringLiteral("----------")) {
                inEntries = true;
                continue;
            }
            if (!inEntries || !trimmed.startsWith(QStringLiteral("Path = ")))
                continue;

            const QString entry = QDir::cleanPath(trimmed.mid(7).trimmed());
            if (!entry.isEmpty() && entry != QStringLiteral("."))
                entries.append(entry);
        }
        return entries;
    }

    const QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        QString entry = line.trimmed();
        if (entry.startsWith(QStringLiteral("./")))
            entry.remove(0, 2);
        entry = QDir::cleanPath(entry);
        if (!entry.isEmpty() && entry != QStringLiteral("."))
            entries.append(entry);
    }

    return entries;
}

QString commonArchiveRootFolder(const QStringList &entries)
{
    QString root;
    for (QString entry : entries) {
        while (entry.startsWith('/'))
            entry.remove(0, 1);
        if (entry.isEmpty())
            continue;

        const QString top = entry.section('/', 0, 0);
        if (top.isEmpty())
            return {};

        if (root.isEmpty())
            root = top;
        else if (top != root)
            return {};
    }

    return root;
}

QStringList nameParts(const QString &name)
{
    const int dotIndex = name.lastIndexOf('.');
    if (dotIndex > 0)
        return {name.left(dotIndex), name.mid(dotIndex)};
    return {name, QString()};
}

struct RenameOperation {
    QString sourcePath;
    QString targetPath;
    QString tempPath;
};

QVariantMap renameResult(bool success, const QString &error = {}, const QStringList &changedPaths = {})
{
    QVariantMap result;
    result["success"] = success;
    result["error"] = error;
    result["changedPaths"] = changedPaths;
    return result;
}

QString temporaryRenamePathFor(const QString &sourcePath)
{
    QString tempPath;
    do {
        tempPath = joinLocation(parentLocation(sourcePath),
                                QStringLiteral(".hyprfm-rename-%1.tmp")
                                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    } while (pathExistsSync(tempPath));

    return tempPath;
}

QString renameTargetError(const QString &targetPath)
{
    const QString fileName = locationFileName(targetPath);
    if (fileName.isEmpty() || fileName == "." || fileName == "..")
        return QStringLiteral("Enter a valid target name");

    const QString parentDir = parentLocation(targetPath);
    if (parentDir.isEmpty() || !pathExistsSync(parentDir))
        return QStringLiteral("Target folder does not exist");

    return {};
}

}

FileOperations::FileOperations(QObject *parent)
    : QObject(parent)
{
}

bool FileOperations::busy() const { return m_busy; }
double FileOperations::progress() const { return m_progress; }
QString FileOperations::statusText() const { return m_statusText; }
QString FileOperations::speed() const { return m_speed; }
QString FileOperations::eta() const { return m_eta; }
bool FileOperations::paused() const { return m_paused; }
QString FileOperations::currentFile() const { return m_currentFile; }

QVariantList FileOperations::activeTransfers() const
{
    QVariantList list;
    for (const auto &t : m_activeTransfers) {
        QVariantMap map;
        map["id"] = t.id;
        map["statusText"] = t.statusText;
        map["progress"] = t.progress;
        map["speed"] = t.speed;
        map["eta"] = t.eta;
        map["currentFile"] = t.currentFile;
        map["paused"] = t.paused;
        list.append(map);
    }
    return list;
}

QStringList FileOperations::pendingTargetPaths() const
{
    QStringList paths;
    for (const auto &transfer : m_activeTransfers) {
        for (const QString &path : transfer.targetPaths) {
            if (!path.isEmpty() && !paths.contains(path))
                paths.append(path);
        }
    }
    return paths;
}

FileOperations::ActiveTransfer *FileOperations::findTransfer(int id)
{
    for (auto &t : m_activeTransfers) {
        if (t.id == id)
            return &t;
    }
    return nullptr;
}

void FileOperations::emitAggregatedState()
{
    const bool wasBusy = m_busy;
    m_busy = !m_activeTransfers.isEmpty();

    if (m_activeTransfers.isEmpty()) {
        // Don't reset m_progress — leave at 1.0 after completion for UI linger
        m_statusText.clear();
        m_speed.clear();
        m_eta.clear();
        m_currentFile.clear();
        m_paused = false;
    } else if (m_activeTransfers.size() == 1) {
        const auto &t = m_activeTransfers.first();
        m_progress = t.progress;
        m_statusText = t.statusText;
        m_speed = t.speed;
        m_eta = t.eta;
        m_currentFile = t.currentFile;
        m_paused = t.paused;
    } else {
        // Multiple transfers: show aggregate
        m_statusText = QString("%1 transfers active").arg(m_activeTransfers.size());
        m_paused = std::all_of(m_activeTransfers.begin(), m_activeTransfers.end(),
                               [](const ActiveTransfer &t) { return t.paused; });

        // Aggregate progress as average
        double totalProgress = 0;
        int countWithProgress = 0;
        for (const auto &t : m_activeTransfers) {
            if (t.progress >= 0) {
                totalProgress += t.progress;
                ++countWithProgress;
            }
        }
        m_progress = countWithProgress > 0 ? totalProgress / countWithProgress : -1.0;
        m_speed.clear();
        m_eta.clear();
        m_currentFile.clear();
    }

    if (wasBusy != m_busy) emit busyChanged();
    emit progressChanged();
    emit statusTextChanged();
    emit speedChanged();
    emit etaChanged();
    emit pausedChanged();
    emit currentFileChanged();
    emit activeTransfersChanged();
}

int FileOperations::copyFiles(const QStringList &sources, const QString &destination)
{
    return transferResolvedItems(transferPlan(sources, destination), false);
}

int FileOperations::copyResolvedItems(const QVariantList &operations)
{
    return transferResolvedItems(operations, false);
}

int FileOperations::moveFiles(const QStringList &sources, const QString &destination)
{
    return transferResolvedItems(transferPlan(sources, destination), true);
}

int FileOperations::moveResolvedItems(const QVariantList &operations)
{
    return transferResolvedItems(operations, true);
}

int FileOperations::trashFiles(const QStringList &paths)
{
    return startSimpleOperation(
        QString("Trashing %1 item(s)...").arg(paths.size()), paths,
        [paths](ProgressReporter report) -> QString {
            QString lastError;
            const int total = paths.size();
            // Inside a Flatpak, GLib's g_file_trash() puts files in the
            // *sandbox's* trash (~/.var/app/<app-id>/data/Trash) because
            // XDG_DATA_HOME is overridden. Shell out to host gio so files
            // land in the user's real ~/.local/share/Trash.
            const bool inFlatpak = runningInFlatpak();
            for (int i = 0; i < total; ++i) {
                const QString normalized = normalizeLocation(paths[i]);
                report(i, total, locationFileName(normalized));

                if (inFlatpak) {
                    QProcess proc;
                    proc.start(QStringLiteral("flatpak-spawn"),
                               {QStringLiteral("--host"), QStringLiteral("gio"),
                                QStringLiteral("trash"), normalized});
                    proc.waitForFinished(10000);
                    if (proc.exitCode() != 0) {
                        const QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
                        if (!err.isEmpty())
                            lastError = err;
                    }
                    continue;
                }

                GFile *file = gFileForLocation(normalized);
                GError *gErr = nullptr;
                if (!g_file_trash(file, nullptr, &gErr)) {
                    if (gErr) {
                        lastError = QString::fromUtf8(gErr->message);
                        g_error_free(gErr);
                    }
                }
                g_object_unref(file);
            }
            return lastError;
        });
}

int FileOperations::restoreFromTrash(const QStringList &paths)
{
    return startSimpleOperation(
        QString("Restoring %1 item(s)...").arg(paths.size()), paths,
        [paths](ProgressReporter report) -> QString {
            QString lastError;
            const int total = paths.size();
            // Restore reads the item's own .trashinfo instead of asking gvfs
            // for trash::orig-path, so it works with no session daemon. That
            // also fixes Flatpak, which used to need a host `gio` hop because
            // the sandbox's XDG_DATA_HOME pointed gvfs at an empty trash.
            for (int i = 0; i < total; ++i) {
                const QString localPath = localTrashPathFor(paths[i]);
                if (localPath.isEmpty())
                    continue;

                report(i, total, locationFileName(paths[i]));

                const XdgTrash::Entry entry = XdgTrash::readEntry(localPath);
                if (entry.originalPath.isEmpty()) {
                    lastError = QStringLiteral("Could not determine where %1 came from")
                                    .arg(locationFileName(localPath));
                    continue;
                }

                // Never clobber whatever now sits at the original location:
                // the user has no way to get it back if we do.
                if (QFileInfo::exists(entry.originalPath)) {
                    lastError = QStringLiteral("%1 already exists at the original location")
                                    .arg(locationFileName(entry.originalPath));
                    continue;
                }

                const QString parent = QFileInfo(entry.originalPath).absolutePath();
                if (!parent.isEmpty() && !QDir().mkpath(parent)) {
                    lastError = QStringLiteral("Could not recreate %1").arg(parent);
                    continue;
                }

                // ponytail: rename only. The spec keeps an item on the volume
                // it was deleted from, so this is always a same-filesystem
                // move; add a copy+delete fallback if a cross-device trash
                // ever turns up in the wild.
                if (!QDir().rename(localPath, entry.originalPath)) {
                    lastError = QStringLiteral("Could not restore %1")
                                    .arg(locationFileName(localPath));
                    continue;
                }

                XdgTrash::removeInfo(localPath);
            }
            return lastError;
        });
}

bool FileOperations::isTrashPath(const QString &path) const
{
    const QString normalized = normalizeLocation(path);
    if (isTrashUriPath(normalized))
        return true;
    if (isRemoteUriPath(normalized))
        return false;
    return !matchingTrashFilesRoot(normalized).isEmpty();
}

QString FileOperations::trashFilesPathFor(const QString &path) const
{
    const QString normalized = normalizeLocation(path);
    if (isTrashUriPath(normalized))
        return trashUriRootPath();
    if (isRemoteUriPath(normalized))
        return homeTrashFilesPath();

    const QString matchedRoot = matchingTrashFilesRoot(normalized);
    if (!matchedRoot.isEmpty())
        return matchedRoot;

    const QStringList roots = trashRootCandidatesForPath(normalized);
    if (!roots.isEmpty())
        return QDir(roots.first()).filePath("files");

    return homeTrashFilesPath();
}

QVariantList FileOperations::transferPlan(const QStringList &sources, const QString &destination) const
{
    QVariantList plan;
    const QString normalizedDestination = normalizeLocation(destination);

    for (const QString &sourcePath : sources) {
        const QVariantMap sourceInfo = sourceInfoForPath(sourcePath);
        if (!sourceInfo.value(QStringLiteral("exists")).toBool())
            continue;

        const QString sourceName = sourceInfo.value(QStringLiteral("fileName")).toString();
        const QString normalizedSourcePath = sourceInfo.value(QStringLiteral("path")).toString();
        const QString targetPath = normalizeLocation(joinLocation(normalizedDestination, sourceName));

        QVariantMap item;
        item["sourcePath"] = normalizedSourcePath;
        item["sourceName"] = sourceName;
        item["targetPath"] = targetPath;
        item["targetName"] = sourceName;
        item["targetExists"] = pathExistsSync(targetPath);
        item["samePath"] = (normalizedSourcePath == targetPath);
        item["isDir"] = sourceInfo.value(QStringLiteral("isDir")).toBool();
        plan.append(item);
    }

    return plan;
}

QString FileOperations::uniqueNameForDestination(const QString &destinationDir, const QString &desiredName,
                                                 const QStringList &blockedNames) const
{
    if (desiredName.isEmpty())
        return {};

    const auto parts = nameParts(desiredName);
    const QString stem = parts.at(0);
    const QString suffix = parts.at(1);
    const QString normalizedDestination = normalizeLocation(destinationDir);

    auto isBlocked = [&](const QString &candidate) {
        return blockedNames.contains(candidate) || pathExistsSync(joinLocation(normalizedDestination, candidate));
    };

    if (!isBlocked(desiredName))
        return desiredName;

    const QString copyStem = stem + " (copy)";
    const QString firstCandidate = copyStem + suffix;
    if (!isBlocked(firstCandidate))
        return firstCandidate;

    for (int i = 2; i < 10000; ++i) {
        const QString candidate = QString("%1 (copy %2)%3").arg(stem).arg(i).arg(suffix);
        if (!isBlocked(candidate))
            return candidate;
    }

    return {};
}

int FileOperations::deleteFiles(const QStringList &paths)
{
    return startSimpleOperation(
        QString("Deleting %1 item(s)...").arg(paths.size()), paths,
        [paths](ProgressReporter report) -> QString {
            QString lastError;
            const int total = paths.size();
            for (int i = 0; i < total; ++i) {
                QString normalized = normalizeLocation(paths[i]);

                // Deleting from the trash goes through the filesystem, not
                // gvfs: the trash backend answers g_file_delete() on anything
                // inside a trashed folder with "Items in the trash may not be
                // modified", which is what made non-empty folders unremovable.
                if (isTrashUriPath(normalized)) {
                    const QString localPath = localTrashPathFor(normalized);
                    if (localPath.isEmpty()) {
                        lastError = QStringLiteral("Could not locate %1 in the trash")
                                        .arg(locationFileName(normalized));
                        continue;
                    }
                    normalized = localPath;
                }

                report(i, total, locationFileName(normalized));

                if (isUriPath(normalized)) {
                    GFile *file = gFileForLocation(normalized);
                    QString err;
                    if (!deleteGFileRecursive(file, &err) && !err.isEmpty())
                        lastError = err;
                    g_object_unref(file);
                } else {
                    QFileInfo info(normalized);
                    // A symlink to a directory must be unlinked, never
                    // descended: isDir() follows the link and
                    // removeRecursively() would empty the target.
                    bool removed = false;
                    if (info.isDir() && !info.isSymLink())
                        removed = QDir(normalized).removeRecursively();
                    else
                        removed = QFile::remove(normalized);

                    if (!removed)
                        lastError = QStringLiteral("Failed to delete one or more items");
                    else
                        // No-op unless this was a top-level trash entry;
                        // otherwise the trash accumulates metadata for files
                        // that no longer exist.
                        XdgTrash::removeInfo(normalized);
                }
            }
            return lastError;
        });
}

int FileOperations::transferResolvedItems(const QVariantList &operations, bool moveOperation)
{
    if (operations.isEmpty()) {
        emit operationFinished(true, QString());
        return -1;
    }

    QVariantList preparedOperations;

    for (const QVariant &variant : operations) {
        QVariantMap item = variant.toMap();
        const QString sourcePath = normalizeLocation(item.value("sourcePath").toString());
        const QString targetPath = normalizeLocation(item.value("targetPath").toString());
        const QString backupPath = normalizeLocation(item.value("backupPath").toString());
        const bool overwrite = item.value("overwrite").toBool();

        if (sourcePath.isEmpty() || targetPath.isEmpty()) {
            emit operationFinished(false, "Transfer operation is missing a source or destination path");
            return -1;
        }

        if (sourcePath == targetPath) {
            emit operationFinished(false, QString("Source and destination are the same for %1").arg(locationFileName(sourcePath)));
            return -1;
        }

        if (targetPath.startsWith(sourcePath + QLatin1Char('/'))) {
            emit operationFinished(false, QString("Cannot copy %1 into itself").arg(locationFileName(sourcePath)));
            return -1;
        }

        item["sourcePath"] = sourcePath;
        item["targetPath"] = targetPath;
        item["backupPath"] = backupPath;
        item["overwrite"] = overwrite;

        preparedOperations.append(item);
    }

    return startGioTransfer(preparedOperations, moveOperation);
}

void FileOperations::resetTransferState()
{
    m_processErrorOutput.clear();
    m_pendingChangedPaths.clear();
}

void FileOperations::setProgressValue(double progress, const QString &speed, const QString &eta)
{
    const bool progressDiff = m_progress != progress;
    const bool speedDiff = m_speed != speed;
    const bool etaDiff = m_eta != eta;

    m_progress = progress;
    m_speed = speed;
    m_eta = eta;

    if (progressDiff) emit progressChanged();
    if (speedDiff) emit speedChanged();
    if (etaDiff) emit etaChanged();
}

void FileOperations::setPendingChangedPaths(const QStringList &paths)
{
    m_pendingChangedPaths = uniqueLocations(paths);
}

void FileOperations::emitPendingChangedPaths()
{
    emitChangedPaths(m_pendingChangedPaths);
    m_pendingChangedPaths.clear();
}

void FileOperations::emitChangedPaths(const QStringList &paths)
{
    const QStringList normalizedPaths = uniqueLocations(paths);
    if (!normalizedPaths.isEmpty())
        emit pathsChanged(normalizedPaths);
}

bool FileOperations::rename(const QString &path, const QString &newName)
{
    const QString normalizedPath = normalizeLocation(path);
    const QString targetPath = joinLocation(parentLocation(normalizedPath), newName);
    const QVariantMap result = renameResolvedItems({QVariantMap {
        {"sourcePath", normalizedPath},
        {"targetPath", targetPath}
    }});
    return result.value("success").toBool();
}

QVariantMap FileOperations::renameResolvedItems(const QVariantList &operations)
{
    QList<RenameOperation> renameOperations;
    QSet<QString> sourcePaths;
    QSet<QString> changedSourcePaths;
    QSet<QString> finalTargetPaths;

    for (const QVariant &variant : operations) {
        const QVariantMap item = variant.toMap();
        const QString sourcePath = normalizeLocation(item.value("sourcePath").toString());
        const QString targetPath = normalizeLocation(item.value("targetPath").toString());

        if (sourcePath.isEmpty() || targetPath.isEmpty())
            return renameResult(false, QStringLiteral("Rename operation is missing a path"));

        if (const QString nameError = entryNameError(locationFileName(targetPath)); !nameError.isEmpty())
            return renameResult(false, nameError);
        if (parentLocation(targetPath) != parentLocation(sourcePath))
            return renameResult(false, QStringLiteral("Rename cannot move an item to another folder"));

        if (sourcePaths.contains(sourcePath))
            return renameResult(false, QStringLiteral("Cannot rename the same item twice in one batch"));

        sourcePaths.insert(sourcePath);

        const QString targetError = renameTargetError(targetPath);
        if (!targetError.isEmpty())
            return renameResult(false, targetError);

        if (finalTargetPaths.contains(targetPath))
            return renameResult(false, QStringLiteral("Two items cannot end with the same name"));

        finalTargetPaths.insert(targetPath);

        if (!pathExistsSync(sourcePath)) {
            return renameResult(false, QStringLiteral("%1 no longer exists")
                .arg(locationFileName(sourcePath)));
        }

        if (sourcePath == targetPath)
            continue;

        changedSourcePaths.insert(sourcePath);
        renameOperations.append({sourcePath, targetPath, temporaryRenamePathFor(sourcePath)});
    }

    for (const RenameOperation &op : renameOperations) {
        if (pathExistsSync(op.targetPath) && !changedSourcePaths.contains(op.targetPath)) {
            return renameResult(false, QStringLiteral("%1 already exists")
                .arg(locationFileName(op.targetPath)));
        }
    }

    if (renameOperations.isEmpty())
        return renameResult(true, {}, {});

    QList<int> stagedIndices;
    QList<int> finalizedIndices;
    auto rollback = [&renameOperations, &stagedIndices, &finalizedIndices]() {
        for (int i = finalizedIndices.size() - 1; i >= 0; --i) {
            const RenameOperation &op = renameOperations.at(finalizedIndices.at(i));
            if (pathExistsSync(op.targetPath))
                moveLocationSync(op.targetPath, op.sourcePath);
        }

        for (int i = stagedIndices.size() - 1; i >= 0; --i) {
            const RenameOperation &op = renameOperations.at(stagedIndices.at(i));
            if (pathExistsSync(op.tempPath))
                moveLocationSync(op.tempPath, op.sourcePath);
        }
    };

    for (int i = 0; i < renameOperations.size(); ++i) {
        const RenameOperation &op = renameOperations.at(i);
        QString error;
        if (!moveLocationSync(op.sourcePath, op.tempPath, &error)) {
            rollback();
            return renameResult(false, QStringLiteral("Could not prepare %1 for renaming")
                .arg(locationFileName(op.sourcePath)));
        }

        stagedIndices.append(i);
    }

    for (int i = 0; i < renameOperations.size(); ++i) {
        const RenameOperation &op = renameOperations.at(i);
        QString error;
        if (!moveLocationSync(op.tempPath, op.targetPath, &error)) {
            rollback();
            return renameResult(false, QStringLiteral("Could not rename %1")
                .arg(locationFileName(op.sourcePath)));
        }

        finalizedIndices.append(i);
    }

    QStringList changedPaths;
    QStringList invalidatedPaths;
    for (const RenameOperation &op : renameOperations) {
        changedPaths.append(op.targetPath);
        invalidatedPaths << op.sourcePath << op.targetPath;
    }

    emitChangedPaths(invalidatedPaths);

    return renameResult(true, {}, changedPaths);
}

void FileOperations::createFolder(const QString &parentPath, const QString &name)
{
    if (const QString nameError = entryNameError(name); !nameError.isEmpty()) {
        emit operationFinished(false, nameError);
        return;
    }
    const QString targetPath = joinLocation(parentPath, name);
    QString error;
    if (makeDirectorySync(targetPath, &error) || error.isEmpty()) {
        emitChangedPaths({targetPath});
        return;
    }
    emit operationFinished(false, error);
}

void FileOperations::createFile(const QString &parentPath, const QString &name)
{
    if (const QString nameError = entryNameError(name); !nameError.isEmpty()) {
        emit operationFinished(false, nameError);
        return;
    }
    const QString targetPath = joinLocation(parentPath, name);
    QString error;
    if (createEmptyFileSync(targetPath, &error) || error.isEmpty()) {
        emitChangedPaths({targetPath});
        return;
    }
    emit operationFinished(false, error);
}

void FileOperations::openFile(const QString &path)
{
    const QString normalized = normalizeLocation(path);

    auto *proc = new QProcess(this);
    connect(proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            proc, &QProcess::deleteLater);

    // gio:// / sftp:// / smb:// / trash:// → use `gio open` which talks
    // to gvfs. On the host this is just `gio open <uri>`; inside a Flatpak
    // we run it on the host so it sees the host's gvfsd mounts.
    if (isUriPath(normalized)) {
        const QStringList args = {QStringLiteral("open"), gioLocationArg(normalized)};
        if (runningInFlatpak()) {
            proc->start(QStringLiteral("flatpak-spawn"),
                        QStringList{QStringLiteral("--host"), QStringLiteral("gio")} + args);
        } else {
            proc->start(QStringLiteral("gio"), args);
        }
        return;
    }

    // Local files. Outside a sandbox: hand off to Qt's QDesktopServices
    // (which uses xdg-open / kde-open / gio-launch under the hood and
    // honors the user's MIME associations). Inside a Flatpak: shell out
    // to `flatpak-spawn --host xdg-open` so the host opens the file with
    // the host's default app, completely bypassing the sandbox. This is
    // the same pattern Nautilus and Dolphin use when running as Flatpaks.
    if (runningInFlatpak()) {
        proc->start(QStringLiteral("flatpak-spawn"),
                    {QStringLiteral("--host"), QStringLiteral("xdg-open"), normalized});
        return;
    }

    proc->deleteLater();
    const QUrl url = QUrl::fromLocalFile(normalized);
    if (!QDesktopServices::openUrl(url))
        qWarning() << "FileOperations::openFile: failed to open" << normalized;
}

bool FileOperations::pathExists(const QString &path) const
{
    return pathExistsSync(path);
}

bool FileOperations::isRemotePath(const QString &path) const
{
    static const QString mountsDir = QDir::homePath() + QStringLiteral("/.local/share/hyprfm/mounts");
    const QString normalized = normalizeLocation(path);
    if (normalized.startsWith(mountsDir))
        return true;
    return isRemoteUriPath(normalized);
}

QString FileOperations::parentPath(const QString &path) const
{
    return parentLocation(path);
}

QString FileOperations::displayNameForPath(const QString &path) const
{
    return locationFileName(path);
}

QVariantList FileOperations::breadcrumbSegments(const QString &path) const
{
    return buildBreadcrumbs(path);
}

int FileOperations::emptyTrash()
{
    return startSimpleOperation(
        QStringLiteral("Emptying trash..."), {},
        [](ProgressReporter report) -> QString {
            // Walks the trash directories itself instead of enumerating
            // trash:// through gvfs, so this works with no session daemon and
            // covers .Trash-<uid> on other volumes at the same time. It also
            // sidesteps the GVFS rule that items inside a trashed folder may
            // not be modified, which used to leave non-empty folders behind.
            const QList<XdgTrash::Entry> entries = XdgTrash::scan();
            if (entries.isEmpty())
                return QString();

            QString lastError;
            const int total = entries.size();
            for (int i = 0; i < total; ++i) {
                const XdgTrash::Entry &entry = entries.at(i);
                report(i, total, entry.name);

                const QFileInfo info(entry.filesPath);
                bool removed = false;
                if (info.isDir() && !info.isSymLink())
                    removed = QDir(entry.filesPath).removeRecursively();
                else
                    removed = QFile::remove(entry.filesPath);

                if (removed)
                    XdgTrash::removeInfo(entry.filesPath);
                else
                    lastError = QStringLiteral("Could not delete %1").arg(entry.name);
            }
            return lastError;
        });
}

// Turn a desktop entry's Exec= line into an argv, substituting the file for
// whichever field code the entry uses. Public and static so it can be tested
// without launching anything.
QStringList FileOperations::desktopExecArguments(const QString &execLine, const QString &file)
{
    QStringList argv;
    bool fileConsumed = false;

    const QStringList parts = QProcess::splitCommand(execLine);
    for (const QString &part : parts) {
        // %f/%F take a path, %u/%U a URI; a local path satisfies both.
        if (part == QLatin1String("%f") || part == QLatin1String("%F")
            || part == QLatin1String("%u") || part == QLatin1String("%U")) {
            if (!file.isEmpty()) {
                argv.append(file);
                fileConsumed = true;
            }
            continue;
        }
        // %i, %c, %k and the deprecated codes carry no meaning for us.
        if (part.size() == 2 && part.startsWith(QLatin1Char('%')))
            continue;

        QString literal = part;
        argv.append(literal.replace(QLatin1String("%%"), QLatin1String("%")));
    }

    // Entries that declare no field code still expect the file as a trailing
    // argument — that is how a launcher would pass it.
    if (!fileConsumed && !file.isEmpty())
        argv.append(file);

    return argv;
}

void FileOperations::openFileWith(const QString &path, const QString &desktopFile)
{
    const QString normalized = normalizeLocation(path);
    const QString entryPath = desktopEntryPath(desktopFile);
    if (entryPath.isEmpty()) {
        emit operationFinished(false,
            QStringLiteral("Could not find the application entry '%1'").arg(desktopFile));
        return;
    }

    QSettings entry(entryPath, QSettings::IniFormat);
    entry.beginGroup(QStringLiteral("Desktop Entry"));

    QString program;
    QStringList args;

    if (entry.value(QStringLiteral("Terminal")).toString()
            .compare(QLatin1String("true"), Qt::CaseInsensitive) == 0) {
        // GLib refuses to launch Terminal=true entries unless it recognises an
        // installed terminal ("Unable to find terminal required for
        // application"), which silently rules out micro, nvim, nano and every
        // other TUI editor. Run those ourselves through $TERMINAL instead.
        const QStringList command =
            desktopExecArguments(entry.value(QStringLiteral("Exec")).toString(), normalized);
        if (command.isEmpty()) {
            emit operationFinished(false,
                QStringLiteral("'%1' has no runnable Exec line").arg(desktopFile));
            return;
        }
        program = qEnvironmentVariable("TERMINAL", QStringLiteral("kitty"));
        args = QStringList{QStringLiteral("-e")} + command;
    } else {
        // `gio launch` comes from glib2, which HyprFM already requires; the
        // gtk-launch this used to call lives in gtk3 and is often absent.
        QString hostEntryPath = entryPath;
        if (hostEntryPath.startsWith(QLatin1String("/run/host/")))
            hostEntryPath.remove(0, qstrlen("/run/host"));
        program = QStringLiteral("gio");
        args = {QStringLiteral("launch"), hostEntryPath, normalized};
    }

    if (runningInFlatpak()) {
        args.prepend(program);
        args.prepend(QStringLiteral("--host"));
        program = QStringLiteral("flatpak-spawn");
    }

    auto *proc = new QProcess(this);
    // The launched app inherits gio's stdout/stderr. With the default piped
    // channels those pipes close when this QProcess is deleted, and the first
    // thing the app logs afterwards kills it with SIGPIPE — which is why
    // chatty apps (Krita, mpv/SVP) "did nothing" while Firefox worked. Let it
    // inherit our own stdio instead; gio's diagnostics land in our log.
    proc->setProcessChannelMode(QProcess::ForwardedChannels);
    connect(proc, &QProcess::errorOccurred, proc, [this, proc, program](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            emit operationFinished(false,
                QStringLiteral("Could not run '%1' — is it installed?").arg(program));
        }
        proc->deleteLater();
    });
    connect(proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), proc,
            [this, proc, desktopFile](int exitCode, QProcess::ExitStatus) {
        // gio exits as soon as the app is launched, so a non-zero code here is
        // a launch failure worth surfacing rather than the app's own exit.
        if (exitCode != 0) {
            emit operationFinished(false,
                QStringLiteral("Could not launch '%1' — see the log for details").arg(desktopFile));
        }
        proc->deleteLater();
    });

    // A terminal stays alive for as long as its window is open, and the TUI's
    // own exit code would be misread as a launch failure, so only spawn errors
    // are interesting there.
    if (program == qEnvironmentVariable("TERMINAL", QStringLiteral("kitty"))) {
        proc->startDetached(program, args);
        proc->deleteLater();
        return;
    }

    proc->start(program, args);
}

// Edit config.toml (Ctrl+Shift+,). xdg-open was the wrong tool for it: most
// systems have no application registered for TOML, so nothing opened and
// nothing said so. Run $VISUAL/$EDITOR in $TERMINAL and fall back to the
// default application only when no editor is configured.
void FileOperations::openInEditor(const QString &path)
{
    QString editor = qEnvironmentVariable("VISUAL");
    if (editor.isEmpty())
        editor = qEnvironmentVariable("EDITOR");
    if (editor.isEmpty()) {
        openFile(path);
        return;
    }

    const QString terminal = qEnvironmentVariable("TERMINAL", QStringLiteral("kitty"));
    QStringList args{QStringLiteral("-e")};
    args += QProcess::splitCommand(editor);
    args << path;
    if (!QProcess::startDetached(terminal, args)) {
        emit operationFinished(false,
            QStringLiteral("Could not run '%1' — set $TERMINAL to your terminal emulator").arg(terminal));
    }
}

bool FileOperations::hasClipboardImage() const
{
    const QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboardImage(clipboard).isNull())
        return true;

    return !clipboardImageData().isEmpty();
}

QString FileOperations::pasteClipboardImage(const QString &destinationDir)
{
    const QString outputPath = uniqueImagePastePath(destinationDir);
    if (outputPath.isEmpty()) {
        emit operationFinished(false, "Destination folder does not exist");
        return {};
    }

    // Prefer the live Qt clipboard image so we paste the current selection
    // instead of stale external clipboard-manager data.
    const QClipboard *clipboard = QGuiApplication::clipboard();
    const QImage image = clipboardImage(clipboard);
    if (!image.isNull()) {
        if (!image.save(outputPath, "PNG")) {
            emit operationFinished(false, "Failed to save clipboard image");
            return {};
        }
        emitChangedPaths({outputPath});
        emit operationFinished(true, QString());
        return outputPath;
    }

    const QByteArray rawImage = clipboardImageData();
    if (rawImage.isEmpty()) {
        emit operationFinished(false, "Clipboard does not contain an image");
        return {};
    }

    QFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly)) {
        emit operationFinished(false, "Failed to write clipboard image");
        return {};
    }
    file.write(rawImage);
    file.close();

    emitChangedPaths({outputPath});
    emit operationFinished(true, QString());
    return outputPath;
}

void FileOperations::copyPathToClipboard(const QString &path)
{
    auto *proc = new QProcess(this);
    proc->start("wl-copy", {path});
    connect(proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            proc, &QProcess::deleteLater);
}

void FileOperations::openInTerminal(const QString &dirPath)
{
    if (isUriPath(dirPath)) {
        emit operationFinished(false, QStringLiteral("Open in Terminal is only available for local folders"));
        return;
    }

    QString terminal = qEnvironmentVariable("TERMINAL", "kitty");
    auto *proc = new QProcess(this);
    proc->setWorkingDirectory(dirPath);
    proc->start(terminal, {});
    connect(proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            proc, &QProcess::deleteLater);
}

// [[context_menu.actions]] from config.toml. The command uses desktop-entry
// field codes (%f/%u...), and runs once per selected path so a plain
// "convert %f out.png" works on a multi-selection without shell quoting.
void FileOperations::runCustomAction(const QString &command, const QStringList &paths)
{
    for (const QString &path : paths) {
        const QString normalized = normalizeLocation(path);
        const QStringList argv = desktopExecArguments(command, normalized);
        if (argv.isEmpty()) {
            emit operationFinished(false, QStringLiteral("Custom action has no command"));
            return;
        }
        QProcess proc;
        proc.setProgram(argv.first());
        proc.setArguments(argv.mid(1));
        proc.setWorkingDirectory(QFileInfo(normalized).absolutePath());
        if (!proc.startDetached()) {
            emit operationFinished(false,
                QStringLiteral("Could not run '%1' — is it installed?").arg(argv.first()));
            return;
        }
    }
}

// Launch a second, independent HyprFM window. `--new-window` makes the child
// skip the single-instance handoff (and the shared session file), so the two
// windows coexist instead of the new process forwarding to this one.
void FileOperations::openNewWindow(const QString &dirPath)
{
    QStringList args{QStringLiteral("--new-window")};
    if (!dirPath.isEmpty())
        args.append(dirPath);

    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), args))
        emit operationFinished(false, QStringLiteral("Failed to open a new window"));
}

int FileOperations::compressFiles(const QStringList &paths, const QString &format)
{
    if (paths.isEmpty()) return -1;

    // Determine output name from first file's parent + name
    QFileInfo first(paths.first());
    QString baseName = (paths.size() == 1) ? first.completeBaseName() : "archive";
    QString parentDir = first.absolutePath();
    QString outputPath;

    QString program;
    QStringList args;
    if (format == "zip") {
        QString outPath = parentDir + "/" + baseName + ".zip";
        outputPath = outPath;
        program = QStringLiteral("zip");
        args = {QStringLiteral("-rv"), outPath, QStringLiteral("--")};
        for (const auto &p : paths)
            args.append(QFileInfo(p).fileName());
    } else if (format == "tar.gz") {
        QString outPath = parentDir + "/" + baseName + ".tar.gz";
        outputPath = outPath;
        program = QStringLiteral("tar");
        args = {QStringLiteral("-cvzf"), outPath, QStringLiteral("-C"), parentDir,
                QStringLiteral("--")};
        for (const auto &p : paths)
            args.append(QFileInfo(p).fileName());
    } else if (format == "tar.xz") {
        QString outPath = parentDir + "/" + baseName + ".tar.xz";
        outputPath = outPath;
        program = QStringLiteral("tar");
        args = {QStringLiteral("-cvJf"), outPath, QStringLiteral("-C"), parentDir,
                QStringLiteral("--")};
        for (const auto &p : paths)
            args.append(QFileInfo(p).fileName());
    } else if (format == "tar.bz2") {
        QString outPath = parentDir + "/" + baseName + ".tar.bz2";
        outputPath = outPath;
        program = QStringLiteral("tar");
        args = {QStringLiteral("-cvjf"), outPath, QStringLiteral("-C"), parentDir,
                QStringLiteral("--")};
        for (const auto &p : paths)
            args.append(QFileInfo(p).fileName());
    } else if (format == "tar") {
        QString outPath = parentDir + "/" + baseName + ".tar";
        outputPath = outPath;
        program = QStringLiteral("tar");
        args = {QStringLiteral("-cvf"), outPath, QStringLiteral("-C"), parentDir,
                QStringLiteral("--")};
        for (const auto &p : paths)
            args.append(QFileInfo(p).fileName());
    } else if (format == "7z") {
        QString outPath = parentDir + "/" + baseName + ".7z";
        outputPath = outPath;
        program = QStringLiteral("7z");
        args = {QStringLiteral("a"), outPath, QStringLiteral("--")};
        for (const auto &p : paths)
            args.append(QFileInfo(p).fileName());
    } else {
        return -1;
    }

    const QString statusText = QString("Compressing %1 item(s)...").arg(paths.size());
    return startSimpleOperation(statusText, {outputPath},
        [paths, parentDir, program, args](ProgressReporter report) -> QString {
            // Pre-count files for progress
            int totalFiles = 0;
            for (const auto &p : paths) {
                QFileInfo fi(p);
                if (fi.isDir()) {
                    QDirIterator it(p, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                                    QDirIterator::Subdirectories);
                    while (it.hasNext()) { it.next(); ++totalFiles; }
                } else {
                    ++totalFiles;
                }
            }
            if (totalFiles <= 0) totalFiles = 1;

            report(0, totalFiles, {});

            // Run with verbose output and count lines for progress
            QProcess proc;
            proc.setWorkingDirectory(parentDir);
            proc.setProcessChannelMode(QProcess::MergedChannels);
            proc.start(program, args);
            if (!proc.waitForStarted(5000))
                return QStringLiteral("Failed to start compression");

            int processed = 0;
            while (proc.state() != QProcess::NotRunning || proc.canReadLine()) {
                if (!proc.canReadLine())
                    proc.waitForReadyRead(200);
                while (proc.canReadLine()) {
                    const QString line = QString::fromUtf8(proc.readLine()).trimmed();
                    if (line.isEmpty()) continue;
                    ++processed;
                    const QString fileName = line.mid(line.lastIndexOf('/') + 1);
                    report(qMin(processed, totalFiles), totalFiles, fileName);
                }
            }

            proc.waitForFinished(5000);
            if (proc.exitCode() != 0)
                return QStringLiteral("Compression failed");
            return {};
        });
}

int FileOperations::extractArchive(const QString &archivePath, const QString &destination)
{
    QString program;
    QStringList args;
    if (!archiveExtractCommand(archivePath, destination, &program, &args))
        return -1;

    // Add verbose flag for progress tracking
    QStringList verboseArgs = args;
    if (program == "tar" || program == "bsdtar")
        verboseArgs.prepend(QStringLiteral("-v"));
    else if (program == "unzip")
        { /* unzip is already verbose by default */ }
    // 7z, gunzip, unxz, bunzip2 — no easy verbose line-per-file

    // Pre-count files in archive for progress
    QString listProg;
    QStringList listArgs;
    const bool canList = archiveListCommand(archivePath, &listProg, &listArgs);

    return startSimpleOperation(QStringLiteral("Extracting..."), {destination},
        [program, verboseArgs, canList, listProg, listArgs](ProgressReporter report) -> QString {
            int totalFiles = 0;
            if (canList) {
                QProcess listProc;
                listProc.start(listProg, listArgs);
                if (listProc.waitForFinished(30000) && listProc.exitCode() == 0) {
                    const QByteArray output = listProc.readAllStandardOutput();
                    totalFiles = output.count('\n');
                }
            }
            if (totalFiles <= 0) totalFiles = 1;

            report(0, totalFiles, {});

            QProcess proc;
            proc.setProcessChannelMode(QProcess::MergedChannels);
            proc.start(program, verboseArgs);
            if (!proc.waitForStarted(5000))
                return QStringLiteral("Failed to start extraction");

            int processed = 0;
            while (proc.state() != QProcess::NotRunning || proc.canReadLine()) {
                if (!proc.canReadLine())
                    proc.waitForReadyRead(200);
                while (proc.canReadLine()) {
                    const QString line = QString::fromUtf8(proc.readLine()).trimmed();
                    if (line.isEmpty()) continue;
                    ++processed;
                    const QString fileName = line.mid(line.lastIndexOf('/') + 1);
                    report(qMin(processed, totalFiles), totalFiles, fileName);
                }
            }

            proc.waitForFinished(5000);
            if (proc.exitCode() != 0)
                return QStringLiteral("Extraction failed");
            return {};
        });
}

// Opening an archive extracts it into a fresh folder next to it. Extracting
// into the parent directory ran unzip -o / 7z -aoa there, which silently
// replaced unrelated files with the same names. Returns the created folder,
// or an empty string after reporting the failure.
QString FileOperations::newExtractionFolder(const QString &archivePath)
{
    const QString normalized = normalizeLocation(archivePath);
    const QString parent = parentLocation(normalized);
    QString base = locationFileName(normalized);
    static const QStringList suffixes = {
        QStringLiteral(".tar.gz"), QStringLiteral(".tar.xz"), QStringLiteral(".tar.bz2"),
        QStringLiteral(".tar.zst"), QStringLiteral(".tgz"), QStringLiteral(".txz"),
        QStringLiteral(".tbz2"), QStringLiteral(".tar"), QStringLiteral(".zip"),
        QStringLiteral(".7z"), QStringLiteral(".rar"), QStringLiteral(".gz"),
        QStringLiteral(".xz"), QStringLiteral(".bz2"), QStringLiteral(".zst")};
    for (const QString &suffix : suffixes) {
        if (base.endsWith(suffix, Qt::CaseInsensitive)) {
            base.chop(suffix.size());
            break;
        }
    }
    if (base.isEmpty())
        base = QStringLiteral("extracted");

    const QString dir = joinLocation(parent, uniqueNameForDestination(parent, base, {}));
    QString error;
    if (!makeDirectorySync(dir, &error)) {
        emit operationFinished(false, error.isEmpty()
            ? QStringLiteral("Could not create a folder to extract into") : error);
        return {};
    }
    return dir;
}

QString FileOperations::archiveRootFolder(const QString &archivePath)
{
    QString program;
    QStringList args;
    if (!archiveListCommand(archivePath, &program, &args))
        return {};

    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForFinished(5000) || proc.exitCode() != 0)
        return {};

    const QString output = QString::fromUtf8(proc.readAllStandardOutput());
    const QStringList entries = archiveEntriesFromOutput(program, output);
    if (entries.isEmpty())
        return {};

    return commonArchiveRootFolder(entries);
}

bool FileOperations::isArchive(const QString &path)
{
    return archiveKindForPath(path) != ArchiveKind::None;
}

void FileOperations::pauseTransfer(int transferId)
{
    if (transferId < 0) {
        for (auto &t : m_activeTransfers) {
            t.worker->pause();
            t.paused = true;
            t.statusText = QStringLiteral("Paused");
        }
    } else if (auto *t = findTransfer(transferId)) {
        t->worker->pause();
        t->paused = true;
        t->statusText = QStringLiteral("Paused");
    }
    emitAggregatedState();
}

void FileOperations::resumeTransfer(int transferId)
{
    if (transferId < 0) {
        for (auto &t : m_activeTransfers) {
            t.worker->resume();
            t.paused = false;
        }
    } else if (auto *t = findTransfer(transferId)) {
        t->worker->resume();
        t->paused = false;
    }
    emitAggregatedState();
}

void FileOperations::cancelTransfer(int transferId)
{
    if (transferId < 0) {
        for (auto &t : m_activeTransfers)
            t.worker->cancel();
    } else if (auto *t = findTransfer(transferId)) {
        t->worker->cancel();
    }
}

void FileOperations::cleanupTransfer(int transferId)
{
    for (int i = 0; i < m_activeTransfers.size(); ++i) {
        if (m_activeTransfers[i].id == transferId) {
            auto &t = m_activeTransfers[i];
            if (t.thread) {
                t.thread->quit();
                t.thread->wait();
                t.thread->deleteLater();
            }
            m_activeTransfers.removeAt(i);
            break;
        }
    }
    emitAggregatedState();
}

int FileOperations::startSimpleOperation(const QString &statusText, const QStringList &changedPaths,
                                           std::function<QString(ProgressReporter)> work)
{
    const int id = m_nextTransferId++;
    ActiveTransfer transfer;
    transfer.id = id;
    transfer.statusText = statusText;
    transfer.progress = -1.0;
    transfer.changedPaths = changedPaths;

    auto *thread = new QThread;
    transfer.thread = thread;
    transfer.worker = nullptr;
    m_activeTransfers.append(transfer);
    emitAggregatedState();

    auto reportProgress = [this, id](int current, int total, const QString &fileName) {
        QMetaObject::invokeMethod(this, [this, id, current, total, fileName]() {
            if (auto *t = findTransfer(id)) {
                t->progress = total > 0 ? static_cast<double>(current) / total : -1.0;
                t->currentFile = fileName;
                emitAggregatedState();
            }
        }, Qt::QueuedConnection);
    };

    auto *runner = new QObject;
    runner->moveToThread(thread);

    connect(thread, &QThread::started, runner, [runner, work, reportProgress, this, id]() {
        const QString error = work(reportProgress);
        const bool ok = error.isEmpty();
        QMetaObject::invokeMethod(this, [this, id, ok, error]() {
            if (auto *t = findTransfer(id)) {
                t->progress = 1.0;
                emitChangedPaths(t->changedPaths);
            }
            m_progress = 1.0;
            emit operationFinished(ok, error, id);
            cleanupTransfer(id);
        }, Qt::QueuedConnection);
        runner->deleteLater();
    });

    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    return id;
}

int FileOperations::startGioTransfer(const QVariantList &operations, bool moveOperation)
{
    QList<GioTransferWorker::TransferItem> items;
    int itemCount = 0;
    QStringList changedPaths;
    QStringList targetPaths;

    for (const QVariant &variant : operations) {
        const QVariantMap item = variant.toMap();
        const QString sourcePath = item.value("sourcePath").toString();
        const QString targetPath = item.value("targetPath").toString();
        const QString backupPath = item.value("backupPath").toString();
        const bool overwrite = item.value("overwrite").toBool();

        items.append({sourcePath, targetPath, backupPath, overwrite});
        ++itemCount;

        if (moveOperation)
            appendUniqueLocation(&changedPaths, sourcePath);
        appendUniqueLocation(&changedPaths, targetPath);
        appendUniqueLocation(&changedPaths, backupPath);
        appendUniqueLocation(&targetPaths, targetPath);
    }

    const int id = m_nextTransferId++;
    ActiveTransfer transfer;
    transfer.id = id;
    transfer.statusText = QString(moveOperation ? "Moving %1 item(s)..." : "Copying %1 item(s)...").arg(itemCount);
    transfer.progress = -1.0;
    transfer.changedPaths = changedPaths;
    transfer.targetPaths = targetPaths;

    auto *thread = new QThread;
    auto *worker = new GioTransferWorker;
    worker->moveToThread(thread);
    transfer.thread = thread;
    transfer.worker = worker;

    m_activeTransfers.append(transfer);

    connect(thread, &QThread::finished, worker, &QObject::deleteLater);

    connect(worker, &GioTransferWorker::progressUpdated, this,
            [this, id](double progress, const QString &speed, const QString &eta) {
        if (auto *t = findTransfer(id)) {
            t->progress = progress;
            t->speed = speed;
            t->eta = eta;
            emitAggregatedState();
        }
    });

    connect(worker, &GioTransferWorker::itemStarted, this,
            [this, id](const QString &sourcePath, const QString &targetPath) {
        Q_UNUSED(targetPath)
        if (auto *t = findTransfer(id)) {
            t->currentFile = sourcePath.mid(sourcePath.lastIndexOf('/') + 1);
            emitAggregatedState();
        }
    });

    connect(worker, &GioTransferWorker::finished, this,
            [this, id](bool success, const QString &error) {
        if (auto *t = findTransfer(id)) {
            emitChangedPaths(t->changedPaths);
            if (success)
                t->progress = 1.0;
        }
        if (success)
            m_progress = 1.0;
        emit operationFinished(success, error, id);
        cleanupTransfer(id);
    });

    emitAggregatedState();

    thread->start();
    QMetaObject::invokeMethod(worker, [worker, items, moveOperation]() {
        worker->execute(items, moveOperation);
    }, Qt::QueuedConnection);
    return id;
}

void FileOperations::runProcess(const QString &program, const QStringList &args)
{
    if (m_process) {
        m_process->kill();
        m_process->deleteLater();
        m_process = nullptr;
    }

    setProgressValue(-1.0);

    if (!m_busy) {
        m_busy = true;
        emit busyChanged();
    }

    m_processErrorOutput.clear();
    m_process = new QProcess(this);

    connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
        const QByteArray data = m_process->readAllStandardError();
        m_processErrorOutput.append(data);
    });

    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) {
        const QString error = exitCode != 0
            ? QString::fromUtf8(m_processErrorOutput + m_process->readAllStandardError())
            : QString();
        m_process->deleteLater();
        m_process = nullptr;

        m_busy = false;
        setProgressValue(1.0);
        m_statusText.clear();
        m_speed.clear();
        m_eta.clear();
        emit busyChanged();
        emit statusTextChanged();
        emit speedChanged();
        emit etaChanged();
        emitPendingChangedPaths();
        emit operationFinished(exitCode == 0, error);
        resetTransferState();
    });

    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        Q_UNUSED(error)
        if (!m_process)
            return;

        const QString processError = m_process->errorString();
        m_process->deleteLater();
        m_process = nullptr;

        m_busy = false;
        m_statusText.clear();
        m_speed.clear();
        m_eta.clear();
        emit busyChanged();
        emit statusTextChanged();
        emit speedChanged();
        emit etaChanged();
        emitPendingChangedPaths();
        emit operationFinished(false, processError);
        resetTransferState();
    });

    m_process->start(program, args);
}

QString FileOperations::uniqueImagePastePath(const QString &destinationDir) const
{
    QDir dir(destinationDir);
    if (!dir.exists())
        return {};

    const QString baseName = "Pasted image";
    const QString extension = ".png";
    QString candidate = dir.filePath(baseName + extension);
    if (!QFileInfo::exists(candidate))
        return candidate;

    for (int i = 2; i < 10000; ++i) {
        candidate = dir.filePath(QString("%1 %2%3").arg(baseName).arg(i).arg(extension));
        if (!QFileInfo::exists(candidate))
            return candidate;
    }

    return {};
}

QString FileOperations::conflictBackupPath(const QString &targetPath) const
{
    QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheRoot.isEmpty())
        cacheRoot = QDir::homePath() + "/.cache/hyprfm";

    QDir backupDir(cacheRoot + "/conflict-backups");
    backupDir.mkpath(".");

    const QString baseName = QFileInfo(targetPath).fileName();
    const QString uniqueName = QUuid::createUuid().toString(QUuid::WithoutBraces) + "-" + baseName;
    return backupDir.filePath(uniqueName);
}

QByteArray FileOperations::clipboardImageData() const
{
    const QString wlPastePath = QStandardPaths::findExecutable("wl-paste");
    if (wlPastePath.isEmpty())
        return {};

    QProcess listProcess;
    listProcess.start(wlPastePath, {"--list-types"});
    if (!listProcess.waitForFinished(1000) || listProcess.exitCode() != 0)
        return {};

    const QStringList types = QString::fromUtf8(listProcess.readAllStandardOutput())
                                  .split('\n', Qt::SkipEmptyParts);
    QString imageType;
    if (types.contains("image/png"))
        imageType = "image/png";
    else {
        for (const QString &type : types) {
            if (type.startsWith("image/")) {
                imageType = type;
                break;
            }
        }
    }

    if (imageType.isEmpty())
        return {};

    QProcess imageProcess;
    imageProcess.start(wlPastePath, {"--no-newline", "--type", imageType});
    if (!imageProcess.waitForFinished(3000) || imageProcess.exitCode() != 0)
        return {};

    return imageProcess.readAllStandardOutput();
}

void FileOperations::setWallpaper(const QString &path)
{
    const QString resolved = QFileInfo(path).absoluteFilePath();
    auto *proc = new QProcess(this);
    connect(proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [proc, resolved](int exitCode, QProcess::ExitStatus) {
                if (exitCode != 0)
                    qWarning() << "FileOperations::setWallpaper: hyprctl failed for" << resolved;
                proc->deleteLater();
            });
    proc->start(QStringLiteral("hyprctl"),
                {QStringLiteral("hyprpaper"), QStringLiteral("wallpaper"),
                 QStringLiteral(",") + resolved});
}

void FileOperations::setHyprlandRounding(const QString &windowTitle, int radius)
{
    auto *proc = new QProcess(this);
    connect(proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [proc](int, QProcess::ExitStatus) { proc->deleteLater(); });
    proc->start(QStringLiteral("hyprctl"),
                {QStringLiteral("setprop"),
                 QStringLiteral("title:") + windowTitle,
                 QStringLiteral("rounding"),
                 QString::number(radius)});
}

void FileOperations::setHyprlandBorder(const QString &windowTitle, int size)
{
    auto *proc = new QProcess(this);
    connect(proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [proc](int, QProcess::ExitStatus) { proc->deleteLater(); });
    proc->start(QStringLiteral("hyprctl"),
                {QStringLiteral("setprop"),
                 QStringLiteral("title:") + windowTitle,
                 QStringLiteral("bordersize"),
                 QString::number(size)});
}
