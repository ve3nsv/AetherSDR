#pragma once

#include "core/RfkitProtocol.h"

#include <QPushButton>
#include <QTimer>
#include <QVector>
#include <QWidget>

class QLabel;

namespace AetherSDR {

class HGauge;

// Dedicated applet for an RF-KIT amplifier — a sibling of AmpApplet (PGXL),
// AcomApplet, SpeApplet, VkampApplet and Kpa1500Applet, not a variant of any
// of them. See docs/architecture/rfkit-amplifier-design.md.
//
// The vendor's REST API exposes the tuner read-only, so by default the tuner
// appears here as a status line and an indicator. A TUNE button appears only
// when the amplifier's server was found to accept a tune request (an
// amplifier whose restServer.py has been modified to add one); it asks, and
// the tuner state shown is always the amplifier's own readback. What the
// vendor API does let the operator change — operate/standby, the active
// antenna, and clearing an error — are the other controls below. The operational interface
// (UNIV/CAT/UDP/TCI) is shown but deliberately not switchable from here:
// changing it can silently cut the amplifier off from the radio that is
// driving it.
class RfkitApplet : public QWidget {
    Q_OBJECT

public:
    explicit RfkitApplet(QWidget* parent = nullptr);

    // One call per status snapshot — the applet reads the fields it shows.
    // Unset optionals render as "—", never as 0: the device answers each
    // endpoint independently, so "not reported yet" and "reported as zero" are
    // genuinely different states here.
    void setStatus(const Rfkit::Status& status);
    void setConnected(bool connected);

signals:
    void operateToggled(bool operate);
    void antennaSelected(const AetherSDR::Rfkit::Antenna& antenna);
    void errorResetRequested();
    void tuneRequested();

private:
    void updateValueLabels();
    void refreshControls();
    void refreshAntennas();
    QString extIndicatorDescription() const;
    void refreshTuneButton();
    // Paints the header's tuner pill: green when the tuner is in use, red when
    // bypassed or off, neutral when unknown or disconnected.
    void setTunerPill(Rfkit::TunerPill pill);
    // True when the amplifier is reporting something: a non-empty status text
    // (idle is the empty string) or an operational-interface error.
    bool errorShowing() const;
    // Colour the reflected-power bar by SWR: orange past 1.5:1, red past 2:1.
    void updateReflectedZones(float forwardWatts);

    HGauge* m_pwrGauge{nullptr};
    HGauge* m_refGauge{nullptr};
    HGauge* m_swrGauge{nullptr};
    // Reflected-power watts at which the REF bar turns orange / red (-1 = unset).
    float m_refYellowW{-1.0f};
    float m_refRedW{-1.0f};

    QLabel* m_pwrLabel{nullptr};
    QLabel* m_refLabel{nullptr};
    QLabel* m_swrLabel{nullptr};

    QLabel* m_nameLabel{nullptr};
    QLabel* m_statusPill{nullptr};
    QLabel* m_tempLabel{nullptr};
    QLabel* m_voltsLabel{nullptr};
    QLabel* m_currentLabel{nullptr};
    QLabel* m_bandLabel{nullptr};
    QLabel* m_freqLabel{nullptr};
    QLabel* m_ifaceLabel{nullptr};
    QLabel* m_tunerLabel{nullptr};
    QLabel* m_faultLabel{nullptr};

    QPushButton* m_operateBtn{nullptr};
    QLabel* m_tunerPill{nullptr};      // read-only tuner state, beside the connection pill
    QPushButton* m_tuneBtn{nullptr};   // asks for a tune; only shown when supported
    QPushButton* m_resetErrorBtn{nullptr};
    // One button per internal antenna (the API allows 1-4), plus a read-only
    // indicator for the amplifier's single external selection.
    QVector<QPushButton*> m_antBtns;
    QLabel* m_extIndicator{nullptr};  // read-only external-antenna indicator

    QTimer m_labelTimer;

    Rfkit::Status m_status;
    bool m_connected{false};
    // The pill's current look, so a repaint that changes nothing does not
    // re-apply the stylesheet.
    QString m_tunerPillStyleKey;
    QString m_tuneHelp;  // the TUNE tooltip before any drive-power note

    // setStatus() arrives at the connection's poll rate — funneled through
    // the m_labelTimer tick, the same throttle convention the other amplifier
    // applets use, rather than repainting and re-announcing accessibility text
    // on every reply.
    bool m_valuesDirty{false};
};

}  // namespace AetherSDR
