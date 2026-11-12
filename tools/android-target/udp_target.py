#!/usr/bin/env python3
"""Controlled UDP traffic for an Android/Termux Wi-Fi target. Standard library only."""
import argparse
import ipaddress
import math
import socket
import struct
import sys
import time


def integer_between(low, high):
    def parse(value):
        try:
            number = int(value)
        except ValueError as error:
            raise argparse.ArgumentTypeError("Enter a whole number.") from error
        if not low <= number <= high:
            raise argparse.ArgumentTypeError("Choose a value from %s to %s." % (low, high))
        return number
    return parse


def source_address(value):
    try:
        address = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError as error:
        raise argparse.ArgumentTypeError("Enter the phone's Wi-Fi IPv4 address.") from error
    if address.is_unspecified or address.is_multicast or address.is_loopback or address.is_reserved:
        raise argparse.ArgumentTypeError("Use the actual Wi-Fi IPv4 address from Android network settings.")
    return str(address)


def broadcast_address(value):
    try:
        address = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError as error:
        raise argparse.ArgumentTypeError("Enter a valid IPv4 broadcast address.") from error
    if address.is_unspecified or address.is_loopback or address.is_multicast:
        raise argparse.ArgumentTypeError("Choose the Wi-Fi broadcast address, usually 255.255.255.255.")
    return str(address)


def next_deadline(previous, actual, interval):
    """Do not emit a burst to catch up after Android pauses the process."""
    late = max(0.0, actual - previous)
    skipped = int(late / interval)
    return (actual + interval if skipped else previous + interval), skipped


def make_payload(sequence, timestamp, size):
    header = struct.pack("!4sQd", b"RDLT", sequence, timestamp)
    return header + b"T" * (size - len(header))


def transmit(source_ip, destination, rate, size, duration, report=print):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sent = skipped = 0
    started = time.monotonic()
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.settimeout(0.5)
        sock.bind((source_ip, 0))
        report("Wi-Fi source: %s:%s" % sock.getsockname())
        report("Broadcast: %s:%s | requested %d datagrams/s | %d payload bytes" % (*destination, rate, size))
        report("Keep Termux visible and the screen on. Ctrl+C stops transmission.")
        started = time.monotonic()
        deadline = started + duration if duration else math.inf
        next_send = started
        interval = 1.0 / rate
        last_report = started
        last_count = 0
        while True:
            now = time.monotonic()
            if now >= deadline:
                break
            if now < next_send:
                time.sleep(min(next_send - now, deadline - now))
                continue
            data = make_payload(sent + 1, now - started, size)
            written = sock.sendto(data, destination)
            if written != len(data):
                raise OSError("The network did not accept the complete datagram.")
            sent += 1
            actual = time.monotonic()
            next_send, missed = next_deadline(next_send, actual, interval)
            skipped += missed
            if actual - last_report >= 5:
                report("%6.1f s | submitted %d | recent %.1f/s | skipped scheduling slots %d" %
                       (actual - started, sent, (sent - last_count) / (actual - last_report), skipped))
                last_report, last_count = actual, sent
    except KeyboardInterrupt:
        report("Stopped by user.")
    finally:
        elapsed = max(0.000001, time.monotonic() - started)
        sock.close()
        report("Submitted %d datagrams in %.2f s (%.2f/s); skipped slots: %d." %
               (sent, elapsed, sent / elapsed, skipped))
        report("Submitted means accepted by the network stack. Check each ESP32's actual packet count.")
    return sent, skipped


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ip", required=True, type=source_address,
                        help="Phone's Wi-Fi IPv4 address, from Android Wi-Fi network details")
    parser.add_argument("--broadcast", default="255.255.255.255", type=broadcast_address,
                        help="Wi-Fi broadcast address; default is the limited broadcast address")
    parser.add_argument("--port", default=5005, type=integer_between(1024, 65535))
    parser.add_argument("--rate", default=20, type=integer_between(1, 100), help="Requested UDP datagrams/second, default 20")
    parser.add_argument("--size", default=128, type=integer_between(64, 1200), help="UDP payload bytes, default 128")
    parser.add_argument("--duration", default=0, type=integer_between(0, 86400), help="Seconds; 0 runs until Ctrl+C")
    parser.add_argument("--dry-run", action="store_true", help="Validate settings without creating a socket or transmitting")
    args = parser.parse_args(argv)
    if args.dry_run:
        print("Settings valid: %s -> %s:%d, %d/s, %d bytes. No traffic sent." %
              (args.source_ip, args.broadcast, args.port, args.rate, args.size))
        return 0
    try:
        transmit(args.source_ip, (args.broadcast, args.port), args.rate, args.size, args.duration)
        return 0
    except OSError as error:
        print("Transmission stopped: %s\nCheck the Wi-Fi IP, connection, and broadcast permission."
              " Keep mobile data and VPN off during the pilot." % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
