.. _hello_dect_broadcaster:

DECT NR+ network heal — broadcaster
###################################

.. contents::
   :local:
   :depth: 2

Beacon / sensor node for a simple DECT NR+ PHY network-heal test.
It periodically transmits a packed payload so a ``hello_dect_listener`` can
discover the node, detect offline gaps, and measure rejoin time after power-cycle.

This application is derived from the NCS ``hello_dect`` PHY sample.

Overview
********

* Initializes the DECT NR+ PHY modem interface.
* Every ``TX_INTERVAL_MS`` (default 500 ms) builds and transmits:

  * ``node_id`` — device ID (from hwinfo)
  * ``sequence`` — packet counter (resets to 0 after reboot)
  * temperature / humidity / battery (demo values)
  * ``uptime_ms`` — ``k_uptime_get()`` at TX time (used by listener for
    ``boot_to_heard_ms``)
  * XOR ``checksum`` over all preceding bytes

* Uses LBT on TX; same carrier and network ID as the listener.

Typical setup: **2 (or more) broadcasters + 1 listener**.

Requirements
************

* Conexio Stratus Pro (``conexio_stratus_pro/nrf9151/ns``) or compatible nRF91x1 NS target
* NCS v3.2.1 (or matching SDK used to build)
* DECT NR+ PHY modem firmware (for example ``mfw-nr+_nrf91x1_1.1.0``)
* Matching ``CONFIG_CARRIER`` / ``CONFIG_NETWORK_ID`` with the listener

Configuration
*************

Important Kconfig options:

* ``CONFIG_CARRIER`` — RF channel (must match listener; required non-zero)
* ``CONFIG_NETWORK_ID`` — network ID (default 91 / ``0x5B``)
* ``CONFIG_TX_POWER`` — TX power index (overlay may set e.g. 10)
* ``CONFIG_MCS`` — modulation coding scheme

Application constants in ``src/main.c``:

* ``TX_INTERVAL_MS`` — time between beacons (default 500)

Payload and listener metrics
****************************

The listener uses ``uptime_ms`` from this payload as ``boot_to_heard_ms`` on
discover/rejoin. Keep the packed struct identical on both sides when changing
fields.

After a power cycle, ``sequence`` restarts at 0 (cold start).

Usage with the listener
***********************

1. Build/flash this app on each beacon board (same overlay as listener).
2. Power on the listener first (optional), then broadcasters.
3. On the listener serial log, confirm ``NEW DEVICE DISCOVERED`` / ``METRIC,event=discover``.
4. Power-cycle this board to exercise rejoin; listener reports
   ``METRIC,event=rejoin,...,boot_to_heard_ms=...``.

For automated serial capture on the **listener**, see
``hello_dect_listener/scripts/README.rst``.

References
**********

* Companion app: ``hello_dect_listener``
* Upstream sample: NCS ``nrf/samples/dect/dect_phy/hello_dect``
* Modem API: ``nrf_modem_dect_phy``
