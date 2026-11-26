#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>

#include <algorithm>
#include <QPdfWriter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <poppler-form.h>
#include <poppler-qt6.h>

#include "backend.h"
#include "items.h"
#include "signatures.h"
#include "signer.h"
#include "stamper.h"

namespace {

void writeBasePdf(const QString &path) {
    QPdfWriter writer(path);
    writer.setResolution(72);
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));
    writer.setPageSize(QPageSize(QPageSize::Letter));
    QPainter painter(&writer);
    painter.drawText(QPointF(60, 120), QStringLiteral("Original text"));
    painter.end();
}

}

class PasswordTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_tempData;
    QTemporaryDir m_tempConfig;
    QString m_dataHome;
    QString m_configHome;

private slots:
    void initTestCase() {
        const QByteArray inheritedData = qgetenv("SIGNED_TEST_DATA_HOME");
        const QByteArray inheritedConfig = qgetenv("SIGNED_TEST_CONFIG_HOME");
        if (!inheritedData.isEmpty() && !inheritedConfig.isEmpty()) {
            m_dataHome = QString::fromLocal8Bit(inheritedData);
            m_configHome = QString::fromLocal8Bit(inheritedConfig);
            QVERIFY(QFileInfo(m_dataHome).isDir());
            QVERIFY(QFileInfo(m_configHome).isDir());
        } else {
            QVERIFY(m_tempData.isValid());
            QVERIFY(m_tempConfig.isValid());
            m_dataHome = QDir(m_tempData.path()).filePath("data");
            m_configHome = m_tempConfig.path();
            QVERIFY(QDir().mkpath(m_dataHome));
        }
        qputenv("XDG_DATA_HOME", m_dataHome.toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", m_configHome.toLocal8Bit());

        QSettings settings;
        if (!settings.contains("certificateStore/passwordProtected"))
            settings.setValue("certificateStore/passwordProtected", true);
    }

    void protectedStoreSignsWithPassword() {
        QCOMPARE(Signer::storeState(), Signer::StoreState::Absent);

        const QString password = QStringLiteral("a long enough password");
        QString nickname;
        const QString error = Signer::createCertificate(QStringLiteral("Protected User"), QString(), &nickname, password);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!nickname.isEmpty());
        QCOMPARE(Signer::storeState(), Signer::StoreState::Protected);

        const QList<Signer::Certificate> certificates = Signer::certificates();
        QCOMPARE(certificates.size(), 1);
        QCOMPARE(certificates.at(0).commonName, QStringLiteral("Protected User"));

        QTemporaryDir dir;
        const QString source = dir.filePath("lease.pdf");
        const QString target = dir.filePath("signed.pdf");
        writeBasePdf(source);
        QVERIFY(Stamper::stamp(source, target, {}).error.isEmpty());

        QVERIFY(!Signer::sign(target, nickname).ok);
        QCOMPARE(Signer::unlock(QStringLiteral("wrong password")), Signer::UnlockResult::WrongPassword);
        Signer::lock();
        QCOMPARE(Signer::unlock(password), Signer::UnlockResult::Ok);
        const Signer::Result result = Signer::sign(target, nickname);
        Signer::lock();
        QVERIFY2(result.ok, qPrintable(result.error));

        std::unique_ptr<Poppler::Document> document = Poppler::Document::load(target);
        QVERIFY(document);
        QCOMPARE(document->signatures().size(), 1);
        const auto signatures = document->signatures();
        QCOMPARE(signatures.at(0)->validateAsync(Poppler::FormFieldSignature::ValidateOptions()).first.signatureStatus(),
                 Poppler::SignatureValidationInfo::SignatureValid);
        signatures.at(0)->validateResult();
    }

    void importingIntoProtectedStore() {
        if (QStandardPaths::findExecutable("openssl").isEmpty())
            QSKIP("openssl is needed for the import test");

        QTemporaryDir dir;
        const QString key = dir.filePath("key.pem");
        const QString certificate = dir.filePath("cert.pem");
        const QString bundle = dir.filePath("bundle.p12");
        QCOMPARE(QProcess::execute("openssl", {"req", "-x509", "-newkey", "rsa:2048", "-nodes",
                                               "-keyout", key, "-out", certificate, "-days", "900",
                                               "-subj", "/CN=Protected Import/O=Tests"}), 0);
        QCOMPARE(QProcess::execute("openssl", {"pkcs12", "-export", "-in", certificate, "-inkey", key,
                                               "-out", bundle, "-passout", "pass:filepass"}), 0);

        const QString storePassword = QStringLiteral("a long enough password");
        bool needsPassword = false;
        QString nickname;
        QVERIFY(!Signer::importCertificate(bundle, QStringLiteral("wrong"), &needsPassword, &nickname,
                                           storePassword).isEmpty());
        QVERIFY(!needsPassword);
        QCOMPARE(Signer::importCertificate(bundle, QStringLiteral("filepass"), &needsPassword, &nickname,
                                           storePassword), QString());
        QVERIFY(!nickname.isEmpty());
        const QList<Signer::Certificate> certificates = Signer::certificates();
        QVERIFY(std::any_of(certificates.cbegin(), certificates.cend(),
                            [&](const Signer::Certificate &cert) { return cert.nickname == nickname; }));
    }

    void savingAsksForTheStorePassword() {
        QSettings settings;
        settings.setValue("certificateStore/passwordProtected", true);

        const QString password = QStringLiteral("a long enough password");
        QString nickname;
        QCOMPARE(Signer::createCertificate(QStringLiteral("Second Protected User"), QString(), &nickname, password), QString());

        QTemporaryDir dir;
        const QString source = dir.filePath("lease.pdf");
        writeBasePdf(source);

        SignatureLibrary library(dir.filePath("signatures"));
        QImage image(60, 30, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QCOMPARE(library.add("David", image), QString());

        Backend backend(nullptr, nullptr, &library, &settings);
        QVERIFY(backend.openUrl(QUrl::fromLocalFile(source)));
        QVERIFY(backend.placeSignature("David", 0, 300, 400) >= 0);
        settings.setValue("signatureKey/David", nickname);

        QSignalSpy required(&backend, &Backend::certificatePasswordRequired);
        QSignalSpy rejected(&backend, &Backend::certificatePasswordRejected);
        const QString target = dir.filePath("needs-password.pdf");
        QVERIFY(!backend.saveTo(target));
        QCOMPARE(required.count(), 1);
        QVERIFY(!QFile::exists(target));

        backend.saveWithPassword(QStringLiteral("wrong password"));
        QCOMPARE(rejected.count(), 1);
        QVERIFY(!QFile::exists(target));

        backend.saveWithPassword(password);
        std::unique_ptr<Poppler::Document> document = Poppler::Document::load(target);
        QVERIFY(document);
        QCOMPARE(document->signatures().size(), 1);
    }

    void reopeningInANewProcess() {
        if (!qgetenv("SIGNED_TEST_REOPEN").isEmpty())
            QSKIP("this is the child process");
        if (Signer::storeState() != Signer::StoreState::Protected)
            QSKIP("requires the protected store from the earlier tests");

        // A new process must be able to rebuild a missing public index once
        // the store password is entered.
        const QString index = QDir(m_dataHome).filePath(QStringLiteral("signed/certificates.json"));
        QFile::remove(index);
        QVERIFY(!QFileInfo::exists(index));

        QProcess child;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("SIGNED_TEST_REOPEN"), QStringLiteral("1"));
        environment.insert(QStringLiteral("SIGNED_TEST_DATA_HOME"), m_dataHome);
        environment.insert(QStringLiteral("SIGNED_TEST_CONFIG_HOME"), m_configHome);
        environment.insert(QStringLiteral("SIGNED_TEST_PASSWORD"), QStringLiteral("a long enough password"));
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("reopenProtectedStore")});
        QVERIFY(child.waitForFinished(30000));
        const QByteArray output = child.readAllStandardOutput() + child.readAllStandardError();
        QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, output.constData());
    }

    void reopenProtectedStore() {
        if (qgetenv("SIGNED_TEST_REOPEN").isEmpty())
            QSKIP("only runs in the reopened process");

        const QString password = QString::fromLocal8Bit(qgetenv("SIGNED_TEST_PASSWORD"));
        QCOMPARE(Signer::storeState(), Signer::StoreState::Protected);
        QCOMPARE(Signer::unlock(password), Signer::UnlockResult::Ok);

        const QList<Signer::Certificate> certificates = Signer::certificates();
        QVERIFY(!certificates.isEmpty());
        const QString nickname = certificates.first().nickname;

        QTemporaryDir dir;
        const QString source = dir.filePath("lease.pdf");
        const QString target = dir.filePath("signed.pdf");
        writeBasePdf(source);
        QVERIFY(Stamper::stamp(source, target, {}).error.isEmpty());
        const Signer::Result result = Signer::sign(target, nickname);
        Signer::lock();
        QVERIFY2(result.ok, qPrintable(result.error));

        std::unique_ptr<Poppler::Document> document = Poppler::Document::load(target);
        QVERIFY(document);
        QCOMPARE(document->signatures().size(), 1);

        // The store is locked again for the next operation.
        const QString secondTarget = dir.filePath("second.pdf");
        QVERIFY(Stamper::stamp(source, secondTarget, {}).error.isEmpty());
        QVERIFY(!Signer::sign(secondTarget, nickname).ok);

        // Disabling the setting does not turn off protection for an existing
        // store.
        QSettings settings;
        settings.setValue("certificateStore/passwordProtected", false);
        Backend backend(nullptr, nullptr, nullptr, &settings);
        QVERIFY(!backend.passwordProtectionEnabled());
        QCOMPARE(Signer::storeState(), Signer::StoreState::Protected);
        QCOMPARE(backend.certificateStoreState(), QStringLiteral("protected"));
    }
};

QTEST_MAIN(PasswordTests)
#include "password_tests.moc"
