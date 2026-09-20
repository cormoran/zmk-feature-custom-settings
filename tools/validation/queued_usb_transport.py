"""Validation adapter for workspace transports that discard coalesced frames.

Requires the workspace's tools/ on PYTHONPATH. This module never opens a
device on import. Use QueuedPyUSBCDCTransport in place of PyUSBCDCTransport.
"""

from collections import deque

from zmk_studio_rpc.transport import PyUSBCDCTransport, TransportTimeoutError


class QueuedPyUSBCDCTransport(PyUSBCDCTransport):
    def __post_init__(self):
        super().__post_init__()
        self._pending_frames = deque()

    def close(self):
        self._pending_frames.clear()
        super().close()

    def read_frame(self, timeout=None):
        if not self._endpoint_in:
            raise RuntimeError("pyusb transport is not open")
        if self._pending_frames:
            return self._pending_frames.popleft()
        timeout_ms = None if timeout is None else max(1, int(timeout * 1000))
        while True:
            try:
                data = bytes(
                    self._endpoint_in.read(self.read_chunk_size, timeout=timeout_ms)
                )
            except Exception as exc:
                if exc.__class__.__name__ == "USBTimeoutError":
                    raise TransportTimeoutError(
                        "Timed out waiting for a Studio RPC USB frame"
                    ) from exc
                raise
            self._pending_frames.extend(self._decoder.feed(data))
            if self._pending_frames:
                return self._pending_frames.popleft()
