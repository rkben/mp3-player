#pragma once

#include <QLabel>
#include <QMap>
#include <QString>

// A status label with priority layers. Independent sources each post a message to a
// numbered slot; the highest-priority (largest slot) non-empty message is shown. This
// lets a transient hint overlay long-running progress without either clobbering the
// other — when the higher layer clears, the one below reappears. Any int is a valid
// priority; post(slot, QString()) clears that slot. The label hides when no layer has
// a message.
//
// Not a Q_OBJECT (it adds no signals/slots), so it needs no moc; it's just a QLabel
// that knows how to pick which of several pending messages to display.
class StatusStack : public QLabel
{
public:
    using QLabel::QLabel;

    // Define a value per source; larger wins. Co-located so the priority ordering of
    // all status sources is visible in one place.
    enum Slot {
        Scan     = 0,    // library scan / background import progress
        Prefetch = 5,    // resolving the *next* remote stream ahead of time (background)
        Fetch    = 10,   // resolving/opening a remote stream (transient, user-initiated)
    };

    void post(int slot, const QString &message)
    {
        if (message.isEmpty())
            m_layers.remove(slot);
        else
            m_layers.insert(slot, message);

        if (m_layers.isEmpty()) {
            clear();
            hide();
        } else {
            setText(m_layers.last());   // QMap is key-ordered: last() == highest slot
            show();
        }
    }

private:
    QMap<int, QString> m_layers;
};
