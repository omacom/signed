#include "items.h"

#include "textlayout.h"

namespace {

constexpr qreal MinimumSignatureWidth = 12;
constexpr qreal MinimumFontSize = 4;
constexpr qreal MaximumFontSize = 144;

}

QSizeF Item::size() const {
    if (kind == Signature)
        return {width, height};
    // An empty line still has a caret's worth of size, so it can be seen and clicked.
    const QSizeF measured = TextLayout::size(text.isEmpty() ? QStringLiteral(" ") : text, fontSize);
    return {std::max(measured.width(), fontSize * .5), measured.height()};
}

ItemModel::ItemModel(QObject *parent) : QAbstractListModel(parent) {}

int ItemModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_items.size();
}

QVariant ItemModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_items.size())
        return {};
    const Item &item = m_items.at(index.row());
    switch (role) {
    case KindRole: return item.kind == Item::Signature ? QStringLiteral("signature") : QStringLiteral("text");
    case PageRole: return item.page;
    case XRole: return item.x;
    case YRole: return item.y;
    case WidthRole: return item.size().width();
    case HeightRole: return item.size().height();
    case TextRole: return item.text;
    case FontSizeRole: return item.fontSize;
    case ImageSourceRole:
        return item.kind == Item::Signature ? QStringLiteral("image://items/%1").arg(item.imageId) : QString();
    }
    return {};
}

QHash<int, QByteArray> ItemModel::roleNames() const {
    return {{KindRole, "kind"}, {PageRole, "page"}, {XRole, "itemX"}, {YRole, "itemY"},
            {WidthRole, "itemWidth"}, {HeightRole, "itemHeight"}, {TextRole, "text"},
            {FontSizeRole, "fontSize"}, {ImageSourceRole, "imageSource"}};
}

void ItemModel::setPageSizes(const QList<QSizeF> &sizes) {
    m_pageSizes = sizes;
}

void ItemModel::clear() {
    beginResetModel();
    m_items.clear();
    m_images.clear();
    endResetModel();
    m_undo.clear();
    m_redo.clear();
    m_changeDepth = 0;
    ++m_revision;
    emit countChanged();
    emit historyChanged();
    emit revisionChanged();
}

QSizeF ItemModel::pageSize(int page) const {
    return page >= 0 && page < m_pageSizes.size() ? m_pageSizes.at(page) : QSizeF(612, 792);
}

// Keeps an item on its page. One bigger than the page sticks to its top-left.
void ItemModel::clamp(Item &item) const {
    const QSizeF page = pageSize(item.page);
    const QSizeF size = item.size();
    item.x = std::max(0.0, std::min(item.x, page.width() - size.width()));
    item.y = std::max(0.0, std::min(item.y, page.height() - size.height()));
}

void ItemModel::record() {
    if (m_changeDepth > 0)
        return;
    m_undo.append(m_items);
    m_redo.clear();
    emit historyChanged();
}

void ItemModel::changed(int row) {
    const QModelIndex i = index(row);
    emit dataChanged(i, i);
    ++m_revision;
    emit revisionChanged();
}

int ItemModel::addSignature(int page, const QImage &image, const QRectF &rect) {
    record();
    Item item;
    item.kind = Item::Signature;
    item.page = page;
    item.x = rect.x();
    item.y = rect.y();
    item.width = rect.width();
    item.height = rect.height();
    item.imageId = m_nextImageId++;
    m_images.insert(item.imageId, image);
    clamp(item);

    const int row = m_items.size();
    beginInsertRows({}, row, row);
    m_items.append(item);
    endInsertRows();
    ++m_revision;
    emit countChanged();
    emit revisionChanged();
    return row;
}

int ItemModel::addText(int page, qreal x, qreal y, const QString &text, qreal fontSize) {
    record();
    Item item;
    item.page = page;
    item.x = x;
    item.y = y;
    item.text = text;
    item.fontSize = std::clamp(fontSize, MinimumFontSize, MaximumFontSize);
    clamp(item);

    const int row = m_items.size();
    beginInsertRows({}, row, row);
    m_items.append(item);
    endInsertRows();
    ++m_revision;
    emit countChanged();
    emit revisionChanged();
    return row;
}

void ItemModel::move(int row, qreal x, qreal y) {
    if (row < 0 || row >= m_items.size())
        return;
    Item item = m_items.at(row);
    item.x = x;
    item.y = y;
    clamp(item);
    if (item == m_items.at(row))
        return;
    record();
    m_items[row] = item;
    changed(row);
}

void ItemModel::nudge(int row, qreal dx, qreal dy) {
    if (row < 0 || row >= m_items.size())
        return;
    move(row, m_items.at(row).x + dx, m_items.at(row).y + dy);
}

void ItemModel::moveToPage(int row, int page) {
    if (row < 0 || row >= m_items.size() || page < 0 || page >= m_pageSizes.size())
        return;
    Item item = m_items.at(row);
    if (item.page == page)
        return;
    item.page = page;
    clamp(item);
    record();
    m_items[row] = item;
    changed(row);
}

// Scaling keeps the aspect ratio and the top-left corner, and never grows an
// item past the edge of its page.
void ItemModel::resize(int row, qreal width) {
    if (row < 0 || row >= m_items.size())
        return;
    Item item = m_items.at(row);
    const QSizeF page = pageSize(item.page);
    const QSizeF size = item.size();
    if (size.width() <= 0 || size.height() <= 0)
        return;
    const qreal room = std::min((page.width() - item.x) / size.width(), (page.height() - item.y) / size.height());
    qreal factor = std::min(width / size.width(), room);

    if (item.kind == Item::Signature) {
        factor = std::max(factor, MinimumSignatureWidth / item.width);
        item.width *= factor;
        item.height *= factor;
    } else {
        item.fontSize = std::clamp(item.fontSize * factor, MinimumFontSize, MaximumFontSize);
    }
    clamp(item);
    if (item == m_items.at(row))
        return;
    record();
    m_items[row] = item;
    changed(row);
}

void ItemModel::scaleBy(int row, qreal factor) {
    if (row < 0 || row >= m_items.size())
        return;
    resize(row, m_items.at(row).size().width() * factor);
}

void ItemModel::setText(int row, const QString &text) {
    if (row < 0 || row >= m_items.size() || m_items.at(row).kind != Item::Text || m_items.at(row).text == text)
        return;
    record();
    m_items[row].text = text;
    changed(row);
}

void ItemModel::remove(int row) {
    if (row < 0 || row >= m_items.size())
        return;
    record();
    beginRemoveRows({}, row, row);
    m_items.removeAt(row);
    endRemoveRows();
    ++m_revision;
    emit countChanged();
    emit revisionChanged();
}

int ItemModel::duplicate(int row) {
    if (row < 0 || row >= m_items.size())
        return -1;
    record();
    Item item = m_items.at(row);
    item.x += 12;
    item.y += 12;
    clamp(item);
    const int newRow = m_items.size();
    beginInsertRows({}, newRow, newRow);
    m_items.append(item);
    endInsertRows();
    ++m_revision;
    emit countChanged();
    emit revisionChanged();
    return newRow;
}

QVariantMap ItemModel::get(int row) const {
    if (row < 0 || row >= m_items.size())
        return {};
    QVariantMap map;
    const auto roles = roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it)
        map.insert(QString::fromUtf8(it.value()), data(index(row), it.key()));
    return map;
}

void ItemModel::beginChange() {
    if (m_changeDepth++ == 0)
        m_changeStart = m_items;
}

void ItemModel::endChange() {
    if (m_changeDepth == 0 || --m_changeDepth > 0)
        return;
    if (m_changeStart != m_items) {
        m_undo.append(m_changeStart);
        m_redo.clear();
        emit historyChanged();
    }
    m_changeStart.clear();
}

void ItemModel::restore(const QList<Item> &items) {
    beginResetModel();
    m_items = items;
    endResetModel();
    ++m_revision;
    emit countChanged();
    emit revisionChanged();
    emit historyChanged();
}

void ItemModel::undo() {
    if (m_undo.isEmpty() || m_changeDepth > 0)
        return;
    m_redo.append(m_items);
    restore(m_undo.takeLast());
}

void ItemModel::redo() {
    if (m_redo.isEmpty() || m_changeDepth > 0)
        return;
    m_undo.append(m_items);
    restore(m_redo.takeLast());
}

QList<Stamp> ItemModel::stamps() const {
    QList<Stamp> result;
    for (const Item &item : m_items) {
        if (item.kind == Item::Text && item.text.trimmed().isEmpty())
            continue;
        Stamp stamp;
        stamp.page = item.page;
        stamp.rect = QRectF(QPointF(item.x, item.y), item.size());
        stamp.image = item.kind == Item::Signature ? m_images.value(item.imageId) : QImage();
        stamp.text = item.kind == Item::Text ? item.text : QString();
        stamp.fontSize = item.fontSize;
        result.append(stamp);
    }
    return result;
}
