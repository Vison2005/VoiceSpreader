#include "main_window.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QMetaObject>
#include <QStyleFactory>
#include <QTimer>

namespace
{
QString loadApplicationFonts()
{
    const QStringList fontResources = {
        QStringLiteral(":/fonts/HarmonyOS_Sans_SC_Regular.ttf"),
        QStringLiteral(":/fonts/HarmonyOS_Sans_SC_Medium.ttf"),
        QStringLiteral(":/fonts/HarmonyOS_Sans_SC_Bold.ttf"),
    };

    QString family;
    for (const QString& resource : fontResources) {
        const int fontId = QFontDatabase::addApplicationFont(resource);
        const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
        if (family.isEmpty() && !families.isEmpty()) {
            family = families.first();
        }
    }
    return family;
}
}

int main(int argc, char* argv[])
{
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif

    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("VoiceSpreader"));
    QApplication::setOrganizationName(QStringLiteral("VoiceSpreader"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        application.setStyle(fusion);
    }

    const QString fontFamily = loadApplicationFonts();
    if (!fontFamily.isEmpty()) {
        QFont applicationFont(fontFamily, 10);
        applicationFont.setHintingPreference(QFont::PreferVerticalHinting);
        applicationFont.setStyleStrategy(QFont::PreferQuality);
        application.setFont(applicationFont);
    }
    application.setWindowIcon(QIcon(QStringLiteral(":/assets/app.png")));

    MainWindow window;
    window.show();

    QString previewPath;
    bool previewDark = false;
    for (const QString& argument : application.arguments()) {
        if (argument.startsWith(QStringLiteral("--render-preview="))) {
            previewPath = argument.mid(QStringLiteral("--render-preview=").size());
        } else if (argument.startsWith(QStringLiteral("--render-preview-dark="))) {
            previewDark = true;
            previewPath = argument.mid(QStringLiteral("--render-preview-dark=").size());
        }
    }
    if (!previewPath.isEmpty()) {
        if (previewDark) {
            QMetaObject::invokeMethod(&window, "toggleTheme", Qt::DirectConnection);
        }
        QTimer::singleShot(600, &application, [&application, &window, previewPath]() {
            application.exit(window.grab().save(previewPath) ? 0 : 2);
        });
    }

    return application.exec();
}
