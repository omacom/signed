#pragma once

#include <QMutex>
#include <QQuickAsyncImageProvider>
#include <QQuickImageProvider>
#include <QThreadPool>

#include <functional>
#include <memory>

// The open PDF, shared with the render thread.
struct RenderSource {
    QMutex mutex;
    QString path;
    QString password;
    int generation = 0;
};

// Renders pages off the GUI thread. Ids are "<generation>/<page>/<width>":
// the generation changes with every opened file, so a page from the last one
// can never come back from the cache.
class PageProvider : public QQuickAsyncImageProvider {
public:
    PageProvider();
    ~PageProvider() override;

    void setDocument(const QString &path, int generation, const QString &password = {});
    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

private:
    std::shared_ptr<RenderSource> m_source;
    QThreadPool m_pool;
};

// Hands QML images that live in C++ (placed signatures, the add-signature preview).
class FunctionImageProvider : public QQuickImageProvider {
public:
    explicit FunctionImageProvider(std::function<QImage(const QString &)> lookup)
        : QQuickImageProvider(QQuickImageProvider::Image), m_lookup(std::move(lookup)) {}

    QImage requestImage(const QString &id, QSize *size, const QSize &) override {
        const QImage image = m_lookup(id);
        if (size)
            *size = image.size();
        return image;
    }

private:
    std::function<QImage(const QString &)> m_lookup;
};
