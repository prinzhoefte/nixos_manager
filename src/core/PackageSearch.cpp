#include "PackageSearch.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

namespace nixm {
namespace {

// search.nixos.org exposes its Elasticsearch index through a public,
// read-only account; the credentials are shipped in the site's own frontend.
// Both the endpoint and the credentials can be overridden from the environment
// for anyone running a private mirror of the index.
constexpr const char *kDefaultEndpoint = "https://search.nixos.org/backend";
constexpr const char *kDefaultUser = "aWVSALXpZv";
constexpr const char *kDefaultPassword = "X8gPHnzL52wFEekuxsfQ9cSh";

// The index name embeds a schema generation that the nixos-search project bumps
// whenever its Elasticsearch mapping changes. We try the generation we know
// about first, then walk outwards, and remember whichever one answers — so the
// app keeps working across an upstream bump without needing an update.
constexpr int kKnownGeneration = 50;
constexpr int kMaxGeneration = 60;
constexpr int kMinGeneration = 40;

QVector<int> candidateGenerations(int remembered)
{
    QVector<int> out;
    auto add = [&out](int g) {
        if (g >= kMinGeneration && g <= kMaxGeneration && !out.contains(g))
            out.push_back(g);
    };
    add(remembered);
    add(kKnownGeneration);
    for (int g = kKnownGeneration + 1; g <= kMaxGeneration; ++g)
        add(g);
    for (int g = kKnownGeneration - 1; g >= kMinGeneration; --g)
        add(g);
    return out;
}

QString envOr(const char *name, const char *fallback)
{
    const QByteArray v = qgetenv(name);
    return v.isEmpty() ? QString::fromLatin1(fallback) : QString::fromLocal8Bit(v);
}

QString firstString(const QJsonValue &v)
{
    if (v.isString())
        return v.toString();
    if (v.isArray()) {
        const QJsonArray a = v.toArray();
        if (!a.isEmpty()) {
            if (a.first().isString())
                return a.first().toString();
            if (a.first().isObject()) {
                const QJsonObject o = a.first().toObject();
                for (const char *key : { "url", "fullName", "shortName", "value" }) {
                    const QString s = o.value(QLatin1String(key)).toString();
                    if (!s.isEmpty())
                        return s;
                }
            }
        }
    }
    return QString();
}

QStringList stringList(const QJsonValue &v)
{
    QStringList out;
    for (const QJsonValue &e : v.toArray()) {
        if (e.isString())
            out << e.toString();
        else if (e.isObject())
            out << firstString(QJsonArray{ e });
    }
    return out;
}

} // namespace

PackageSearch::PackageSearch(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
    QSettings s;
    m_generation = s.value(QStringLiteral("search/indexGeneration"), 0).toInt();
    m_ttlHours = s.value(QStringLiteral("search/cacheTtlHours"), 24).toInt();
}

PackageSearch::~PackageSearch()
{
    cancel();
}

void PackageSearch::setChannel(const QString &channel)
{
    if (!channel.isEmpty())
        m_channel = channel;
}

QString PackageSearch::cacheDir() const
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return QDir(base).absoluteFilePath(QStringLiteral("packages/") + m_channel);
}

QString PackageSearch::cachePathFor(const QString &query, int limit) const
{
    const QString key = query.trimmed().toLower() + QLatin1Char('|') + QString::number(limit);
    const QByteArray hash
        = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QDir(cacheDir()).absoluteFilePath(QString::fromLatin1(hash) + QStringLiteral(".json"));
}

bool PackageSearch::loadFromCache(const QString &query, int limit, QVector<PackageResult> *out,
    bool ignoreExpiry) const
{
    const QString path = cachePathFor(query, limit);
    QFileInfo info(path);
    if (!info.exists())
        return false;
    if (!ignoreExpiry && m_ttlHours > 0
        && info.lastModified().secsTo(QDateTime::currentDateTime()) > m_ttlHours * 3600)
        return false;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    *out = parseResponse(f.readAll());
    return true;
}

void PackageSearch::storeInCache(const QString &query, int limit, const QByteArray &payload) const
{
    QDir().mkpath(cacheDir());
    QFile f(cachePathFor(query, limit));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(payload);
}

void PackageSearch::clearCache()
{
    QDir(QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
             .absoluteFilePath(QStringLiteral("packages")))
        .removeRecursively();
}

void PackageSearch::cancel()
{
    if (!m_reply)
        return;
    QNetworkReply *r = m_reply;
    m_reply = nullptr;
    r->disconnect(this);
    r->abort();
    r->deleteLater();
    emit busyChanged(false);
}

void PackageSearch::search(const QString &query, int limit)
{
    const QString q = query.trimmed();
    if (q.isEmpty()) {
        emit resultsReady(q, {}, true);
        return;
    }

    QVector<PackageResult> cached;
    if (loadFromCache(q, limit, &cached, false)) {
        emit resultsReady(q, cached, true);
        return;
    }

    cancel();
    sendRequest(q, limit, 0);
}

QByteArray PackageSearch::buildQueryBody(const QString &query, int limit) const
{
    // Mirrors the query shape used by search.nixos.org: exact attribute matches
    // rank highest, then program names, then pname, then the descriptions.
    const QJsonArray fields = {
        QStringLiteral("package_attr_name^9"),
        QStringLiteral("package_attr_name.*^5.4"),
        QStringLiteral("package_programs^9"),
        QStringLiteral("package_programs.*^5.4"),
        QStringLiteral("package_pname^6"),
        QStringLiteral("package_pname.*^3.6"),
        QStringLiteral("package_description^1.3"),
        QStringLiteral("package_description.*^0.78"),
        QStringLiteral("package_longDescription^1"),
        QStringLiteral("package_longDescription.*^0.6"),
    };

    QJsonObject multiMatch{
        { QStringLiteral("type"), QStringLiteral("cross_fields") },
        { QStringLiteral("query"), query },
        { QStringLiteral("analyzer"), QStringLiteral("whitespace") },
        { QStringLiteral("auto_generate_synonyms_phrase_query"), false },
        { QStringLiteral("operator"), QStringLiteral("and") },
        { QStringLiteral("fields"), fields },
    };

    QJsonObject wildcard{ { QStringLiteral("package_attr_name"),
        QJsonObject{ { QStringLiteral("value"), QStringLiteral("*%1*").arg(query.toLower()) },
            { QStringLiteral("case_insensitive"), true } } } };

    QJsonObject disMax{
        { QStringLiteral("tie_breaker"), 0.7 },
        { QStringLiteral("queries"),
            QJsonArray{ QJsonObject{ { QStringLiteral("multi_match"), multiMatch } },
                QJsonObject{ { QStringLiteral("wildcard"), wildcard } } } },
    };

    QJsonObject boolQuery{
        { QStringLiteral("filter"),
            QJsonArray{ QJsonObject{ { QStringLiteral("term"),
                QJsonObject{ { QStringLiteral("type"),
                    QJsonObject{ { QStringLiteral("value"), QStringLiteral("package") } } } } } } } },
        { QStringLiteral("must"),
            QJsonArray{ QJsonObject{ { QStringLiteral("dis_max"), disMax } } } },
    };

    QJsonObject body{
        { QStringLiteral("from"), 0 },
        { QStringLiteral("size"), limit },
        { QStringLiteral("query"), QJsonObject{ { QStringLiteral("bool"), boolQuery } } },
    };

    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

void PackageSearch::sendRequest(const QString &query, int limit, int candidateIndex)
{
    const QVector<int> candidates = candidateGenerations(m_generation);

    if (candidateIndex >= candidates.size()) {
        // Nothing answered. Fall back to a stale cache entry if we have one.
        QVector<PackageResult> stale;
        if (loadFromCache(query, limit, &stale, true)) {
            emit resultsReady(query, stale, true);
            emit failed(tr("Could not reach the package index; showing cached results."));
        } else {
            emit failed(tr("Could not reach the package index. Check your network connection, "
                           "or set NIXOS_MANAGER_SEARCH_URL to a reachable mirror."));
        }
        emit busyChanged(false);
        return;
    }

    const int generation = candidates.at(candidateIndex);
    const QString endpoint = envOr("NIXOS_MANAGER_SEARCH_URL", kDefaultEndpoint);
    const QString user = envOr("NIXOS_MANAGER_SEARCH_USER", kDefaultUser);
    const QString password = envOr("NIXOS_MANAGER_SEARCH_PASSWORD", kDefaultPassword);
    const QString url = QStringLiteral("%1/latest-%2-%3/_search")
                            .arg(endpoint)
                            .arg(generation)
                            .arg(m_channel);

    QNetworkRequest req{ QUrl(url) };
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json");
    const QByteArray auth
        = (user + QLatin1Char(':') + password).toUtf8().toBase64();
    req.setRawHeader("Authorization", "Basic " + auth);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(20000);

    m_reply = m_net->post(req, buildQueryBody(query, limit));
    emit busyChanged(true);
    connect(m_reply, &QNetworkReply::finished, this,
        [this, query, limit, candidateIndex] { handleReply(query, limit, candidateIndex); });
}

void PackageSearch::handleReply(const QString &query, int limit, int candidateIndex)
{
    const QVector<int> candidates = candidateGenerations(m_generation);
    const int generation
        = candidateIndex < candidates.size() ? candidates.at(candidateIndex) : kKnownGeneration;

    QNetworkReply *reply = m_reply;
    if (!reply)
        return;
    m_reply = nullptr;
    reply->deleteLater();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();

    if (reply->error() != QNetworkReply::NoError || status != 200) {
        // A missing index just means we guessed the generation wrong.
        if (status == 404 || status == 400) {
            sendRequest(query, limit, candidateIndex + 1);
            return;
        }
        QVector<PackageResult> stale;
        if (loadFromCache(query, limit, &stale, true)) {
            emit resultsReady(query, stale, true);
            emit failed(tr("Search failed (%1); showing cached results.").arg(reply->errorString()));
        } else {
            emit failed(tr("Search failed: %1").arg(reply->errorString()));
        }
        emit busyChanged(false);
        return;
    }

    if (m_generation != generation) {
        m_generation = generation;
        QSettings().setValue(QStringLiteral("search/indexGeneration"), generation);
    }

    storeInCache(query, limit, body);
    emit resultsReady(query, parseResponse(body), false);
    emit busyChanged(false);
}

QVector<PackageResult> PackageSearch::parseResponse(const QByteArray &json)
{
    QVector<PackageResult> out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    const QJsonArray hits
        = doc.object().value(QStringLiteral("hits")).toObject().value(QStringLiteral("hits")).toArray();

    out.reserve(hits.size());
    for (const QJsonValue &hit : hits) {
        const QJsonObject src = hit.toObject().value(QStringLiteral("_source")).toObject();
        PackageResult r;
        r.attr = src.value(QStringLiteral("package_attr_name")).toString();
        r.pname = src.value(QStringLiteral("package_pname")).toString();
        r.version = src.value(QStringLiteral("package_pversion")).toString();
        r.description = src.value(QStringLiteral("package_description")).toString();
        r.longDescription = src.value(QStringLiteral("package_longDescription")).toString();
        r.homepage = firstString(src.value(QStringLiteral("package_homepage")));
        r.license = firstString(src.value(QStringLiteral("package_license")));
        if (r.license.isEmpty())
            r.license = firstString(src.value(QStringLiteral("package_license_set")));
        r.programs = stringList(src.value(QStringLiteral("package_programs")));
        r.position = src.value(QStringLiteral("package_position")).toString();
        if (!r.attr.isEmpty())
            out.push_back(r);
    }
    return out;
}

} // namespace nixm
