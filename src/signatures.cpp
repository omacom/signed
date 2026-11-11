#include "signatures.h"

#include <QCollator>
#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <array>

namespace {

// Phone photos are far bigger than a signature needs; this keeps them sharp
// at any sensible size on the page while keeping the PDF small.
constexpr int MaximumWidth = 1600;

QImage cropToInk(const QImage &image) {
    int left = image.width(), top = image.height(), right = -1, bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) > 24) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    if (right < 0)
        return {};
    const int pad = 4;
    const QRect bounds = QRect(QPoint(left, top), QPoint(right, bottom)).adjusted(-pad, -pad, pad, pad)
                             .intersected(image.rect());
    QImage cropped = image.copy(bounds);
    if (cropped.width() > MaximumWidth)
        cropped = cropped.scaledToWidth(MaximumWidth, Qt::SmoothTransformation);
    return cropped;
}

bool hasTransparency(const QImage &image) {
    if (!image.hasAlphaChannel())
        return false;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(line[x]) < 250)
                return true;
    }
    return false;
}

int luminance(QRgb pixel) {
    return (qRed(pixel) * 299 + qGreen(pixel) * 587 + qBlue(pixel) * 114) / 1000;
}

}

SignatureLibrary::SignatureLibrary(const QString &directory, QObject *parent)
    : QAbstractListModel(parent),
      m_directory(directory.isEmpty()
                      ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/signed/signatures"
                      : directory) {
    reload();
}

int SignatureLibrary::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_names.size();
}

QVariant SignatureLibrary::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_names.size())
        return {};
    const QString &name = m_names.at(index.row());
    if (role == NameRole)
        return name;
    if (role == SourceRole) {
        // The generation changes the URL whenever the library does, so a
        // replaced signature never shows its old image from the cache.
        QUrl url = QUrl::fromLocalFile(pathFor(name));
        url.setQuery(QStringLiteral("v=%1").arg(m_generation));
        return url;
    }
    return {};
}

QHash<int, QByteArray> SignatureLibrary::roleNames() const {
    return {{NameRole, "name"}, {SourceRole, "source"}};
}

QString SignatureLibrary::pathFor(const QString &name) const {
    return QDir(m_directory).filePath(name + QStringLiteral(".png"));
}

QString SignatureLibrary::validateName(const QString &name) const {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return tr("Give the signature a name.");
    if (trimmed.startsWith(QLatin1Char('.')) || trimmed.contains(QLatin1Char('/')) || trimmed.contains(QChar(0)))
        return tr("A name can't start with a dot or contain a slash.");
    if (trimmed.size() > 60)
        return tr("That name is too long.");
    return {};
}

bool SignatureLibrary::contains(const QString &name) const {
    return m_names.contains(name.trimmed());
}

void SignatureLibrary::reload() {
    QStringList names;
    for (const QFileInfo &file : QDir(m_directory).entryInfoList({QStringLiteral("*.png")}, QDir::Files))
        if (!file.completeBaseName().startsWith(QLatin1Char('.')))
            names.append(file.completeBaseName());
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    std::sort(names.begin(), names.end(), collator);

    beginResetModel();
    m_names = names;
    ++m_generation;
    endResetModel();
    emit countChanged();
}

QString SignatureLibrary::add(const QString &name, const QImage &image) {
    const QString error = validateName(name);
    if (!error.isEmpty())
        return error;
    if (image.isNull())
        return tr("There's no signature in that image.");
    if (!QDir().mkpath(m_directory))
        return tr("Can't create %1.").arg(m_directory);

    QSaveFile file(pathFor(name.trimmed()));
    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit())
        return tr("Couldn't save the signature: %1").arg(file.errorString());
    reload();
    return {};
}

bool SignatureLibrary::remove(const QString &name) {
    if (!contains(name))
        return false;
    const bool removed = QFile::remove(pathFor(name.trimmed()));
    reload();
    return removed;
}

QImage SignatureLibrary::image(const QString &name) const {
    if (!contains(name))
        return {};
    return QImage(pathFor(name.trimmed())).convertToFormat(QImage::Format_ARGB32);
}

QImage SignatureLibrary::load(const QString &path, QString *error) {
    QImageReader reader(path);
    reader.setAutoTransform(true);  // phone photos are often stored sideways with an EXIF turn
    QImage image = reader.read();
    if (image.isNull() && error)
        *error = tr("Can't read %1: %2").arg(QFileInfo(path).fileName(), reader.errorString());
    return image;
}

QImage SignatureLibrary::removeBackground(const QImage &source, qreal threshold) {
    if (source.isNull())
        return {};
    QImage image = source.convertToFormat(QImage::Format_ARGB32);
    if (hasTransparency(image))
        return cropToInk(image);

    if (std::max(image.width(), image.height()) > 2400)
        image = image.scaled(2400, 2400, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    // The paper is what most of the picture is. Taking a bright-ish percentile
    // rather than the brightest pixel keeps glare from setting the level.
    std::array<int, 256> histogram{};
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            ++histogram[luminance(line[x])];
    }
    const qint64 total = qint64(image.width()) * image.height();
    qint64 seen = 0;
    int paper = 255;
    for (int level = 0; level < 256; ++level) {
        seen += histogram[level];
        if (seen >= total * 7 / 10) {
            paper = level;
            break;
        }
    }
    paper = std::max(paper, 32);

    // Alpha ramps up softly with darkness instead of cutting at one level, so
    // stroke edges stay smooth without a white fringe. Ink keeps its hue (blue
    // stays blue) but is deepened, since photos wash it out.
    const qreal low = threshold;
    const qreal high = threshold + 0.2;
    QImage result(image.size(), QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *in = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        QRgb *out = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const qreal darkness = std::clamp((paper - luminance(in[x])) / qreal(paper), 0.0, 1.0);
            const qreal t = std::clamp((darkness - low) / (high - low), 0.0, 1.0);
            const qreal alpha = t * t * (3 - 2 * t);
            if (alpha <= 0) {
                out[x] = 0;
                continue;
            }
            QColor ink = QColor::fromRgb(in[x]).toHsv();
            ink.setHsvF(ink.hsvHueF() < 0 ? 0 : ink.hsvHueF(), ink.hsvSaturationF(), std::min(ink.valueF(), 0.3f));
            ink.setAlphaF(alpha);
            out[x] = ink.rgba();
        }
    }
    return cropToInk(result);
}
