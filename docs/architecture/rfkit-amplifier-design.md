# RF-KIT amplifier — design note

Status: implemented; tested on an RF2K-S (firmware G200C267). Other RF-KIT models are untested.

## 1. Scope

A direct-connect peripheral for the RF-KIT amplifier, alongside ACOM, SPE
Expert, VK3AMP and the Elecraft KPA1500. It has no FlexRadio awareness: the
radio neither discovers it nor relays its telemetry, so it is a
`peripheral(rfkit)` accessory, not an `IRadioBackend` implementor, and it
deliberately does not route through `AmpModel` (the same reasoning as the ACOM
and KPA1500 notes: a shared model would make the PGXL panel appear whenever an
unrelated peripheral connects).

## 2. Protocol authority

The vendor's published "RFKIT API and SDK Documentation" (OpenAPI 0.9.0), a
manufacturer document and therefore a clean-room input (Constitution Principle
IV). Nothing here is reverse-engineered. Anything the document does not state
is listed in §7 rather than guessed.

The wire is HTTP/1.1 with JSON bodies, unauthenticated, on a port the document
shows as 8080.

| Method | Path | Use |
|---|---|---|
| GET | `/info` | connect handshake; device name, firmware versions |
| GET | `/power` | forward / reflected / SWR / temperature / voltage / current |
| GET | `/data` | band, frequency, free-text status |
| GET | `/tuner` | tuner mode, setup, L / C |
| PUT | `/tuner` | start an automatic tune (`{"tuner_mode":"START"}`); not in the vendor document, only on an amplifier whose REST server has been modified to add it, and discovered per connection (below) |
| GET | `/antennas` | every antenna with `ACTIVE` / `AVAILABLE` / `DISABLED` |
| PUT | `/antennas/active` | select an antenna |
| GET / PUT | `/operate-mode` | `OPERATE` / `STANDBY` |
| GET | `/operational-interface` | `UNIV` / `CAT` / `UDP` / `TCI`, plus a read-only `error` |
| POST | `/error/reset` | clear the error state |

`GET /antennas/active` and `PUT /operational-interface` exist in the API and are
deliberately not used (§5).

## 3. Layers

- `core/RfkitProtocol.{h,cpp}` — stateless: one parser per endpoint (bytes in,
  `std::optional` out) and one body builder per writable endpoint. Covered by
  `tests/rfkit_protocol_test.cpp`.
- `core/RfkitConnection.{h,cpp}` — owns the `QNetworkAccessManager`, the poll
  schedule, the request queue and reconnect. Covered by
  `tests/rfkit_connection_test.cpp`, a loopback mock of the documented HTTP API
  (the case `docs/agents/tests-ci.md` allows).
- `gui/RfkitApplet.{h,cpp}` — the applet, category *Amplifiers*, id `RFKIT`.
- Wiring in `MainWindow_Wiring.cpp`; the Peripherals row in
  `RadioSetupDialog_Peripherals.cpp` (settings group `Rfkit`: `ManualIp`,
  `ManualPort`).

## 4. Connection model

HTTP has no connection to be "up", so:

- `connectNetwork()` issues `GET /info`. The link is **connected** only when
  that returns an RF-KIT-shaped document. A 404, an HTML page, a non-JSON body
  or an oversized body fails the connect (`connectionFailed`), so pointing the
  field at a router or another web server does not half-connect.
- **Disconnected** after three consecutive transport failures (refused,
  timeout, reset, oversized). An HTTP error *status* (4xx/5xx) is the device
  answering, so it is logged and does not count as link loss.
- Requests are **strictly serialized**, one in flight. Commands go ahead of
  queued polls but stay in the order they were issued (two antenna clicks reach
  the amplifier in click order); a poll round is only enqueued when the queue is empty, so a slow device
  lowers the poll rate instead of growing a backlog. The device is a small
  embedded controller.
- Poll: `/power` and `/data` every 250 ms, plus one of `/operate-mode`,
  `/antennas`, `/tuner`, `/operational-interface` per tick in rotation. A full
  refresh runs once on connect so the applet fills immediately.
- Each reply carries the session epoch it was issued under; a late reply from a
  previous session is discarded unread (#2380).
- Boundary hygiene (Principle VII): bodies capped at 64 KiB (also enforced
  while streaming), no redirects, no proxy, no cache, a 4 s transfer timeout (an
  inactivity timeout) plus a 10 s deadline on the whole of every request, so a
  peer that trickles bytes cannot hold the single serialized slot; a transfer
  that breaks after the headers counts as a transport failure, while an HTTP
  error status does not. The address field is built into a `QUrl` in strict mode so
  a stray `/`, `@` or space cannot change the request target.

## 5. What the operator can and cannot do

Controls: OPERATE/STANDBY, antenna selection (internal 1–4,
range-checked before the wire), and error reset, which is one click with no
confirmation and is sent only while the amplifier is reporting an
error (a non-empty `/data` status, idle being the empty string, or an
operational-interface error); the amplifier refuses one it cannot do (409).

The red rule is "any non-empty status text", and that text is shown in the same red
banner as the interface error ("Error: High SWR"). The amplifier's manual lists eight
error messages (High Antenna Reflection, High Input Power, Severe Error LPF,
Wrong Frequency, No internal high voltage, High Current, Overheating, High SWR),
each a safety-circuit trip cleared by Reset, and an idle amplifier reports an
empty status, so no listed message is informational. A status the manual does
not list is also treated as an error; that can only show red when it need not,
never green when it should be red.

Never latched optimistically (Principle II): every control asks, and the
applet repaints only from the device's own reply; the antenna buttons only
move their highlight when the device's list confirms it.

The external antenna is shown, not switchable. A real RF2K-S (firmware
G200C267) always lists `{"type":"EXTERNAL","state":"AVAILABLE"}`, with no number
and even with its Ext. Antenna switch setting off, so a button would be offered
to every owner, including those with no external switch, and one press would
move the amplifier's RF path. Nobody has seen what the amplifier does with
that request, so the applet only lights an **EXT** indicator while the
amplifier reports the external antenna active, and adds the port when it
sends one ("EXT 16"). The API document marks `ExternalAntenna.number` (1–16)
readOnly: it is the port of the operator's external antenna switch, reported
but not settable, which is why a selection request would carry no number. What
a unit reports while the external antenna is active has not been observed; the
document's example is `{"type":"EXTERNAL","number":16,"state":"ACTIVE"}`. The
parser keeps that port separately (`AntennaWithState::externalPort`) and the
protocol layer can build the number-less request, so a selectable button is a
small change once someone with an external switch has tried it.

The device refuses an antenna change with 409 when changes are not allowed;
the condition is internal to the amplifier and is not interlocked here.

Deliberately absent:

- **Transmit keying.** The API has no keying endpoint; nothing here can put RF
  on the air (Principle VI).
- **Operational interface changes.** `PUT /operational-interface` could cut the
  amplifier off from the radio that is driving it, silently. It is shown, not
  switchable.
- **Tuner configuration.** Beyond asking for a tune (below), nothing sets the
  tuner's mode or network; the vendor API exposes the tuner read-only.

### Tune start (modified servers only)

The vendor document has no tune command. A community modification of the
amplifier's `restServer.py` adds `PUT /tuner {"tuner_mode":"START"}`, which
calls the amplifier's own autotune. The amplifier tunes only while a tune
carrier is already on the air, so this request keys nothing and the applet
never transmits (Principle VI).

- **Discovery, not assumption.** After connecting, the connection sends
  `PUT /tuner` with the empty body `{}`. A modified server answers 400 (or a
  200 naming OFF or BYPASS); a stock server has no such route and answers 405.
  2xx and 400 mean supported; 404, 405 and 501 mean not supported; anything
  else stays unknown and is asked again, up to three times per connection. The
  probe cannot start a tune on a server that, like the one verified on an
  RF2K-S, rejects a body without `tuner_mode` with a 400. A modified server that
  treated a missing `tuner_mode` as START would defeat that.
- **Gate.** `startTune()` sends only when the probe found support and the last
  tuner readback is AUTO or MANUAL. It is refused while the tuner is OFF,
  bypassed, already tuning, or unread.
- **No latch (Principle II).** The reply to the start request is not taken as
  state: the connection re-reads `/tuner` and `/data`. The TUNE button reads
  TUNING only when the amplifier's mode says so (`AUTO_TUNING_FROM_AUTO`
  decodes as tuning).

## 6. Applet behaviour

- Gauges use fixed scales that match the amplifier's own display (forward
  0–1600 W, reflected 0–200 W, SWR on its 1 / 1.5 / 2 / 3 / 5 / ∞ scale). The
  device's `max_value` is not used: it reports the present power limit, not the
  meter range. The SWR bar turns orange from 1.5:1 and red from 2:1; the
  reflected-power bar uses the same two ratios, converted to watts from the
  present forward power. Reflected power and SWR are held at baseline below 1 W
  forward, as in the other amplifier applets.
- A read-only tuner pill in the header, beside the connection pill, names the
  amplifier's tuner state from the reported mode and setup, worded like the
  KPA1500 applet's ATU: ATU IN (AUTO, or a tune in progress) and ATU MAN are
  green; ATU MAN BYP (mode MANUAL, setup BYPASS), ATU BYP, ATU OFF and ATU NO TUNE (AUTO or
  MANUAL with setup `NOT TUNED`, where the manual says transmission is blocked)
  are red. The word `OFF` is
  what a real RF2K-S sends for a switched-off tuner, so the parser takes it as
  well as the document's literal `false`. The pill is a label, not a button.
- The amplifier's name is operator-set text, so it sits in an elided label that
  gives way to the pills and shows the whole name in a tooltip.
- A TUNE button is hidden unless the amplifier's server accepted the discovery
  probe. It is enabled in every tuner state except BYPASS and OFF (where the
  amplifier's server refuses a tune), disables itself on click until the next
  status arrives (one start per click), and reads TUNING (amber) only from the
  amplifier's own tuner readback. A blue outline hints that the forward power
  from `/power` is inside the 4 to 39 W the manual gives for tuning (section 7.2;
  10 W recommended, noted in the tooltip); it is a hint only, derived from the
  reading and never a gate. OPERATE, TUNE and RESET share one row at one
  width, sized to the longest label each can show; the antenna row holds only
  the antenna buttons.
- An unset field renders as "—", never 0; disconnecting drops every remembered
  reading.
- Frequency is formatted from the unit the device reports (Hz / kHz / MHz); an
  unrecognized unit is shown as given rather than mis-scaled. Temperature is
  converted if the device reports °F.

## 7. Open points — to check on real hardware

The API document is silent or ambiguous on these; the implementation chooses
the conservative reading and each is a one-line change:

1. The unit strings actually sent for `band` and `frequency`.
2. The vocabulary of `/data`'s `status` text. It is displayed verbatim; it is
   not parsed into a fault state.
3. The `false` value in the tuner `mode` enum (a generator artifact in the
   document). Accepted as a boolean or the string `"false"` and shown as `OFF`.
4. Whether the manual's eight error messages are the whole vocabulary; any non-empty status is treated as an error (§5).
5. Real poll latency: if the device cannot sustain 250 ms rounds, the
   serialization above degrades the rate gracefully, but the interval may want
   raising.
