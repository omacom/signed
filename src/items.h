#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QImage>
#include <QList>
#include <QSizeF>

#include "stamper.h"

// Everything placed on the document, in PDF points from the top-left of the
// displayed page. A text item's size always follows from its text and font
// size, so it is never stored.
struct Item {
    enum Kind { Signature, Text };

    Kind kind = Text;
    int page = 0;
    qreal x = 0;
    qreal y = 0;
    qreal width = 0;    // signatures only
    qreal height = 0;   // signatures only
    QString text;
    qreal fontSize = 12;
    int imageId = 0;

    QSizeF size() const;
    bool operator==(const Item &other) const = default;
};

class ItemModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)

public:
    enum Role { KindRole = Qt::UserRole + 1, PageRole, XRole, YRole, WidthRole, HeightRole,
                TextRole, FontSizeRole, ImageSourceRole };

    explicit ItemModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_items.size(); }
    int revision() const { return m_revision; }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }

    void setPageSizes(const QList<QSizeF> &sizes);
    void clear();

    // Signatures carry their own copy of the image, so the library can change
    // without touching anything already placed.
    int addSignature(int page, const QImage &image, const QRectF &rect);
    QImage image(int imageId) const { return m_images.value(imageId); }
    const QList<Item> &items() const { return m_items; }
    QList<Stamp> stamps() const;

    Q_INVOKABLE int addText(int page, qreal x, qreal y, const QString &text, qreal fontSize);
    Q_INVOKABLE void move(int row, qreal x, qreal y);
    Q_INVOKABLE void resize(int row, qreal width);
    Q_INVOKABLE void scaleBy(int row, qreal factor);
    Q_INVOKABLE void nudge(int row, qreal dx, qreal dy);
    Q_INVOKABLE void center(int row);
    Q_INVOKABLE void setText(int row, const QString &text);
    Q_INVOKABLE void remove(int row);
    Q_INVOKABLE int duplicate(int row);
    Q_INVOKABLE QVariantMap get(int row) const;

    // A drag or a text edit is one undo step however many changes it makes:
    // beginChange remembers the items, endChange records them if they moved on.
    Q_INVOKABLE void beginChange();
    Q_INVOKABLE void endChange();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

signals:
    void countChanged();
    void revisionChanged();
    void historyChanged();

private:
    void record();
    void changed(int row);
    void restore(const QList<Item> &items);
    void clamp(Item &item) const;
    QSizeF pageSize(int page) const;

    QList<Item> m_items;
    QList<QSizeF> m_pageSizes;
    QHash<int, QImage> m_images;
    int m_nextImageId = 1;
    int m_revision = 0;
    int m_changeDepth = 0;
    QList<Item> m_changeStart;
    QList<QList<Item>> m_undo;
    QList<QList<Item>> m_redo;
};
