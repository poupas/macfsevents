import logging
import os
import threading
import _fsevents

# flags from FSEvents to match event types:
FSE_MUST_SCAN_SUBDIRS_FLAG = 0x00000001
FSE_USER_DROPPED_FLAG = 0x00000002
FSE_KERNEL_DROPPED_FLAG = 0x00000004
FSE_CREATED_FLAG = 0x00000100
FSE_REMOVED_FLAG = 0x00000200
FSE_RENAMED_FLAG = 0x00000800
FSE_MODIFIED_FLAG = 0x00001000
FSE_ITEM_CHANGE_OWNER_FLAG = 0x00004000
FSE_ITEM_IS_FILE_FLAG = 0x00010000
FSE_ITEM_IS_DIR_FLAG = 0x00020000


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
        self.stream = _fsevents.streamobject(self._callback, file_events,
                                             latency)
        self.lock = threading.Lock()
        threading.Thread.__init__(self)
        self.daemon = True
        self.running = threading.Event()

    def run(self):
        # start CF loop
        self.stream.initialize()
        self.running.set()
        self.stream.loop()

    def schedule(self, path):
        if not isinstance(path, bytes):
            path = path.encode('utf-8')
        with self.lock:
            self.running.wait()
            self.stream.schedule(path)

    def unschedule(self, path):
        if not isinstance(path, bytes):
            path = path.encode('utf-8')
        with self.lock:
            self.running.wait()
            self.stream.unschedule(path)

    def stop(self):
        with self.lock:
            self.stream.stop()

    def _callback(self, paths, masks):
        for path, mask in zip(paths, masks):
            self.callback(path, mask)
