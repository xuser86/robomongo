#pragma once

#include <QHeaderView>

namespace Robomongo
{
    class BsonHeaderView : public QHeaderView 
    {
        Q_OBJECT
    public:
        BsonHeaderView(Qt::Orientation orientation, QWidget *parent = Q_NULLPTR);
        
    protected: 
        void resizeEvent(QResizeEvent *);
        
    private:
        int resizeCount = 0;
        bool _defaultHeaderState;
        bool _blockSectionResizedSlot;
        
    private Q_SLOTS:
        void _sectionResized(int logicalIndex, int oldSize, int newSize);
    };
}
