#include "backend.h"

#include <QDate>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QVariantMap>
#include <QLocale>
#include <QSet>


#include <poppler-qt6.h>

#include "filepicker.h"
#include "pageprovider.h"
#include "signer.h"
#include "stamper.h"

namespace {

constexpr qreal DefaultSignatureWidth = 108;  // an inch and a half
constexpr qreal DefaultTextSize = 12;

bool samePath(const QString &a, const QString &b) {
    const QFileInfo left(a), right(b);
    if (left.exists() && right.exists())
        return left.canonicalFilePath() == right.canonicalFilePath();
    return left.absoluteFilePath() == right.absoluteFilePath();
}

}

Backend::Backend(FilePicker *picker, PageProvider *pages, SignatureLibrary *signatures,
                 QSettings *settings, QObject *parent)
    : QObject(parent), m_picker(picker), m_pages(pages), m_signatures(signatures), m_settings(settings) {
    connect(&m_items, &ItemModel::revisionChanged, this, &Backend::dirtyChanged);
    if (m_picker) {
        connect(m_picker, &FilePicker::pdfSelected, this, [this](const QUrl &url) { openUrl(url); });
        connect(m_picker, &FilePicker::imageSelected, this, [this](const QUrl &url) {
            m_settings->setValue("imageFolder", QFileInfo(url.toLocalFile()).absolutePath());
            loadSignatureImage(url.toLocalFile());
        });
        connect(m_picker, &FilePicker::p12Selected, this, [this](const QUrl &url) {
            m_settings->setValue("certFolder", QFileInfo(url.toLocalFile()).absolutePath());
            import(url.toLocalFile(), QString());
        });
        connect(m_picker, &FilePicker::saveSelected, this, [this](const QUrl &url) { saveTo(url.toLocalFile()); });
        connect(m_picker, &FilePicker::failed, this, &Backend::setNotice);
    }
}

QString Backend::fileName() const {
    return QFileInfo(m_path).fileName();
}

qreal Backend::widestPage() const {
    qreal widest = 0;
    for (const QSizeF &size : m_pageSizes)
        widest = std::max(widest, size.width());
    return widest > 0 ? widest : 612;
}

qreal Backend::pageWidth(int page) const {
    return page >= 0 && page < m_pageSizes.size() ? m_pageSizes.at(page).width() : 612;
}

qreal Backend::pageHeight(int page) const {
    return page >= 0 && page < m_pageSizes.size() ? m_pageSizes.at(page).height() : 792;
}

void Backend::setNotice(const QString &notice) {
    if (m_notice == notice)
        return;
    m_notice = notice;
    emit noticeChanged();
}

void Backend::clearNotice() {
    setNotice({});
}

void Backend::open() {
    if (m_picker)
        m_picker->openPdf(m_settings->value("folder").toString());
}

bool Backend::openUrl(const QUrl &url) {
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    const QString name = QFileInfo(path).fileName();
    if (!QFileInfo(path).isFile()) {
        setNotice(QStringLiteral("Can't find %1.").arg(name));
        return false;
    }

    std::unique_ptr<Poppler::Document> document = Poppler::Document::load(path);
    if (!document) {
        setNotice(QStringLiteral("%1 isn't a PDF Signed can read.").arg(name));
        return false;
    }
    if (document->isLocked()) {
        setNotice(QStringLiteral("%1 is password protected.").arg(name));
        return false;
    }
    // Saving goes through qpdf, so make sure it can read the file too before
    // anyone spends time signing it.
    QString error;
    if (Stamper::pageSizes(path, &error).isEmpty()) {
        setNotice(QStringLiteral("Can't sign %1: %2").arg(name, error));
        return false;
    }

    QList<QSizeF> sizes;
    for (int i = 0; i < document->numPages(); ++i) {
        std::unique_ptr<Poppler::Page> page = document->page(i);
        sizes.append(page ? page->pageSizeF() : QSizeF(612, 792));
    }
    if (sizes.isEmpty()) {
        setNotice(QStringLiteral("%1 has no pages.").arg(name));
        return false;
    }

    m_path = QFileInfo(path).absoluteFilePath();
    m_pageSizes = sizes;
    ++m_generation;
    if (m_pages)
        m_pages->setDocument(m_path, m_generation);
    m_items.clear();
    m_items.setPageSizes(sizes);
    m_savedRevision = m_items.revision();
    m_settings->setValue("folder", QFileInfo(m_path).absolutePath());
    setNotice({});
    emit documentChanged();
    emit dirtyChanged();
    return true;
}

void Backend::save() {
    if (!hasDocument() || !m_picker)
        return;
    const QFileInfo source(m_path);
    m_picker->savePdf(QUrl::fromLocalFile(source.absoluteDir().filePath(source.completeBaseName() + "-signed.pdf")));
}

bool Backend::saveTo(const QString &target) {
    if (!hasDocument() || target.isEmpty())
        return false;
    if (samePath(target, m_path)) {
        setNotice(QStringLiteral("Save the signed copy under a new name — the original stays as it is."));
        return false;
    }

    const int revision = m_items.revision();
    const Stamper::Result result = Stamper::stamp(m_path, target, m_items.stamps());
    if (!result.error.isEmpty()) {
        setNotice(QStringLiteral("Couldn't save: %1").arg(result.error));
        return false;
    }
    m_savedRevision = revision;
    emit dirtyChanged();
    emit saved();

    // The cryptographic layer is opt-in: only a signature someone gave a
    // certificate signs the copy, one pass per distinct certificate. A
    // failure never loses the save.
    QSet<QString> boundKeys;
    for (const Item &item : m_items.items()) {
        if (item.kind != Item::Signature)
            continue;
        const QString key = m_settings->value(QStringLiteral("signatureKey/") + m_signatureNames.value(item.imageId)).toString();
        if (!key.isEmpty())
            boundKeys.insert(key);
    }
    const QList<Signer::Certificate> available = boundKeys.isEmpty() ? QList<Signer::Certificate>() : Signer::certificates();
    QString failure;
    bool cancelled = false;
    QStringList signedWith;
    if (!boundKeys.isEmpty()) {
        QStringList ids = boundKeys.values();
        ids.sort();
        for (const QString &id : ids) {
            QString description = id;
            for (const Signer::Certificate &cert : available) {
                if (cert.nickname != id)
                    continue;
                description = cert.commonName.isEmpty() ? cert.nickname : cert.commonName;
                break;
            }
            const Signer::Result outcome = Signer::sign(target, id);
            if (!outcome.ok) {
                failure = outcome.error;
                cancelled = outcome.cancelled;
                break;
            }
            signedWith.append(description);
        }
    }

    // A failure outranks a partial success: a copy missing one of its
    // certificates must say so.
    if (cancelled)
        setNotice(QStringLiteral("Saved %1, but signing was cancelled.").arg(QFileInfo(target).fileName()));
    else if (!failure.isEmpty())
        setNotice(QStringLiteral("Saved %1, but signing failed: %2").arg(QFileInfo(target).fileName(), failure));
    else if (!signedWith.isEmpty())
        setNotice(QStringLiteral("Saved %1, signed with %2").arg(QFileInfo(target).fileName(), signedWith.join(QStringLiteral(", "))));
    else
        setNotice(result.warning.isEmpty() ? QStringLiteral("Saved %1").arg(QFileInfo(target).fileName()) : result.warning);
    return true;
}

QString Backend::lastSignature() const {
    const QString name = m_settings->value("lastSignature").toString();
    return m_signatures->contains(name) ? name : QString();
}

int Backend::placeSignature(const QString &name, int page, qreal centerX, qreal centerY) {
    const QImage image = m_signatures->image(name);
    if (image.isNull() || page < 0 || page >= pageCount()) {
        setNotice(QStringLiteral("Can't find the signature “%1”.").arg(name));
        return -1;
    }
    qreal width = m_settings->value("signatureWidth/" + name, DefaultSignatureWidth).toReal();
    width = std::min(width, pageWidth(page) * .8);
    const qreal height = width * image.height() / image.width();
    const int row = m_items.addSignature(page, image,
                                         QRectF(centerX - width / 2, centerY - height / 2, width, height));
    m_settings->setValue("lastSignature", name);
    m_signatureNames.insert(m_items.items().at(row).imageId, name);
    emit lastSignatureChanged();
    return row;
}

qreal Backend::textSize() const {
    return m_settings->value("textSize", DefaultTextSize).toReal();
}

int Backend::placeText(int page, qreal x, qreal centerY, const QString &text, bool fromLeft) {
    if (page < 0 || page >= pageCount())
        return -1;
    Item probe;
    probe.text = text;
    probe.fontSize = textSize();
    const QSizeF size = probe.size();
    const qreal left = fromLeft ? x : x - size.width() / 2;
    return m_items.addText(page, left, centerY - size.height() / 2, text, probe.fontSize);
}

// The locale's own short date, with the full year: a signed document should
// never be ambiguous about the century.
QString Backend::currentDate() {
    const QLocale locale = QLocale::system();
    QString format = locale.dateFormat(QLocale::ShortFormat);
    if (!format.contains(QLatin1String("yyyy")))
        format.replace(QLatin1String("yy"), QLatin1String("yyyy"));
    return locale.toString(QDate::currentDate(), format);
}

// Remembers how big the user likes each signature and their text, so the next
// placement starts there.
void Backend::rememberSize(int row) {
    if (row < 0 || row >= m_items.count())
        return;
    const Item &item = m_items.items().at(row);
    if (item.kind == Item::Text) {
        m_settings->setValue("textSize", item.fontSize);
        return;
    }
    const QString name = m_signatureNames.value(item.imageId);
    if (!name.isEmpty())
        m_settings->setValue("signatureWidth/" + name, item.width);
}

void Backend::chooseSignatureImage() {
    if (m_picker)
        m_picker->openImage(m_settings->value("imageFolder").toString());
}

bool Backend::loadSignatureImage(const QString &path) {
    QString error;
    const QImage image = SignatureLibrary::load(path, &error);
    if (image.isNull()) {
        setNotice(error);
        return false;
    }
    m_previewSource = image;
    m_previewName = m_signatures->count() == 0 ? QStringLiteral("Signature") : QString();
    updatePreview();
    emit signatureImageReady();
    return true;
}

void Backend::previewDrawing(const QVariantList &strokes) {
    QImage image(900, 360, QImage::Format_ARGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Qt::black, 6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    for (const QVariant &strokeValue : strokes) {
        QPainterPath path;
        bool started = false;
        for (const QVariant &pointValue : strokeValue.toList()) {
            const QVariantMap point = pointValue.toMap();
            const QPointF at(point.value(QStringLiteral("x")).toReal() * image.width(),
                             point.value(QStringLiteral("y")).toReal() * image.height());
            if (!started) {
                path.moveTo(at);
                started = true;
            } else {
                path.lineTo(at);
            }
        }
        if (started)
            painter.drawPath(path);
    }
    m_previewSource = image;
    if (m_previewName.isEmpty() && m_signatures->count() == 0)
        m_previewName = QStringLiteral("Signature");
    updatePreview();
    emit signatureImageReady();
}

void Backend::setPreviewThreshold(qreal threshold) {
    if (qFuzzyCompare(threshold, m_previewThreshold))
        return;
    m_previewThreshold = threshold;
    updatePreview();
}

void Backend::updatePreview() {
    m_preview = SignatureLibrary::removeBackground(m_previewSource, m_previewThreshold);
    ++m_previewRevision;
    emit previewChanged();
}

QImage Backend::previewImage(const QString &which) const {
    return which.startsWith(QLatin1String("source")) ? m_previewSource : m_preview;
}

QString Backend::saveSignature(const QString &name, const QString &organization, const QString &certId) {
    const QString error = m_signatures->add(name, m_preview);
    if (!error.isEmpty())
        return error;
    // A replaced signature should start at its new natural size.
    m_settings->remove("signatureWidth/" + name.trimmed());

    // The dialog sends a certificate id, "new" for a fresh self-signed one
    // made for this signature (Common Name = name, Organization =
    // organization), or nothing for no certificate at all.
    QString bound = certId;
    if (bound == QLatin1String("new")) {
        const QString failure = Signer::createCertificate(name, organization, &bound);
        if (!failure.isEmpty())
            return failure;
    }
    if (bound.isEmpty())
        m_settings->remove("signatureKey/" + name.trimmed());
    else
        m_settings->setValue("signatureKey/" + name.trimmed(), bound);
    m_settings->setValue("lastSignature", name.trimmed());
    emit lastSignatureChanged();
    m_previewSource = {};
    m_preview = {};
    ++m_previewRevision;
    emit previewChanged();
    return {};
}

void Backend::removeSignature(const QString &name) {
    m_signatures->remove(name);
    m_settings->remove("signatureWidth/" + name);
    m_settings->remove("signatureKey/" + name);
    emit lastSignatureChanged();
}

void Backend::importCertificate() {
    if (m_picker)
        m_picker->openCertificate(m_settings->value("certFolder").toString());
}

void Backend::importCertificatePassword(const QString &password) {
    // copy: import() clears m_pendingImport, which would reach this string
    const QString path = m_pendingImport;
    import(path, password);
}

void Backend::cancelImport() {
    m_pendingImport.clear();
}

void Backend::import(const QString &path, const QString &password) {
    if (path.isEmpty())
        return;
    m_pendingImport.clear();
    bool needsPassword = false;
    QString nickname;
    const QString error = Signer::importCertificate(path, password, &needsPassword, &nickname);
    if (error.isEmpty()) {
        emit certificateImported(nickname);
    } else if (needsPassword) {
        m_pendingImport = path;
        emit certificateImportNeedsPassword();
    } else {
        // A wrong password can be tried again.
        if (!password.isEmpty())
            m_pendingImport = path;
        emit certificateImportFailed(error);
    }
}

QVariantList Backend::signingCertificates() const {
    QVariantList out;
    for (const Signer::Certificate &cert : Signer::certificates())
        out.append(QVariantMap{
            {"id", cert.nickname},
            {"name", cert.commonName},
            {"organization", cert.organization},
        });
    return out;
}

QString Backend::signatureKey(const QString &name) const {
    return m_settings->value("signatureKey/" + name).toString();
}
