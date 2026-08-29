import re
import serial
import time
import csv
import sys
from datetime import datetime

# ─── CONFIG — change these before running ────────────────────────
PORT      = 'COM7'      # Board 2 COM port (check Device Manager)
BAUD      = 115200
CHANNEL   = 1711
SERVER_ID = 1           # Board 1 transmitter ID (from dect sett -r)
DURATION  = 10          # seconds per test
# ─────────────────────────────────────────────────────────────────

MCS_LIST  = [0, 1, 2, 3, 4]
PWR_LIST  = [13, 10, 7, 5, 3]
DIST_LIST = ['1m', '3m', '5m', '10m', '15m', '20m']

# ─── Strip ANSI color codes from terminal output ─────────────────
ANSI_RE = re.compile(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])')
def strip_ansi(text):
    return ANSI_RE.sub('', text)

# ─── Logger — writes to screen AND log file simultaneously ────────
class Logger:
    def __init__(self, log_path):
        self.terminal = sys.stdout
        self.log      = open(log_path, 'w', encoding='utf-8')

    def write(self, message):
        self.terminal.write(message)
        self.log.write(message)
        self.log.flush()

    def flush(self):
        self.terminal.flush()
        self.log.flush()

    def close(self):
        self.log.close()

# ─────────────────────────────────────────────────────────────────
def divider(char='─', width=60):
    print(char * width)

def section(title):
    print()
    divider('═')
    print(f'  {title}')
    divider('═')

def step(title):
    print(f'\n── {title}')
    divider()

def confirm(msg):
    print(f'\n  ⚠️  ACTION NEEDED: {msg}')
    input('     Press Enter when ready...')

# ─────────────────────────────────────────────────────────────────
def send_cmd(ser, cmd, wait=2):
    # Flush buffer before sending — prevents command bleed
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    time.sleep(0.5)

    print(f'\n  CMD → {cmd}')
    ser.write((cmd + '\r\n').encode())
    time.sleep(wait)

    output   = ''
    deadline = time.time() + 4
    while time.time() < deadline:
        if ser.in_waiting:
            chunk    = ser.read(ser.in_waiting).decode(errors='ignore')
            output  += chunk
            deadline = time.time() + 1.5
        time.sleep(0.1)

    # Strip ANSI codes
    output = strip_ansi(output)

    # Print output indented
    for line in output.splitlines():
        if line.strip():
            print(f'       {line}')

    return output

# ─────────────────────────────────────────────────────────────────
def parse_result(output):
    result = {
        'pkt_sent':    '',
        'pkt_rx':      '',
        'throughput':  '',
        'crc_pcc':     '',
        'crc_pdc':     '',
        'out_of_seq':  '',
        'rx_restarts': '',
        'rssi_min':    '',
        'rssi_max':    '',
        'snr_min':     '',
        'pkt_loss':    '',
    }

    # Split into client and server sections cleanly
    client_section = output
    server_section = ''

    if 'perf server rx operation' in output:
        parts          = output.split('perf server rx operation')
        client_section = parts[0]
        server_section = parts[1] if len(parts) > 1 else ''

    # ── Parse client section — pkt_sent ──────────────────────────
    for line in client_section.splitlines():
        l = line.strip()
        if 'packet count:' in l and result['pkt_sent'] == '':
            result['pkt_sent'] = l.split(':')[-1].strip()

    # ── Parse server section — everything else ────────────────────
    for line in server_section.splitlines():
        l = line.strip()

        if 'packet count:' in l and result['pkt_rx'] == '':
            result['pkt_rx'] = l.split(':')[-1].strip()

        if 'data rates:' in l and 'kbits' in l and result['throughput'] == '':
            result['throughput'] = l.split(':')[-1].strip()

        if 'PCC CRC errors:' in l:
            result['crc_pcc'] = l.split(':')[-1].strip()

        if 'PDC CRC errors:' in l:
            result['crc_pdc'] = l.split(':')[-1].strip()

        if 'out of seqs:' in l:
            result['out_of_seq'] = l.split(':')[-1].strip()

        if 'rx restarted count:' in l:
            result['rx_restarts'] = l.split(':')[-1].strip()

        if 'RSSI: min:' in l:
            parts = l.split(',')
            result['rssi_min'] = parts[0].split('min:')[-1].strip()
            if len(parts) > 1:
                result['rssi_max'] = (parts[1].split('max:')[-1]
                                      .replace('dBm', '').strip())

        if 'SNR: min:' in l:
            result['snr_min'] = l.split('min:')[-1].split(',')[0].strip()

    # Calculate packet loss %
    try:
        sent             = int(result['pkt_sent'])
        rx               = int(result['pkt_rx'])
        result['pkt_loss'] = f'{((sent - rx) / sent * 100):.2f}%'
    except Exception:
        result['pkt_loss'] = 'N/A'

    return result

# ─────────────────────────────────────────────────────────────────
def print_summary(label, r):
    loss_str  = r['pkt_loss']
    has_loss  = loss_str not in ('0.00%', 'N/A', '')
    flag      = ' ⚠️' if has_loss else ' ✅'
    print(f'\n  RESULT [{label}]:{flag}')
    print(f'    Packets   : {r["pkt_sent"]} sent → {r["pkt_rx"]} received')
    print(f'    Loss      : {r["pkt_loss"]}')
    print(f'    Throughput: {r["throughput"]}')
    print(f'    RSSI      : {r["rssi_min"]} ~ {r["rssi_max"]} dBm')
    print(f'    SNR min   : {r["snr_min"]}')
    print(f'    CRC(PDC)  : {r["crc_pdc"]}  Out-of-seq: {r["out_of_seq"]}')

# ─────────────────────────────────────────────────────────────────
# MAIN
# ─────────────────────────────────────────────────────────────────
timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
csv_file  = f'dect_results_{timestamp}.csv'
log_file  = f'dect_log_{timestamp}.txt'

logger     = Logger(log_file)
sys.stdout = logger

section('DECT NR+ Automated Test Script')
print(f'  Channel    : {CHANNEL}')
print(f'  Server ID  : {SERVER_ID}')
print(f'  Distances  : {DIST_LIST}')
print(f'  Duration   : {DURATION}s per test')
print(f'  CSV file   : {csv_file}')
print(f'  Log file   : {log_file}')
divider('═')

print("""
  SETUP CHECKLIST — do this before pressing Enter:
  ─────────────────────────────────────────────────
  Board 1 (server):
      desh:~$ dect sett --reset
      desh:~$ dect activate
      desh:~$ dect perf -s -t -1 --channel 1711

  Board 2 (client) — controlled by this script via COM port.
  Close VS Code serial terminal for Board 2 before continuing.
  ─────────────────────────────────────────────────
""")

confirm('Board 1 is running as perf server')

ser = serial.Serial(PORT, BAUD, timeout=2)
time.sleep(1)

csvfile = open(csv_file, 'w', newline='')
writer  = csv.writer(csvfile)
writer.writerow([
    'Distance', 'Test Type', 'MCS', 'TX Power',
    'Pkts Sent', 'Pkts RX', 'Pkt Loss %', 'Throughput',
    'PCC CRC', 'PDC CRC', 'Out of Seq', 'RX Restarts',
    'RSSI Min (dBm)', 'RSSI Max (dBm)', 'SNR Min',
])

# Initialize Board 2
step('Initializing Board 2')
send_cmd(ser, 'dect sett --reset', wait=2)
send_cmd(ser, 'dect activate',     wait=4)

# ── Main distance loop ───────────────────────────────────────────
for dist_idx, distance in enumerate(DIST_LIST):

    section(f'DISTANCE: {distance}  ({dist_idx+1}/{len(DIST_LIST)})')

    confirm(
        f'Move boards to {distance} apart. '
        f'Confirm Board 1 perf server is still running '
        f'(dect perf -s -t -1 --channel {CHANNEL})'
    )

    # Pre-test RSSI scan
    step(f'Pre-test RSSI scan at {distance}')
    send_cmd(ser, 'dect rssi_scan', wait=6)

    # ── MCS Sweep ────────────────────────────────────────────────
    step(f'MCS Sweep — TX Power=13, {distance}')
    for mcs in MCS_LIST:
        print(f'\n  Testing MCS {mcs}...')
        cmd = (f'dect perf -c'
               f' --c_tx_mcs {mcs}'
               f' --c_tx_pwr 13'
               f' --s_tx_id {SERVER_ID}'
               f' -t {DURATION}'
               f' --channel {CHANNEL}')
        out = send_cmd(ser, cmd, wait=DURATION + 10)
        r   = parse_result(out)
        print_summary(f'MCS {mcs}', r)

        writer.writerow([
            distance, 'MCS_SWEEP', mcs, 13,
            r['pkt_sent'],    r['pkt_rx'],     r['pkt_loss'],
            r['throughput'],  r['crc_pcc'],    r['crc_pdc'],
            r['out_of_seq'],  r['rx_restarts'],
            r['rssi_min'],    r['rssi_max'],   r['snr_min'],
        ])
        csvfile.flush()
        time.sleep(4)  # ← longer pause between tests

    # ── TX Power Sweep ───────────────────────────────────────────
    step(f'TX Power Sweep — MCS=1, {distance}')
    for pwr in PWR_LIST:
        print(f'\n  Testing TX Power {pwr}...')
        cmd = (f'dect perf -c'
               f' --c_tx_mcs 1'
               f' --c_tx_pwr {pwr}'
               f' --s_tx_id {SERVER_ID}'
               f' -t {DURATION}'
               f' --channel {CHANNEL}')
        out = send_cmd(ser, cmd, wait=DURATION + 10)
        r   = parse_result(out)
        print_summary(f'PWR {pwr}', r)

        writer.writerow([
            distance, 'PWR_SWEEP', 1, pwr,
            r['pkt_sent'],    r['pkt_rx'],     r['pkt_loss'],
            r['throughput'],  r['crc_pcc'],    r['crc_pdc'],
            r['out_of_seq'],  r['rx_restarts'],
            r['rssi_min'],    r['rssi_max'],   r['snr_min'],
        ])
        csvfile.flush()
        time.sleep(4)  # ← longer pause between tests

    print(f'\n  ✅ {distance} complete. CSV saved.')

# ── Done ─────────────────────────────────────────────────────────
csvfile.close()
ser.close()
sys.stdout = logger.terminal
logger.close()

divider('═')
print('  ALL DISTANCES COMPLETE')
print(f'  CSV  → {csv_file}')
print(f'  Log  → {log_file}')
divider('═')