#include "api_client.hpp"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {
constexpr int kTimeoutMs = 30000;
}

ApiClient::ApiClient(QObject* parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)) {}

void ApiClient::setBaseUrl(const QString& url) {
    QString trimmed = url.trimmed();
    while (trimmed.endsWith('/')) trimmed.chop(1);
    if (!trimmed.isEmpty()) base_url_ = trimmed;
}

void ApiClient::trackStart() {
    if (++in_flight_ == 1) emit busyChanged(true);
}

void ApiClient::trackFinish() {
    if (--in_flight_ <= 0) {
        in_flight_ = 0;
        emit busyChanged(false);
    }
}

void ApiClient::post(const QString& path, const QJsonObject& body, JsonCallback on_ok,
                     ErrorCallback on_error) {
    QNetworkRequest request(QUrl(base_url_ + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(kTimeoutMs);

    trackStart();
    auto* reply = network_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));

    connect(reply, &QNetworkReply::finished, this, [this, reply, on_ok, on_error]() {
        reply->deleteLater();
        trackFinish();

        const QByteArray payload = reply->readAll();
        QJsonParseError parse_error{};
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &parse_error);

        if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
            // A transport failure usually means no JSON body at all.
            if (reply->error() != QNetworkReply::NoError) {
                on_error(reply->errorString());
            } else {
                on_error(QStringLiteral("server sent a malformed response: %1")
                             .arg(parse_error.errorString()));
            }
            return;
        }

        const QJsonObject object = doc.object();
        // The API reports its own failures in an "error" field, which is more
        // useful than Qt's generic status text.
        if (object.contains(QStringLiteral("error"))) {
            on_error(object.value(QStringLiteral("error")).toString());
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            on_error(reply->errorString());
            return;
        }
        on_ok(object);
    });
}

void ApiClient::get(const QString& path, JsonCallback on_ok, ErrorCallback on_error) {
    QNetworkRequest request(QUrl(base_url_ + path));
    request.setTransferTimeout(kTimeoutMs);

    trackStart();
    auto* reply = network_->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, on_ok, on_error]() {
        reply->deleteLater();
        trackFinish();

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            on_error(reply->error() != QNetworkReply::NoError
                         ? reply->errorString()
                         : QStringLiteral("server sent a malformed response"));
            return;
        }
        const QJsonObject object = doc.object();
        if (object.contains(QStringLiteral("error"))) {
            on_error(object.value(QStringLiteral("error")).toString());
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            on_error(reply->errorString());
            return;
        }
        on_ok(object);
    });
}

void ApiClient::getArray(const QString& path, std::function<void(const QJsonArray&)> on_ok,
                         ErrorCallback on_error) {
    QNetworkRequest request(QUrl(base_url_ + path));
    request.setTransferTimeout(kTimeoutMs);

    trackStart();
    auto* reply = network_->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, on_ok, on_error]() {
        reply->deleteLater();
        trackFinish();

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (reply->error() != QNetworkReply::NoError) {
            on_error(reply->errorString());
            return;
        }
        if (!doc.isArray()) {
            on_error(QStringLiteral("expected a JSON array"));
            return;
        }
        on_ok(doc.array());
    });
}

void ApiClient::price(const QJsonObject& request, JsonCallback on_ok, ErrorCallback on_error) {
    post(QStringLiteral("/price"), request, std::move(on_ok), std::move(on_error));
}

void ApiClient::curve(const QJsonObject& request, JsonCallback on_ok, ErrorCallback on_error) {
    post(QStringLiteral("/price/curve"), request, std::move(on_ok), std::move(on_error));
}

void ApiClient::impliedVol(const QJsonObject& request, JsonCallback on_ok,
                           ErrorCallback on_error) {
    post(QStringLiteral("/implied_vol"), request, std::move(on_ok), std::move(on_error));
}

void ApiClient::snapshot(const QString& symbol, JsonCallback on_ok, ErrorCallback on_error) {
    get(QStringLiteral("/market/snapshot/") + symbol, std::move(on_ok), std::move(on_error));
}

void ApiClient::history(const QString& symbol, int days, JsonCallback on_ok,
                        ErrorCallback on_error) {
    get(QStringLiteral("/market/history/%1?days=%2").arg(symbol).arg(days), std::move(on_ok),
        std::move(on_error));
}

void ApiClient::chain(const QString& symbol, JsonCallback on_ok, ErrorCallback on_error) {
    get(QStringLiteral("/market/chain/") + symbol, std::move(on_ok), std::move(on_error));
}

void ApiClient::health(JsonCallback on_ok, ErrorCallback on_error) {
    get(QStringLiteral("/health"), std::move(on_ok), std::move(on_error));
}

void ApiClient::engines(std::function<void(const QJsonArray&)> on_ok, ErrorCallback on_error) {
    getArray(QStringLiteral("/engines"), std::move(on_ok), std::move(on_error));
}
