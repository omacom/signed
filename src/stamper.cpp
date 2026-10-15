#include "stamper.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <cmath>
#include <cstdio>
#include <exception>

#include "textlayout.h"

namespace {

// Where a page's visible area sits in its own user space, and how it's turned
// for display. Mirrors what poppler shows: the CropBox (clipped to the
// MediaBox), rotated clockwise by /Rotate.
struct Geometry {
    double llx = 0, lly = 0, urx = 0, ury = 0;
    int rotate = 0;

    double width() const { return urx - llx; }
    double height() const { return ury - lly; }
    QSizeF displaySize() const {
        return rotate % 180 ? QSizeF(height(), width()) : QSizeF(width(), height());
    }
};

QPDFObjectHandle::Rectangle rectangle(QPDFObjectHandle box) {
    if (box.isRectangle()) {
        const auto r = box.getArrayAsRectangle();
        return {std::min(r.llx, r.urx), std::min(r.lly, r.ury), std::max(r.llx, r.urx), std::max(r.lly, r.ury)};
    }
    return {0, 0, 612, 792};
}

Geometry geometry(QPDFPageObjectHelper &page) {
    const auto media = rectangle(page.getMediaBox());
    const auto crop = rectangle(page.getCropBox());

    Geometry g;
    g.llx = std::max(media.llx, crop.llx);
    g.lly = std::max(media.lly, crop.lly);
    g.urx = std::min(media.urx, crop.urx);
    g.ury = std::min(media.ury, crop.ury);
    if (g.urx <= g.llx || g.ury <= g.lly) {
        g.llx = media.llx; g.lly = media.lly; g.urx = media.urx; g.ury = media.ury;
    }

    const auto rotate = page.getAttribute("/Rotate", false);
    if (rotate.isInteger()) {
        const int degrees = ((rotate.getIntValueAsInt() % 360) + 360) % 360;
        if (degrees % 90 == 0)
            g.rotate = degrees;
    }
    return g;
}

// The cm matrix taking overlay user space (a page overlayHeight points tall,
// drawn top-down by Qt) onto the source page so that the overlay's top-left is
// the displayed page's top-left. Display (dx, dy) is overlay (ox, H - oy);
// each case undoes the page's clockwise /Rotate.
QString placement(const Geometry &g, double overlayHeight) {
    double a, b, c, d, e, f;
    const double h = overlayHeight;
    switch (g.rotate) {
    case 90:  a = 0;  b = 1;  c = -1; d = 0;  e = g.llx + h; f = g.lly;     break;
    case 180: a = -1; b = 0;  c = 0;  d = -1; e = g.urx;     f = g.lly + h; break;
    case 270: a = 0;  b = -1; c = 1;  d = 0;  e = g.urx - h; f = g.ury;     break;
    default:  a = 1;  b = 0;  c = 0;  d = 1;  e = g.llx;     f = g.ury - h; break;
    }
    auto n = [](double v) { return QString::number(v, 'f', 4); };
    return QStringLiteral("%1 %2 %3 %4 %5 %6").arg(n(a), n(b), n(c), n(d), n(e), n(f));
}

QList<Geometry> readGeometry(QPDF &pdf) {
    QList<Geometry> result;
    for (auto &page : QPDFPageDocumentHelper(pdf).getAllPages())
        result.append(geometry(page));
    return result;
}

void openPdf(QPDF &pdf, const QString &path, const QString &password) {
    pdf.setSuppressWarnings(true);
    const QByteArray encoded = password.toUtf8();
    pdf.processFile(QFile::encodeName(path).constData(),
                    password.isEmpty() ? nullptr : encoded.constData());
}

QString describe(const std::exception &e) {
    const QString message = QString::fromUtf8(e.what());
    if (message.contains(QLatin1String("password"), Qt::CaseInsensitive))
        return QStringLiteral("This PDF is password protected.");
    return message;
}

// Draws the stamps into a fresh PDF, one page per stamped source page, sized
// to that page's displayed size. QPdfWriter at 72 DPI makes painter units
// PDF points. Returns the source page index of each overlay page.
QList<int> writeOverlay(const QString &path, const QList<Geometry> &pages, const QList<Stamp> &stamps) {
    QMap<int, QList<Stamp>> byPage;
    for (const Stamp &stamp : stamps)
        if (stamp.page >= 0 && stamp.page < pages.size())
            byPage[stamp.page].append(stamp);
    if (byPage.isEmpty())
        return {};

    // Page sizes are rounded up to whole points (QPageSize keeps points as
    // integers); the placement reads the overlay's real height back, so the
    // extra fraction just hangs off the bottom, outside the page.
    auto pageSize = [&](int page) {
        const QSizeF size = pages.at(page).displaySize();
        return QPageSize(QSizeF(std::ceil(size.width()), std::ceil(size.height())), QPageSize::Point,
                         QString(), QPageSize::ExactMatch);
    };

    QPdfWriter writer(path);
    writer.setResolution(72);
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));
    writer.setPageSize(pageSize(byPage.firstKey()));
    writer.setCreator(QStringLiteral("Signed"));

    QPainter painter(&writer);
    if (!painter.isActive())
        throw std::runtime_error("Could not write the signature overlay.");
    painter.setRenderHint(QPainter::LosslessImageRendering);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    QList<int> order;
    for (auto it = byPage.cbegin(); it != byPage.cend(); ++it) {
        if (!order.isEmpty()) {
            writer.setPageSize(pageSize(it.key()));
            if (!writer.newPage())
                throw std::runtime_error("Could not add a page to the signature overlay.");
        }
        order.append(it.key());
        for (const Stamp &stamp : it.value()) {
            if (!stamp.image.isNull())
                painter.drawImage(stamp.rect, stamp.image);
            else if (!stamp.text.isEmpty())
                TextLayout::paint(painter, stamp.rect.topLeft(), stamp.text, stamp.fontSize);
        }
    }
    painter.end();
    return order;
}

}

namespace Stamper {

QList<QSizeF> pageSizes(const QString &source, QString *error, const QString &password) {
    try {
        QPDF pdf;
        openPdf(pdf, source, password);
        QList<QSizeF> sizes;
        for (const Geometry &g : readGeometry(pdf))
            sizes.append(g.displaySize());
        return sizes;
    } catch (const std::exception &e) {
        if (error)
            *error = describe(e);
        return {};
    }
}

Result stamp(const QString &source, const QString &target, const QList<Stamp> &stamps,
             const QString &password) {
    Result result;
    const QFileInfo targetInfo(target);
    QTemporaryDir work;
    if (!work.isValid())
        return {QStringLiteral("Could not create a temporary directory."), {}};

    // The finished file is written beside the target and renamed over it, so a
    // failure never leaves a half-written or clobbered file behind.
    QTemporaryFile output(targetInfo.absoluteDir().filePath(QStringLiteral(".%1.XXXXXX").arg(targetInfo.fileName())));
    if (!output.open())
        return {QStringLiteral("Can't write to %1.").arg(targetInfo.absolutePath()), {}};
    output.close();

    try {
        QPDF pdf;
        openPdf(pdf, source, password);
        pdf.pushInheritedAttributesToPage();
        auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
        QList<Geometry> geometries;
        for (auto &page : pages)
            geometries.append(geometry(page));

        const QString overlayPath = work.filePath(QStringLiteral("overlay.pdf"));
        const QList<int> order = writeOverlay(overlayPath, geometries, stamps);

        QPDF overlay;
        if (!order.isEmpty())
            openPdf(overlay, overlayPath);
        auto overlayPages = order.isEmpty() ? std::vector<QPDFPageObjectHelper>()
                                            : QPDFPageDocumentHelper(overlay).getAllPages();

        for (int i = 0; i < order.size(); ++i) {
            const int index = order.at(i);
            QPDFPageObjectHelper &page = pages.at(index);
            QPDFPageObjectHelper &overlayPage = overlayPages.at(i);
            const double overlayHeight = rectangle(overlayPage.getMediaBox()).ury - rectangle(overlayPage.getMediaBox()).lly;

            QPDFObjectHandle form = pdf.copyForeignObject(overlayPage.getFormXObjectForPage(false));

            // Resources may be shared between pages; give this page its own
            // dictionaries before adding the overlay to them.
            QPDFObjectHandle pageObject = page.getObjectHandle();
            QPDFObjectHandle resources = pageObject.getKey("/Resources");
            resources = resources.isDictionary() ? resources.shallowCopy() : QPDFObjectHandle::newDictionary();
            QPDFObjectHandle xobjects = resources.getKey("/XObject");
            xobjects = xobjects.isDictionary() ? xobjects.shallowCopy() : QPDFObjectHandle::newDictionary();
            int suffix = 0;
            const std::string name = resources.getUniqueResourceName("/SigFx", suffix);
            xobjects.replaceKey(name, form);
            resources.replaceKey("/XObject", xobjects);
            pageObject.replaceKey("/Resources", resources);

            // Wrapping the original content in q … Q means a page that leaves
            // the graphics state changed can't move or skew the overlay.
            page.addPageContents(QPDFObjectHandle::newStream(&pdf, "q\n"), true);
            const QString content = QStringLiteral("\nQ\nq %1 cm %2 Do Q\n")
                                        .arg(placement(geometries.at(index), overlayHeight),
                                             QString::fromStdString(name));
            page.addPageContents(QPDFObjectHandle::newStream(&pdf, content.toStdString()), false);
        }

        QPDFWriter writer(pdf, QFile::encodeName(output.fileName()).constData());
        writer.write();

        if (pdf.anyWarnings())
            result.warning = QStringLiteral("The original PDF had problems that were repaired in the signed copy.");
    } catch (const std::exception &e) {
        QFile::remove(output.fileName());
        return {describe(e), {}};
    }

    if (std::rename(QFile::encodeName(output.fileName()).constData(), QFile::encodeName(target).constData()) != 0) {
        QFile::remove(output.fileName());
        return {QStringLiteral("Could not save %1.").arg(targetInfo.fileName()), {}};
    }
    output.setAutoRemove(false);
    QFile(target).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                 QFileDevice::ReadGroup | QFileDevice::ReadOther);
    return result;
}

}
