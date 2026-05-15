def run(provisioning_requested=False, error=""):
    import app_minimal
    app_minimal.run(provisioning_requested=provisioning_requested, recovery_error=error)
