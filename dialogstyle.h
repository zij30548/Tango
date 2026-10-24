#ifndef DIALOGSTYLE_H
#define DIALOGSTYLE_H

#include <QAbstractButton>
#include <QHeaderView>
#include <QPalette>
#include <QTableWidget>
#include <QWidget>

inline void applyDialogStyle(QWidget *dialog)
{
    // Clear styles left by the original Designer forms before applying one theme.
    for (auto *widget : dialog->findChildren<QWidget *>())
        widget->setStyleSheet(QString());
    QPalette palette = dialog->palette();
    palette.setColor(QPalette::Window, QColor("#121614"));
    palette.setColor(QPalette::WindowText, QColor("#ecf0ea"));
    palette.setColor(QPalette::Base, QColor("#191e1b"));
    palette.setColor(QPalette::AlternateBase, QColor("#1c221e"));
    palette.setColor(QPalette::Text, QColor("#ecf0ea"));
    palette.setColor(QPalette::Button, QColor("#252d27"));
    palette.setColor(QPalette::ButtonText, QColor("#ecf0ea"));
    palette.setColor(QPalette::Highlight, QColor("#b4d9a0"));
    palette.setColor(QPalette::HighlightedText, QColor("#121614"));
    dialog->setPalette(palette);
    dialog->setStyleSheet(QStringLiteral(R"(
        QDialog { background: #121614; color: #ecf0ea; font-size: 13px; font-weight: 400; }
        QLabel { color: #ecf0ea; background: transparent; font-weight: 400; }
        QLabel[role="subtitle"] { color: #8b9890; font-size: 12px; }
        QLabel[role="heading"] { font-size: 22px; font-weight: 600; }
        QGroupBox { color: #ecf0ea; border: none; margin-top: 28px; font-size: 17px; font-weight: 600; }
        QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; }
        QLineEdit, QPlainTextEdit, QTableWidget {
            color: #ecf0ea; background: #191e1b; border: 1px solid #2c352e;
            border-radius: 10px; selection-background-color: #344b37;
            selection-color: #ecf0ea; font-size: 13px; font-weight: 400;
        }
        QLineEdit { min-height: 22px; padding: 7px 10px; }
        QLineEdit:focus { border-color: #b4d9a0; }
        QPlainTextEdit { padding: 10px; }
        QTableWidget { gridline-color: #252d27; alternate-background-color: #1c221e; }
        QTableWidget::item { padding: 7px 10px; border-bottom: 1px solid #252d27; }
        QTableWidget::item:selected { background: #344b37; color: #ecf0ea; }
        QHeaderView::section { background: #191e1b; color: #8b9890; padding: 12px 10px;
            border: none; border-bottom: 1px solid #2c352e; font-weight: normal; }
        QTableCornerButton::section { background: #191e1b; border: none; }
        QPushButton { background: #252d27; color: #ecf0ea; border: 1px solid #354038;
            border-radius: 8px; padding: 8px 16px; min-height: 18px; }
        QPushButton:hover { background: #303c32; border-color: #526350; }
        QPushButton:pressed { background: #3a483c; }
        QPushButton:disabled { background: #1a201c; color: #526057; border-color: #252d27; }
        QPushButton:default, QPushButton[role="primary"] { background: #b4d9a0;
            color: #121614; border-color: #b4d9a0; font-weight: 600; }
        QPushButton[role="primary"]:hover { background: #c5e7b4; }
        QPushButton[role="danger"] { color: #e8aba3; }
        QRadioButton { color: #ecf0ea; spacing: 8px; padding: 6px; }
        QRadioButton::indicator { width: 15px; height: 15px; border-radius: 8px;
            background: #191e1b; border: 1px solid #526350; }
        QRadioButton::indicator:checked { background: #b4d9a0; border: 1px solid #b4d9a0; }
        QMenu { background: #191e1b; color: #ecf0ea; border: 1px solid #354038; padding: 5px; }
        QMenu::item { padding: 8px 20px; }
        QMenu::item:selected { background: #344b37; border-radius: 4px; }
        QScrollBar:vertical { background: #191e1b; width: 9px; margin: 4px 1px; }
        QScrollBar::handle:vertical { background: #445047; min-height: 28px; border-radius: 4px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
    )"));
    for (auto *button : dialog->findChildren<QAbstractButton *>())
        button->setCursor(Qt::PointingHandCursor);
    for (auto *table : dialog->findChildren<QTableWidget *>()) {
        table->setShowGrid(false);
        table->setAlternatingRowColors(true);
        table->setWordWrap(false);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->verticalHeader()->hide();
        table->verticalHeader()->setDefaultSectionSize(42);
        table->horizontalHeader()->setHighlightSections(false);
    }
}

#endif // DIALOGSTYLE_H
