#include "pageprovider.h"

#include <QRunnable>

#include <poppler-qt6.h>

namespace {

// Big enough for a page filling a 4K screen; beyond it zooming gets softer
// rather than eating memory.
constexpr int MaximumRenderWidth = 4000;

class PageResponse : public QQuickImageResponse, public QRunnable {
public:
    PageResponse(std::shared_ptr<RenderSource> source, int generation, int page, int width)
        : m_source(std::move(source)), m_generation(generation), m_page(page), m_width(width) {
        setAutoDelete(false);
    }

    QQuickTextureFactory *textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    void run() override {
        render();
        emit finished();
    }

private:
    void render() {
        QMutexLocker lock(&m_source->mutex);
        // A request for a document that has since been replaced isn't worth rendering.
        if (m_generation != m_source->generation)
            return;
        if (m_source->loadedGeneration != m_source->generation) {
            m_source->document = Poppler::Document::load(m_source->path);
            m_source->loadedGeneration = m_source->generation;
            if (m_source->document && m_source->document->isLocked()) {
                const QByteArray password = m_source->password.toUtf8();
                m_source->document->unlock(password, password);
            }
            if (m_source->document) {
                m_source->document->setRenderHint(Poppler::Document::Antialiasing);
                m_source->document->setRenderHint(Poppler::Document::TextAntialiasing);
            }
        }
        if (!m_source->document || m_page < 0 || m_page >= m_source->document->numPages())
            return;
        std::unique_ptr<Poppler::Page> page = m_source->document->page(m_page);
        if (!page)
            return;
        const qreal points = page->pageSizeF().width();
        const int width = std::clamp(m_width, 16, MaximumRenderWidth);
        const qreal dpi = 72.0 * width / points;
        m_image = page->renderToImage(dpi, dpi);
    }

    std::shared_ptr<RenderSource> m_source;
    int m_generation;
    int m_page;
    int m_width;
    QImage m_image;
};

}

PageProvider::PageProvider() : m_source(std::make_shared<RenderSource>()) {
    m_pool.setMaxThreadCount(1);
}

PageProvider::~PageProvider() {
    m_pool.clear();
    m_pool.waitForDone();
}

void PageProvider::setDocument(const QString &path, int generation, const QString &password) {
    // Waits for a render in progress, so the document is never swapped under it.
    QMutexLocker lock(&m_source->mutex);
    m_source->path = path;
    m_source->password = password;
    m_source->generation = generation;
    m_source->document.reset();
    m_source->loadedGeneration = -1;
}

QQuickImageResponse *PageProvider::requestImageResponse(const QString &id, const QSize &) {
    const QStringList parts = id.split(QLatin1Char('/'));
    auto *response = new PageResponse(m_source, parts.value(0).toInt(), parts.value(1).toInt(), parts.value(2).toInt());
    m_pool.start(response);
    return response;
}
