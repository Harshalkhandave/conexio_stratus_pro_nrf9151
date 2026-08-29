.. _evaluation_scripts:

DECT evaluation scripts
#######################

.. contents::
   :local:
   :depth: 2

Shared automation scripts for Conexio Stratus Pro / nRF9151 DECT NR+ evaluation
work under this repository's ``evaluation/`` tree.

This folder currently holds the **Range & Parameter Evaluation** tool used with
Nordic's ``dect_shell`` sample (MCS and TX-power sweeps over distance).

For network-heal serial capture, see
``evaluation/network_heal/hello_dect_listener/scripts/``.

Files
*****

* ``dect_test.py`` — drives Board 2 (client) over serial; runs MCS + TX power
  sweeps at each distance; writes CSV + text log
* ``requirements.txt`` — Python dependencies (``pyserial``)

Purpose
*******

Automate the indoor range / parameter campaign documented in the
*DECT NR+ Range & Parameter Evaluation Report*:

* Distances: 1 m, 3 m, 5 m, 10 m, 15 m, 20 m
* MCS sweep: 0–4 at fixed TX power index 13
* TX power sweep: indices 13, 10, 7, 5, 3 at fixed MCS 1
* Parses PDR-related counts, throughput, RSSI min/max, SNR min into CSV

Requirements
************

* Two boards running ``dect_shell`` (DECT NR+ PHY modem firmware)
* Python 3 with ``pyserial``
* Free COM port for **Board 2** (close VS Code / nRF serial terminals first)

Setup
*****

::

   cd evaluation/Scripts
   python -m venv .venv
   .venv\Scripts\activate
   python -m pip install -r requirements.txt

Configuration
*************

Edit the constants at the top of ``dect_test.py`` before running:

* ``PORT`` — Board 2 COM port (e.g. ``COM7``)
* ``BAUD`` — default ``115200``
* ``CHANNEL`` — e.g. ``1711`` (US)
* ``SERVER_ID`` — Board 1 transmitter ID from ``dect sett -r``
* ``DURATION`` — seconds per perf test (e.g. ``10``)

Also adjust ``MCS_LIST``, ``PWR_LIST``, and ``DIST_LIST`` if the campaign changes.

Board setup (before / during the script)
****************************************

Board 1 (server) — start manually and keep running::

   dect sett --reset
   dect activate
   dect perf -s -t -1 --channel 1711

Board 2 (client) — controlled by ``dect_test.py`` over serial.
The script will reset/activate Board 2 and issue ``dect perf -c ...`` commands.

When prompted, move the boards to the next distance and confirm Board 1's
perf server is still running.

Run
***

::

   python dect_test.py

Outputs (created in the current working directory):

* ``dect_results_YYYYMMDD_HHMMSS.csv`` — metrics table for the report
* ``dect_log_YYYYMMDD_HHMMSS.txt`` — full terminal capture

CSV columns include distance, test type (``MCS_SWEEP`` / ``PWR_SWEEP``), MCS,
TX power, packet counts, loss, throughput, CRC counters, RSSI, and SNR.

Notes
*****

* Only one process may own the COM port on Windows.
* The script pauses for Enter between distances so you can reposition hardware.
* Keep ``SERVER_ID`` and ``CHANNEL`` consistent with the live ``dect_shell``
  session on Board 1.
