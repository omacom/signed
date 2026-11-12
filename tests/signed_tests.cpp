#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QImageWriter>
#include <QSettings>
#include <QPainter>
#include <QProcess>
#include <QPdfWriter>
#include <QTemporaryDir>
#include <QtTest>

#include <poppler-form.h>
#include <poppler-qt6.h>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include "backend.h"
#include "items.h"
#include "signatures.h"
#include "signer.h"
#include "stamper.h"
#include "textlayout.h"
#include "theme.h"
#include "translator.h"

namespace {

// A plain Letter page full of gray stripes and text, so stamping over it and
// comparing renders shows both where the stamp went and that nothing else moved.
void writeBasePdf(const QString &path, int pages) {
    QPdfWriter writer(path);
    writer.setResolution(72);
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));
    writer.setPageSize(QPageSize(QPageSize::Letter));
    QPainter painter(&writer);
    for (int p = 0; p < pages; ++p) {
        if (p)
            writer.newPage();
        for (int y = 0; y < 792; y += 24)
            painter.fillRect(QRectF(40, y, 532, 8), QColor(200, 200, 200));
        painter.setFont(QFont("Noto Sans", 14));
        painter.drawText(QPointF(60, 120), QStringLiteral("Original text on page %1").arg(p + 1));
    }
}

// Rewrites a PDF's page dictionaries with qpdf to produce the awkward cases
// real-world PDFs have.
void editPdf(const QString &path, const std::function<void(QPDF &, std::vector<QPDFPageObjectHelper> &)> &edit) {
    QPDF pdf;
    pdf.processFile(QFile::encodeName(path).constData());
    auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
    edit(pdf, pages);
    const QString out = path + ".tmp";
    QPDFWriter writer(pdf, QFile::encodeName(out).constData());
    writer.write();
    QFile::remove(path);
    QFile::rename(out, path);
}

QPDFObjectHandle box(double a, double b, double c, double d) {
    return QPDFObjectHandle::newArray(std::vector<QPDFObjectHandle>{
        QPDFObjectHandle::newReal(a, 2), QPDFObjectHandle::newReal(b, 2),
        QPDFObjectHandle::newReal(c, 2), QPDFObjectHandle::newReal(d, 2)});
}

// A ring of solid color with a transparent middle: the ring shows where the
// image landed, the hole shows transparency is kept.
QImage ring(const QColor &color, int size) {
    QImage image(size, size, QImage::Format_ARGB32);
    image.fill(color);
    const int hole = size / 3;
    for (int y = hole; y < size - hole; ++y)
        for (int x = hole; x < size - hole; ++x)
            image.setPixelColor(x, y, Qt::transparent);
    return image;
}

QImage render(const QString &path, int page) {
    std::unique_ptr<Poppler::Document> document = Poppler::Document::load(path);
    if (!document)
        return {};
    std::unique_ptr<Poppler::Page> p = document->page(page);
    return p ? p->renderToImage(72, 72).convertToFormat(QImage::Format_RGB32) : QImage();
}

QRect bounds(const QImage &image, const std::function<bool(QColor)> &match) {
    QRect result;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (match(image.pixelColor(x, y)))
                result = result.united(QRect(x, y, 1, 1));
    return result;
}

// Whether the bytes still match the signature; the certificate check poppler
// starts beside it is waited out, so no thread outlives the document.
Poppler::SignatureValidationInfo::SignatureStatus signatureStatus(const Poppler::FormFieldSignature &signature) {
    const auto status = signature.validateAsync(Poppler::FormFieldSignature::ValidateOptions()).first.signatureStatus();
    signature.validateResult();
    return status;
}

void writeFile(const QString &path, const QByteArray &contents) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(contents);
}

bool isRed(QColor c) { return c.red() > 200 && c.green() < 70 && c.blue() < 70; }
bool isBlue(QColor c) { return c.blue() > 200 && c.red() < 70 && c.green() < 70; }

bool near(const QRect &actual, const QRect &expected) {
    return std::abs(actual.left() - expected.left()) <= 1 && std::abs(actual.top() - expected.top()) <= 1 &&
           std::abs(actual.right() - expected.right()) <= 1 && std::abs(actual.bottom() - expected.bottom()) <= 1;
}

// The folder of the app's sources, where its catalogs live.
QString sourceFolder() {
    return QFileInfo(QFINDTESTDATA("../src/main.cpp")).absolutePath();
}

// A source file with its comments left out (their examples aren't calls)
// and its strings kept whole, so neither hides the other.
QString withoutComments(const QString &text, bool cpp) {
    QString out;
    for (qsizetype i = 0; i < text.size();) {
        const QChar c = text.at(i);
        if (text.mid(i, 2) == "//") {
            const qsizetype end = text.indexOf('\n', i);
            i = end < 0 ? text.size() : end;
        } else if (text.mid(i, 2) == "/*") {
            const qsizetype end = text.indexOf("*/", i + 2);
            i = end < 0 ? text.size() : end + 2;
        } else if (cpp && c == '"' && i > 0 && text.at(i - 1) == 'R') {
            // A raw string holds quotes of its own; it's no translation, so it's left out whole.
            const qsizetype open = text.indexOf('(', i);
            const QString close = ')' + text.mid(i + 1, open - i - 1) + '"';
            out += "\"\"";
            i = text.indexOf(close, open) + close.size();
        } else if (c == '"' || c == '\'' || c == '`') {
            qsizetype end = i + 1;
            while (end < text.size() && text.at(end) != c)
                end += text.at(end) == '\\' ? 2 : 1;
            out += text.mid(i, end + 1 - i);
            i = end + 1;
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

// A literal's text as the language reads it.
QString unescaped(const QString &literal) {
    QString out;
    for (qsizetype i = 0; i < literal.size(); ++i) {
        QChar c = literal.at(i);
        if (c == '\\' && i + 1 < literal.size()) {
            c = literal.at(++i);
            c = c == 'n' ? QChar('\n') : c == 't' ? QChar('\t') : c;
        }
        out += c;
    }
    return out;
}

// Every text the sources hand to qsTr(), tr() or QT_TR_NOOP(), and whether
// any call also passes it a count (its third argument). C++'s adjacent
// literals are one text. A call whose text isn't a literal can't be in a
// catalog, and is reported in `problems`; the signer's and stamper's own
// tr() helpers are definitions, not calls.
QMap<QString, bool> translatables(const QString &folder, QStringList *problems) {
    static const QRegularExpression call(R"((?<!QString )\b(qsTr|tr|QT_TR_NOOP)\s*\()");
    static const QRegularExpression literal(R"(^\s*(["'])((?:\\.|(?!\1).)*)\1)",
                                            QRegularExpression::DotMatchesEverythingOption);
    QMap<QString, bool> found;
    for (const QFileInfo &info : QDir(folder).entryInfoList({"*.qml", "*.js", "*.cpp", "*.h"}, QDir::Files)) {
        QFile file(info.filePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const bool cpp = info.suffix() == "cpp" || info.suffix() == "h";
        const QString code = withoutComments(QString::fromUtf8(file.readAll()), cpp);
        for (auto calls = call.globalMatch(code); calls.hasNext();) {
            const auto match = calls.next();
            qsizetype at = match.capturedEnd();
            QString text;
            bool isLiteral = false;
            for (auto part = literal.match(code.mid(at)); part.hasMatch(); part = literal.match(code.mid(at))) {
                text += unescaped(part.captured(2));
                at += part.capturedEnd();
                isLiteral = true;
                if (!cpp)
                    break;
            }
            if (!isLiteral) {
                *problems << info.fileName() + ": not a literal: " + code.mid(match.capturedStart(), 60);
                continue;
            }
            // The arguments after the text, up to the call's closing parenthesis.
            int depth = 0, commas = 0;
            for (; at < code.size(); ++at) {
                const QChar c = code.at(at);
                if (c == '"' || c == '\'') {
                    for (++at; at < code.size() && code.at(at) != c; at += code.at(at) == '\\' ? 2 : 1) { }
                } else if (c == '(' || c == '[' || c == '{') {
                    ++depth;
                } else if (c == ')' || c == ']' || c == '}') {
                    if (depth-- == 0)
                        break;
                } else if (c == ',' && depth == 0) {
                    ++commas;
                }
            }
            found[text] = found.value(text) || commas >= 2;
        }
    }
    return found;
}

QJsonObject readCatalog(const QString &path, QStringList *problems) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *problems << path + ": can't be read";
        return {};
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (!document.isObject())
        *problems << path + ": not a JSON object: " + error.errorString();
    return document.object();
}

// The placeholders in a text, in order: %1 to %9, and the count (%n, %Ln).
QStringList placeholders(const QString &text) {
    static const QRegularExpression placeholder("%L?[1-9n]");
    QStringList found;
    for (auto it = placeholder.globalMatch(text); it.hasNext();)
        found << it.next().captured();
    found.sort();
    return found;
}

// Omarchy's 49 translations (default/i18n/locales.json), as the sync's LANGUAGES has them.
const QStringList &languageCodes() {
    static const QStringList codes{"ar", "bg", "bn", "ca", "cs", "da", "de", "el", "es-MX", "et", "fa", "fi", "fil",
                                   "fr", "ga", "he", "hi", "hr", "hu", "id", "is", "it", "ja", "ko", "lt", "lv", "ms",
                                   "nl", "no", "pl", "pt", "pt-BR", "ro", "ru", "si", "sk", "sl", "sr", "sv", "sw",
                                   "ta", "th", "tr", "uk", "ur", "uz", "vi", "zh-CN", "zh-TW"};
    return codes;
}

// What's wrong with the translations in `folder`, measured against its
// en.json: an unknown language, empty or broken translations, and plural
// forms that don't fit the language's rule. A string a catalog lacks, or one
// en.json no longer has, is work for the translation sync
// (scripts/i18n-sync.py), not a problem: the window shows the English
// meanwhile, so a change to the English never waits on 49 languages.
QStringList catalogProblems(const QString &folder) {
    const QStringList &codes = languageCodes();
    QStringList problems;
    const QJsonObject english = readCatalog(folder + "/en.json", &problems);
    for (const QFileInfo &file : QDir(folder).entryInfoList({"*.json"}, QDir::Files)) {
        const QString code = file.completeBaseName();
        if (code == "en")
            continue;
        if (!codes.contains(code)) {
            problems << file.fileName() + ": not one of Omarchy's language codes";
            continue;
        }
        const QJsonObject catalog = readCatalog(file.filePath(), &problems);

        // The categories the language's rule reaches, and "other" always.
        QSet<QString> categories{"other", Translator::pluralCategory(code, 1000000)};
        for (long long n = 0; n <= 1000; ++n)
            categories << Translator::pluralCategory(code, n);

        for (auto it = english.begin(); it != english.end(); ++it) {
            const QJsonValue translated = catalog.value(it.key());
            const QString where = code + ": " + it.key() + ": ";
            if (translated.isUndefined())
                continue;
            if (it.value().isString()) {
                if (!translated.isString() || translated.toString().trimmed().isEmpty())
                    problems << where + "needs a translation";
                else if (placeholders(translated.toString()) != placeholders(it.key()))
                    problems << where + "has other placeholders than the English";
                continue;
            }
            const QJsonObject forms = translated.toObject();
            const QStringList given = forms.keys();
            if (!translated.isObject() || QSet<QString>(given.begin(), given.end()) != categories) {
                QStringList wanted(categories.begin(), categories.end());
                wanted.sort();
                problems << where + "needs exactly the plural forms " + wanted.join(", ");
                continue;
            }
            // Every form says every placeholder; one that isn't "other" may
            // say the number in words instead of the count.
            const QStringList all = placeholders(it.value().toObject().value("other").toString());
            const QSet<QString> needed(all.begin(), all.end());
            QSet<QString> fixed = needed;
            fixed.remove("%n");
            fixed.remove("%Ln");
            for (auto form = forms.begin(); form != forms.end(); ++form) {
                const QStringList has = placeholders(form.value().toString());
                const QSet<QString> present(has.begin(), has.end());
                if (form.value().toString().trimmed().isEmpty())
                    problems << where + form.key() + " needs a translation";
                else if (!(form.key() == "other" ? present == needed : present.contains(fixed) && needed.contains(present)))
                    problems << where + form.key() + " has other placeholders than the English";
            }
        }
    }
    return problems;
}

}

class SignedTests : public QObject {
    Q_OBJECT

    // Everything crypto in these tests happens in a throwaway certificate
    // database, never the user's real one: XDG_DATA_HOME is redirected before
    // any Signer call, which is where the app's NSS database lives.
    QTemporaryDir dataHome;

private slots:
    void initTestCase() {
        QVERIFY(dataHome.isValid());
        QVERIFY(QDir(dataHome.path()).mkpath("data"));
        qputenv("XDG_DATA_HOME", QDir(dataHome.path()).filePath("data").toLocal8Bit());
    }

    void keyringStartsEmptyAndUnwritten() {
        QVERIFY(Signer::certificates().isEmpty());
        // Looking isn't asking: no database appears until a certificate does.
        QVERIFY(!QFileInfo::exists(QDir(dataHome.path()).filePath("data/signed/certificates")));
    }
    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeFollowsHyprlandRounding() {
        QTemporaryDir dir;
        const auto response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        auto write = [](const QString &path, const QByteArray &data) {
            QFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
        };
        QVERIFY(write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n"));
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(write(response, "{\"int\":0}"));
        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        QVERIFY(write(response, "{\"int\":6}"));
        QTRY_COMPARE(theme.radius(), 6);
        QVERIFY(write(response, "unavailable"));
        QTest::qWait(1200);
        QCOMPARE(theme.radius(), 6);
        QVERIFY(write(response, "{\"int\":0}"));
        QTRY_COMPARE(theme.radius(), 0);
    }

    void themeReadsAccent() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QFile colors(dir.path() + "/theme/colors.toml");
        QVERIFY(colors.open(QIODevice::WriteOnly));
        colors.write("accent = \"#112233\"\n");
        colors.close();

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void translatorFallsBackToEnglish() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile() {
        QTemporaryDir dir;
        writeFile(dir.path() + "/language", "pt-BR\n");
        writeFile(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})");
        writeFile(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorFollowsAFirstPick() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/omarchy/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        // Omarchy may create the file's directory only when a language is first picked.
        QVERIFY(QDir(dir.path()).mkpath("omarchy"));
        writeFile(dir.path() + "/omarchy/language", "da\n");
        QTRY_COMPARE(translator.language(), QString("da"));
    }

    void translatorPicksPluralForms() {
        QTemporaryDir dir;
        writeFile(dir.path() + "/language", "ru\n");
        writeFile(dir.path() + "/ru.json", R"({"%n items": {"one": "%n элемент", "few": "%n элемента", "many": "%n элементов", "other": "%n элемента"}})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n items", nullptr, 21), QString("%n элемент"));
        QCOMPARE(translator.translate("Main", "%n items", nullptr, 22), QString("%n элемента"));
        QCOMPARE(translator.translate("Main", "%n items", nullptr, 25), QString("%n элементов"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));

        // A language without the string counts with English's rules, not its own.
        writeFile(dir.path() + "/language", "ja\n");
        writeFile(dir.path() + "/en.json", R"({"%n items": {"one": "%n item", "other": "%n items"}})");
        Translator japanese(dir.path() + "/language", dir.path());
        QCOMPARE(japanese.translate("Main", "%n items", nullptr, 1), QString("%n item"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    }

    void englishCatalogMatchesTheSources() {
        // en.json is the source translators work from: every text the
        // sources translate is in it, as itself or (when a count is passed)
        // as its English plural forms, and nothing else is.
        QStringList problems;
        const QMap<QString, bool> used = translatables(sourceFolder(), &problems);
        QVERIFY(used.size() > 100);
        QVERIFY(used.value("%Ln items"));
        const QJsonObject english = readCatalog(sourceFolder() + "/i18n/en.json", &problems);
        for (auto it = used.begin(); it != used.end(); ++it) {
            const QJsonValue value = english.value(it.key());
            if (value.isUndefined())
                problems << "missing from en.json: " + it.key();
            else if (it.value() && (value.toObject().keys() != QStringList{"one", "other"}
                                    || value.toObject().value("other").toString() != it.key()))
                problems << "counted, so needs its one and other forms: " + it.key();
            else if (!it.value() && value.toString() != it.key())
                problems << "should be itself: " + it.key();
        }
        for (const QString &key : english.keys())
            if (!used.contains(key))
                problems << "in en.json but no source translates it: " + key;
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void catalogsMatchTheEnglishOne() {
        QStringList problems = catalogProblems(sourceFolder() + "/i18n");
        // Every language has its catalog, empty until the sync fills it: the
        // sync only changes catalogs that exist, and only these are compiled in.
        for (const QString &code : languageCodes())
            if (!QFileInfo::exists(sourceFolder() + "/i18n/" + code + ".json"))
                problems << code + ".json: missing";
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void catalogChecksCatchBrokenTranslations() {
        QTemporaryDir dir;
        writeFile(dir.filePath("en.json"), R"({"Open": "Open", "Open %1": "Open %1",
                                              "%Ln items": {"one": "%Ln item", "other": "%Ln items"},
                                              "%Ln items in %1": {"one": "%Ln item in %1", "other": "%Ln items in %1"}})");
        // Polish needs one, few, many and other; a form may say "one" in
        // words, but not drop the document.
        writeFile(dir.filePath("pl.json"), R"({"Open": "Otwórz", "Open %1": "Otwórz",
                                              "%Ln items": {"one": "jeden element", "few": "%Ln elementy", "other": "%Ln elementu"},
                                              "%Ln items in %1": {"one": "jeden element", "few": "%Ln elementy w %1",
                                                                  "many": "%Ln elementów w %1", "other": "%Ln elementu w %1"}})");
        writeFile(dir.filePath("da.json"), R"({"Open": " ", "Open %1": "Åbn %1",
                                              "%Ln items": {"one": "ét element", "other": "  "},
                                              "%Ln items in %1": {"one": "%Ln element i %1", "other": "%Ln elementer i %1"}})");
        // German hasn't caught up yet: one string still to translate and one
        // the English has since dropped.
        writeFile(dir.filePath("de.json"), R"({"Open": "Öffnen", "Opened": "Geöffnet"})");
        writeFile(dir.filePath("xx.json"), "{}");
        QStringList problems = catalogProblems(dir.path());
        problems.sort();
        QCOMPARE(problems, QStringList({"da: %Ln items: other needs a translation",
                                        "da: Open: needs a translation",
                                        "pl: %Ln items in %1: one has other placeholders than the English",
                                        "pl: %Ln items: needs exactly the plural forms few, many, one, other",
                                        "pl: Open %1: has other placeholders than the English",
                                        "xx.json: not one of Omarchy's language codes"}));
    }

    void everyCatalogIsCompiledIn() {
        QFile resources(sourceFolder() + "/resources.qrc");
        QVERIFY(resources.open(QIODevice::ReadOnly));
        const QString listed = QString::fromUtf8(resources.readAll());
        QStringList left;
        for (const QString &name : QDir(sourceFolder() + "/i18n").entryList({"*.json"}, QDir::Files))
            if (!listed.contains("<file>i18n/" + name + "</file>"))
                left << name;
        QVERIFY2(left.isEmpty(), qPrintable("not in resources.qrc: " + left.join(", ")));
    }

    void stampLandsWherePlaced_data() {
        QTest::addColumn<QString>("variant");
        QTest::newRow("plain") << "plain";
        QTest::newRow("rotate 90") << "rotate90";
        QTest::newRow("rotate 180") << "rotate180";
        QTest::newRow("rotate 270") << "rotate270";
        QTest::newRow("inherited rotate") << "inherited";
        QTest::newRow("cropbox offset") << "crop";
        QTest::newRow("negative mediabox") << "negative";
        QTest::newRow("trimbox differs") << "trim";
        QTest::newRow("rotated crop") << "rotatedCrop";
        QTest::newRow("unbalanced cm") << "unbalanced";
        QTest::newRow("mixed sizes") << "mixed";
    }

    void stampLandsWherePlaced() {
        QFETCH(QString, variant);
        QTemporaryDir dir;
        const QString source = dir.filePath("source.pdf");
        const QString target = dir.filePath("signed.pdf");
        writeBasePdf(source, 2);

        editPdf(source, [&](QPDF &pdf, std::vector<QPDFPageObjectHelper> &pages) {
            auto page = pages.at(1).getObjectHandle();
            if (variant.startsWith("rotate") && variant != "rotatedCrop")
                page.replaceKey("/Rotate", QPDFObjectHandle::newInteger(variant.mid(6).toInt()));
            if (variant == "inherited") {
                pdf.getRoot().getKey("/Pages").replaceKey("/Rotate", QPDFObjectHandle::newInteger(90));
            }
            if (variant == "crop")
                page.replaceKey("/CropBox", box(50, 60, 500, 700));
            if (variant == "negative") {
                page.replaceKey("/MediaBox", box(-100, -200, 512, 592));
            }
            if (variant == "trim") {
                page.replaceKey("/CropBox", box(20, 20, 590, 770));
                page.replaceKey("/TrimBox", box(100, 100, 300, 300));
            }
            if (variant == "rotatedCrop") {
                page.replaceKey("/CropBox", box(50, 60, 500, 700));
                page.replaceKey("/Rotate", QPDFObjectHandle::newInteger(90));
            }
            if (variant == "unbalanced")
                pages.at(1).addPageContents(QPDFObjectHandle::newStream(&pdf, "\n2 0 0 2 50 50 cm\n"), false);
            if (variant == "mixed")
                page.replaceKey("/MediaBox", box(0, 0, 595.28, 841.89));
        });

        // Our idea of the page agrees with poppler's, which is what's shown.
        QString error;
        const QList<QSizeF> sizes = Stamper::pageSizes(source, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        std::unique_ptr<Poppler::Document> document = Poppler::Document::load(source);
        QVERIFY(document);
        QCOMPARE(sizes.size(), document->numPages());
        for (int i = 0; i < sizes.size(); ++i) {
            const QSizeF shown = document->page(i)->pageSizeF();
            QVERIFY2(std::abs(shown.width() - sizes[i].width()) < 0.01 && std::abs(shown.height() - sizes[i].height()) < 0.01,
                     qPrintable(QString("page %1: %2x%3 vs %4x%5").arg(i).arg(shown.width()).arg(shown.height())
                                    .arg(sizes[i].width()).arg(sizes[i].height())));
        }

        const QSizeF size = sizes.at(1);
        const QRect redAt(10, 10, 30, 30);
        const QRect blueAt(int(size.width()) - 40, int(size.height()) - 40, 24, 24);
        Stamp red;
        red.page = 1;
        red.rect = redAt;
        red.image = ring(Qt::red, 30);
        Stamp blue;
        blue.page = 1;
        blue.rect = blueAt;
        blue.image = ring(Qt::blue, 24);

        const Stamper::Result result = Stamper::stamp(source, target, {red, blue});
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));

        const QImage before = render(source, 1);
        const QImage after = render(target, 1);
        QCOMPARE(after.size(), before.size());

        const QRect redFound = bounds(after, isRed);
        const QRect blueFound = bounds(after, isBlue);
        QVERIFY2(near(redFound, redAt), qPrintable(QString("red at %1,%2 %3x%4").arg(redFound.x()).arg(redFound.y())
                                                       .arg(redFound.width()).arg(redFound.height())));
        QVERIFY2(near(blueFound, blueAt), qPrintable(QString("blue at %1,%2 %3x%4").arg(blueFound.x()).arg(blueFound.y())
                                                         .arg(blueFound.width()).arg(blueFound.height())));

        // Outside the stamps, and through their transparent middles, the page
        // renders exactly as it did.
        const QRect redHole = QRect(redAt.x() + 12, redAt.y() + 12, 6, 6);
        int changed = 0;
        for (int y = 0; y < after.height(); ++y)
            for (int x = 0; x < after.width(); ++x) {
                const QPoint p(x, y);
                if ((redAt.adjusted(-2, -2, 2, 2).contains(p) && !redHole.contains(p)) ||
                    blueAt.adjusted(-2, -2, 2, 2).contains(p))
                    continue;
                if (after.pixel(p) != before.pixel(p))
                    ++changed;
            }
        QCOMPARE(changed, 0);

        // Pages without stamps are untouched.
        QCOMPARE(render(target, 0), render(source, 0));
    }

    void stampedTextIsText() {
        QTemporaryDir dir;
        const QString source = dir.filePath("source.pdf");
        const QString target = dir.filePath("signed.pdf");
        writeBasePdf(source, 1);
        editPdf(source, [](QPDF &, std::vector<QPDFPageObjectHelper> &pages) {
            pages.at(0).getObjectHandle().replaceKey("/Rotate", QPDFObjectHandle::newInteger(90));
        });

        Stamp text;
        text.page = 0;
        text.rect = QRectF(QPointF(100, 200), TextLayout::size(QStringLiteral("Zoë Ångström"), 14));
        text.text = QStringLiteral("Zoë Ångström");
        text.fontSize = 14;
        QVERIFY(Stamper::stamp(source, target, {text}).error.isEmpty());

        std::unique_ptr<Poppler::Document> document = Poppler::Document::load(target);
        QVERIFY(document);
        bool found = false;
        for (const auto &box : document->page(0)->textList()) {
            if (box->text() == QStringLiteral("Zoë")) {
                found = true;
                const QRectF r = box->boundingBox();
                QVERIFY2(std::abs(r.left() - 100) < 2 && r.top() > 199 && r.top() < 206,
                         qPrintable(QString("Zoë at %1,%2").arg(r.left()).arg(r.top())));
            }
        }
        QVERIFY(found);
    }

    void stampingTwiceStartsFromTheOriginal() {
        QTemporaryDir dir;
        const QString source = dir.filePath("source.pdf");
        const QString target = dir.filePath("signed.pdf");
        writeBasePdf(source, 1);

        Stamp red;
        red.rect = QRectF(10, 10, 30, 30);
        red.image = ring(Qt::red, 30);
        QVERIFY(Stamper::stamp(source, target, {red}).error.isEmpty());
        red.rect = QRectF(200, 200, 30, 30);
        QVERIFY(Stamper::stamp(source, target, {red}).error.isEmpty());

        QVERIFY(near(bounds(render(target, 0), isRed), QRect(200, 200, 30, 30)));
    }

    void failedStampLeavesTargetAlone() {
        QTemporaryDir dir;
        const QString target = dir.filePath("signed.pdf");
        QFile existing(target);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("keep me");
        existing.close();
        QFile garbage(dir.filePath("broken.pdf"));
        QVERIFY(garbage.open(QIODevice::WriteOnly));
        garbage.write("not a pdf");
        garbage.close();

        const Stamper::Result result = Stamper::stamp(garbage.fileName(), target, {});
        QVERIFY(!result.error.isEmpty());
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("keep me"));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).size(), 2);
    }
    // --- signatures ---

    void backgroundRemovalKeepsOnlyInk() {
        QImage photo(200, 100, QImage::Format_RGB32);
        photo.fill(QColor(236, 232, 224));  // off-white paper
        QPainter painter(&photo);
        painter.fillRect(QRect(50, 40, 100, 10), QColor(20, 20, 30));
        painter.end();

        const QImage ink = SignatureLibrary::removeBackground(photo, 0.25);
        QVERIFY(!ink.isNull());
        QVERIFY(ink.hasAlphaChannel());
        QVERIFY(std::abs(ink.width() - 108) <= 1);
        QVERIFY(std::abs(ink.height() - 18) <= 1);
        QCOMPARE(ink.pixelColor(0, 0).alpha(), 0);
        const QColor middle = ink.pixelColor(ink.width() / 2, ink.height() / 2);
        QCOMPARE(middle.alpha(), 255);
        QVERIFY(middle.value() < 80);
    }

    void blueInkStaysBlue() {
        QImage photo(100, 60, QImage::Format_RGB32);
        photo.fill(Qt::white);
        QPainter painter(&photo);
        painter.fillRect(QRect(20, 20, 60, 20), QColor(30, 50, 160));
        painter.end();

        const QColor ink = SignatureLibrary::removeBackground(photo, 0.25).pixelColor(30, 10);
        QVERIFY(ink.blue() > ink.red() * 2);
    }

    void blankImageHasNoSignature() {
        QImage paper(100, 60, QImage::Format_RGB32);
        paper.fill(Qt::white);
        QVERIFY(SignatureLibrary::removeBackground(paper, 0.25).isNull());
    }

    void transparentImageIsOnlyCropped() {
        QImage png(100, 60, QImage::Format_ARGB32);
        png.fill(Qt::transparent);
        QPainter painter(&png);
        painter.fillRect(QRect(10, 10, 20, 20), QColor(200, 30, 30));
        painter.end();

        const QImage result = SignatureLibrary::removeBackground(png, 0.25);
        QCOMPARE(result.size(), QSize(28, 28));
        QCOMPARE(result.pixelColor(14, 14), QColor(200, 30, 30));
    }

    void photosAreTurnedUpright() {
        QTemporaryDir dir;
        const QString path = dir.filePath("sideways.jpg");
        QImage image(120, 40, QImage::Format_RGB32);
        image.fill(Qt::white);
        QImageWriter writer(path);
        if (!writer.supportsOption(QImageIOHandler::ImageTransformation))
            QSKIP("This JPEG writer can't store an orientation.");
        writer.setTransformation(QImageIOHandler::TransformationRotate90);
        QVERIFY(writer.write(image));

        QCOMPARE(SignatureLibrary::load(path).size(), QSize(40, 120));
    }

    void libraryAddsListsAndRemoves() {
        QTemporaryDir dir;
        SignatureLibrary library(dir.filePath("signatures"));
        QImage ink(40, 20, QImage::Format_ARGB32);
        ink.fill(Qt::black);

        QCOMPARE(library.add("initials", ink), QString());
        QCOMPARE(library.add("David", ink), QString());
        QCOMPARE(library.count(), 2);
        QCOMPARE(library.data(library.index(0), SignatureLibrary::NameRole).toString(), QString("David"));
        QCOMPARE(library.data(library.index(1), SignatureLibrary::NameRole).toString(), QString("initials"));
        QCOMPARE(library.image("David").size(), QSize(40, 20));

        // Replacing keeps one entry with the new image.
        QImage wider(80, 20, QImage::Format_ARGB32);
        wider.fill(Qt::black);
        QCOMPARE(library.add("David", wider), QString());
        QCOMPARE(library.count(), 2);
        QCOMPARE(library.image("David").size(), QSize(80, 20));

        QVERIFY(library.remove("David"));
        QCOMPARE(library.count(), 1);
        QVERIFY(!library.contains("David"));
    }

    void badNamesAreRejected() {
        QTemporaryDir dir;
        SignatureLibrary library(dir.filePath("signatures"));
        QImage ink(4, 4, QImage::Format_ARGB32);
        ink.fill(Qt::black);
        QVERIFY(!library.add("", ink).isEmpty());
        QVERIFY(!library.add("   ", ink).isEmpty());
        QVERIFY(!library.add(".hidden", ink).isEmpty());
        QVERIFY(!library.add("../escape", ink).isEmpty());
        QVERIFY(!library.add("ok", QImage()).isEmpty());
        QCOMPARE(library.count(), 0);
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::NoDotAndDotDot).size(), 0);
    }

    // --- items ---

    void itemsStayOnTheirPage() {
        ItemModel items;
        items.setPageSizes({QSizeF(612, 792)});
        QImage ink(100, 50, QImage::Format_ARGB32);
        const int row = items.addSignature(0, ink, QRectF(10, 10, 100, 50));
        items.move(row, -40, 900);
        QCOMPARE(items.items().at(row).x, 0.0);
        QCOMPARE(items.items().at(row).y, 742.0);
        items.move(row, 600, 0);
        QCOMPARE(items.items().at(row).x, 512.0);

        // Text typed near the edge moves in rather than running off the page.
        const int text = items.addText(0, 560, 100, "A", 12);
        items.setText(text, "A line far too long to fit beside the right edge");
        QVERIFY(items.items().at(text).x + items.items().at(text).size().width() <= 612);
    }

    void scalingKeepsTheShape() {
        ItemModel items;
        items.setPageSizes({QSizeF(612, 792)});
        QImage ink(100, 50, QImage::Format_ARGB32);
        const int row = items.addSignature(0, ink, QRectF(10, 10, 100, 50));
        items.resize(row, 200);
        QCOMPARE(items.items().at(row).width, 200.0);
        QCOMPARE(items.items().at(row).height, 100.0);
        // Never past the page's edge.
        items.resize(row, 5000);
        QVERIFY(items.items().at(row).x + items.items().at(row).width <= 612.001);
    }

    void textSizeFollowsItsText() {
        ItemModel items;
        items.setPageSizes({QSizeF(612, 792)});
        const int row = items.addText(0, 10, 10, "Hello", 12);
        const QSizeF small = items.items().at(row).size();
        QCOMPARE(small, TextLayout::size("Hello", 12));
        items.scaleBy(row, 2);
        QVERIFY(std::abs(items.items().at(row).fontSize - 24) < 0.01);
        items.setText(row, "Hello\nthere");
        QVERIFY(std::abs(items.items().at(row).size().height() - small.height() * 4) < 0.01);
    }

    void dragsAreOneUndoStep() {
        ItemModel items;
        items.setPageSizes({QSizeF(612, 792)});
        const int row = items.addText(0, 10, 10, "Hi", 12);
        items.beginChange();
        for (int i = 0; i < 20; ++i)
            items.move(row, 10 + i, 10 + i);
        items.endChange();
        QCOMPARE(items.items().at(row).x, 29.0);
        items.undo();
        QCOMPARE(items.items().at(row).x, 10.0);
        items.redo();
        QCOMPARE(items.items().at(row).x, 29.0);
        items.undo();
        items.undo();
        QCOMPARE(items.count(), 0);
        QVERIFY(!items.canUndo());
    }

    void abandonedNewTextLeavesNoTrace() {
        ItemModel items;
        items.setPageSizes({QSizeF(612, 792)});
        items.beginChange();
        const int row = items.addText(0, 10, 10, "", 12);
        items.remove(row);
        items.endChange();
        QCOMPARE(items.count(), 0);
        QVERIFY(!items.canUndo());
    }

    // --- saving ---

    void savingRefusesTheOriginal() {
        QTemporaryDir dir;
        const QString source = dir.filePath("lease.pdf");
        writeBasePdf(source, 1);
        QFile::link(source, dir.filePath("alias.pdf"));
        const QByteArray original = [&] { QFile f(source); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }();

        QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
        SignatureLibrary library(dir.filePath("signatures"));
        Backend backend(nullptr, nullptr, &library, &settings);
        QVERIFY(backend.openUrl(QUrl::fromLocalFile(source)));
        backend.placeText(0, 100, 100, "Signed");

        QVERIFY(!backend.saveTo(source));
        QVERIFY(!backend.saveTo(dir.filePath("alias.pdf")));
        QVERIFY(backend.dirty());
        QFile f(source);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), original);

        QVERIFY(backend.saveTo(dir.filePath("lease-signed.pdf")));
        QVERIFY(!backend.dirty());
    }

    void undoingBackToTheSavedCopyIsClean() {
        QTemporaryDir dir;
        const QString source = dir.filePath("lease.pdf");
        writeBasePdf(source, 1);
        QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
        SignatureLibrary library(dir.filePath("signatures"));
        Backend backend(nullptr, nullptr, &library, &settings);
        QVERIFY(backend.openUrl(QUrl::fromLocalFile(source)));
        const int row = backend.placeText(0, 100, 100, "Signed");
        QVERIFY(backend.saveTo(dir.filePath("lease-signed.pdf")));

        backend.items()->nudge(row, 10, 0);
        QVERIFY(backend.dirty());
        backend.items()->undo();
        QVERIFY(!backend.dirty());
        // Removing the last item is unsaved too, though nothing is left on the page.
        backend.items()->remove(row);
        QVERIFY(backend.dirty());
    }

    void placedSignatureSurvivesLibraryDeletion() {
        QTemporaryDir dir;
        const QString source = dir.filePath("lease.pdf");
        writeBasePdf(source, 1);
        QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
        SignatureLibrary library(dir.filePath("signatures"));
        QImage red(60, 30, QImage::Format_ARGB32);
        red.fill(Qt::red);
        QCOMPARE(library.add("David", red), QString());

        Backend backend(nullptr, nullptr, &library, &settings);
        QVERIFY(backend.openUrl(QUrl::fromLocalFile(source)));
        const int row = backend.placeSignature("David", 0, 300, 400);
        QVERIFY(row >= 0);
        backend.removeSignature("David");
        QCOMPARE(library.count(), 0);

        const QString target = dir.filePath("signed.pdf");
        QVERIFY(backend.saveTo(target));
        QVERIFY(!bounds(render(target, 0), isRed).isEmpty());
    }

    void failedDeletionKeepsTheSeal() {
        QTemporaryDir dir;
        QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
        SignatureLibrary library(dir.filePath("signatures"));
        QImage red(60, 30, QImage::Format_ARGB32);
        red.fill(Qt::red);
        QCOMPARE(library.add("David", red), QString());
        settings.setValue("signatureKey/David", "some-certificate");

        Backend backend(nullptr, nullptr, &library, &settings);
        const auto writable = QFile::permissions(library.directory());
        QVERIFY(QFile::setPermissions(library.directory(), QFile::ReadOwner | QFile::ExeOwner));
        if (QFileInfo(library.directory()).isWritable()) {
            QFile::setPermissions(library.directory(), writable);
            QSKIP("Root can delete from a read-only directory.");
        }
        backend.removeSignature("David");
        QFile::setPermissions(library.directory(), writable);
        QVERIFY(library.contains("David"));
        QCOMPARE(backend.signatureKey("David"), QString("some-certificate"));
        QVERIFY(!backend.notice().isEmpty());
    }

    void passwordProtectedPdfsAreRefused() {
        QTemporaryDir dir;
        const QString plain = dir.filePath("plain.pdf");
        const QString locked = dir.filePath("locked.pdf");
        writeBasePdf(plain, 1);
        {
            QPDF pdf;
            pdf.processFile(QFile::encodeName(plain).constData());
            QPDFWriter writer(pdf, QFile::encodeName(locked).constData());
            writer.setR6EncryptionParameters("secret", "owner", true, true, true, true, true, true,
                                             qpdf_r3p_full, true);
            writer.write();
        }
        QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
        SignatureLibrary library(dir.filePath("signatures"));
        Backend backend(nullptr, nullptr, &library, &settings);
        QVERIFY(!backend.openUrl(QUrl::fromLocalFile(locked)));
        QVERIFY(backend.notice().contains("password"));
        QVERIFY(!backend.hasDocument());
    }

    void todayHasTheFullYear() {
        QSettings settings(QTemporaryDir().filePath("s.ini"), QSettings::IniFormat);
        SignatureLibrary library(QTemporaryDir().filePath("x"));
        Backend backend(nullptr, nullptr, &library, &settings);
        QVERIFY(backend.today().contains(QString::number(QDate::currentDate().year())));
    }

    void createdKeySignsAndVerifies() {
        QString nickname;
        // A comma or a leading # in a name is part of it, not subject syntax.
        QCOMPARE(Signer::createCertificate(QStringLiteral("#1 Morgan, Ærø"), QString(), &nickname), QString());
        QVERIFY(!nickname.isEmpty());
        const QList<Signer::Certificate> certs = Signer::certificates();
        QCOMPARE(certs.size(), 1);
        QCOMPARE(certs.at(0).nickname, nickname);
        QCOMPARE(certs.at(0).commonName, QStringLiteral("#1 Morgan, Ærø"));
        QCOMPARE(certs.at(0).organization, QStringLiteral("#1 Morgan, Ærø"));

        QTemporaryDir dir;
        const QString source = dir.filePath("lease.pdf");
        writeBasePdf(source, 1);
        Stamp stamp;
        stamp.page = 0;
        stamp.rect = QRectF(100, 200, 120, 60);
        stamp.image = ring(Qt::red, 30);
        const QString target = dir.filePath("signed.pdf");
        QVERIFY(Stamper::stamp(source, target, {stamp}).error.isEmpty());

        // The signature is invisible: signing changes nothing on the page.
        const QImage before = render(target, 0);
        const QFile::Permissions permissions = QFile::permissions(target);
        const Signer::Result result = Signer::sign(target, nickname);
        if (!result.ok)
            QFAIL(qPrintable(QStringLiteral("signing failed: %1").arg(result.error)));
        QCOMPARE(render(target, 0), before);
        // Sealing doesn't make the copy private.
        QCOMPARE(QFile::permissions(target), permissions);

        std::unique_ptr<Poppler::Document> document = Poppler::Document::load(target);
        QVERIFY(document);
        const auto signatures = document->signatures();
        QCOMPARE(signatures.size(), 1);
        QCOMPARE(signatures.at(0)->signatureType(), Poppler::FormFieldSignature::AdbePkcs7detached);
        QCOMPARE(signatureStatus(*signatures.at(0)), Poppler::SignatureValidationInfo::SignatureValid);
    }

    void failedCertificateLeavesTheSignatureUnsaved() {
        QTemporaryDir dir;
        QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
        SignatureLibrary library(dir.filePath("signatures"));
        Backend backend(nullptr, nullptr, &library, &settings);
        QImage source(60, 30, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter(&source).fillRect(10, 10, 40, 10, Qt::black);
        const QString path = dir.filePath("ink.png");
        QVERIFY(source.save(path));
        QVERIFY(backend.loadSignatureImage(path));

        // Without certutil on the PATH, no certificate can be made.
        const QByteArray searchPath = qgetenv("PATH");
        qputenv("PATH", dir.path().toLocal8Bit());
        const QString error = backend.saveSignature("Fresh", QString(), "new");
        qputenv("PATH", searchPath);
        QVERIFY(!error.isEmpty());
        QVERIFY(!library.contains("Fresh"));

        // Nor does a signature that can't be saved leave its new certificate behind.
        const int before = Signer::certificates().size();
        QVERIFY(QDir().mkpath(library.directory()));
        QVERIFY(QFile::setPermissions(library.directory(), QFile::ReadOwner | QFile::ExeOwner));
        if (!QFileInfo(library.directory()).isWritable()) {
            QVERIFY(!backend.saveSignature("Fresh", QString(), "new").isEmpty());
            QCOMPARE(Signer::certificates().size(), before);
        }
        QFile::setPermissions(library.directory(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }

    void savingSealsOnlySignaturesGivenACertificate() {
        QTemporaryDir dir;
        const QString source = dir.filePath("lease.pdf");
        writeBasePdf(source, 1);
        QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
        SignatureLibrary library(dir.filePath("signatures"));
        QImage red(60, 30, QImage::Format_ARGB32);
        red.fill(Qt::red);
        QCOMPARE(library.add("David", red), QString());
        Backend backend(nullptr, nullptr, &library, &settings);
        QVERIFY(backend.openUrl(QUrl::fromLocalFile(source)));
        QVERIFY(backend.placeSignature("David", 0, 300, 400) >= 0);
        // A certificate exists, but nobody chose it for this signature: the
        // seal is opt-in, so the copy saves unsigned.
        QCOMPARE(Signer::certificates().size(), 1);
        const QString unsealed = dir.filePath("unsealed.pdf");
        QVERIFY(backend.saveTo(unsealed));
        QCOMPARE(backend.notice(), QStringLiteral("Saved unsealed.pdf"));
        QCOMPARE(int(Poppler::Document::load(unsealed)->signatures().size()), 0);
        // Once chosen, it seals.
        settings.setValue("signatureKey/David", Signer::certificates().at(0).nickname);
        const QString sealed = dir.filePath("sealed.pdf");
        QVERIFY(backend.saveTo(sealed));
        QVERIFY(backend.notice().contains(QStringLiteral("signed with #1 Morgan, Ærø")));
        QCOMPARE(int(Poppler::Document::load(sealed)->signatures().size()), 1);
    }
    void savingASignatureWithoutACertificateUnbindsIt() {
        QTemporaryDir dir;
        QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
        SignatureLibrary library(dir.filePath("signatures"));
        Backend backend(nullptr, nullptr, &library, &settings);
        settings.setValue("signatureKey/David", Signer::certificates().at(0).nickname);
        QImage source(60, 30, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter(&source).fillRect(10, 10, 40, 10, Qt::black);
        const QString path = dir.filePath("ink.png");
        QVERIFY(source.save(path));
        QVERIFY(backend.loadSignatureImage(path));
        QCOMPARE(backend.saveSignature("David"), QString());
        QCOMPARE(backend.signatureKey("David"), QString());
        QCOMPARE(Signer::certificates().size(), 1);
    }
    void twoKeysOnOnePageSignTwice() {
        QString second;
        // Long enough that its length takes more than one byte, and it looks like attributes of its own.
        const QString organization = QStringLiteral("Other Org, O=Elsewhere; OU=Nowhere ") + QString(120, QLatin1Char('x'));
        QCOMPARE(Signer::createCertificate(QStringLiteral("Other User"), organization, &second), QString());
        QVERIFY(!second.isEmpty());
        QCOMPARE(Signer::certificates().last().organization, organization);

        QTemporaryDir dir;
        const QString source = dir.filePath("lease.pdf");
        writeBasePdf(source, 1);
        QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
        SignatureLibrary library(dir.filePath("signatures"));
        QImage blue(60, 30, QImage::Format_ARGB32);
        blue.fill(Qt::blue);
        QCOMPARE(library.add("David", QImage(60, 30, QImage::Format_ARGB32)), QString());
        QCOMPARE(library.add("Other", blue), QString());

        settings.setValue("signatureKey/David", Signer::certificates().at(0).nickname);
        settings.setValue("signatureKey/Other", second);
        Backend backend(nullptr, nullptr, &library, &settings);
        QVERIFY(backend.openUrl(QUrl::fromLocalFile(source)));
        QVERIFY(backend.placeSignature("David", 0, 300, 400) >= 0);
        QVERIFY(backend.placeSignature("Other", 0, 300, 100) >= 0);

        const QString target = dir.filePath("signed.pdf");
        QVERIFY(backend.saveTo(target));
        std::unique_ptr<Poppler::Document> document = Poppler::Document::load(target);
        const auto signatures = document->signatures();
        QCOMPARE(int(signatures.size()), 2);
        // The second pass appends to the file, so the first stays valid.
        for (const auto &signature : signatures)
            QCOMPARE(signatureStatus(*signature), Poppler::SignatureValidationInfo::SignatureValid);
    }

    void importingP12CertsAndSigningWithThem() {
        // A .p12 as every browser and CA tool exports it, protected.
        QTemporaryDir dir;
        const QString keyPem = dir.filePath("k.pem"), certPem = dir.filePath("c.pem"), p12 = dir.filePath("c.p12");
        QProcess::execute(QStringLiteral("openssl"), {QStringLiteral("req"), QStringLiteral("-x509"),
            QStringLiteral("-newkey"), QStringLiteral("rsa:2048"), QStringLiteral("-keyout"), keyPem,
            QStringLiteral("-out"), certPem, QStringLiteral("-days"), QStringLiteral("900"),
            QStringLiteral("-nodes"), QStringLiteral("-subj"), QStringLiteral("/CN=Imported Person/O=Imported Org")});
        QProcess::execute(QStringLiteral("openssl"), {QStringLiteral("pkcs12"), QStringLiteral("-export"),
            QStringLiteral("-in"), certPem, QStringLiteral("-inkey"), keyPem, QStringLiteral("-out"), p12,
            QStringLiteral("-passout"), QStringLiteral("pass:secret123")});
        QVERIFY(QFile::exists(p12));

        bool needsPassword = false;
        QString nickname;
        QVERIFY(!Signer::importCertificate(p12, QString(), &needsPassword, &nickname).isEmpty());
        QVERIFY(needsPassword);  // protected: the app asks for the file's password
        QCOMPARE(Signer::importCertificate(p12, QStringLiteral("secret123"), &needsPassword, &nickname), QString());
        QVERIFY(!nickname.isEmpty());
        QCOMPARE(Signer::certificates().last().commonName, QStringLiteral("Imported Person"));

        // The imported certificate signs like a created one.
        const QString source = dir.filePath("lease.pdf");
        writeBasePdf(source, 1);
        const QString target = dir.filePath("signed.pdf");
        QVERIFY(Stamper::stamp(source, target, {}).error.isEmpty());
        const Signer::Result result = Signer::sign(target, nickname);
        if (!result.ok)
            QFAIL(qPrintable(QStringLiteral("signing failed: %1").arg(result.error)));
        std::unique_ptr<Poppler::Document> document = Poppler::Document::load(target);
        QVERIFY(document);
        const auto signatures = document->signatures();
        QCOMPARE(signatures.size(), 1);
        QCOMPARE(signatureStatus(*signatures.at(0)), Poppler::SignatureValidationInfo::SignatureValid);
    }
};

QTEST_MAIN(SignedTests)
#include "signed_tests.moc"
