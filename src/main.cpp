#include "main_window.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QMetaObject>
#include <QStyleFactory>
#include <QSystemTrayIcon>
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
    application.setQuitOnLastWindowClosed(false);
    QApplication::setApplicationName(QStringLiteral("VoiceSpreader"));
    QApplication::setOrganizationName(QStringLiteral("VoiceSpreader"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        application.setStyle(fusion);
    }

    const QString fontFamily = loadApplicationFonts();
    if (!fontFamily.isEmpty()) {
        QFont applicationFont(fontFamily);
        applicationFont.setPointSizeF(10.0);
        applicationFont.setHintingPreference(QFont::PreferFullHinting);
        applicationFont.setStyleStrategy(
            static_cast<QFont::StyleStrategy>(QFont::PreferQuality
                                              | QFont::PreferAntialias));
        application.setFont(applicationFont);
    }
    application.setWindowIcon(QIcon(QStringLiteral(":/assets/app.png")));

    QString previewPath;
    QString bluetoothPreviewPath;
    bool previewDark = false;
    bool previewPhoneConnected = false;
    bool startMinimized = false;
    double previewPhoneLevel = -160.0;
    QSize requestedWindowSize;
    for (const QString& argument : application.arguments()) {
        if (argument.startsWith(QStringLiteral("--render-preview="))) {
            previewPath = argument.mid(QStringLiteral("--render-preview=").size());
        } else if (argument.startsWith(
                       QStringLiteral("--render-bluetooth-preview="))) {
            bluetoothPreviewPath = argument.mid(
                QStringLiteral("--render-bluetooth-preview=").size());
        } else if (argument.startsWith(QStringLiteral("--render-preview-dark="))) {
            previewDark = true;
            previewPath = argument.mid(QStringLiteral("--render-preview-dark=").size());
        } else if (argument.startsWith(QStringLiteral("--window-size="))) {
            const QStringList parts = argument
                                          .mid(QStringLiteral("--window-size=").size())
                                          .split(QLatin1Char('x'));
            bool widthValid = false;
            bool heightValid = false;
            const int width = parts.value(0).toInt(&widthValid);
            const int height = parts.value(1).toInt(&heightValid);
            if (parts.size() == 2 && widthValid && heightValid
                && width > 0 && height > 0) {
                requestedWindowSize = QSize(width, height);
            }
        } else if (argument.startsWith(QStringLiteral("--preview-phone-level="))) {
            bool valid = false;
            const double level = argument
                                     .mid(QStringLiteral("--preview-phone-level=").size())
                                     .toDouble(&valid);
            if (valid) {
                previewPhoneConnected = true;
                previewPhoneLevel = level;
            }
        } else if (argument == QStringLiteral("--start-minimized")) {
            startMinimized = true;
        }
    }

    MainWindow window;
    if (requestedWindowSize.isValid()) {
        window.resize(requestedWindowSize);
    }
    const bool previewRequested = !previewPath.isEmpty()
                                  || !bluetoothPreviewPath.isEmpty();
    if (!startMinimized || previewRequested
        || !QSystemTrayIcon::isSystemTrayAvailable()) {
        window.show();
    }

    if (previewPhoneConnected) {
        QMetaObject::invokeMethod(
            &window,
            "updatePhoneConnection",
            Qt::DirectConnection,
            Q_ARG(bool, true),
            Q_ARG(QString, QStringLiteral("预览手机")));
        QMetaObject::invokeMethod(
            &window,
            "updatePhoneMicrophoneLevel",
            Qt::DirectConnection,
            Q_ARG(double, previewPhoneLevel));
    }

    if (!bluetoothPreviewPath.isEmpty()) {
        QTimer::singleShot(
            0,
            &window,
            [&application, &window, bluetoothPreviewPath] {
                QTimer::singleShot(
                    1600,
                    &application,
                    [&application, bluetoothPreviewPath] {
                        QWidget* modal = QApplication::activeModalWidget();
                        const bool saved = modal != nullptr
                                           && modal->grab().save(
                                               bluetoothPreviewPath);
                        if (modal != nullptr) {
                            modal->close();
                        }
                        application.exit(saved ? 0 : 2);
                    });
                QMetaObject::invokeMethod(&window,
                                          "showBluetoothAudioReceiver",
                                          Qt::DirectConnection);
            });
    }

    if (!previewPath.isEmpty() && bluetoothPreviewPath.isEmpty()) {
        if (previewDark) {
            QMetaObject::invokeMethod(&window, "toggleTheme", Qt::DirectConnection);
        }
        QTimer::singleShot(600, &application, [&application, &window, previewPath]() {
            application.exit(window.grab().save(previewPath) ? 0 : 2);
        });
    }

    return application.exec();
}
