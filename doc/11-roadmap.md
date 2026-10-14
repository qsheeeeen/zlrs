# Roadmap

The product scope is one bidirectional DATA stream with P1 authentication,
stop-and-wait ARQ, frequency hopping, dynamic power and fixed or coordinated
automatic 2.4 GHz/SubG operation. Management, signed upgrades, rollback, logs,
entropy and local status support that service. Design contracts are specified in
[architecture](05-architecture.md), [protocol](07-protocol.md) and
[security](08-security.md).

## RF and product qualification

| Priority | Work | Required outcome |
| --- | --- | --- |
| High | Throughput and loss characterization on LF/HF | Repeatable bidirectional byte-exact transfers with clean and impaired RF; distinguish PHY loss from ARQ delivery |
| High | Power and antenna calibration | Measured PA output, current and RSSI for each module/path and finished carrier |
| High | Dynamic-power calibration | Packet-loss curves around thresholds; verified steps, floor, hold periods and feedback-loss fallback |
| High | Regional duty qualification | Deferred DATA/control, correct accounting and recovery in every supported region |
| High | Automatic switching qualification | Poor quality, one-way/total loss, announcement loss, both probe outcomes, restart and maintenance under continuous DATA; preserved session/duty progress |
| Medium | Reliability and servicing | Long-running duplex load, brownout, watchdog, storage recovery and confirmed/unconfirmed upgrades |

Automated host, simulation and bench evidence must remain separate from design
requirements. Hardware coverage must identify its module SKU, region, antennas,
configuration and interference conditions. LF observations do not qualify HF or
EU868 hardware behavior. Thresholds remain operating policy pending calibration.

## Planned scope

An encrypted profile requires an agreed wire format, encryption/key/nonce
contract and interoperable host/hardware acceptance. P2 is not available through
the current protocol or settings. Any encryption extension must preserve the
replay and delivery guarantees and define its authentication-strength boundary.

The [carrier](03-carrier.md) specifies the two-module PCB form. A smaller
UART-only variant requires its own hardware description, power design and
service/update transport. Simultaneous DATA transmission on both bands is outside
the single-radio design.
