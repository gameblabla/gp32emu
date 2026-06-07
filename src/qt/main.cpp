#include "GP32MainWindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QTimer>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("GP32emu");
    QCoreApplication::setApplicationName("GP32emu");
    GP32MainWindow w;
    w.show();
    if (argc > 1) QTimer::singleShot(0, [&w, argc, argv]() { w.engine()->configureFromArgs(argc, argv); w.engine()->start(); });
    return app.exec();
}
