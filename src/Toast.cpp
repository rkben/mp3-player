#include "Toast.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QEvent>
#include <QMetaObject>
#include <QFontMetrics>

ToastArea *ToastArea::s_instance = nullptr;

namespace {
constexpr int kToastMs = 4000;     // how long a toast lingers
constexpr int kMaxToasts = 4;      // cap visible stack
// Vertical anchor: toasts sit between a top and bottom stretch weighted
// kTopWeight:kBottomWeight, i.e. ~75% down the overlay — clear of the transport bar
// at the bottom. Raise kTopWeight to push lower, lower it to raise. Pills are
// right-aligned (see addToast).
constexpr int kTopWeight = 3;
constexpr int kBottomWeight = 1;
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
    // Top and bottom stretches sandwich the toast stack at ~75% height (see the
    // weight knobs above). Toasts are inserted between them in addToast().
    m_layout->addStretch(kTopWeight);
    m_layout->addStretch(kBottomWeight);

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
    // Sizing approach distilled from niklashenning/qt-toast: don't trust the layout's
    // heightForWidth for a word-wrapped label (it clipped the text). Instead measure the
    // wrapped text with QFontMetrics, place the label at a fixed geometry, and give the
    // pill a fixed size = text box + padding. The QVBoxLayout just stacks fixed pills.
    constexpr int padX = 14, padY = 9;
    auto *pill = new QWidget(this);
    pill->setObjectName("toast");
    // Self-contained styling so a toast reads on any theme without a global stylesheet.
    pill->setStyleSheet(
        "QWidget#toast {"
        "  background: rgba(40,40,40,235);"
        "  border: 1px solid rgba(255,255,255,40); border-radius: 8px; }");

    auto *label = new QLabel(text, pill);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    label->setStyleSheet(QStringLiteral("color: #f0f0f0; background: transparent;"));

    const int maxTextW = qMax(220, qMin(520, width() - 48)) - 2 * padX;
    const QRect br = label->fontMetrics().boundingRect(
        QRect(0, 0, maxTextW, 0), Qt::TextWordWrap, text);
    const int textW = qMin(br.width() + 1, maxTextW);
    const int textH = br.height();
    label->setGeometry(padX, padY, textW, textH);
    pill->setFixedSize(textW + 2 * padX, textH + 2 * padY);
    // Insert just before the bottom stretch (last item), anchored to the right.
    m_layout->insertWidget(m_layout->count() - 1, pill, 0, Qt::AlignRight);

    // Evict the oldest toast if we exceed the cap. The stack is bracketed by two
    // stretches, so toast count is count()-2 and the oldest toast is at index 1.
    while (m_layout->count() - 2 > kMaxToasts) {
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
