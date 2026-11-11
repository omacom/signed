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

// The signing certificates of the app's database.
QList<Certificate> certificates();

// Generates a self-signed certificate with an unprotected private key (so
// signing never prompts) and registers it in the app database. An empty
// organization falls back to the name. Returns "" and the nickname on
// success, an error message otherwise.
QString createCertificate(const QString &name, const QString &organization, QString *nickname = nullptr);

// Deletes a certificate and its key from the app database.
bool removeCertificate(const QString &nickname);

// Imports an existing PKCS#12 file (.p12/.pfx, the export format of browsers
// and CA tools) into the app database. The file's password is the only
// password in this feature; a missing/wrong one reports needsPassword.
// Returns "" and the imported certificate's nickname on success, an error
// message otherwise.
QString importCertificate(const QString &path, const QString &password,
                          bool *needsPassword = nullptr, QString *nickname = nullptr);

struct Result {
    bool ok = false;
    bool cancelled = false;
    QString error;       // empty on success
};

// Appends an invisible PKCS#7 signature to the PDF at path. Signs to a temp
// sibling file, then renames over path.
Result sign(const QString &path, const QString &certNickname);

}
