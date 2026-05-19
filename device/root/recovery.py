import sys

if "recovery" not in sys.path:
    sys.path.insert(0, "recovery")


def run(wifi_setup_requested=False, error=""):
    import recovery_app
    recovery_app.run(wifi_setup_requested=wifi_setup_requested, recovery_error=error)
