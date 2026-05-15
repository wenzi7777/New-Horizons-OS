import storage


class CalibrationStore:
    def __init__(self, base_dir=".device/calibration"):
        self.base_dir = base_dir
        self.index_path = self.base_dir + "/index.json"
        self.points = {}

    def _sensor_key(self, analog_pin, select_pin):
        return "{}:{}".format(int(analog_pin), int(select_pin))

    def load(self):
        index = storage.load_json(self.index_path, {"points": {}})
        self.points = index.get("points", {})
        return self.points

    def save(self):
        storage.save_json(self.index_path, {"points": self.points})

    def set_point(self, analog_pin, select_pin, level, raw_mv):
        sensor_key = self._sensor_key(analog_pin, select_pin)
        points = self.points.setdefault(sensor_key, {})
        points[self._level_key(level)] = float(raw_mv)

    def delete_level(self, level):
        target = self._level_key(level)
        for sensor_key in list(self.points.keys()):
            sensor_points = self.points[sensor_key]
            if target in sensor_points:
                del sensor_points[target]
            if not sensor_points:
                del self.points[sensor_key]

    def dump(self):
        return self.points

    def list_levels(self):
        levels = {}
        for sensor_points in self.points.values():
            for level_key in sensor_points.keys():
                levels[level_key] = True
        return sorted(levels.keys(), key=lambda item: float(item))

    def dump_level(self, level, analog_pins, select_pins):
        level_key = self._level_key(level)
        matrix = []
        for analog_pin in analog_pins:
            row = []
            for select_pin in select_pins:
                sensor_key = self._sensor_key(analog_pin, select_pin)
                sensor_points = self.points.get(sensor_key, {})
                value = sensor_points.get(level_key)
                row.append(float(value) if value is not None else None)
            matrix.append(row)
        return {
            "level": level_key,
            "analog_pins": list(analog_pins),
            "select_pins": list(select_pins),
            "matrix": matrix,
        }

    def has_sensor_curve(self, analog_pin, select_pin):
        sensor_key = self._sensor_key(analog_pin, select_pin)
        return sensor_key in self.points and len(self.points[sensor_key]) >= 2

    def apply(self, analog_pin, select_pin, raw_mv):
        sensor_key = self._sensor_key(analog_pin, select_pin)
        points = self.points.get(sensor_key)
        if not points or len(points) < 2:
            return float(raw_mv)

        curve = []
        for level_key, sample_mv in points.items():
            curve.append((float(sample_mv), float(level_key)))
        curve.sort(key=lambda item: item[0])

        value = float(raw_mv)
        if value <= curve[0][0]:
            return curve[0][1]
        if value >= curve[-1][0]:
            return curve[-1][1]

        for idx in range(len(curve) - 1):
            mv0, level0 = curve[idx]
            mv1, level1 = curve[idx + 1]
            if mv0 <= value <= mv1:
                if mv1 == mv0:
                    return level1
                ratio = (value - mv0) / (mv1 - mv0)
                return level0 + ratio * (level1 - level0)

        return value

    def _level_key(self, level):
        return "{:.3f}".format(float(level))
