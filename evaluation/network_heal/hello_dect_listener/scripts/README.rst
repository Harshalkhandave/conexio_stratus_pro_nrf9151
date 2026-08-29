.. _hello_dect_listener_scripts:

Serial log capture scripts
##########################

.. contents::
   :local:
   :depth: 2

Python helpers to mirror the listener (or any board) UART to the terminal and
save a ``.txt`` file for network-heal reports.

Files
*****

* ``serial_logger.py`` — live serial mirror + file logger
* ``requirements.txt`` — Python dependencies (``pyserial``)
* ``logs/`` — default output directory for captures (created automatically)

Setup
*****

From this ``scripts`` directory::

   python -m venv .venv
   .venv\Scripts\activate
   python -m pip install -r requirements.txt

On Linux/macOS activate with ``source .venv/bin/activate``.

Usage
*****

List COM ports::

   python serial_logger.py --list

Capture listener output (example COM9, 115200)::

   python serial_logger.py COM9

Custom output path::

   python serial_logger.py COM9 --out logs/trial1.txt

Options:

* ``--baud`` — baud rate (default ``115200``)
* ``--out`` — output ``.txt`` path (default ``logs/listener_YYYYMMDD_HHMMSS.txt``)
* ``--no-timestamp`` — do not prefix host timestamps on each line
* ``--list`` — list serial ports and exit

Stop with **Ctrl+C**. The file is flushed continuously.

Important: COM port access
**************************

Only **one** application can open a COM port on Windows.

Before running ``serial_logger.py`` (or ``newtmgr`` / nRF Serial Terminal):

* Close other serial monitors
* Stop any previous ``serial_logger.py`` instance

``Access is denied`` almost always means the port is already in use.

Using logs for the heal report
******************************

Search the saved ``.txt`` for lines containing ``METRIC``::

   METRIC,event=discover,...
   METRIC,event=offline,...
   METRIC,event=rejoin,id=...,boot_to_heard_ms=...,silence_gap_ms=...,...

Copy ``event=rejoin`` rows into the report. The primary heal number is
``boot_to_heard_ms`` (milliseconds from broadcaster power-on until the listener
heard it).

Example workflow
****************

1. Flash listener + broadcasters; note the listener COM port.
2. Start logging::

      python serial_logger.py COMx

3. Power boards / run power-cycle trials.
4. Stop the logger (Ctrl+C).
5. Open ``logs/listener_*.txt`` and extract ``METRIC`` lines.
