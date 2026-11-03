"""检查真实边缘框的浮点舍入，以及越界和无效框的处理。"""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src/board/runtime'))
from policy import boxes_q16


def detection(coords):
    return dict(xyxy=coords, confidence=0.5, class_id=0)


class BoxPolicyTests(unittest.TestCase):
    def test_real_float32_edge_box(self):
        # 这组值来自640×480相机返回的x、y、宽、高，右端多出1.5e-5像素。
        coords = [204.44960021972656, 2.64013671875, 640.0000152587891, 196.44577026367188]
        original = coords.copy()
        self.assertEqual(boxes_q16([detection(coords)], 640, 480), [[20936, 360, 65535, 26821]])
        self.assertEqual(coords, original)

    def test_full_image_box(self):
        self.assertEqual(boxes_q16([detection([0, 0, 640, 480])], 640, 480), [[0, 0, 65535, 65535]])

    def test_invalid_boxes(self):
        cases = [[0, 0, 640.01, 200], [-0.01, 0, 20, 30], [10, 20, 9, 40],
                 [0, 0, 0, 20], [0, 0, float('nan'), 20],
                 [640.00001, 0, 640.00002, 20]]
        for coords in cases:
            with self.subTest(coords=coords), self.assertRaises(ValueError):
                boxes_q16([detection(coords)], 640, 480)


if __name__ == '__main__':
    unittest.main()
