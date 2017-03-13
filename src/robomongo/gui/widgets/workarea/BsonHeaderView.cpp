#include <QHeaderView>
#include <QResizeEvent>
#include <QDebug>

#include "robomongo/core/utils/QtUtils.h"
#include "robomongo/gui/widgets/workarea/BsonHeaderView.h"

namespace Robomongo
{
    BsonHeaderView::BsonHeaderView(Qt::Orientation orientation, QWidget *parent) 
        : QHeaderView(orientation,parent)
    {
         qDebug() << "creating " << this;
         VERIFY(connect(this, SIGNAL(sectionResized(int,int,int)), this, SLOT(_sectionResized(int,int,int))));
         
         _defaultHeaderState = true;
         _blockSectionResizedSlot = false;
    }
    
    BsonHeaderView::~BsonHeaderView() 
    {
        qDebug() << "deleting " << this;
    }
    
    void BsonHeaderView::resizeEvent(QResizeEvent *event) 
    {
        QHeaderView::resizeEvent(event);
        
        resizeCount ++;
        
        if ( _defaultHeaderState ) {
            _blockSectionResizedSlot = true;
            
            qDebug() << "Stretch";
            
            resizeSections(QHeaderView::Stretch);
            
            _blockSectionResizedSlot = false;
            //_defaultHeaderState = true;
        }
        
        qDebug() << "resize header nr:" << resizeCount << " "
                 << " oldSize " << event->oldSize().width() << "x" << event->oldSize().height() << " "
                 << " newSize " << event->size().width() << "x" << event->size().height();
    }
    
    void BsonHeaderView::_sectionResized(int logicalIndex, int oldSize, int newSize) 
    {
        if ( _blockSectionResizedSlot ) {
            return;
        }
        
        qDebug() << "section resized idx:" << logicalIndex
                 << " oldSize:" << oldSize 
                 << " newSize:" << newSize;
                 
        _defaultHeaderState = false;
    }
}
