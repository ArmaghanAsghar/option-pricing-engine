#include <QApplication>

#include "main_window.hpp"

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // QSettings needs these to pick a stable location for the backend URL.
    QApplication::setOrganizationName(QStringLiteral("pricer"));
    QApplication::setApplicationName(QStringLiteral("OptionPricer"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    MainWindow window;
    window.show();
    return app.exec();
}
