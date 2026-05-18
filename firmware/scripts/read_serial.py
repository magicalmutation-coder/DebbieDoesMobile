import serial, time, sys

PORT = 'COM8'
BAUD = 115200
DURATION = 12.0

try:
    ser = serial.Serial(PORT, BAUD, timeout=0.1)
except Exception as e:
    print(f"ERROR: opening {PORT}: {e}", file=sys.stderr)
    sys.exit(2)

print(f"OPENED {ser.name} at {BAUD} bps", file=sys.stderr)
end = time.time() + DURATION
try:
    while time.time() < end:
        data = ser.read(4096)
        if data:
            try:
                sys.stdout.buffer.write(data)
                sys.stdout.flush()
            except Exception:
                sys.stdout.write(data.decode(errors='replace'))
                sys.stdout.flush()
finally:
    ser.close()
    print('\nSERIAL READ COMPLETE', file=sys.stderr)
