#!/usr/bin/env python3
"""Writes the checked-in seed corpora for the fuzz harnesses in this directory.

Every seed is a hex vector from tests/test_mirrorproto.cpp, named after the test it comes from, so
the corpus starts on exactly the inputs the pin tests already hold the parser to. Rerun after
changing the table: `python3 tests/fuzz/make_seeds.py`.
"""
import pathlib

HERE = pathlib.Path(__file__).resolve().parent

DEVICE_MESSAGE_PARSER = {
    # deviceMessages: clipboard, an unknown type (skipped), ack, concatenated.
    "deviceMessages": "00000003016869" "000000037e0000" "00000009020000000000000042",
    # deviceMessages, the first record alone: one complete clipboard record.
    "deviceMessages-clipboard": "00000003016869",
    # deviceMessagesIncremental: the first two chunks, a truncated size and a truncated body.
    "deviceMessagesIncremental-size": "000000",
    "deviceMessagesIncremental-body": "000000030168",
    # deviceAckIgnoresTrailingBytes: a grown ack, then a clipboard record.
    "deviceAckIgnoresTrailingBytes": "0000000a020000000000000042ff" "00000003016869",
    # deviceFramingErrorLatches: a size-0 record poisoning a valid one after it,
    "deviceFramingErrorLatches-size0": "00000000" "00000003016869",
    # an ack whose body is too short,
    "deviceFramingErrorLatches-shortAck": "00000003020000",
    # one byte over the control cap,
    "deviceFramingErrorLatches-overCap": "00100001",
    # and exactly the cap, waiting for its body.
    "deviceFramingErrorLatches-atCap": "0010000001",
}


def write(corpus: str, seeds: dict) -> None:
    out = HERE / "corpus" / corpus
    out.mkdir(parents=True, exist_ok=True)
    for name, hexed in seeds.items():
        (out / name).write_bytes(bytes.fromhex(hexed))


if __name__ == "__main__":
    write("device_message_parser", DEVICE_MESSAGE_PARSER)
