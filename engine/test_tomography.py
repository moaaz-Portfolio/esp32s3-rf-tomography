import unittest

import numpy as np

from tomography import fista_nonnegative, fresnel_matrix


class TomographyTests(unittest.TestCase):
    def test_matrix_shape_and_finite(self):
        cfg = {
            "room": {"width_m": 5, "height_m": 4},
            "receiver": {"x": 0.2, "y": 2},
            "grid": {"nx": 12, "ny": 10},
            "links": {
                "A": {"x": 4.8, "y": 0.5},
                "B": {"x": 4.8, "y": 3.5},
            },
        }
        matrix, shape = fresnel_matrix(cfg)
        self.assertEqual(matrix.shape, (2, 120))
        self.assertEqual(shape, (10, 12))
        self.assertTrue(np.all(np.isfinite(matrix)))

    def test_fista_nonnegative(self):
        A = np.array([[1.0, 0.0], [0.0, 1.0], [0.5, 0.5]])
        y = np.array([1.0, 0.0, 0.5])
        x = fista_nonnegative(A, y, iterations=200, sparsity=0.001)
        self.assertTrue(np.all(x >= 0))
        self.assertGreater(x[0], x[1])


if __name__ == "__main__":
    unittest.main()
