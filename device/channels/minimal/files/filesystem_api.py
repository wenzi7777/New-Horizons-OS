import storage


class FilesystemAPI:
    def __init__(self, root=".device"):
        self.root = root

    def list_files(self):
        return storage.list_tree(self.root)

    def read_file(self, relative_path):
        path = self.root.rstrip("/") + "/" + relative_path.lstrip("/")
        text = storage.read_text(path)
        if text is not None:
            return {"path": relative_path, "encoding": "text", "content": text}
        data = storage.read_bytes(path)
        if data is None:
            return None
        return {"path": relative_path, "encoding": "hex", "content": data.hex()}

    def delete_file(self, relative_path):
        path = self.root.rstrip("/") + "/" + relative_path.lstrip("/")
        return storage.remove(path)
