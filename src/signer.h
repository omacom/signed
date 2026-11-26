#pragma once

#include <QList>
#include <QString>

// The X.509/PKCS#7 side of signing: a self-signed certificate per signature,
// in an app-owned NSS database, and the Poppler signing step. The visual
// signature is the stamp already on the page; this is the cryptographic layer
// over it, in the format every PDF viewer understands.
namespace Signer {

struct Certificate {
    QString nickname;    // unique id in the database, what certNickname wants
    QString commonName;
    QString organization;
};

enum class StoreState { Absent, Unprotected, Protected };
enum class UnlockResult { Ok, WrongPassword, NoStore, Error };

// Whether the app's certificate store exists and, if so, whether its private
// keys need a password. The answer is stable while the store is locked; the
// store is locked after every signing pass.
StoreState storeState();

// Validates and unlocks the store for one signing pass. lock() must be called
// afterwards, including after a failure.
UnlockResult unlock(const QString &password, QString *error = nullptr);
void lock();

// The signing certificates of the app's database. This does not prompt and
// works while the store is locked.
QList<Certificate> certificates();

// Generates a self-signed certificate. An empty organization falls back to
// the name. storePassword is used when creating a new protected store, or
// when adding to an existing protected one; an empty password creates or uses
// an unprotected store.
QString createCertificate(const QString &name, const QString &organization, QString *nickname = nullptr,
                          const QString &storePassword = {});

// Deletes a certificate and its key from the app database.
bool removeCertificate(const QString &nickname, const QString &storePassword = {});

// Imports an existing PKCS#12 file (.p12/.pfx, the export format of browsers
// and CA tools) into the app database. password is the file's own password.
// storePassword is used when creating a protected store or adding to one. A
// missing/wrong file password reports needsPassword.
QString importCertificate(const QString &path, const QString &password, bool *needsPassword = nullptr,
                          QString *nickname = nullptr, const QString &storePassword = {});

struct Result {
    bool ok = false;
    bool cancelled = false;
    QString error;       // empty on success
};

// Appends an invisible PKCS#7 signature to the PDF at path. Signs to a temp
// sibling file, then renames over path. A protected store must be unlocked
// first with unlock().
Result sign(const QString &path, const QString &certNickname);

}
