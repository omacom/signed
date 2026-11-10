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
        QString path;
        QByteArray password;
        {
            QMutexLocker lock(&m_source->mutex);
            // A request for a document that has since been replaced isn't worth rendering.
            if (m_generation != m_source->generation)
                return;
            path = m_source->path;
            password = m_source->password.toUtf8();
        }
        // A fresh document for every render: poppler 26.08 hangs (or aborts) when
        // a page is rendered again after another page of the same document has
        // been. Loading only parses the xref, a millisecond or two.
        std::unique_ptr<Poppler::Document> document = Poppler::Document::load(path);
        if (!document)
            return;
        if (document->isLocked())
            document->unlock(password, password);
        if (document->isLocked() || m_page < 0 || m_page >= document->numPages())
            return;
        document->setRenderHint(Poppler::Document::Antialiasing);
        document->setRenderHint(Poppler::Document::TextAntialiasing);
        std::unique_ptr<Poppler::Page> page = document->page(m_page);
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
    QMutexLocker lock(&m_source->mutex);
    m_source->path = path;
    m_source->password = password;
    m_source->generation = generation;
}

QQuickImageResponse *PageProvider::requestImageResponse(const QString &id, const QSize &) {
    const QStringList parts = id.split(QLatin1Char('/'));
    auto *response = new PageResponse(m_source, parts.value(0).toInt(), parts.value(1).toInt(), parts.value(2).toInt());
    m_pool.start(response);
    return response;
}
