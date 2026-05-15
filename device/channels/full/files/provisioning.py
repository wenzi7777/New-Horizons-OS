class ProvisioningManager:
    def __init__(self, logger=None):
        self.logger = logger
        self.state = "idle"
        self.last_error = ""
        self.available = False
        self.provider = None
        self._load_provider()

    def _load_provider(self):
        for module_name in ("esp_provisioning", "provisioning_native"):
            try:
                module = __import__(module_name)
                self.provider = module
                self.available = True
                return
            except ImportError:
                continue

    def request(self):
        self.state = "provision_requested"

    def start(self):
        self.state = "provision_active"
        if not self.available:
            self.last_error = "provisioning_not_supported"
            if self.logger:
                self.logger.warn(self.last_error)
            return False

        try:
            started = bool(self.provider.start())
        except Exception as exc:
            self.last_error = str(exc)
            if self.logger:
                self.logger.error("provisioning_start_failed {}".format(self.last_error))
            return False

        if not started:
            self.last_error = "provisioning_start_failed"
            return False
        self.last_error = ""
        return True

    def status(self):
        return {
            "state": self.state,
            "available": self.available,
            "last_error": self.last_error,
        }
