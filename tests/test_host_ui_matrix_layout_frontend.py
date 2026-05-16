import importlib.util
import json
import subprocess
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def load_host_ui():
    path = REPO_ROOT / "host_ui.py"
    spec = importlib.util.spec_from_file_location("host_ui_matrix_test", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class HostUiMatrixLayoutFrontendTests(unittest.TestCase):
    def test_matrix_layout_uses_logical_pin_labels(self):
        module = load_host_ui()
        script = module.INDEX_HTML.split("<script>", 1)[1].split(
            'document.getElementById("addDeviceBtn").onclick', 1
        )[0]
        script_under_test = script + """
renderPinGrid("matrixRows", [1, 2], [1]);
renderPinGrid("matrixCols", [13, 14], [13]);
console.log(JSON.stringify({
  rows: elements.matrixRows.innerHTML,
  cols: elements.matrixCols.innerHTML
}));
"""
        harness = f"""
const elements = {{
  matrixRows: {{ innerHTML: "" }},
  matrixCols: {{ innerHTML: "" }},
  analogPin: {{ innerHTML: "" }},
  selectPin: {{ innerHTML: "" }},
  levelSelect: {{ innerHTML: "" }}
}};
global.document = {{
  getElementById(id) {{
    return elements[id] || {{ innerHTML: "", style: {{}} }};
  }},
  querySelectorAll() {{
    return [];
  }}
}};
eval({json.dumps(script_under_test)});
"""
        result = subprocess.run(
            ["node", "-e", harness],
            cwd=REPO_ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        rendered = json.loads(result.stdout)

        self.assertIn('value="1" checked', rendered["rows"])
        self.assertIn(">A0<", rendered["rows"])
        self.assertIn(">A1<", rendered["rows"])
        self.assertIn('value="13" checked', rendered["cols"])
        self.assertIn(">D0<", rendered["cols"])
        self.assertIn(">D1<", rendered["cols"])

    def test_calibration_selectors_use_logical_pin_labels(self):
        module = load_host_ui()
        script = module.INDEX_HTML.split("<script>", 1)[1].split(
            'document.getElementById("addDeviceBtn").onclick', 1
        )[0]
        script_under_test = script + """
syncPinSelectors({
  status: {
    available_rows: [1, 2, 3],
    active_rows: [1, 3],
    available_cols: [13, 14, 15],
    active_cols: [14]
  }
});
console.log(JSON.stringify({
  rows: elements.analogPin.innerHTML,
  cols: elements.selectPin.innerHTML
}));
"""
        harness = f"""
const elements = {{
  matrixRows: {{ innerHTML: "" }},
  matrixCols: {{ innerHTML: "" }},
  analogPin: {{ innerHTML: "" }},
  selectPin: {{ innerHTML: "" }},
  levelSelect: {{ innerHTML: "" }}
}};
global.document = {{
  getElementById(id) {{
    return elements[id] || {{ innerHTML: "", style: {{}} }};
  }},
  querySelectorAll() {{
    return [];
  }}
}};
eval({json.dumps(script_under_test)});
"""
        result = subprocess.run(
            ["node", "-e", harness],
            cwd=REPO_ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        rendered = json.loads(result.stdout)

        self.assertIn('value="1">A0<', rendered["rows"])
        self.assertIn('value="3">A2<', rendered["rows"])
        self.assertIn('value="14">D1<', rendered["cols"])

    def test_dirty_matrix_layout_draft_survives_status_refresh(self):
        module = load_host_ui()
        script = module.INDEX_HTML.split("<script>", 1)[1].split(
            'document.getElementById("addDeviceBtn").onclick', 1
        )[0]
        script_under_test = script + """
const device = {
  key: "board-a",
  status: {
    available_rows: [1, 2],
    active_rows: [1],
    available_cols: [13, 14],
    active_cols: [13]
  }
};
syncMatrixLayout(device);
state.matrixLayoutDeviceKey = "board-a";
state.matrixLayoutDirty = true;
state.matrixLayoutDraft = { active_rows: [1, 2], active_cols: [13] };
syncMatrixLayout(device);
console.log(JSON.stringify({
  rows: elements.matrixRows.innerHTML,
  cols: elements.matrixCols.innerHTML
}));
"""
        harness = f"""
const elements = {{
  matrixRows: {{ innerHTML: "" }},
  matrixCols: {{ innerHTML: "" }},
  analogPin: {{ innerHTML: "" }},
  selectPin: {{ innerHTML: "" }},
  levelSelect: {{ innerHTML: "" }}
}};
global.document = {{
  getElementById(id) {{
    return elements[id] || {{ innerHTML: "", style: {{}} }};
  }},
  querySelectorAll() {{
    return [];
  }}
}};
eval({json.dumps(script_under_test)});
"""
        result = subprocess.run(
            ["node", "-e", harness],
            cwd=REPO_ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        rendered = json.loads(result.stdout)

        self.assertIn('value="1" checked', rendered["rows"])
        self.assertIn('value="2" checked', rendered["rows"])
        self.assertIn('value="13" checked', rendered["cols"])

    def test_heatmap_shape_follows_selected_matrix_layout(self):
        module = load_host_ui()
        script = module.INDEX_HTML.split("<script>", 1)[1].split(
            'document.getElementById("addDeviceBtn").onclick', 1
        )[0]
        script_under_test = script + """
let capturedHeatmap = null;
renderGrid = (targetId, values, rows, cols) => {
  if (targetId === "heatmap") capturedHeatmap = { values, rows, cols };
};
state.selectedDevice = {
  key: "board-a",
  host: "192.168.1.50",
  port: 22345,
  status: {
    matrix_configured: true,
    active_rows: [1, 2, 3],
    active_cols: [13, 14],
    available_rows: [1, 2, 3],
    available_cols: [13, 14],
    runtime: {}
  },
  packet: {
    frame_id: 7,
    rows: 1,
    cols: 6,
    matrix: [1, 2, 3, 4, 5, 6]
  }
};
renderSelected();
console.log(JSON.stringify(capturedHeatmap));
"""
        harness = f"""
const elements = {{}};
function element(id) {{
  if (!elements[id]) elements[id] = {{
    id,
    innerHTML: "",
    textContent: "",
    value: "",
    style: {{}}
  }};
  return elements[id];
}}
global.document = {{
  getElementById(id) {{
    return element(id);
  }},
  querySelectorAll() {{
    return [];
  }}
}};
eval({json.dumps(script_under_test)});
"""
        result = subprocess.run(
            ["node", "-e", harness],
            cwd=REPO_ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        rendered = json.loads(result.stdout)

        self.assertEqual(rendered["rows"], 3)
        self.assertEqual(rendered["cols"], 2)

    def test_render_selected_reports_scan_and_ui_fps_separately(self):
        module = load_host_ui()
        script = module.INDEX_HTML.split("<script>", 1)[1].split(
            'document.getElementById("addDeviceBtn").onclick', 1
        )[0]
        script_under_test = script + """
renderGrid = () => {};
const device = {
  key: "board-a",
  host: "192.168.1.50",
  port: 22345,
  scan_fps: 96.4,
  status: {
    matrix_configured: true,
    active_rows: [1],
    active_cols: [13],
    available_rows: [1],
    available_cols: [13],
    runtime: {}
  },
  packet: { frame_id: 1, matrix: [1] }
};
state.selectedDevice = device;
nowValues.push(0);
renderSelected();
device.packet = { frame_id: 2, matrix: [2] };
nowValues.push(1000);
renderSelected();
console.log(JSON.stringify({
  scanFps: elements.scanFpsVal.textContent,
  uiFps: elements.uiFpsVal.textContent
}));
"""
        harness = f"""
const elements = {{}};
const nowValues = [];
function element(id) {{
  if (!elements[id]) elements[id] = {{
    id,
    innerHTML: "",
    textContent: "",
    value: "",
    style: {{}}
  }};
  return elements[id];
}}
global.document = {{
  getElementById(id) {{
    return element(id);
  }},
  querySelectorAll() {{
    return [];
  }}
}};
Date.now = () => nowValues.shift() ?? 0;
eval({json.dumps(script_under_test)});
"""
        result = subprocess.run(
            ["node", "-e", harness],
            cwd=REPO_ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        rendered = json.loads(result.stdout)

        self.assertEqual(rendered["scanFps"], "96.4")
        self.assertEqual(rendered["uiFps"], "2.0")

    def test_frontend_uses_event_stream_for_realtime_selected_device_updates(self):
        module = load_host_ui()
        html = module.INDEX_HTML

        self.assertIn("async function deviceListLoop()", html)
        self.assertIn("setTimeout(deviceListLoop, 1000);", html)
        self.assertIn('new EventSource("/api/stream")', html)
        self.assertIn("requestAnimationFrame(flushSelectedRender)", html)
        self.assertIn('typeof EventSource !== "function"', html)
        self.assertIn("async function selectedDeviceFallbackLoop()", html)
        self.assertNotIn("async function selectedDeviceLoop()", html)

    def test_packet_stream_updates_use_lightweight_frame_render_path(self):
        module = load_host_ui()
        html = module.INDEX_HTML
        source = (REPO_ROOT / "host_ui.py").read_text()

        self.assertIn('self._publish_record(record, kind="packet")', source)
        self.assertIn("function renderSelectedFrame(device)", html)
        self.assertIn('if (kind === "packet") {', html)


if __name__ == "__main__":
    unittest.main()
