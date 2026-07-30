#!/usr/bin/env python3
"""Test client for the private remote heated T-Cal HID command."""

from __future__ import annotations

import argparse
import math
import secrets
import struct
import sys
import time
from dataclasses import dataclass

VID = 0x1209
PID = 0x7690
REPORT_SIZE = 64

HID_TYPE_ACK = 251
HID_TYPE_REQUEST = 254
HID_OPCODE_REMOTE_HEATED_TCAL = 0xC8  # HID namespace, not the ESB constant
TARGET_ALL = 0xFF

ACTION = {"start": 1, "stop": 2, "abort": 3}
STATUS_STARTED = 7
STATUS_REMOTE_FAILED = 8
DEFAULT_TEMPERATURE = -32768
NO_RESPONSE = 0xE
NOT_CONSIDERED = 0xF

RESULT_NAMES = (
    "OK",
    "INVALID",
    "UNSUPPORTED",
    "BUSY",
    "POWER_REQUIRED",
    "NOT_READY",
    "TIMEOUT",
    "HARDWARE_ERROR",
    "NOT_ACTIVE",
    "INTERNAL",
    "CANCELED",
)


def parse_target(value: str) -> int:
    if value.lower() == "all":
        return TARGET_ALL
    target = int(value, 0)
    if not 0 <= target < 16:
        raise ValueError("Tracker ID must be 0-15 or 'all'")
    return target


def encode_temperature(value: str | float | None) -> int:
    if value is None:
        return DEFAULT_TEMPERATURE
    temperature = float(value)
    if not math.isfinite(temperature):
        raise ValueError("temperature must be finite")
    encoded = round(temperature * 100.0)
    if not -32768 <= encoded <= 32767:
        raise ValueError("temperature cannot be encoded as centi-degrees")
    return encoded


def build_tcal_request(
    sequence: int,
    target: int,
    action: str,
    temperature: str | float | None = None,
) -> bytes:
    if action not in ACTION:
        raise ValueError(f"unknown action: {action}")
    if not 0 < sequence <= 0xFF:
        raise ValueError("sequence must be 1-255")
    if target != TARGET_ALL and not 0 <= target < 16:
        raise ValueError("target must be 0-15 or 0xFF")

    if action == "start":
        encoded = encode_temperature(temperature)
    else:
        if temperature is not None:
            raise ValueError("STOP/ABORT do not accept a temperature")
        encoded = 0

    report = bytearray(REPORT_SIZE)
    report[0] = HID_TYPE_REQUEST
    report[1] = sequence
    report[2] = HID_OPCODE_REMOTE_HEATED_TCAL
    report[3] = target
    report[4] = ACTION[action]
    struct.pack_into("<h", report, 5, encoded)
    return bytes(report)


@dataclass(frozen=True)
class Ack:
    sequence: int
    overall: int
    considered_mask: int
    reply_mask: int
    tracker_status: tuple[int, ...]


def decode_ack(report: bytes) -> Ack:
    if len(report) < 16:
        raise ValueError("ACK is shorter than 16 bytes")
    if report[0] != HID_TYPE_ACK or report[2] != HID_OPCODE_REMOTE_HEATED_TCAL:
        raise ValueError("not a remote heated T-Cal ACK")
    statuses: list[int] = []
    for tracker in range(16):
        packed = report[8 + tracker // 2]
        statuses.append((packed >> (4 if tracker & 1 else 0)) & 0xF)
    return Ack(
        sequence=report[1],
        overall=report[3],
        considered_mask=struct.unpack_from("<H", report, 4)[0],
        reply_mask=struct.unpack_from("<H", report, 6)[0],
        tracker_status=tuple(statuses),
    )


def result_name(value: int) -> str:
    if value < len(RESULT_NAMES):
        return RESULT_NAMES[value]
    if value == NO_RESPONSE:
        return "NO_RESPONSE"
    if value == NOT_CONSIDERED:
        return "NOT_CONSIDERED"
    return f"UNKNOWN({value})"


def format_ack(ack: Ack) -> str:
    lines = [
        f"ACK seq={ack.sequence} overall={ack.overall} "
        f"considered=0x{ack.considered_mask:04x} "
        f"reply=0x{ack.reply_mask:04x}"
    ]
    for tracker, status in enumerate(ack.tracker_status):
        if ack.considered_mask & (1 << tracker):
            lines.append(f"  tracker {tracker}: {result_name(status)}")
    if (
        ack.overall != STATUS_STARTED
        and ack.considered_mask & ~ack.reply_mask
    ):
        lines.append(
            "WARNING: NO_RESPONSE means the result is unknown; START may "
            "have executed. Send STOP or ABORT if needed."
        )
    return "\n".join(lines)


class Client:
    def __init__(self) -> None:
        try:
            import hid  # type: ignore
        except ImportError as exc:
            raise RuntimeError("Install hidapi: pip install hidapi") from exc
        self.device = hid.device()
        self.device.open(VID, PID)
        self.device.set_nonblocking(True)
        # Avoid reusing sequence 1 on every GUI click/reconnect, where a stale
        # ACK from the previous USB session could otherwise be mis-associated.
        self.sequence = secrets.randbelow(255)
        self.pending_acks: list[Ack] = []

    def close(self) -> None:
        self.device.close()

    def next_sequence(self) -> int:
        self.sequence = (self.sequence + 1) & 0xFF
        if self.sequence == 0:
            self.sequence = 1
        return self.sequence

    def take_pending_ack(self, sequence: int) -> Ack | None:
        for index, ack in enumerate(self.pending_acks):
            if ack.sequence == sequence:
                return self.pending_acks.pop(index)
        return None

    def wait_ack(self, sequence: int, timeout: float) -> Ack:
        pending = self.take_pending_ack(sequence)
        if pending is not None:
            return pending

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            data = bytes(self.device.read(REPORT_SIZE, 50))
            for offset in range(0, len(data), 16):
                subreport = data[offset : offset + 16]
                if (
                    len(subreport) == 16
                    and subreport[0] == HID_TYPE_ACK
                    and subreport[2] == HID_OPCODE_REMOTE_HEATED_TCAL
                ):
                    self.pending_acks.append(decode_ack(subreport))
            pending = self.take_pending_ack(sequence)
            if pending is not None:
                return pending
        raise TimeoutError("No matching receiver ACK")

    def run(
        self, target: int, action: str, temperature: str | None = None
    ) -> list[Ack]:
        sequence = self.next_sequence()
        request = build_tcal_request(sequence, target, action, temperature)
        self.device.write(b"\0" + request)
        first = self.wait_ack(sequence, 2.0)
        replies = [first]
        if first.overall == STATUS_STARTED:
            replies.append(self.wait_ack(sequence, 7.0))
        return replies


def run_gui() -> None:
    import tkinter as tk
    from tkinter import messagebox, ttk

    root = tk.Tk()
    root.title("Remote Heated T-Cal Test")
    target = tk.StringVar(value="all")
    action = tk.StringVar(value="start")
    temperature = tk.StringVar(value="")
    output = tk.Text(root, width=72, height=20)

    ttk.Label(root, text="Tracker (0-15/all)").grid(row=0, column=0, sticky="w")
    ttk.Entry(root, textvariable=target).grid(row=0, column=1, sticky="ew")
    ttk.Label(root, text="Action").grid(row=1, column=0, sticky="w")
    ttk.Combobox(
        root, textvariable=action, values=("start", "stop", "abort"), state="readonly"
    ).grid(row=1, column=1, sticky="ew")
    ttk.Label(root, text="Target °C (blank = Tracker default; P00 10-45)").grid(
        row=2, column=0, sticky="w"
    )
    ttk.Entry(root, textvariable=temperature).grid(row=2, column=1, sticky="ew")

    def submit() -> None:
        try:
            client = Client()
            try:
                temp = temperature.get().strip() or None
                replies = client.run(
                    parse_target(target.get().strip()), action.get(), temp
                )
            finally:
                client.close()
            output.delete("1.0", tk.END)
            output.insert(tk.END, "\n\n".join(format_ack(ack) for ack in replies))
        except Exception as exc:  # GUI boundary
            messagebox.showerror("Remote T-Cal", str(exc))

    ttk.Button(root, text="Send", command=submit).grid(row=3, column=0, columnspan=2)
    output.grid(row=4, column=0, columnspan=2, sticky="nsew")
    root.columnconfigure(1, weight=1)
    root.rowconfigure(4, weight=1)
    root.mainloop()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gui", action="store_true")
    parser.add_argument("command", nargs="?")
    parser.add_argument("target", nargs="?")
    parser.add_argument("tcal", nargs="?")
    parser.add_argument("heat", nargs="?")
    parser.add_argument("action", nargs="?")
    parser.add_argument("temperature", nargs="?")
    args = parser.parse_args(argv)
    if args.gui:
        run_gui()
        return 0
    if (
        args.command != "send"
        or args.target is None
        or args.tcal != "tcal"
        or args.heat != "heat"
        or args.action not in ACTION
    ):
        parser.error(
            "use: send <id|all> tcal heat <start [target_C]|stop|abort>"
        )

    target = parse_target(args.target)
    if args.temperature is not None:
        encoded = encode_temperature(args.temperature)
        if not 1000 <= encoded <= 4500:
            print("Note: current P00 range is 10-45 C; Tracker may return INVALID.")
    client = Client()
    try:
        for ack in client.run(target, args.action, args.temperature):
            print(format_ack(ack))
    finally:
        client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
