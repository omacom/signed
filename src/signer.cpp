#include "signer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUuid>
#include <QRandomGenerator>
#include <QTemporaryFile>

#include <algorithm>
#include <cstdio>

#include <poppler-form.h>
#include <poppler-qt6.h>

namespace Signer {

namespace {

QString tr(const char *text) {
    return QCoreApplication::translate("Signer", text);
}

// The app-owned NSS database, next to the signatures directory.
QString databaseDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/signed/certificates");
}

bool hasDatabase() {
    return QFileInfo::exists(databaseDir() + QStringLiteral("/key4.db"));
}
// Point poppler's NSS backend at the app database. NSS keeps this state for
// the process, so it's set once. Nothing is written to disk until someone
// asks for a certificate (create is true): a person who never uses one never
// gets a database.
QString ensureBackend(bool create) {
    static bool done = false;
    if (done)
        return {};
    const QString dir = databaseDir();
    if (!hasDatabase()) {
        if (!create)
            return tr("there are no certificates yet");
        QDir().mkpath(dir);
        QProcess process;
        process.start(QStringLiteral("certutil"), {QStringLiteral("-N"), QStringLiteral("-d"),
                                                   QStringLiteral("sql:") + dir, QStringLiteral("--empty-password")});
        if (!process.waitForFinished(30000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
            return tr("the certificate database couldn't be created (is nss installed?)");
    }
    Poppler::setNSSDir(dir);
    Poppler::setActiveCryptoSignBackend(Poppler::CryptoSignBackend::NSS);
    done = true;
    return {};
}

// A subject attribute's value as the hex of a DER UTF8String, which certutil
// takes as is, so a comma, quote, or leading # in a name stays part of it.
QString subjectValue(const QString &value) {
    const QByteArray text = value.toUtf8();
    QByteArray length;
    for (qsizetype n = text.size(); n > 0 || length.isEmpty(); n >>= 8)
        length.prepend(char(n & 0xff));
    if (text.size() >= 0x80)
        length.prepend(char(0x80 | length.size()));
    return QLatin1Char('#') + QString::fromLatin1((QByteArray(1, '\x0c') + length + text).toHex());
}
}
QList<Certificate> certificates() {
    if (!ensureBackend(false).isEmpty())
        return {};
    QList<Certificate> out;
    const QVector<Poppler::CertificateInfo> certs = Poppler::getAvailableSigningCertificates();
    for (const Poppler::CertificateInfo &cert : certs) {
        Certificate entry;
        entry.nickname = cert.nickName();
        entry.commonName = cert.subjectInfo(Poppler::CertificateInfo::EntityInfoKey::CommonName);
        entry.organization = cert.subjectInfo(Poppler::CertificateInfo::EntityInfoKey::Organization);
        out.append(entry);
    }
    return out;
}

QString createCertificate(const QString &name, const QString &organization, QString *nickname) {
    const QString commonName = name.trimmed();
    if (commonName.isEmpty())
        return tr("A name is needed for the certificate.");
    const QString org = organization.trimmed().isEmpty() ? commonName : organization.trimmed();

    if (const QString failure = ensureBackend(true); !failure.isEmpty())
        return failure;
    const QString dir = databaseDir();

    // certutil needs a seed file for key generation; the nickname is a unique
    // id (the Common Name is what viewers show).
    QString noisePath;
    QTemporaryFile noise(QDir::tempPath() + QStringLiteral("/signed-noise-XXXXXX"));
    if (noise.open()) {
        noise.write(QUuid::createUuid().toRfc4122() + QUuid::createUuid().toRfc4122());
        noisePath = noise.fileName();
        noise.close();
    }
    if (noisePath.isEmpty())
        return tr("couldn't create a temporary file");

    const QString nick = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QProcess process;
    process.start(QStringLiteral("certutil"), {
        QStringLiteral("-S"), QStringLiteral("-d"), QStringLiteral("sql:") + dir,
        QStringLiteral("-n"), nick, QStringLiteral("-x"), QStringLiteral("-t"), QStringLiteral("CT,,"),
        // self-signed certs all share the same issuer (themselves), so the
        // serial must differ between them
        QStringLiteral("-m"), QString::number(QRandomGenerator::global()->generate64() % 999999999),
        QStringLiteral("-s"), QStringLiteral("CN=%1, O=%2").arg(subjectValue(commonName), subjectValue(org)),
        QStringLiteral("-k"), QStringLiteral("rsa"), QStringLiteral("-g"), QStringLiteral("2048"),
        QStringLiteral("-v"), QStringLiteral("1200"), QStringLiteral("-Z"), QStringLiteral("SHA256"),
        QStringLiteral("--keyUsage"), QStringLiteral("digitalSignature,nonRepudiation"),
        QStringLiteral("-z"), noisePath,
    });
    if (!process.waitForFinished(30000))
        return tr("the certificate couldn't be created.");
    QFile::remove(noisePath);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString output = QString::fromLocal8Bit(process.readAllStandardError().trimmed());
        return tr("the certificate couldn't be created: %1").arg(output);
    }

    if (nickname)
        *nickname = nick;
    return {};
}

bool removeCertificate(const QString &nickname) {
    if (!hasDatabase())
        return false;
    QProcess process;
    process.start(QStringLiteral("certutil"), {QStringLiteral("-F"), QStringLiteral("-d"),
                                               QStringLiteral("sql:") + databaseDir(), QStringLiteral("-n"), nickname});
    return process.waitForFinished(30000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

QString importCertificate(const QString &path, const QString &password,
                          bool *needsPassword, QString *nickname) {
    if (const QString failure = ensureBackend(true); !failure.isEmpty())
        return failure;

    const QList<Certificate> before = certificates();

    // A password goes through a private file rather than the command line,
    // where any process could read it from /proc while pk12util runs.
    QTemporaryFile passwordFile;
    QStringList passwordArgs{QStringLiteral("-W"), QString()};
    if (!password.isEmpty()) {
        if (!passwordFile.open())
            return tr("couldn't create a temporary file");
        passwordFile.write(password.toUtf8());
        passwordFile.close();
        passwordArgs = {QStringLiteral("-w"), passwordFile.fileName()};
    }
    QProcess process;
    process.start(QStringLiteral("pk12util"), QStringList{
        QStringLiteral("-i"), path,
        QStringLiteral("-d"), QStringLiteral("sql:") + databaseDir(),
        QStringLiteral("-K"), QString(),
    } + passwordArgs);
    if (!process.waitForFinished(30000))
        return tr("the certificate couldn't be imported.");

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (needsPassword)
            *needsPassword = password.isEmpty();
        const QString output = QString::fromLocal8Bit(process.readAllStandardError().trimmed());
        return tr("import failed (exit %1, file %2): %3")
            .arg(process.exitCode()).arg(path, output);
    }

    // The nickname comes from the file; find what's new since the import.
    const QList<Certificate> after = certificates();
    for (const Certificate &cert : after) {
        const bool known = std::any_of(before.cbegin(), before.cend(),
                                       [&](const Certificate &c) { return c.nickname == cert.nickname; });
        if (!known) {
            if (nickname)
                *nickname = cert.nickname;
            return {};
        }
    }
    return tr("the certificate is already in the database.");
}

Result sign(const QString &path, const QString &certNickname) {
    if (const QString failure = ensureBackend(false); !failure.isEmpty())
        return {false, false, failure};

    std::unique_ptr<Poppler::Document> document = Poppler::Document::load(path);
    if (!document)
        return {false, false, tr("the PDF couldn't be reopened for signing")};
    std::unique_ptr<Poppler::PDFConverter> converter = document->pdfConverter();
    if (!converter)
        return {false, false, tr("this poppler can't sign")};

    // poppler signs to a new file; a temp sibling keeps a failure from
    // touching what's already there.
    QTemporaryFile temp(QFileInfo(path).absolutePath() + QStringLiteral("/.signed-XXXXXX.pdf"));
    temp.setAutoRemove(false);
    if (!temp.open())
        return {false, false, tr("couldn't create a temporary file")};
    const QString tempName = temp.fileName();
    temp.close();

    // A signature field with no area and no text renders nothing: the
    // signature is in the file, not on the page. The placed stamp is the
    // visual part.
    Poppler::PDFConverter::NewSignatureData data;
    data.setCertNickname(certNickname);
    data.setPage(0);
    data.setBoundingRectangle(QRectF());
    data.setSignatureText(QStringLiteral(" "));
    data.setFontSize(10);
    data.setBorderWidth(0);
    converter->setOutputFileName(tempName);

    if (!converter->sign(data)) {
        QFile::remove(tempName);
        using SR = Poppler::PDFConverter::SigningResult;
        switch (converter->lastSigningResult()) {
        case SR::UserCancelled:
            return {false, true, tr("signing was cancelled")};
        case SR::KeyMissing:
            return {false, false, tr("the certificate is no longer in the database")};
        case SR::BadPassphrase:
            return {false, false, tr("the passphrase didn't work")};
        case SR::WriteFailed:
            return {false, false, tr("the file couldn't be written")};
        default: {
            const Poppler::ErrorString details = converter->lastSigningErrorDetails();
            if (details.type == Poppler::ErrorStringType::UserString && !details.data.toString().isEmpty())
                return {false, false, details.data.toString()};
            return {false, false, tr("signing failed")};
        }
        }
    }

    // The temp file was created private; the signed copy keeps the
    // permissions the unsigned one had.
    QFile::setPermissions(tempName, QFile::permissions(path));

    // Swap the signed temp file into place in one step, so a crash leaves
    // either the unsigned copy or the signed one, never neither.
    if (std::rename(QFile::encodeName(tempName).constData(), QFile::encodeName(path).constData()) != 0) {
        QFile::remove(tempName);
        return {false, false, tr("the signed file couldn't replace the original")};
    }
    return {true, false, {}};
}

}
