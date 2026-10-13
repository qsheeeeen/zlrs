# Roadmap

The baseline design specifies one bidirectional DATA link on a fixed band or coordinated automatic 2.4 GHz/SubG switching. The near-term work is to measure the data service under real RF conditions and calibrate the dynamic power policy. The DATA port and management protocol should remain stable while the radio scheduler evolves.

| Priority | Work | Acceptance |
| --- | --- | --- |
| High | Characterize throughput and loss on LF/HF boards | Repeated bidirectional byte-exact transfers, under clean and impaired RF conditions. |
| High | Calibrate dynamic DATA TX power | Measure RSSI/LQ against packet loss and confirm the 3 dB steps, hold time, floor and feedback-loss fallback. |
| High | Validate sub-G duty behavior | Confirm deferred transmission and recovery under each supported region. |
| High | Validate automatic band switching | Sustained poor quality, one-way/total loss, lost announcements, 2.4 GHz probe success/failure, staggered startup and reboot; byte-exact delivery across transitions and preserved duty budget. |
| Medium | Complete encrypted profile | Implement and validate P2 with interoperable host and hardware tests. |
| Medium | Reliability and serviceability | Exercise brownout, watchdog, update rollback, log recovery and long-running transfers. |

Automatic switching uses authenticated future-frame plans, hysteresis and asymmetric recovery searches, as specified in [protocol](08-protocol.md#自动跨频段切换). Fixed-band configuration remains available. Parallel DATA operation on both bands is outside this design.
