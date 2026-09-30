#pragma once

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QJsonObject>
#include <QSpinBox>
#include <QWidget>
#include <optional>

/// The contract and market inputs. Emits `changed` whenever any field moves so
/// the window can reprice and redraw.
class PricingPanel : public QWidget {
    Q_OBJECT

public:
    explicit PricingPanel(QWidget* parent = nullptr);

    /// Builds the JSON body for POST /price.
    QJsonObject priceRequest(bool want_paths_and_convergence) const;

    /// Builds the JSON body for POST /price/curve over the given axis.
    QJsonObject curveRequest(const QString& axis, double from, double to, int points) const;

    /// Fills spot, volatility and dividend from a market snapshot.
    void applySnapshot(const QJsonObject& suggested_market);

    double spot() const { return spot_->value(); }
    double strike() const { return strike_->value(); }
    double expiry() const { return expiry_->value(); }
    double barrier() const;
    QString engineName() const;
    QString payoffKind() const;
    bool isStochastic() const;

    /// A sensible spot range for the payoff and Greek charts.
    std::pair<double, double> spotRange() const;

signals:
    void changed();

private slots:
    void onPayoffKindChanged();

private:
    QGroupBox* buildContractGroup();
    QGroupBox* buildMarketGroup();
    QGroupBox* buildEngineGroup();

    // Contract.
    QComboBox* payoff_kind_;
    QComboBox* option_type_;
    QComboBox* exercise_;
    QDoubleSpinBox* strike_;
    QDoubleSpinBox* expiry_;

    // Path-dependent extras.
    QWidget* asian_row_;
    QSpinBox* averaging_steps_;
    QWidget* barrier_row_;
    QComboBox* barrier_direction_;
    QDoubleSpinBox* barrier_level_;
    QDoubleSpinBox* rebate_;

    // Market.
    QDoubleSpinBox* spot_;
    QDoubleSpinBox* vol_;
    QDoubleSpinBox* rate_;
    QDoubleSpinBox* dividend_;

    // Engine.
    QComboBox* engine_;
    QSpinBox* steps_;
    QSpinBox* paths_thousands_;
    QSpinBox* seed_;
    QWidget* tree_row_;
    QWidget* mc_row_;
};
