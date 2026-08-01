#include "ui/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QNetworkProxyFactory>

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

    QIcon::setThemeName(QIcon::themeName());
    if (QIcon::hasThemeIcon(QStringLiteral("org.nixos.manager")))
        QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("org.nixos.manager")));

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
