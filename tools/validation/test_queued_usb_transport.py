"""Offline checks: no USB access is performed."""

import unittest
from unittest.mock import Mock

from queued_usb_transport import QueuedPyUSBCDCTransport
from zmk_studio_rpc.framing import encode_frame


class QueuedTransportTest(unittest.TestCase):
    def test_coalesced_frames_are_retained(self):
        transport = QueuedPyUSBCDCTransport()
        endpoint = Mock()
        endpoint.read.return_value = encode_frame(b"notification") + encode_frame(
            b"response"
        )
        transport._endpoint_in = endpoint
        self.assertEqual(transport.read_frame(), b"notification")
        self.assertEqual(transport.read_frame(), b"response")
        endpoint.read.assert_called_once()

    def test_close_clears_pending_and_partial_frames(self):
        transport = QueuedPyUSBCDCTransport()
        transport._pending_frames.append(b"stale")
        transport._decoder.feed(encode_frame(b"partial")[:-1])
        transport.close()
        self.assertFalse(transport._pending_frames)
        endpoint = Mock()
        endpoint.read.return_value = encode_frame(b"fresh")
        transport._endpoint_in = endpoint
        self.assertEqual(transport.read_frame(), b"fresh")


if __name__ == "__main__":
    unittest.main()
