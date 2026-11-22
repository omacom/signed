#include <QtTest>
#include <QFileInfo>
#include <QPointingDevice>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickImageProvider>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QWheelEvent>
#include <memory>
#include "textitem.h"

class ScrollTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QQuickStyle::setStyle(QStringLiteral("Material"));
        qmlRegisterType<TextItem>("Signed", 1, 0, "TextItem");
    }
    void gestureDoesNotCoastUntilItEnds_data();
    void gestureDoesNotCoastUntilItEnds();
    void navigationStopsCoasting_data();
    void navigationStopsCoasting();
};

class BlankPages : public QQuickImageProvider {
public:
    BlankPages() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &, QSize *size, const QSize &) override {
        QImage image(1, 1, QImage::Format_RGB32);
        image.fill(Qt::white);
        if (size)
            *size = image.size();
        return image;
    }
};

class ScrollWindow {
public:
    ScrollWindow() {
        engine.addImageProvider(QStringLiteral("pages"), new BlankPages);
        const QDir tests = QFileInfo(QString::fromUtf8(__FILE__)).dir();
        QQmlComponent backendComponent(&engine, QUrl::fromLocalFile(tests.filePath("ScrollBackend.qml")));
        backend.reset(backendComponent.create());
        if (!backend)
            qWarning() << backendComponent.errors();
        engine.rootContext()->setContextProperty("backend", backend.get());
        QQmlComponent themeComponent(&engine);
        themeComponent.setData("import QtQuick; QtObject { property color accent: \"#FFD60A\"; "
                               "property color accentForeground: \"black\"; property int radius: 0 }", QUrl());
        theme.reset(themeComponent.create());
        if (!theme)
            qWarning() << themeComponent.errors();
        engine.rootContext()->setContextProperty("theme", theme.get());
        engine.load(QUrl::fromLocalFile(tests.filePath("../src/Main.qml")));
        if (!engine.rootObjects().isEmpty())
            window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if (window)
            list = window->findChild<QQuickItem *>(QStringLiteral("pageList"));
    }

    void wheel(int dy, Qt::ScrollPhase phase) {
        const QPointF at = list->mapToScene(QPointF(list->width() / 2, list->height() / 2));
        QWheelEvent event(at, window->mapToGlobal(at.toPoint()), QPoint(0, dy), QPoint(),
                          Qt::NoButton, Qt::NoModifier, phase, false,
                          Qt::MouseEventSynthesizedBySystem, &touchpad);
        QCoreApplication::sendEvent(window, &event);
    }

    std::unique_ptr<QObject> backend, theme;
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;
    QQuickItem *list = nullptr;
    QPointingDevice touchpad { "scroll test", 1, QInputDevice::DeviceType::TouchPad,
                              QPointingDevice::PointerType::Finger,
                              QInputDevice::Capability::Position | QInputDevice::Capability::Scroll | QInputDevice::Capability::PixelScroll,
                              1, 0 };
};

void ScrollTests::gestureDoesNotCoastUntilItEnds_data() {
    QTest::addColumn<bool>("phases");
    QTest::newRow("explicit gesture end") << true;
    QTest::newRow("idle fallback without phase events") << false;
}

void ScrollTests::gestureDoesNotCoastUntilItEnds() {
    QFETCH(bool, phases);
    ScrollWindow view;
    QVERIFY(view.window);
    QVERIFY(view.list);
    QTest::qWait(30);
    QVERIFY(view.list->setProperty("contentY", 500));
    view.wheel(-16, phases ? Qt::ScrollBegin : Qt::NoScrollPhase);
    QCOMPARE(view.list->property("contentY").toReal(), 516.0);
    QTest::qWait(32);
    QCOMPARE(view.list->property("contentY").toReal(), 516.0);
    view.wheel(-16, phases ? Qt::ScrollUpdate : Qt::NoScrollPhase);
    QCOMPARE(view.list->property("contentY").toReal(), 532.0);
    QTest::qWait(32);
    QCOMPARE(view.list->property("contentY").toReal(), 532.0);
    if (phases)
        view.wheel(0, Qt::ScrollEnd);
    QTRY_VERIFY(view.list->property("contentY").toReal() > 532.0);
}

void ScrollTests::navigationStopsCoasting_data() {
    QTest::addColumn<bool>("newDocument");
    QTest::newRow("page navigation") << false;
    QTest::newRow("document reset") << true;
}

void ScrollTests::navigationStopsCoasting() {
    QFETCH(bool, newDocument);
    ScrollWindow view;
    QVERIFY(view.window);
    QVERIFY(view.list);
    QTest::qWait(30);
    QVERIFY(view.list->setProperty("contentY", 500));
    view.wheel(-16, Qt::ScrollBegin);
    view.wheel(0, Qt::ScrollEnd);
    QTRY_VERIFY(view.list->property("contentY").toReal() > 516.0);
    if (newDocument)
        QVERIFY(QMetaObject::invokeMethod(view.backend.get(), "documentChanged"));
    else
        QVERIFY(QMetaObject::invokeMethod(view.window, "goToPage", Q_ARG(QVariant, 3)));
    const qreal chosen = view.list->property("contentY").toReal();
    QCOMPARE(view.list->property("coastVelocity").toReal(), 0.0);
    QTest::qWait(80);
    QCOMPARE(view.list->property("contentY").toReal(), chosen);
}

QTEST_MAIN(ScrollTests)
#include "scroll_tests.moc"
