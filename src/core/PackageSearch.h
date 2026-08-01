#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;

namespace nixm {

struct PackageResult {
    QString attr;              // "firefox", "kdePackages.filelight"
    QString pname;
    QString version;
    QString description;
    QString longDescription;
    QString homepage;
    QString license;
    QStringList programs;      // binaries the package installs
    QString position;          // nixpkgs source position, if known
};

/// Searches nixpkgs.
///
/// The online index behind search.nixos.org is the primary source: it is fast,
/// carries descriptions and versions, and does not require evaluating nixpkgs.
/// Every answer is cached on disk, so repeating a search works offline and
/// results survive restarts.
class PackageSearch : public QObject
{
    Q_OBJECT

public:
    explicit PackageSearch(QObject *parent = nullptr);
    ~PackageSearch() override;

    /// e.g. "nixos-unstable", "nixos-25.05".
    void setChannel(const QString &channel);
    QString channel() const { return m_channel; }

    void setCacheTtlHours(int hours) { m_ttlHours = hours; }
    int cacheTtlHours() const { return m_ttlHours; }

    void search(const QString &query, int limit = 60);
    void cancel();
    bool isBusy() const { return m_reply != nullptr; }

    void clearCache();
    QString cacheDir() const;

signals:
    void resultsReady(const QString &query, const QVector<PackageResult> &results, bool fromCache);
    void failed(const QString &message);
    void busyChanged(bool busy);

private:
    /// `candidateIndex` walks the list of plausible index generations.
    void sendRequest(const QString &query, int limit, int candidateIndex);
    void handleReply(const QString &query, int limit, int candidateIndex);
    QByteArray buildQueryBody(const QString &query, int limit) const;
    QString cachePathFor(const QString &query, int limit) const;
    bool loadFromCache(const QString &query, int limit, QVector<PackageResult> *out,
        bool ignoreExpiry) const;
    void storeInCache(const QString &query, int limit, const QByteArray &payload) const;
    static QVector<PackageResult> parseResponse(const QByteArray &json);

    QNetworkAccessManager *m_net = nullptr;
    QNetworkReply *m_reply = nullptr;
    QString m_channel = QStringLiteral("nixos-unstable");
    int m_ttlHours = 24;
    int m_generation = 0;   // search.nixos.org index generation, probed once
};

} // namespace nixm
