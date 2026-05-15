class FilterChain:
    def __init__(self, sensor_count, enabled=False, median=3, alpha=0.25):
        self.sensor_count = max(0, int(sensor_count))
        self.enabled = bool(enabled)
        self.median = self._normalize_median(median)
        self.alpha = self._normalize_alpha(alpha)
        self.windows = [[] for _ in range(self.sensor_count)]
        self.iir_state = [None] * self.sensor_count

    def _normalize_median(self, median):
        try:
            value = int(median)
        except Exception:
            value = 3
        if value not in (1, 3, 5):
            raise ValueError("median must be 1, 3, or 5")
        return value

    def _normalize_alpha(self, alpha):
        value = float(alpha)
        if value < 0.05 or value > 0.6:
            raise ValueError("alpha out of range")
        return value

    def reset(self):
        self.windows = [[] for _ in range(self.sensor_count)]
        self.iir_state = [None] * self.sensor_count

    def apply_config(self, enabled, median, alpha):
        self.enabled = bool(enabled)
        self.median = self._normalize_median(median)
        self.alpha = self._normalize_alpha(alpha)
        self.reset()

    def process(self, sensor_index, value):
        value = float(value)
        if not self.enabled:
            return value

        if sensor_index < 0 or sensor_index >= self.sensor_count:
            return value

        median_value = self._process_median(sensor_index, value)
        prev = self.iir_state[sensor_index]
        if prev is None:
            filtered = median_value
        else:
            filtered = (self.alpha * median_value) + ((1.0 - self.alpha) * prev)
        self.iir_state[sensor_index] = filtered
        return filtered

    def _process_median(self, sensor_index, value):
        if self.median == 1:
            return value
        window = self.windows[sensor_index]
        window.append(value)
        if len(window) > self.median:
            window.pop(0)
        ordered = sorted(window)
        return ordered[len(ordered) // 2]
