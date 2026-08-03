#include "main_window.h"

#include <QApplication>
#include <QStyleFactory>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("VoiceSpreader"));
    QApplication::setOrganizationName(QStringLiteral("VoiceSpreader"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        application.setStyle(fusion);
    }

    MainWindow window;
    window.show();
    return application.exec();
}
