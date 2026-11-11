#include "cli.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSaveFile>
#include <QSettings>
#include <cstdio>

#include <poppler-qt6.h>

#include "backend.h"
#include "items.h"
#include "signatures.h"
#include "stamper.h"

namespace {

constexpr qreal DefaultSignatureWidth = 108;  // as in the app: an inch and a half
constexpr qreal DefaultTextSize = 12;
constexpr qreal MinimumSignatureWidth = 12;
constexpr qreal MinimumFontSize = 4;
constexpr qreal MaximumFontSize = 144;

void print(FILE *stream, const QString &text) { fprintf(stream, "%s\n", qPrintable(text)); }
void print(const QJsonObject &object) { fputs(QJsonDocument(object).toJson().constData(), stdout); }
int fail(const QString &message) {
    print(stderr, message);
    return 1;
}
QString points(qreal value) { return QString::number(value, 'f', 1); }

bool samePath(const QString &a, const QString &b) {
    const QFileInfo left(a), right(b);
    if (left.exists() && right.exists())
        return left.canonicalFilePath() == right.canonicalFilePath();
    return left.absoluteFilePath() == right.absoluteFilePath();
}

struct Command {
    QCommandLineParser parser;
    Command(const QString &name, const QString &description, const QString &argument = {},
            const QString &argumentHelp = {}) {
        parser.setApplicationDescription(description);
        parser.addHelpOption();
        parser.addPositionalArgument(name, description, name);
        if (!argument.isEmpty())
            parser.addPositionalArgument(argument, argumentHelp, "<" + argument + ">");
    }
    void json() { parser.addOption({"json", "Print the result as JSON"}); }
    bool isJson() const { return parser.isSet("json"); }
    QString argument(int index) const { return parser.positionalArguments().value(index); }
};

// A PDF as the app sees it: poppler for pages and text, and the same check
// the app makes that qpdf can write a signed copy of it.
std::unique_ptr<Poppler::Document> loadPdf(const QString &path) {
    if (path.isEmpty()) {
        print(stderr, "Name a PDF.");
        return nullptr;
    }
    if (!QFileInfo(path).isFile()) {
        print(stderr, "Can't find " + path + ".");
        return nullptr;
    }
    auto document = Poppler::Document::load(path);
    if (!document) {
        print(stderr, path + " isn't a PDF Signed can read.");
        return nullptr;
    }
    if (document->isLocked()) {
        print(stderr, path + " is password protected.");
        return nullptr;
    }
    if (document->numPages() == 0) {
        print(stderr, path + " has no pages.");
        return nullptr;
    }
    document->setRenderHint(Poppler::Document::Antialiasing);
    document->setRenderHint(Poppler::Document::TextAntialiasing);
    return document;
}

// Pages are numbered from 1 on the command line, as people and PDF viewers count them.
int pageOption(const Command &command, const Poppler::Document &document, bool *ok) {
    *ok = true;
    if (!command.parser.isSet("page"))
        return 0;
    const int number = command.parser.value("page").toInt();
    if (number < 1 || number > document.numPages()) {
        *ok = false;
        print(stderr, QString("Page %1 does not exist; there are %2 pages.")
                          .arg(command.parser.value("page")).arg(document.numPages()));
    }
    return number;
}

int signatures(const QStringList &arguments) {
    Command command("signatures", "List the saved signatures.");
    command.json();
    command.parser.process(arguments);
    SignatureLibrary library;
    QJsonArray list;
    for (int row = 0; row < library.count(); ++row) {
        const QString name = library.data(library.index(row), SignatureLibrary::NameRole).toString();
        const QImage image = library.image(name);
        const QString path = QDir(library.directory()).filePath(name + ".png");
        list << QJsonObject{{"name", name}, {"path", path}, {"width", image.width()}, {"height", image.height()},
                            {"aspect", image.height() ? qRound(1000.0 * image.width() / image.height()) / 1000.0 : 0}};
        if (!command.isJson())
            print(stdout, QString("%1  %2×%3  %4").arg(name).arg(image.width()).arg(image.height()).arg(path));
    }
    if (command.isJson())
        print({{"directory", library.directory()}, {"signatures", list}});
    else if (list.isEmpty())
        print(stderr, "No saved signatures. Add one with: signed add-signature <name> <image>");
    return 0;
}

int addSignature(const QStringList &arguments) {
    Command command("add-signature", "Save a signature from a scan or photo of it on paper. The paper is made "
                                     "transparent and the image cropped to the ink.",
                    "name", "Name to save it under");
    command.parser.addPositionalArgument("image", "Picture of the signature", "<image>");
    command.parser.addOption({"threshold", "How dark, against the paper, a pixel must be to count as ink, "
                                           "from 0.05 to 0.6 (default: 0.25). Raise it to drop more paper.",
                              "amount", "0.25"});
    command.parser.addOption({"replace", "Replace a saved signature with the same name"});
    command.json();
    command.parser.process(arguments);
    const QString name = command.argument(1).trimmed(), path = command.argument(2);
    if (name.isEmpty() || path.isEmpty())
        return fail("Name the signature and the image: signed add-signature <name> <image>");
    bool ok = false;
    const qreal threshold = command.parser.value("threshold").toDouble(&ok);
    if (!ok || threshold < .05 || threshold > .6)
        return fail("The threshold must be between 0.05 and 0.6.");
    SignatureLibrary library;
    const QString invalid = library.validateName(name);
    if (!invalid.isEmpty())
        return fail(invalid);
    if (library.contains(name) && !command.parser.isSet("replace"))
        return fail("There's already a signature named “" + name + "”. Pass --replace to replace it.");
    QString error;
    const QImage source = SignatureLibrary::load(path, &error);
    if (source.isNull())
        return fail(error.isEmpty() ? "Can't read " + path + "." : error);
    const QImage ink = SignatureLibrary::removeBackground(source, threshold);
    error = library.add(name, ink);
    if (!error.isEmpty())
        return fail(error);
    // A new image starts from the default size, as it does in the app.
    QSettings().remove("signatureWidth/" + name);
    const QString saved = QDir(library.directory()).filePath(name + ".png");
    if (command.isJson())
        print({{"name", name}, {"path", saved}, {"width", ink.width()}, {"height", ink.height()}});
    else
        print(stdout, "Saved “" + name + "” to " + saved);
    return 0;
}

int removeSignature(const QStringList &arguments) {
    Command command("remove-signature", "Delete a saved signature. Documents already signed with it keep it.",
                    "name", "Name of the signature");
    command.json();
    command.parser.process(arguments);
    SignatureLibrary library;
    const QString name = command.argument(1).trimmed();
    if (!library.contains(name))
        return fail("There's no signature named “" + name + "”. See: signed signatures");
    if (!library.remove(name))
        return fail("Couldn't remove “" + name + "”.");
    // As the app does, so a signature added again under this name starts afresh.
    QSettings settings;
    settings.remove("signatureWidth/" + name);
    settings.remove("signatureKey/" + name);
    if (command.isJson())
        print({{"removed", name}});
    else
        print(stdout, "Removed “" + name + "”");
    return 0;
}

int pages(const QStringList &arguments) {
    Command command("pages", "List the pages with their sizes in points (1/72 inch), as displayed.", "pdf", "PDF file");
    command.json();
    command.parser.process(arguments);
    const auto document = loadPdf(command.argument(1));
    if (!document)
        return 1;
    QJsonArray list;
    for (int i = 0; i < document->numPages(); ++i) {
        const auto page = document->page(i);
        const QSizeF size = page ? page->pageSizeF() : QSizeF();
        list << QJsonObject{{"page", i + 1}, {"width", size.width()}, {"height", size.height()}};
        if (!command.isJson())
            print(stdout, QString("%1  %2 × %3 pt").arg(i + 1, 3).arg(points(size.width()), points(size.height())));
    }
    if (command.isJson())
        print({{"pdf", QFileInfo(command.argument(1)).absoluteFilePath()}, {"pages", list}});
    return 0;
}

int text(const QStringList &arguments) {
    Command command("text", "Print each line of text on the pages with its box in points from the top-left, "
                            "to find labels and blanks to fill.", "pdf", "PDF file");
    command.parser.addOption({"page", "Only this page (from 1)", "number"});
    command.json();
    command.parser.process(arguments);
    const auto document = loadPdf(command.argument(1));
    if (!document)
        return 1;
    bool ok;
    const int only = pageOption(command, *document, &ok);
    if (!ok)
        return 1;
    QJsonArray list;
    for (int i = 0; i < document->numPages(); ++i) {
        if (only && i + 1 != only)
            continue;
        const auto page = document->page(i);
        if (!page)
            continue;
        // Poppler gives words; a run of words linked as next words is a line.
        const auto words = page->textList();
        QString line;
        QRectF box;
        auto flush = [&] {
            if (line.trimmed().isEmpty())
                return;
            list << QJsonObject{{"page", i + 1}, {"text", line.trimmed()}, {"x", qRound(box.x() * 10) / 10.0},
                                {"y", qRound(box.y() * 10) / 10.0}, {"width", qRound(box.width() * 10) / 10.0},
                                {"height", qRound(box.height() * 10) / 10.0}};
            if (!command.isJson())
                print(stdout, QString("%1  %2,%3  %4×%5  %6")
                                  .arg(i + 1, 3)
                                  .arg(points(box.x()), points(box.y()), points(box.width()), points(box.height()))
                                  .arg(line.trimmed()));
            line.clear();
            box = {};
        };
        for (size_t w = 0; w < words.size(); ++w) {
            const auto &word = words[w];
            line += word->text() + (word->hasSpaceAfter() ? " " : "");
            box = box.isNull() ? word->boundingBox() : box.united(word->boundingBox());
            if (w + 1 == words.size() || word->nextWord() != words[w + 1].get())
                flush();
        }
    }
    if (command.isJson())
        print({{"lines", list}});
    return 0;
}

// Lines every 50 points, labeled, so a rendered page shows where to place things.
void drawGrid(QImage &image, qreal scale) {
    QPainter painter(&image);
    QFont font("Noto Sans");
    font.setPixelSize(qMax(9, qRound(7 * scale)));
    painter.setFont(font);
    const qreal width = image.width() / scale, height = image.height() / scale;
    for (int step = 0; step <= qMax(width, height); step += 50) {
        const QColor color = step % 100 ? QColor(0, 120, 255, 50) : QColor(0, 120, 255, 110);
        painter.setPen(QPen(color, 1));
        if (step <= width)
            painter.drawLine(QPointF(step * scale, 0), QPointF(step * scale, image.height()));
        if (step <= height)
            painter.drawLine(QPointF(0, step * scale), QPointF(image.width(), step * scale));
        if (step % 100 || !step)
            continue;
        painter.setPen(QColor(0, 90, 220));
        if (step <= width)
            painter.drawText(QPointF(step * scale + 3, font.pixelSize() + 2), QString::number(step));
        if (step <= height)
            painter.drawText(QPointF(3, step * scale - 3), QString::number(step));
    }
}

int render(const QStringList &arguments) {
    Command command("render", "Render a page, or every page, to PNG, to look at a document or check a signed copy.",
                    "pdf", "PDF file");
    command.parser.addOption({"page", "Only this page (from 1)", "number"});
    command.parser.addOption({{"o", "output"}, "PNG for one page, or directory for all (default: page-N.png, "
                                               "or pages/)", "path"});
    command.parser.addOption({"width", "Image width in pixels (default: 1224, 2 pixels per point on Letter)",
                              "pixels", "1224"});
    command.parser.addOption({"grid", "Draw a labeled grid every 50 points, to read off positions"});
    command.json();
    command.parser.process(arguments);
    const auto document = loadPdf(command.argument(1));
    if (!document)
        return 1;
    bool ok;
    const int only = pageOption(command, *document, &ok);
    if (!ok)
        return 1;
    const int width = command.parser.value("width").toInt();
    if (width < 100 || width > 8000)
        return fail("Width must be between 100 and 8000.");
    QString output = command.parser.value("output");
    const bool single = only != 0;
    if (!single) {
        if (output.isEmpty())
            output = "pages";
        if (!QDir().mkpath(output))
            return fail("Can't create " + output + ".");
    }
    QJsonArray list;
    for (int i = 0; i < document->numPages(); ++i) {
        if (single && i + 1 != only)
            continue;
        const auto page = document->page(i);
        if (!page)
            return fail(QString("Can't read page %1.").arg(i + 1));
        const qreal scale = width / page->pageSizeF().width();
        QImage image = page->renderToImage(72 * scale, 72 * scale);
        if (image.isNull())
            return fail(QString("Can't render page %1.").arg(i + 1));
        if (command.parser.isSet("grid"))
            drawGrid(image, scale);
        const QString name = QString("page-%1.png").arg(i + 1);
        QString path = single ? (output.isEmpty() ? name : output) : QDir(output).filePath(name);
        if (single && QFileInfo(path).isDir())
            path = QDir(path).filePath(name);
        if (!path.endsWith(".png", Qt::CaseInsensitive))
            return fail("Name an output ending in .png, or a directory.");
        if (samePath(path, command.argument(1)))
            return fail("Render to a new file; the PDF stays as it is.");
        QDir().mkpath(QFileInfo(path).absolutePath());
        // Written beside the target and renamed into place, so even a hard
        // link to the PDF is replaced rather than written through.
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit())
            return fail("Could not save " + path);
        list << QJsonObject{{"page", i + 1}, {"image", QFileInfo(path).absoluteFilePath()},
                            {"width", image.width()}, {"height", image.height()},
                            {"pixelsPerPoint", qRound(scale * 10000) / 10000.0}};
        if (!command.isJson())
            print(stdout, QString("Rendered page %1 to %2 (%3 pixels per point)").arg(i + 1).arg(path)
                              .arg(QString::number(scale, 'g', 4)));
    }
    if (command.isJson())
        print({{"pages", list}});
    return 0;
}

// One thing to place, read from JSON and checked against its page.
struct Placement {
    QString kind;   // signature, text, or date
    QString name;   // the signature
    Stamp stamp;
};

QString describe(int index) { return QString("Item %1").arg(index + 1); }

bool number(const QJsonObject &object, const QString &key, qreal *value) {
    if (!object.value(key).isDouble())
        return false;
    *value = object.value(key).toDouble();
    return true;
}

QString readPlacement(const QJsonValue &value, int index, const QList<QSizeF> &sizes, SignatureLibrary &library,
                      QSettings &settings, Placement *placement) {
    const QString item = describe(index);
    if (!value.isObject())
        return item + " isn't a JSON object.";
    const QJsonObject object = value.toObject();
    const bool isSignature = object.contains("signature"), isDate = object.value("date").toBool();
    if (!isSignature && !isDate && !object.contains("text"))
        return item + " needs \"signature\", \"text\", or \"date\": true.";
    if (!object.value("page").isDouble())
        return item + " needs a \"page\", counted from 1.";
    const int page = object.value("page").toInt();
    if (page < 1 || page > sizes.size())
        return item + QString(": page %1 does not exist; there are %2 pages.").arg(page).arg(sizes.size());
    const QSizeF pageSize = sizes.at(page - 1);
    qreal x = 0, y = 0, bottom = 0;
    if (!number(object, "x", &x))
        return item + " needs \"x\", in points from the left edge of the page.";
    const bool hasY = number(object, "y", &y), hasBottom = number(object, "bottom", &bottom);
    if (hasY == hasBottom)
        return item + " needs either \"y\" (its top) or \"bottom\" (its bottom edge), in points from the top of the page.";

    placement->stamp.page = page - 1;
    QSizeF size;
    if (isSignature) {
        placement->kind = "signature";
        placement->name = object.value("signature").toString().trimmed();
        const QImage image = library.image(placement->name);
        if (image.isNull())
            return item + ": there's no signature named “" + placement->name + "”. See: signed signatures";
        const qreal aspect = qreal(image.height()) / image.width();
        qreal width = 0, height = 0;
        for (const char *key : {"width", "height"})
            if (object.contains(key) && !object.value(key).isDouble())
                return item + QString(": \"%1\" must be a number of points.").arg(key);
        if (number(object, "width", &width))
            size = QSizeF(width, width * aspect);
        else if (number(object, "height", &height))
            size = QSizeF(height / aspect, height);
        else {
            width = std::min(settings.value("signatureWidth/" + placement->name, DefaultSignatureWidth).toReal(),
                             pageSize.width() * .8);
            size = QSizeF(width, width * aspect);
        }
        if (size.width() < MinimumSignatureWidth)
            return item + QString(": a signature must be at least %1 points wide.").arg(MinimumSignatureWidth);
        placement->stamp.image = image;
    } else {
        if (object.contains("text") && !object.value("text").isString())
            return item + ": \"text\" must be a string.";
        placement->kind = isDate && !object.contains("text") ? "date" : "text";
        placement->stamp.text = object.contains("text") ? object.value("text").toString() : Backend::currentDate();
        if (placement->stamp.text.trimmed().isEmpty())
            return item + " has no text.";
        qreal fontSize = settings.value("textSize", DefaultTextSize).toReal();
        if (object.contains("size") && !number(object, "size", &fontSize))
            return item + ": \"size\" must be a number of points.";
        if (fontSize < MinimumFontSize || fontSize > MaximumFontSize)
            return item + QString(": text size must be between %1 and %2 points.").arg(MinimumFontSize).arg(MaximumFontSize);
        placement->stamp.fontSize = fontSize;
        Item probe;
        probe.text = placement->stamp.text;
        probe.fontSize = fontSize;
        size = probe.size();
    }
    placement->stamp.rect = QRectF(QPointF(x, hasY ? y : bottom - size.height()), size);
    // Half a point of slack, so a box drawn exactly to the edge isn't refused for rounding.
    const QRectF rect = placement->stamp.rect, inside(QPointF(-.5, -.5), pageSize + QSizeF(1, 1));
    if (!inside.contains(rect))
        return item + QString(" would reach past the page: it spans %1,%2 to %3,%4, and page %5 is %6 × %7 points.")
                          .arg(points(rect.left()), points(rect.top()), points(rect.right()), points(rect.bottom()))
                          .arg(page)
                          .arg(points(pageSize.width()), points(pageSize.height()));
    return {};
}

int sign(const QStringList &arguments) {
    Command command("sign", "Write a signed copy of a PDF with signatures, text, and dates placed on it. "
                            "See signed help items for the item format.",
                    "pdf", "PDF to sign; it is never changed");
    command.parser.addPositionalArgument("output", "Where to write the signed copy", "<output>");
    command.parser.addOption({"items", "JSON file with the items to place, or - for standard input", "file"});
    command.parser.addOption({"item", "One item as a JSON object; repeat for more", "json"});
    command.parser.addOption({"force", "Replace the output if it exists"});
    command.json();
    command.parser.process(arguments);
    const QString source = command.argument(1), target = command.argument(2);
    if (source.isEmpty() || target.isEmpty())
        return fail("Name the PDF and the output: signed sign <pdf> <output> --items items.json");
    if (!target.endsWith(".pdf", Qt::CaseInsensitive))
        return fail("Name an output file ending in .pdf.");
    if (samePath(source, target))
        return fail("Write the signed copy under a new name; the original stays as it is.");
    if (QFileInfo::exists(target) && !command.parser.isSet("force"))
        return fail(target + " already exists. Pass --force to replace it.");
    if (!loadPdf(source))
        return 1;
    QString error;
    const QList<QSizeF> sizes = Stamper::pageSizes(source, &error);
    if (sizes.isEmpty())
        return fail("Can't sign " + source + ": " + error);

    QJsonArray items;
    if (command.parser.isSet("items")) {
        const QString path = command.parser.value("items");
        QFile file;
        if (path == "-" ? !file.open(stdin, QIODevice::ReadOnly) : (file.setFileName(path), !file.open(QIODevice::ReadOnly)))
            return fail("Can't read " + path + ".");
        QJsonParseError parseError;
        const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError)
            return fail(QString("%1: %2 at offset %3").arg(path, parseError.errorString()).arg(parseError.offset));
        items = json.isArray() ? json.array() : json.object().value("items").toArray();
        if (!json.isArray() && !json.object().value("items").isArray())
            return fail(path + " must hold a JSON array of items, or an object with an \"items\" array.");
    }
    for (const QString &value : command.parser.values("item")) {
        QJsonParseError parseError;
        const QJsonDocument json = QJsonDocument::fromJson(value.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !json.isObject())
            return fail("--item must be one JSON object: " + value);
        items << json.object();
    }
    if (items.isEmpty())
        return fail("Nothing to place. Pass --items items.json or --item '{...}'; see signed help items.");

    SignatureLibrary library;
    QSettings settings;
    QList<Placement> placements;
    QStringList problems;
    for (int i = 0; i < items.size(); ++i) {
        Placement placement;
        const QString problem = readPlacement(items.at(i), i, sizes, library, settings, &placement);
        if (problem.isEmpty())
            placements << placement;
        else
            problems << problem;
    }
    // Report every problem at once, so they can all be fixed in one go.
    if (!problems.isEmpty()) {
        for (const QString &problem : problems)
            print(stderr, problem);
        return 1;
    }

    QList<Stamp> stamps;
    for (const Placement &placement : placements)
        stamps << placement.stamp;
    const Stamper::Result result = Stamper::stamp(source, target, stamps);
    if (!result.error.isEmpty())
        return fail("Couldn't sign: " + result.error);
    if (!result.warning.isEmpty())
        print(stderr, result.warning);

    QJsonArray placed;
    for (const Placement &placement : placements) {
        const QRectF &rect = placement.stamp.rect;
        QJsonObject object{{"type", placement.kind}, {"page", placement.stamp.page + 1},
                           {"x", qRound(rect.x() * 10) / 10.0}, {"y", qRound(rect.y() * 10) / 10.0},
                           {"width", qRound(rect.width() * 10) / 10.0}, {"height", qRound(rect.height() * 10) / 10.0}};
        if (placement.kind == "signature")
            object.insert("signature", placement.name);
        else
            object.insert("text", placement.stamp.text), object.insert("size", placement.stamp.fontSize);
        placed << object;
    }
    if (command.isJson())
        print({{"output", QFileInfo(target).absoluteFilePath()}, {"items", placed}});
    else
        print(stdout, QString("Signed %1 with %2 items").arg(target).arg(placed.size()));
    return 0;
}

// As Hype does: one shared copy, linked into Claude Code, found directly by Codex.
int skill(const QStringList &arguments) {
    Command command("skill", "Print the agent skill, or install it for your coding agents with: skill install.");
    command.parser.addPositionalArgument("install", "Copy to ~/.agents/skills/signed and link into ~/.claude/skills",
                                         "[install]");
    command.json();
    command.parser.process(arguments);
    QFile bundled(":/skill.md");
    if (!bundled.open(QIODevice::ReadOnly))
        return 1;
    const QByteArray text = bundled.readAll();
    if (command.argument(1).isEmpty()) {
        if (command.isJson())
            print({{"skill", QString::fromUtf8(text)}});
        else
            fputs(text.constData(), stdout);
        return 0;
    }
    if (command.argument(1) != "install")
        return fail("Use signed skill to print the skill, or signed skill install.");
    const QDir home = QDir::home();
    const QString directory = home.filePath(".agents/skills/signed");
    QSaveFile file(directory + "/SKILL.md");
    if (!QDir().mkpath(directory) || !file.open(QIODevice::WriteOnly) || file.write(text) < 0 || !file.commit())
        return fail("Could not write " + file.fileName());
    QJsonObject result{{"installed", file.fileName()}};
    auto done = [&] {
        if (command.isJson())
            print(result);
        return 0;
    };
    if (!command.isJson())
        print(stdout, "Installed " + file.fileName());
    if (!home.exists(".claude"))
        return done();
    const QString link = home.filePath(".claude/skills/signed");
    const QFileInfo existing(link);
    if (existing.isSymLink())
        QFile::remove(link);
    else if (existing.exists())
        return fail(link + " exists and is not a link; leaving it alone.");
    if (!home.mkpath(".claude/skills") || !QFile::link("../../.agents/skills/signed", link))
        return fail("Could not link " + link);
    result.insert("linked", link);
    if (!command.isJson())
        print(stdout, "Linked " + link);
    return done();
}

int help(const QStringList &arguments) {
    const QString topic = arguments.value(2);
    if (topic == "items") {
        QFile guide(":/items.md");
        if (!guide.open(QIODevice::ReadOnly))
            return 1;
        fputs(guide.readAll().constData(), stdout);
        return 0;
    }
    if (isCliCommand(topic) && topic != "help")
        return runCli({arguments[0], topic, "--help"});
    print(stdout, "Usage: signed <command> [options]\n\n" + cliSummary());
    return 0;
}

}

QString cliSummary() {
    return "App:\n"
           "  open [pdf]                        Open Signed, on a PDF if one is named\n\n"
           "Commands that need no display (signed help <command> for options):\n"
           "  signatures                        List the saved signatures\n"
           "  add-signature <name> <image>      Save a signature from a scan or photo\n"
           "  remove-signature <name>           Delete a saved signature\n"
           "  pages <pdf>                       Page sizes in points\n"
           "  text <pdf>                        Lines of text with their positions\n"
           "  render <pdf>                      Render pages to PNG, optionally with a grid\n"
           "  sign <pdf> <output>               Write a signed copy with items placed on it\n"
           "  help items                        How to describe what to place\n"
           "  skill [install]                   Print the skill for coding agents, or install it";
}

bool isCliCommand(const QString &word) {
    return QStringList{"signatures", "add-signature", "remove-signature", "pages", "text", "render", "sign",
                       "skill", "help"}
        .contains(word);
}

int runCli(const QStringList &arguments) {
    const QString command = arguments.value(1);
    return command == "signatures"         ? signatures(arguments)
           : command == "add-signature"    ? addSignature(arguments)
           : command == "remove-signature" ? removeSignature(arguments)
           : command == "pages"            ? pages(arguments)
           : command == "text"             ? text(arguments)
           : command == "render"           ? render(arguments)
           : command == "sign"             ? sign(arguments)
           : command == "skill"            ? skill(arguments)
                                           : help(arguments);
}
