#pragma once

#include <QObject>
#include <QUrl>

// Open and save dialogs, behind an interface so tests can stand in for the
// desktop portal.
class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    virtual void openPdf(const QString &folder) = 0;
    virtual void openImage(const QString &folder) = 0;
    virtual void openCertificate(const QString &folder) = 0;
    virtual void savePdf(const QUrl &suggestedUrl) = 0;

signals:
    void pdfSelected(const QUrl &url);
    void imageSelected(const QUrl &url);
    void p12Selected(const QUrl &url);
    void saveSelected(const QUrl &url);
    void cancelled();
    void failed(const QString &message);
};
