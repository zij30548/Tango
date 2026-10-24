#ifndef PASSWORDVISIBILITY_H
#define PASSWORDVISIBILITY_H

#include <QAction>
#include <QIcon>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>

// A native trailing action keeps the reveal control inside the password field.
// Both modes retain sensitive-input hints; no password is copied into labels.
inline QIcon passwordVisibilityIcon(bool revealed)
{
    QPixmap icon(24, 24);
    icon.fill(Qt::transparent);
    QPainter painter(&icon);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#b4d9a0")), 1.6, Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    QPainterPath eye;
    eye.moveTo(3, 12);
    eye.cubicTo(7, 5, 17, 5, 21, 12);
    eye.cubicTo(17, 19, 7, 19, 3, 12);
    painter.drawPath(eye);
    painter.drawEllipse(QPointF(12, 12), 2.6, 2.6);
    if (revealed) painter.drawLine(QPointF(4, 4), QPointF(20, 20));
    return QIcon(icon);
}

inline QAction *installPasswordVisibility(QLineEdit *field, const QString &actionName,
                                          const QString &fieldDescription)
{
    auto *action = new QAction(field);
    action->setObjectName(actionName);
    action->setCheckable(true);
    field->addAction(action, QLineEdit::TrailingPosition);
    const auto refresh = [field, action, fieldDescription](bool revealed) {
        field->setEchoMode(revealed ? QLineEdit::Normal : QLineEdit::Password);
        field->setInputMethodHints(Qt::ImhSensitiveData | Qt::ImhNoPredictiveText |
                                   Qt::ImhNoAutoUppercase | Qt::ImhHiddenText);
        const QString title = (revealed ? QStringLiteral("隐藏") : QStringLiteral("显示")) + fieldDescription;
        action->setText(title);
        action->setToolTip(title);
        action->setIcon(passwordVisibilityIcon(revealed));
        for (auto *button : field->findChildren<QToolButton *>()) {
            if (button->defaultAction() && button->defaultAction() != action) continue;
            button->setObjectName(action->objectName() + QStringLiteral("Button"));
            button->setAccessibleName(title);
            button->setAccessibleDescription(QStringLiteral("切换密码可见性，不更改输入内容"));
            button->setCursor(Qt::PointingHandCursor);
            button->setFocusPolicy(Qt::StrongFocus);
        }
    };
    QObject::connect(action, &QAction::toggled, field, refresh);
    refresh(false);
    return action;
}

inline void concealPassword(QLineEdit *field, QAction *action)
{
    if (action) action->setChecked(false);
    field->setEchoMode(QLineEdit::Password);
}

#endif
