#include "mainwindow.h"
#include <QApplication>
#include <QPalette>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion"));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#121614"));
    palette.setColor(QPalette::WindowText, QColor("#ecf0ea"));
    palette.setColor(QPalette::Base, QColor("#191e1b"));
    palette.setColor(QPalette::AlternateBase, QColor("#1c221e"));
    palette.setColor(QPalette::Text, QColor("#ecf0ea"));
    palette.setColor(QPalette::Button, QColor("#252d27"));
    palette.setColor(QPalette::ButtonText, QColor("#ecf0ea"));
    palette.setColor(QPalette::Highlight, QColor("#4b6943"));
    palette.setColor(QPalette::HighlightedText, QColor("#ecf0ea"));
    a.setPalette(palette);
    QCoreApplication::setOrganizationName("Zijing");
    // Keep the existing storage identity so history, favorites and music
    // credentials remain available after the product is renamed.
    QCoreApplication::setApplicationName("MusicPlayer2");
    QApplication::setApplicationDisplayName(QStringLiteral("探歌 · Tango"));
    MainWindow w;
    w.show();
    const QStringList files = a.arguments().mid(1);
    if (!files.isEmpty())
        QTimer::singleShot(0, &w, [&w, files]() { w.addLocalFiles(files); });
    return a.exec();
}
