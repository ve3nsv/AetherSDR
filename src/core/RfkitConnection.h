#pragma once

#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>

#include "RfkitProtocol.h"

namespace AetherSDR {

// Peripheral transport for an RF-KIT amplifier.
//
// A standalone Ethernet device with no FlexRadio awareness at all, so this is
// a peripheral(rfkit) accessory alongside AcomConnection/SpeConnection/
// VkampConnection/Kpa1500Connection, not an IRadioBackend implementor. See
// docs/architecture/rfkit-amplifier-design.md.
//
// The device speaks a REST API over HTTP/1.1 (RfkitProtocol.h), not a
// streaming socket protocol, and it never pushes: every reading below arrives
// because this class asked for it. So the shape differs from the socket-based
// peripherals in two ways:
//
//  * "Connected" is not a TCP state. connectNetwork() issues GET /info, and
//    the link counts as up only when that returns an RF-KIT-shaped document.
//    It counts as down again after kMaxConsecutiveFailures transport failures
//    in a row, so one dropped request does not flicker the applet.
//  * Requests are strictly serialized, one in flight at a time. The device is
//    a small embedded controller, and a command that has to queue behind a
//    full poll round on a congested link is better than a burst that makes it
//    drop requests. Commands jump the queue; polls are only enqueued when the
//    queue is empty, so a slow device lowers the poll rate instead of
//    building a backlog.
//
// Like every peripheral here, control writes never latch an optimistic value:
// the device is the authority on its own state, and the applet repaints from
// the reply (constitution Principle II).
class RfkitConnection : public QObject {
    Q_OBJECT

public:
    explicit RfkitConnection(QObject* parent = nullptr);
    ~RfkitConnection() override;

    bool isConnected() const { return m_connected; }
    QString description() const;  // "192.168.1.60:8080", for status display

    void connectNetwork(const QString& host, quint16 port = Rfkit::kDefaultPort);
    void disconnect();
    void setAutoReconnect(bool on) { m_autoReconnect = on; }

    // Control (host -> amp). Each is a no-op, logged, when not connected.
    void setOperate(bool operate);
    // An antenna outside the API document's range for its type is refused,
    // logged, and never sent.
    void selectAntenna(const Rfkit::Antenna& antenna);
    // POST /error/reset. Clears the amplifier's error state; the device answers
    // 409 while the function is disabled, which is logged.
    void resetError();
    // PUT /tuner {"tuner_mode":"START"}: asks the amplifier to run its automatic
    // tune. Ignored while a previous start is still outstanding. Sent only when the amplifier's server was found to accept it
    // (status().tuneApi) and its tuner is in automatic or manual mode; refused,
    // logged, and never sent otherwise. The amplifier tunes only while a tune
    // carrier is already being transmitted. Nothing here keys the radio.
    void startTune();

    // Tells the connection whether the radio is transmitting (MOX, TUNE or any
    // other transmit), so it can poll faster while power and SWR are moving. It
    // only chooses the polling cadence: nothing here keys the radio.
    void setTransmitting(bool transmitting);
    // While on, and the amplifier's REST server was found to accept PUT /tuner,
    // sends PUT /tuner {} as soon as that is known and then once a minute. On the
    // server inspected the PUT handlers reset the amplifier's sleep timer and wake
    // its sleep screen before they read the body, and {} is not a tune start, so
    // this keeps the amplifier from going to sleep (a sleeping RF2K-S drops its
    // tuner to bypass) while AetherSDR is connected. Off by default: it overrides
    // the owner's sleep setting, and a stock amplifier has no such PUT route, so
    // it does nothing there. Skipped while transmitting.
    void setKeepAwake(bool on);
    bool keepAwake() const { return m_keepAwake; }

signals:
    void connected();
    void disconnected();
    void connectionFailed(const QString& errorString);
    // Full current snapshot, emitted when a reply actually changed something.
    void statusUpdated(const AetherSDR::Rfkit::Status& status);

private:
    struct Request {
        enum class Method { Get, Put, Post, Options };
        Method method{Method::Get};
        QString path;
        QByteArray body;
        bool probe{false};  // the connect handshake (GET /info)
        bool command{false};  // operator-issued; kept in issue order ahead of polls
        bool tuneProbe{false};  // OPTIONS /tuner: does this server have a PUT route?
        bool keepAlive{false};  // PUT /tuner {}: wakes the amplifier, never starts a tune
    };

    static bool isTuneStart(const Request& request);
    void enqueue(const Request& request, bool front = false);
    void pump();
    void onReplyFinished(QNetworkReply* reply, const Request& request, int epoch);
    // Returns true when the reply changed the snapshot.
    bool applyReply(const Request& request, const QByteArray& body);
    void onProbeSucceeded();
    void onTransportFailure(const QString& errorString, const Request& request);
    void onTransportDown(const QString& errorString);
    void poll();
    void enqueueFullRefresh();
    // Asks, with OPTIONS /tuner, whether the route allows PUT. Retried only while the answer stays
    // Unknown, at most kMaxTuneProbes times per session, and never with one
    // already outstanding.
    void maybeProbeTuneApi();
    // Sends one keep-awake request when enabled, supported, connected, idle and
    // not transmitting; and starts or stops the repeat timer to match.
    void syncKeepAwake();
    void sendKeepAlive();
    int pollIntervalMs() const;
    void armReconnect();
    void resetSession();

    QNetworkAccessManager m_nam;
    QUrl m_baseUrl;
    QPointer<QNetworkReply> m_reply;
    QQueue<Request> m_queue;
    bool m_inFlight{false};

    Rfkit::Status m_status;
    // The last body seen per path, so an unchanged slow reply does not
    // re-emit statusUpdated().
    QHash<QString, QByteArray> m_lastBody;
    // Paths already warned about for an unparseable 2xx body, so a firmware
    // that answers with an unexpected shape logs once per connection rather
    // than four times a second.
    QSet<QString> m_warnedPaths;

    QString m_lastHost;
    quint16 m_lastPort{Rfkit::kDefaultPort};

    bool m_connected{false};
    bool m_autoReconnect{false};
    bool m_deliberateDisconnect{false};

    // Bumped on every connect/disconnect. A reply carries the epoch it was
    // issued under, and one from a previous session is discarded unread: a
    // late answer from attempt N must never touch attempt N+1 (#2380).
    int m_epoch{0};
    int m_consecutiveFailures{0};
    static constexpr int kMaxConsecutiveFailures = 3;

    // Per-request transfer timeout. Also bounds the connect handshake, so an
    // unreachable address reports failure here rather than after the OS TCP
    // connect timeout (~21s on Windows).
    static constexpr int kRequestTimeoutMs = 4000;
    // Writes get longer: every PUT handler on the server wakes a sleeping
    // amplifier before it answers, and a bare PUT /tuner took about 5 s on an
    // RF2K-S that was asleep. Still inside the whole-request deadline below.
    static constexpr int kCommandTimeoutMs = 9000;
    // The transfer timeout above only fires when no bytes move; this bounds the
    // whole request, so a peer that trickles bytes cannot hold the one serialized
    // slot (and every command queued behind it) indefinitely. Principle VII.
    static constexpr int kRequestDeadlineMs = 10000;

    QTimer m_reconnectTimer;
    static constexpr int kReconnectIntervalMs = 5000;

    // One endpoint per tick from the schedule in RfkitProtocol.h (pollPath()),
    // 150 ms receiving and 15 ms transmitting.
    QTimer m_pollTimer;
    QTimer m_deadlineTimer;
    int m_pollSlot{0};
    // Kept across sessions: the radio's transmit state does not depend on the
    // amplifier link.
    bool m_transmitting{false};

    bool m_keepAwake{false};
    bool m_keepAlivePending{false};
    QTimer m_keepAwakeTimer;

    bool m_tuneProbePending{false};
    // A START is outstanding (queued or in flight). The tuner readback that
    // would disable the button can be a second old, so this keeps a double
    // click from sending START twice. It is a send guard, not displayed state.
    bool m_tuneStartPending{false};
    int m_tuneProbeAttempts{0};
    static constexpr int kMaxTuneProbes = 3;
};

}  // namespace AetherSDR
