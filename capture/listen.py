#!/usr/bin/env python3
"""Receive capture.cpp UDP packets from any sender, append them to a log and print a summary.

Usage: python3 -I listen.py <log file>
"""
import socket
import sys
import time

log = open(sys.argv[1], "ab")
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("", 1514))
print("listening on UDP 1514", flush=True)
start = time.monotonic()
senders = set()
packets = lines = 0
last_print = 0.0
while True:
    data, sender = sock.recvfrom(65535)
    log.write(data)
    log.flush()
    now = time.monotonic() - start
    text = data.decode(errors="replace").splitlines()
    packets += 1
    lines += len(text)
    if sender not in senders:
        senders.add(sender)
        first = text[0].strip() if text else ""
        print(f"{now:7.1f}s  new sender {sender[0]}:{sender[1]}  first line: {first}", flush=True)
    for line in text:
        if "dropped" in line:
            print(f"{now:7.1f}s  status: {line.strip()}", flush=True)
    if now - last_print >= 1:
        last_print = now
        print(f"{now:7.1f}s  {packets} packets, {lines} lines so far", flush=True)
