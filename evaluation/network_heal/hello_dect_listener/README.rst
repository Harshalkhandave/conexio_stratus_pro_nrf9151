.. _hello_dect_listener:

DECT NR+ network heal — listener
################################

.. contents::
   :local:
   :depth: 2

Listener / monitor node for a simple DECT NR+ PHY beacon network.
It receives sensor/heartbeat packets from one or more broadcasters, tracks
online/offline state, and prints automatic rejoin metrics for network-heal
evaluation.

This application is derived from the NCS ``hello_dect`` PHY sample and is intended
for presence / recovery testing, not full FT/PT association.

Overview
********

* Continuously listens on a configured DECT carrier and network ID.
* Parses packed sensor payloads (node ID, sequence, temp/humidity/battery,
  ``uptime_ms``, checksum).
* Tracks each broadcaster (discover, offline, rejoin, RX count, checksum fails).
* Declares a device **offline** after ``OFFLINE_THRESHOLD_MS`` (default 2 s) of silence.
* On rejoin, logs machine-readable ``METRIC`` lines including
  ``boot_to_heard_ms`` (node uptime in the first packet after power-on).

Typical setup: **1 listener + 2 broadcasters**.

Requirements
************

* Conexio Stratus Pro (``conexio_stratus_pro/nrf9151/ns``) or compatible nRF91x1 NS target
* NCS v3.2.1 (or matching SDK used to build)
* DECT NR+ PHY modem firmware (for example ``mfw-nr+_nrf91x1_1.1.0``)
* Matching ``CONFIG_CARRIER`` / ``CONFIG_NETWORK_ID`` with the broadcasters
* At least one broadcaster running ``hello_dect_broadcaster``


Configuration
*************

Important Kconfig options (see ``Kconfig``, overlays):

* ``CONFIG_CARRIER`` — RF channel (must match broadcasters; required non-zero)
* ``CONFIG_NETWORK_ID`` — network / short-network filter (default 91 / ``0x5B``)
* ``CONFIG_RX_PERIOD_S`` — RX window length in seconds (default 5)

Application constants in ``src/main.c``:

* ``OFFLINE_THRESHOLD_MS`` — silence before OFFLINE (default 2000)
* ``OFFLINE_WARN_PERIOD_MS`` — throttle for “still offline” logs

Expected log / METRIC lines
***************************

Discover::

   METRIC,event=discover,id=<id>,boot_to_heard_ms=<ms>,seq=<n>,rssi_x2=<v>

Offline::

   METRIC,event=offline,id=<id>,silence_before_flag_ms=<ms>,...

Rejoin (main heal metric)::

   METRIC,event=rejoin,id=<id>,boot_to_heard_ms=<ms>,silence_gap_ms=<ms>,...

``boot_to_heard_ms`` is taken from the broadcaster’s ``uptime_ms`` field and is the
time from node power-on until the listener heard it again (no stopwatch needed).

Serial capture script
*********************

See ``scripts/README.rst``. Example::

   cd scripts
   python -m venv .venv
   .venv\Scripts\activate
   pip install -r requirements.txt
   python serial_logger.py --list
   python serial_logger.py COM9

Logs are written under ``scripts/logs/``.

Network heal test (short)
*************************

1. Flash listener on board L; broadcaster on B1 and B2 (same carrier/network).
2. Start ``serial_logger.py`` on the listener COM port.
3. Power on L, then B1/B2; confirm discover ``METRIC`` lines.
4. Power-cycle one broadcaster; copy ``METRIC,event=rejoin`` lines into the report.
5. Confirm the other broadcaster stays online.

Scope note: this measures **beacon presence recovery**, not MAC association heal.

References
**********

* Companion app: ``hello_dect_broadcaster``
* Upstream sample: NCS ``nrf/samples/dect/dect_phy/hello_dect``
* Modem API: ``nrf_modem_dect_phy``
