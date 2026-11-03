"""离线核对查看服务的只读路由，不绑定网络端口、不访问板卡。"""
from pathlib import Path
from tempfile import TemporaryDirectory
import importlib.util
import json
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'src/board/runtime'
sys.path.insert(0, str(RUNTIME))
spec = importlib.util.spec_from_file_location('viewer_under_test', RUNTIME / 'viewer.py')
viewer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(viewer)


class ViewerRoutesTest(unittest.TestCase):
    def setUp(self):
        self.temp = TemporaryDirectory()
        self.data = Path(self.temp.name)
        (self.data / 'latest.jpg').write_bytes(b'jpeg-test')
        self.prev_current, self.prev_snapshot = viewer.current, viewer.snapshot
        viewer.current = lambda: ({'session': 'camera_test'}, self.data)
        viewer.snapshot = lambda: {'model': '11', 'live': True}

    def tearDown(self):
        viewer.current, viewer.snapshot = self.prev_current, self.prev_snapshot
        self.temp.cleanup()

    def request(self, path):
        handler = object.__new__(viewer.Handler)
        handler.path = path
        handler.output = lambda data, mime='application/json', status=200: (status, mime, data)
        return handler.do_GET()

    def test_status_and_preview_remain_available(self):
        status, mime, data = self.request('/api/state')
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(data)['model'], '11')
        self.assertEqual(self.request('/preview.jpg'), (200, 'image/jpeg', b'jpeg-test'))

    def test_obsolete_root_page_is_absent(self):
        self.assertEqual(self.request('/')[0], 404)


if __name__ == '__main__':
    unittest.main()
