#include <QtMath>
#include <QPainter>
#include <QTransform>
#include <QVector2D>
#include "connector.h"
#include "node.h"
#include "label.h"
#include "../utils.h"
#include "wire.h"

const qreal SIZE               = 1;
const QColor COLOR_BODY_FILL   = QColor(Qt::green);
const QColor COLOR_BODY_BORDER = QColor(Qt::black);
const qreal PEN_WIDTH          = 1.5;
const int TEXT_PADDING         = 15;

using namespace QSchematic;

Connector::Connector(int type, const QPoint& gridPoint, const QString& text, QGraphicsItem* parent) :
    Item(type, parent),
    _snapPolicy(NodeSizerectOutline),
    _forceTextDirection(false),
    _textDirection(Direction::LeftToRight),
    _wire(nullptr),
    _wirePointIndex(-1)
{
    // Label
    _label = std::make_shared<Label>();
    _label->setParentItem(this);
    _label->setText(text);

    // Flags
    setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);

    // Make sure that we are above the parent
    if (parentItem()) {
        setZValue(parentItem()->zValue() + 1);
    }

    // Connections
    connect(this, &Connector::moved, [this]{ calculateTextDirection(); });
    connect(this, &Connector::moved, this, &Connector::moveWirePoint);
    Node* node = static_cast<Node*>(parentItem());
    if (node) {
        connect(node, &Item::moved, this, &Connector::moveWirePoint);
        connect(node, &Item::rotated, this, &Connector::moveWirePoint);
    }

    // Misc
    setGridPos(gridPoint);
    calculateSymbolRect();
    calculateTextDirection();
}

Gpds::Container Connector::toContainer() const
{
    // Root
    Gpds::Container root;
    addItemTypeIdToContainer(root);
    root.addValue("item", Item::toContainer());
    root.addValue("snap_policy", snapPolicy());
    root.addValue("force_text_direction", forceTextDirection());
    root.addValue("text_direction", textDirection());
    root.addValue("label", _label->toContainer());

    return root;
}

void Connector::fromContainer(const Gpds::Container& container)
{
    Item::fromContainer( *container.getValue<Gpds::Container*>( "item" ) );
    setSnapPolicy( static_cast<SnapPolicy>( container.getValue<int>( "snap_policy" ) ) );
    setForceTextDirection( container.getValue<bool>( "force_text_direction" ) );
    _textDirection = static_cast<Direction>( container.getValue<int>( "text_direction" ) );
    _label->fromContainer( *container.getValue<Gpds::Container*>( "label" ) );
}

std::shared_ptr<Item> Connector::deepCopy() const
{
    auto clone = std::make_shared<Connector>(type(), gridPos(), text(), parentItem());
    copyAttributes(*(clone.get()));

    return clone;
}

void Connector::copyAttributes(Connector& dest) const
{
    Q_ASSERT(_label);

    // Base class
    Item::copyAttributes(dest);

    // Label
    dest._label = std::dynamic_pointer_cast<Label>(_label->deepCopy());
    dest._label->setParentItem(&dest);

    // Attributes
    dest._snapPolicy = _snapPolicy;
    dest._symbolRect = _symbolRect;
    dest._forceTextDirection = _forceTextDirection;
    dest._textDirection = _textDirection;
}

void Connector::pointInserted(int index)
{
    // Do nothing if the connected point is the first
    if (_wirePointIndex == 0) {
        return;
    }
    // Inserted point comes before the connected point or the last point is connected
    else if (_wirePointIndex >= index or _wirePointIndex == _wire->pointsAbsolute().count()-2) {
        _wirePointIndex++;
    }
}

void Connector::pointRemoved(int index)
{
    if (_wirePointIndex >= index) {
        _wirePointIndex--;
    }
}

void Connector::setSnapPolicy(Connector::SnapPolicy policy)
{
    _snapPolicy = policy;
}

Connector::SnapPolicy Connector::snapPolicy() const
{
    return _snapPolicy;
}

void Connector::setText(const QString& text)
{
    _label->setText(text);

    calculateTextDirection();
}

QString Connector::text() const
{
    return _label->text();
}

void Connector::setForceTextDirection(bool enabled)
{
    _forceTextDirection = enabled;
}

bool Connector::forceTextDirection() const
{
    return _forceTextDirection;
}

void Connector::setForcedTextDirection(Direction direction)
{
    _textDirection = direction;

    update();
}

Direction Connector::textDirection() const
{
    return _textDirection;
}

void Connector::update()
{
    calculateSymbolRect();
    calculateTextDirection();

    Item::update();
}

QPointF Connector::connectionPoint() const
{
    return QPointF(0, 0);
}

QRectF Connector::boundingRect() const
{
    qreal adj = qCeil(PEN_WIDTH / 2.0);
    if (isHighlighted()) {
        adj += _settings.highlightRectPadding;
    }

    return _symbolRect.adjusted(-adj, -adj, adj, adj);
}

QVariant Connector::itemChange(QGraphicsItem::GraphicsItemChange change, const QVariant& value)
{
    switch (change) {
    // Snap to whatever we're supposed to snap to
    case QGraphicsItem::ItemPositionChange:
    {
        QPointF proposedPos = value.toPointF();

        // Retrieve parent Node's size rect
        const Node* parentNode = qgraphicsitem_cast<const Node*>(parentItem());
        if (!parentNode) {
            return proposedPos;
        }
        QRectF parentNodeSizeRect(0, 0, parentNode->size().width(), parentNode->size().height());

        // Honor snap policy
        switch (_snapPolicy) {
        case Anywhere:
            break;

        case NodeSizerect:
            proposedPos = Utils::clipPointToRect(proposedPos, parentNodeSizeRect);
            break;

        case NodeSizerectOutline:
            proposedPos = Utils::clipPointToRectOutline(proposedPos, parentNodeSizeRect);
            break;
        }

        // Honor snap-to-grid
        if (parentNode->canSnapToGrid() and snapToGrid()) {
            proposedPos = _settings.snapToGrid(proposedPos);
        }

        return proposedPos;
    }

    case QGraphicsItem::ItemPositionHasChanged:
    {
        calculateTextDirection();
        alignLabel();
        break;
    }

    case QGraphicsItem::ItemParentHasChanged:
    {
        Node* node = static_cast<Node*>(parentItem());
        if (node) {
            connect(node, &Item::moved, this, &Connector::moveWirePoint);
            connect(node, &Item::rotated, this, &Connector::moveWirePoint);
        }
        calculateTextDirection();
        alignLabel();
        break;
    }

    default:
        break;
    }

    return QGraphicsItem::itemChange(change, value);
}

void Connector::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget)
{
    Q_UNUSED(option)
    Q_UNUSED(widget)

    // Draw the bounding rect if debug mode is enabled
    if (_settings.debug) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QBrush(Qt::red));
        painter->drawRect(boundingRect());
    }

    // Body pen
    QPen bodyPen;
    bodyPen.setWidthF(PEN_WIDTH);
    bodyPen.setStyle(Qt::SolidLine);
    bodyPen.setColor(COLOR_BODY_BORDER);

    // Body brush
    QBrush bodyBrush;
    bodyBrush.setStyle(Qt::SolidPattern);
    bodyBrush.setColor(COLOR_BODY_FILL);

    // Draw the component body
    painter->setPen(bodyPen);
    painter->setBrush(bodyBrush);
    painter->drawRoundedRect(_symbolRect, _settings.gridSize/4, _settings.gridSize/4);
}

std::shared_ptr<Label> Connector::label() const
{
    return _label;
}

void Connector::alignLabel()
{
    QPointF labelNewPos = _label->pos();
    QTransform t;
    const QRectF& textRect = _label->textRect();

    switch (_textDirection) {
        case LeftToRight:
            labelNewPos.rx() = TEXT_PADDING;
            labelNewPos.ry() = textRect.height()/4;
            t.rotate(0);
            break;

        case RightToLeft:
            labelNewPos.rx() = -textRect.width() - TEXT_PADDING;
            labelNewPos.ry() = textRect.height()/4;
            t.rotate(0);
            break;

        case TopToBottom:
            labelNewPos.rx() = textRect.height()/4;
            labelNewPos.ry() = textRect.width() + TEXT_PADDING;
            t.rotate(-90);
            break;

        case BottomToTop:
            labelNewPos.rx() = textRect.height()/4;
            labelNewPos.ry() = - TEXT_PADDING;
            t.rotate(-90);
            break;
    }

    _label->setPos(labelNewPos);
    _label->setTransform(t);
}

void Connector::calculateSymbolRect()
{
    _symbolRect = QRectF(-SIZE*_settings.gridSize/2.0, -SIZE*_settings.gridSize/2.0, SIZE*_settings.gridSize, SIZE*_settings.gridSize);
}

void Connector::calculateTextDirection()
{
    // Honor forced override
    if (_forceTextDirection) {
        return;
    }

    // Nothing to do if there's no text
    if (text().isEmpty()) {
        _textDirection = LeftToRight;
        return;
    }

    // Figure out the text direction
    {
        _textDirection = LeftToRight;
        const Node* parentNode = qgraphicsitem_cast<const Node*>(parentItem());
        if (parentNode) {

            // Create list of edges
            QVector<QLineF> edges(4);
            const QRectF& rect = parentNode->sizeRect();
            edges[0] = QLineF(rect.topLeft(), rect.topRight());
            edges[1] = QLineF(rect.topRight(), rect.bottomRight());
            edges[2] = QLineF(rect.bottomRight(), rect.bottomLeft());
            edges[3] = QLineF(rect.bottomLeft(), rect.topLeft());

            // Figure out which edge we're closest to
            auto closestEdgeIterator = Utils::lineClosestToPoint(edges, pos());
            int edgeIndex = closestEdgeIterator - edges.constBegin();

            // Set the correct text direction
            switch (edgeIndex) {
            case 0:
                _textDirection = TopToBottom;
                break;

            case 1:
                _textDirection = RightToLeft;
                break;

            case 2:
                _textDirection = BottomToTop;
                break;

            case 3:
            default:
                _textDirection = LeftToRight;
                break;
            }
        }
    }
}

void Connector::moveWirePoint() const
{
    if (not _wire) {
        return;
    }

    // If the wire is also moving the connector must not update the wire
    if (_wire->isSelected()) {
        return;
    }

    if (_wirePointIndex < -1 or _wire->wirePointsRelative().count() <= _wirePointIndex) {
        return;
    }

    QPointF oldPos = _wire->wirePointsAbsolute().at(_wirePointIndex).toPointF();
    QVector2D moveBy = QVector2D(scenePos() - oldPos);
    _wire->movePointBy(_wirePointIndex, moveBy);
}

void Connector::attachWire(Wire* wire, int index)
{
    if (not wire) {
        return;
    }

    if (index < -1 or wire->wirePointsRelative().count() < index) {
        return;
    }

    // Detach wire if there is already one attached
    detachWire();

    _wire = wire;
    _wirePointIndex = index;

    // Update index when points are inserted/removed
    connect(wire, &Wire::pointInserted, this, &Connector::pointInserted);
    connect(wire, &Wire::pointRemoved, this, &Connector::pointRemoved);
    connect(wire, &QObject::destroyed, this, &Connector::detachWire);
}

void Connector::detachWire()
{
    if (!_wire) {
        return;
    }

    disconnect(_wire, nullptr, this, nullptr);
    _wire = nullptr;
    _wirePointIndex = -1;
}

const Wire* Connector::attachedWire() const
{
    return _wire;
}

int Connector::attachedWirepoint() const
{
    return _wirePointIndex;
}
