#include "svgitem.h"

#include <QByteArray>
#include <QSvgRenderer>

namespace geometrize
{

namespace scene
{

SvgItem::SvgItem(const QByteArray& data) : QGraphicsSvgItem{}
{
    setSharedRenderer(new QSvgRenderer(data));
    setFlag(ItemIsMovable, false);
}

SvgItem::~SvgItem()
{
	delete renderer();
}

void SvgItem::setDocument(const QByteArray& data)
{
    // setSharedRenderer 不接管所有权,渲染器由本 item 持有(构造函数 new 出来、析构函数删),
    // 故换文档时先取旧指针、装新渲染器、再删旧的——旧渲染器此刻已无人引用。
    QSvgRenderer* previous{renderer()};
    setSharedRenderer(new QSvgRenderer(data));
    delete previous;
}

}

}
