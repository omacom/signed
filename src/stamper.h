#pragma once

#include <QImage>
#include <QList>
#include <QRectF>
#include <QSizeF>
#include <QString>

// One thing to put on a page: a signature image or a block of text.
// Coordinates are PDF points from the top-left of the page as it's displayed,
// after the page's /Rotate.
struct Stamp {
    int page = 0;
    QRectF rect;        // image placement; for text only the top-left is used
    QImage image;       // set for a signature
    QString text;       // set for text
    qreal fontSize = 12;
};

// Writes a copy of a PDF with stamps laid over its pages. The original pages
// are kept as they are; the stamps are drawn into a separate overlay PDF and
// placed onto each page as a form XObject, so text stays text and nothing is
// re-rendered.
namespace Stamper {

struct Result {
    QString error;      // empty on success
    QString warning;    // qpdf recovered from something in the source
};

// The displayed size, in points, of every page — what poppler shows and what
// stamp coordinates are relative to. Empty with an error if it can't be read.
QList<QSizeF> pageSizes(const QString &source, QString *error = nullptr,
                       const QString &password = {});

Result stamp(const QString &source, const QString &target, const QList<Stamp> &stamps,
             const QString &password = {});

}
