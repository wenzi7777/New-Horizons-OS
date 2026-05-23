# resource_guard.py


class ResourceGuard:
    def __init__(self, thresholds=None, degraded_send_every_n_frames=2):
        self.thresholds = thresholds or default_thresholds()
        self.degraded_send_every_n_frames = int(degraded_send_every_n_frames or 2)

    @classmethod
    def from_config(cls, config):
        thresholds = {
            "watch": {
                "heap_free": int(getattr(config, "RESOURCE_WATCH_HEAP_FREE", 32768)),
                "heap_largest_free_block": int(getattr(config, "RESOURCE_WATCH_LARGEST_FREE_BLOCK", 16384)),
            },
            "degraded": {
                "heap_free": int(getattr(config, "RESOURCE_DEGRADED_HEAP_FREE", 20480)),
                "heap_largest_free_block": int(getattr(config, "RESOURCE_DEGRADED_LARGEST_FREE_BLOCK", 12288)),
            },
            "critical": {
                "heap_free": int(getattr(config, "RESOURCE_CRITICAL_HEAP_FREE", 16384)),
                "heap_largest_free_block": int(getattr(config, "RESOURCE_CRITICAL_LARGEST_FREE_BLOCK", 8192)),
            },
        }
        return cls(
            thresholds=thresholds,
            degraded_send_every_n_frames=int(getattr(config, "RESOURCE_DEGRADED_SEND_EVERY_N_FRAMES", 2)),
        )

    def evaluate(self, heap_free=0, heap_largest_free_block=0, user_send_every_n_frames=1):
        heap_free = _positive_int(heap_free)
        heap_largest_free_block = _positive_int(heap_largest_free_block)
        user_send_every_n_frames = max(1, _positive_int(user_send_every_n_frames) or 1)

        for state in ("critical", "degraded", "watch"):
            reason = self._reason_for_state(state, heap_free, heap_largest_free_block)
            if reason:
                return self._payload(state, reason, user_send_every_n_frames)
        return self._payload("normal", "", user_send_every_n_frames)

    def _reason_for_state(self, state, heap_free, heap_largest_free_block):
        threshold = self.thresholds[state]
        reasons = []
        heap_min = int(threshold.get("heap_free", 0) or 0)
        largest_min = int(threshold.get("heap_largest_free_block", 0) or 0)
        if heap_free and heap_min and heap_free < heap_min:
            reasons.append("heap_free={}<{}".format(heap_free, heap_min))
        if heap_largest_free_block and largest_min and heap_largest_free_block < largest_min:
            reasons.append("heap_largest_free_block={}<{}".format(heap_largest_free_block, largest_min))
        return " ".join(reasons)

    def _payload(self, state, reason, user_send_every_n_frames):
        action = "none"
        override = None
        if state == "watch":
            action = "monitor"
        elif state == "degraded":
            action = "decimate_udp"
            if user_send_every_n_frames < self.degraded_send_every_n_frames:
                override = self.degraded_send_every_n_frames
        elif state == "critical":
            action = "pause_stream"
        return {
            "state": state,
            "reason": reason,
            "action": action,
            "thresholds": self.thresholds,
            "send_every_n_frames_override": override,
        }


def default_thresholds():
    return {
        "watch": {
            "heap_free": 32768,
            "heap_largest_free_block": 16384,
        },
        "degraded": {
            "heap_free": 20480,
            "heap_largest_free_block": 12288,
        },
        "critical": {
            "heap_free": 16384,
            "heap_largest_free_block": 8192,
        },
    }


def configured_send_every(runtime, config):
    runtime = runtime if isinstance(runtime, dict) else {}
    scan_timing = runtime.get("scan_timing", {}) or {}
    try:
        send_every = int(scan_timing.get("send_every_n_frames", getattr(config, "SEND_EVERY_N_FRAMES", 1)) or 1)
    except Exception:
        send_every = int(getattr(config, "SEND_EVERY_N_FRAMES", 1) or 1)
    max_send_every = int(getattr(config, "MAX_SEND_EVERY_N_FRAMES", 8) or 8)
    return max(1, min(max_send_every, send_every))


def effective_send_every(runtime, config, guard):
    send_every = configured_send_every(runtime, config)
    override = (guard or {}).get("send_every_n_frames_override")
    if override:
        try:
            send_every = max(send_every, int(override))
        except Exception:
            pass
    max_send_every = int(getattr(config, "MAX_SEND_EVERY_N_FRAMES", 8) or 8)
    return max(1, min(max_send_every, send_every))


def guarded_heap_free(heap_free=0, native=None):
    values = []
    heap_free = _positive_int(heap_free)
    if heap_free:
        values.append(heap_free)
    native_heap = _positive_int((native or {}).get("heap_free", 0))
    if native_heap:
        values.append(native_heap)
    return min(values) if values else 0


def gc_mem_free(gc_module):
    try:
        return int(gc_module.mem_free())
    except Exception:
        return 0


def gc_mem_alloc(gc_module):
    try:
        return int(gc_module.mem_alloc())
    except Exception:
        return 0


def _positive_int(value):
    try:
        value = int(value or 0)
    except Exception:
        return 0
    return value if value > 0 else 0
