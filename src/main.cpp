#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QNetworkProxyFactory>
#include <QSettings>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("nixos-manager"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("nixos.org"));
    QCoreApplication::setApplicationName(QStringLiteral("nixos-manager"));
    QCoreApplication::setApplicationVersion(QStringLiteral(NIXOS_MANAGER_VERSION));
    QGuiApplication::setDesktopFileName(QStringLiteral("org.nixos.manager"));

    // Honour http_proxy / https_proxy and the desktop's proxy settings, so
    // package search works on networks that require one.
    QNetworkProxyFactory::setUseSystemConfiguration(true);

    // The JR-IT palette, typography and widget style. Everything built after
    // this point picks it up automatically.
    const QString themeSetting
        = QSettings().value(QStringLiteral("appearance/theme"), QStringLiteral("system"))
              .toString();
    nixm::Theme::apply(&app,
        themeSetting == QLatin1String("dark")    ? nixm::Theme::Dark
            : themeSetting == QLatin1String("light") ? nixm::Theme::Light
                                                     : nixm::Theme::System);
    QApplication::setWindowIcon(nixm::Theme::logo());

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Browse, edit and rebuild NixOS configurations."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("config"),
        QStringLiteral("Directory holding flake.nix or configuration.nix "
                       "(defaults to $NIXOS_MANAGER_CONFIG, then /etc/nixos)."));
    parser.process(app);

    const QStringList positional = parser.positionalArguments();

    nixm::MainWindow window;
    window.resize(1280, 860);
    window.show();
    window.openInitialProject(positional.isEmpty() ? QString() : positional.first());

    return app.exec();
}
