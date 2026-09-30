#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <functional>

/// Async JSON client for pricer-api. Every call takes a callback; nothing
/// blocks the GUI thread.
class ApiClient : public QObject {
    Q_OBJECT

public:
    using JsonCallback = std::function<void(const QJsonObject&)>;
    using ErrorCallback = std::function<void(const QString&)>;

    explicit ApiClient(QObject* parent = nullptr);

    void setBaseUrl(const QString& url);
    QString baseUrl() const { return base_url_; }

    // Pricing.
    void price(const QJsonObject& request, JsonCallback on_ok, ErrorCallback on_error);
    void curve(const QJsonObject& request, JsonCallback on_ok, ErrorCallback on_error);
    void impliedVol(const QJsonObject& request, JsonCallback on_ok, ErrorCallback on_error);

    // Market data.
    void snapshot(const QString& symbol, JsonCallback on_ok, ErrorCallback on_error);
    void history(const QString& symbol, int days, JsonCallback on_ok, ErrorCallback on_error);
    void chain(const QString& symbol, JsonCallback on_ok, ErrorCallback on_error);

    // Service.
    void health(JsonCallback on_ok, ErrorCallback on_error);
    void engines(std::function<void(const QJsonArray&)> on_ok, ErrorCallback on_error);

signals:
    /// Emitted around every in-flight request so the window can show progress.
    void busyChanged(bool busy);

private:
    void post(const QString& path, const QJsonObject& body, JsonCallback on_ok,
              ErrorCallback on_error);
    void get(const QString& path, JsonCallback on_ok, ErrorCallback on_error);
    void getArray(const QString& path, std::function<void(const QJsonArray&)> on_ok,
                  ErrorCallback on_error);

    void trackStart();
    void trackFinish();

    QNetworkAccessManager* network_;
    QString base_url_ = QStringLiteral("http://localhost:8080");
    int in_flight_ = 0;
};
