import logging
import os
import threading
import _fsevents

from itertools import izip

# inotify event flags
IN_MODIFY = 0x00000002
IN_ATTRIB = 0x00000004
IN_CREATE = 0x00000100
IN_DELETE = 0x00000200
IN_MOVED_FROM = 0x00000040
IN_MOVED_TO = 0x00000080

# flags from FSEvents to match event types:
FSE_CREATED_FLAG = 0x0100
FSE_MODIFIED_FLAG = 0x1000
FSE_REMOVED_FLAG = 0x0200
FSE_RENAMED_FLAG = 0x0800


# loggin
def logger_init():
    log = logging.getLogger("fsevents")
    console_handler = logging.StreamHandler()
    console_handler.setFormatter(
        logging.Formatter("[%(asctime)s %(name)s %(levelname)s] %(message)s"))
    log.addHandler(console_handler)
    log.setLevel(20)
    return log

log = logger_init()


class Observer(threading.Thread):
    def __init__(self, callback, file_events=False, latency=0.01):
        self.callback = callback
        self.stream = _fsevents.streamobject(
                self._callback, file_events, latency)
        self.lock = threading.Lock()
        threading.Thread.__init__(self)
        self.daemon = True

    def run(self):
        # start CF loop
        self.stream.loop()

    def schedule(self, path):
        with self.lock:
            self.stream.schedule(path)

    def unschedule(self, path):
        with self.lock:
            self.stream.unschedule(path)

    def stop(self):
        with self.lock:
            _fsevents.stop(self)

    def _callback(self, paths, masks):
        for path, mask in izip(paths, masks):
            self.callback(path, mask)

