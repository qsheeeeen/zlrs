# Roadmap

The baseline design specifies one bidirectional DATA link on a selected 2.4 GHz or sub-GHz band. The near-term work is to measure the data service under real RF conditions and calibrate the dynamic power policy. The DATA port and management protocol should remain stable while the radio scheduler evolves.

| Priority | Work | Acceptance |
| --- | --- | --- |
| High | Characterize throughput and loss on LF/HF boards | Repeated bidirectional byte-exact transfers, under clean and impaired RF conditions. |
| High | Calibrate dynamic DATA TX power | Measure RSSI/LQ against packet loss and confirm the 3 dB steps, hold time, floor and feedback-loss fallback. |
| High | Validate sub-G duty behavior | Confirm deferred transmission and recovery under each supported region. |
| Medium | Improve band use | Evaluate automatic DATA switching or parallel operation on 2.4 GHz and sub-G without changing serial semantics. |
| Medium | Complete encrypted profile | Implement and validate P2 with interoperable host and hardware tests. |
| Medium | Reliability and serviceability | Exercise brownout, watchdog, update rollback, log recovery and long-running transfers. |

Any band-switching design needs explicit coordination between peers, hysteresis, regulatory accounting and a way to recover when the new band is unavailable. An explicitly configured `set-band` choice remains the baseline for future coordinated switching.
