#include "Toast.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QEvent>
#include <QMetaObject>

ToastArea *ToastArea::s_instance = nullptr;

namespace {
constexpr int kToastMs = 4000;     // how long a toast lingers
constexpr int kMaxToasts = 4;      // cap visible stack
}

ToastArea::ToastArea(QWidget *parent)
    : QWidget(parent)
{
    s_instance = this;
    // A transparent overlay that doesn't intercept clicks meant for the UI below.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(12, 12, 12, 16);
    m_layout->setSpacing(6);
    m_layout->addStretch(1);   // pin toasts to the bottom

    parent->installEventFilter(this);   // track the parent's size/position
    reposition();
    raise();
}

ToastArea::~ToastArea()
{
    if (s_instance == this)
        s_instance = nullptr;
}

void ToastArea::post(const QString &text)
{
    if (!s_instance || text.trimmed().isEmpty())
        return;
    // Marshal to the GUI thread: workers (scan/import) may post from off-thread.
    QMetaObject::invokeMethod(s_instance, "addToast", Qt::QueuedConnection,
                              Q_ARG(QString, text));
}

void ToastArea::addToast(const QString &text)
{
    auto *pill = new QLabel(text, this);
    pill->setObjectName("toast");
    pill->setWordWrap(true);
    pill->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    pill->setMargin(0);
    // Self-contained styling so a toast reads on any theme without a global stylesheet.
    pill->setStyleSheet(
        "QLabel#toast {"
        "  background: rgba(40,40,40,235); color: #f0f0f0;"
        "  border: 1px solid rgba(255,255,255,40); border-radius: 8px;"
        "  padding: 8px 12px; }");
    m_layout->addWidget(pill, 0, Qt::AlignHCenter);

    // Evict the oldest toast if we exceed the cap (index 0 is the stretch).
    while (m_layout->count() - 1 > kMaxToasts) {
        if (auto *item = m_layout->itemAt(1)) {
            if (QWidget *w = item->widget())
                w->deleteLater();
            m_layout->removeItem(item);
            delete item;
        } else {
            break;
        }
    }

    raise();
    QTimer::singleShot(kToastMs, pill, [pill] { pill->deleteLater(); });
}

bool ToastArea::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parent()
        && (event->type() == QEvent::Resize || event->type() == QEvent::Move))
        reposition();
    return QWidget::eventFilter(watched, event);
}

void ToastArea::reposition()
{
    if (auto *p = parentWidget()) {
        setGeometry(p->rect());
        raise();
    }
}
