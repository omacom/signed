#include "signer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <QRandomGenerator>
#include <QTemporaryFile>

#include <algorithm>
#include <cstdio>

#include <poppler-form.h>
#include <poppler-qt6.h>

#undef slots
#include <nss.h>
#include <pk11pub.h>

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

QString indexPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/signed/certificates.json");
}

QString markerPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/signed/certificate-store.json");
}

bool hasDatabase() {
    return QFileInfo::exists(databaseDir() + QStringLiteral("/key4.db"));
}

bool markedProtected() {
    QFile file(markerPath());
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    return object.value(QStringLiteral("protected")).toBool();
}

void writeMarker(bool protectedStore) {
    QDir().mkpath(QFileInfo(markerPath()).absolutePath());
    QSaveFile file(markerPath());
    if (!file.open(QIODevice::WriteOnly))
        return;
    const QJsonObject object{{QStringLiteral("version"), 1}, {QStringLiteral("protected"), protectedStore}};
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    file.commit();
}

// A password file keeps the password out of another process's argv. NSS and
// certutil both read the password as bytes, so use UTF-8 without a newline.
class PasswordFile {
public:
    explicit PasswordFile(const QString &password) {
        if (password.isEmpty())
            return;
        if (!m_file.open())
            return;
        m_file.write(password.toUtf8());
        m_file.close();
        m_ready = true;
    }

    bool isReady() const { return m_ready; }
    QString path() const { return m_file.fileName(); }

private:
    QTemporaryFile m_file{QDir::tempPath() + QStringLiteral("/signed-password-XXXXXX")};
    bool m_ready = false;
};

// Point poppler's NSS backend at the app database, creating it if asked. NSS
// keeps this state for the process, so it's set once. Nothing is written to
// disk until someone asks for a certificate (create is true): a person who
// never uses one never gets a database.
QString ensureBackend(bool create, const QString &password) {
    static bool done = false;
    if (done)
        return {};
    const QString dir = databaseDir();
    if (!hasDatabase()) {
        if (!create)
            return tr("there are no certificates yet");
        QDir().mkpath(dir);
        QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

        PasswordFile passwordFile(password);
        if (!password.isEmpty() && !passwordFile.isReady())
            return tr("couldn't create a temporary file");

        QStringList arguments{QStringLiteral("-N"), QStringLiteral("-d"), QStringLiteral("sql:") + dir};
        if (password.isEmpty())
            arguments << QStringLiteral("--empty-password");
        else
            arguments << QStringLiteral("-f") << passwordFile.path();

        QProcess process;
        process.start(QStringLiteral("certutil"), arguments);
        if (!process.waitForFinished(30000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
            return tr("the certificate database couldn't be created (is nss installed?)");
        QFile::remove(indexPath());
        writeMarker(!password.isEmpty());
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

QList<Certificate> readIndex() {
    QFile file(indexPath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonArray entries = root.value(QStringLiteral("certificates")).toArray();
    QList<Certificate> certificates;
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        Certificate certificate;
        certificate.nickname = entry.value(QStringLiteral("nickname")).toString();
        certificate.commonName = entry.value(QStringLiteral("commonName")).toString();
        certificate.organization = entry.value(QStringLiteral("organization")).toString();
        if (!certificate.nickname.isEmpty())
            certificates.append(certificate);
    }
    return certificates;
}

bool writeIndex(const QList<Certificate> &certificates) {
    if (!QDir().mkpath(QFileInfo(indexPath()).absolutePath()))
        return false;
    QJsonArray entries;
    for (const Certificate &certificate : certificates) {
        entries.append(QJsonObject{
            {QStringLiteral("nickname"), certificate.nickname},
            {QStringLiteral("commonName"), certificate.commonName},
            {QStringLiteral("organization"), certificate.organization},
        });
    }
    QSaveFile file(indexPath());
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QJsonObject root{{QStringLiteral("version"), 1}, {QStringLiteral("certificates"), entries}};
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (file.write(json) != json.size())
        return false;
    return file.commit();
}

// Serialize read-modify-write updates of the certificate index. The NSS
// database has its own locking; this covers the metadata file that protected
// stores use to list certificates while they are locked.
class IndexLock {
public:
    IndexLock() {
        QDir().mkpath(QFileInfo(indexPath()).absolutePath());
        m_lock.setStaleLockTime(10000);
        m_locked = m_lock.tryLock(2000);
    }
    ~IndexLock() {
        if (m_locked)
            m_lock.unlock();
    }
    bool isLocked() const { return m_locked; }

private:
    QLockFile m_lock{indexPath() + QStringLiteral(".lock")};
    bool m_locked = false;
};

QList<Certificate> certificatesFromDatabase() {
    if (!ensureBackend(false, {}).isEmpty())
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

QList<Certificate> refreshCertificates(bool *written = nullptr) {
    IndexLock lock;
    const QList<Certificate> certificates = certificatesFromDatabase();
    const bool saved = lock.isLocked() && writeIndex(certificates);
    if (written)
        *written = saved;
    return certificates;
}

bool rememberCertificate(const Certificate &certificate) {
    IndexLock lock;
    if (!lock.isLocked())
        return false;
    QList<Certificate> certificates = readIndex();
    certificates.erase(std::remove_if(certificates.begin(), certificates.end(),
                                      [&](const Certificate &c) { return c.nickname == certificate.nickname; }),
                       certificates.end());
    certificates.append(certificate);
    return writeIndex(certificates);
}

bool forgetCertificate(const QString &nickname) {
    IndexLock lock;
    if (!lock.isLocked())
        return false;
    QList<Certificate> certificates = readIndex();
    certificates.erase(std::remove_if(certificates.begin(), certificates.end(),
                                      [&](const Certificate &c) { return c.nickname == nickname; }),
                       certificates.end());
    return writeIndex(certificates);
}

}

StoreState storeState() {
    if (!hasDatabase())
        return StoreState::Absent;
    if (QFileInfo::exists(markerPath()))
        return markedProtected() ? StoreState::Protected : StoreState::Unprotected;

    // Stores made before the marker existed are normally unprotected. Check
    // the token once so an externally protected store is not mistaken for one
    // that can be opened without a password.
    if (ensureBackend(false, {}).isEmpty()) {
        PK11SlotInfo *slot = PK11_GetInternalKeySlot();
        if (slot) {
            const bool needsLogin = PK11_NeedLogin(slot) == PR_TRUE;
            PK11_FreeSlot(slot);
            writeMarker(needsLogin);
            return needsLogin ? StoreState::Protected : StoreState::Unprotected;
        }
    }
    return StoreState::Unprotected;
}

UnlockResult unlock(const QString &password, QString *error) {
    if (!hasDatabase())
        return UnlockResult::NoStore;
    if (const QString failure = ensureBackend(false, {}); !failure.isEmpty()) {
        if (error)
            *error = failure;
        return UnlockResult::Error;
    }
    if (storeState() != StoreState::Protected)
        return UnlockResult::Ok;

    PK11SlotInfo *slot = PK11_GetInternalKeySlot();
    if (!slot) {
        if (error)
            *error = tr("the passphrase didn't work");
        return UnlockResult::Error;
    }
    const SECStatus status = PK11_CheckUserPassword(slot, password.toUtf8().constData());
    PK11_FreeSlot(slot);
    if (status == SECSuccess) {
        // Rebuild the public metadata while the database is open. This also
        // repairs an index that was lost or could not be written earlier.
        refreshCertificates();
        return UnlockResult::Ok;
    }
    return UnlockResult::WrongPassword;
}

void lock() {
    PK11_LogoutAll();
}

QList<Certificate> certificates() {
    const StoreState state = storeState();
    if (state == StoreState::Absent)
        return {};
    if (state == StoreState::Protected)
        return readIndex();
    return refreshCertificates();
}

QString createCertificate(const QString &name, const QString &organization, QString *nickname,
                          const QString &storePassword) {
    const QString commonName = name.trimmed();
    if (commonName.isEmpty())
        return tr("A name is needed for the certificate.");
    const QString org = organization.trimmed().isEmpty() ? commonName : organization.trimmed();

    if (const QString failure = ensureBackend(true, storePassword); !failure.isEmpty())
        return failure;
    const bool protectedStore = storeState() == StoreState::Protected;
    if (protectedStore && storePassword.isEmpty())
        return tr("the passphrase didn't work");

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

    PasswordFile passwordFile(storePassword);
    if (protectedStore && !passwordFile.isReady())
        return tr("couldn't create a temporary file");

    const QString nick = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QStringList arguments{
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
    };
    if (protectedStore)
        arguments << QStringLiteral("-f") << passwordFile.path();

    QProcess process;
    process.start(QStringLiteral("certutil"), arguments);
    if (!process.waitForFinished(30000))
        return tr("the certificate couldn't be created.");
    QFile::remove(noisePath);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString output = QString::fromLocal8Bit(process.readAllStandardError().trimmed());
        return tr("the certificate couldn't be created: %1").arg(output);
    }

    if (!rememberCertificate({nick, commonName, org})) {
        bool written = false;
        if (protectedStore) {
            if (unlock(storePassword) == UnlockResult::Ok) {
                refreshCertificates(&written);
                lock();
            }
        } else {
            refreshCertificates(&written);
        }
        if (!written) {
            // The certificate database changed but the selection list could
            // not be updated. Remove the new certificate so the operation is
            // all-or-nothing, and report the failure.
            removeCertificate(nick, storePassword);
            return tr("the certificate couldn't be created.");
        }
    }
    if (nickname)
        *nickname = nick;
    return {};
}

bool removeCertificate(const QString &nickname, const QString &storePassword) {
    if (!hasDatabase())
        return false;
    const bool protectedStore = storeState() == StoreState::Protected;
    if (protectedStore && storePassword.isEmpty())
        return false;

    PasswordFile passwordFile(storePassword);
    if (protectedStore && !passwordFile.isReady())
        return false;

    QStringList arguments{QStringLiteral("-F"), QStringLiteral("-d"), QStringLiteral("sql:") + databaseDir(),
                          QStringLiteral("-n"), nickname};
    if (protectedStore)
        arguments << QStringLiteral("-f") << passwordFile.path();

    QProcess process;
    process.start(QStringLiteral("certutil"), arguments);
    if (!process.waitForFinished(30000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return false;
    forgetCertificate(nickname);
    return true;
}

QString importCertificate(const QString &path, const QString &password, bool *needsPassword, QString *nickname,
                          const QString &storePassword) {
    if (const QString failure = ensureBackend(true, storePassword); !failure.isEmpty())
        return failure;

    const bool protectedStore = storeState() == StoreState::Protected;
    if (protectedStore && storePassword.isEmpty())
        return tr("the passphrase didn't work");

    bool unlocked = false;
    if (protectedStore) {
        const UnlockResult result = unlock(storePassword);
        if (result != UnlockResult::Ok)
            return tr("the passphrase didn't work");
        unlocked = true;
    }

    const QList<Certificate> before = certificates();

    PasswordFile storePasswordFile(storePassword);
    if (protectedStore && !storePasswordFile.isReady()) {
        if (unlocked)
            lock();
        return tr("couldn't create a temporary file");
    }

    // A password goes through a private file rather than the command line,
    // where any process could read it from /proc while pk12util runs.
    QTemporaryFile passwordFile;
    QStringList passwordArgs{QStringLiteral("-W"), QString()};
    if (!password.isEmpty()) {
        if (!passwordFile.open()) {
            if (unlocked)
                lock();
            return tr("couldn't create a temporary file");
        }
        passwordFile.write(password.toUtf8());
        passwordFile.close();
        passwordArgs = {QStringLiteral("-w"), passwordFile.fileName()};
    }

    QStringList arguments{QStringLiteral("-i"), path, QStringLiteral("-d"), QStringLiteral("sql:") + databaseDir()};
    if (protectedStore)
        arguments << QStringLiteral("-k") << storePasswordFile.path();
    else
        arguments << QStringLiteral("-K") << QString();
    arguments << passwordArgs;

    QProcess process;
    process.start(QStringLiteral("pk12util"), arguments);
    if (!process.waitForFinished(30000)) {
        if (unlocked)
            lock();
        return tr("the certificate couldn't be imported.");
    }

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (needsPassword)
            *needsPassword = password.isEmpty();
        const QString output = QString::fromLocal8Bit(process.readAllStandardError().trimmed());
        if (unlocked)
            lock();
        return tr("import failed (exit %1, file %2): %3")
            .arg(process.exitCode()).arg(path, output);
    }

    // The nickname comes from the file; find what's new since the import.
    const QList<Certificate> after = refreshCertificates();
    for (const Certificate &cert : after) {
        const bool known = std::any_of(before.cbegin(), before.cend(),
                                       [&](const Certificate &c) { return c.nickname == cert.nickname; });
        if (!known) {
            if (nickname)
                *nickname = cert.nickname;
            if (unlocked)
                lock();
            return {};
        }
    }
    if (unlocked)
        lock();
    return tr("the certificate is already in the database.");
}

Result sign(const QString &path, const QString &certNickname) {
    if (const QString failure = ensureBackend(false, {}); !failure.isEmpty())
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
