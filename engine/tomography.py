"""Coarse 2-D RF change imaging from calibrated transmitter/receiver links."""
from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import numpy as np


def load_config(path: str | Path) -> dict[str, Any]:
    with open(path, "r", encoding="utf-8") as handle:
        cfg = json.load(handle)
    if not cfg.get("links"):
        raise ValueError("At least one calibrated transmitter link is required")
    return cfg


def fresnel_matrix(cfg: dict[str, Any]) -> tuple[np.ndarray, tuple[int, int]]:
    room = cfg["room"]
    grid = cfg["grid"]
    nx, ny = int(grid["nx"]), int(grid["ny"])
    xs = (np.arange(nx) + 0.5) * float(room["width_m"]) / nx
    ys = (np.arange(ny) + 0.5) * float(room["height_m"]) / ny
    xx, yy = np.meshgrid(xs, ys)
    points = np.column_stack((xx.ravel(), yy.ravel()))
    rx = np.array([cfg["receiver"]["x"], cfg["receiver"]["y"]], dtype=float)
    frequency = float(cfg.get("radio", {}).get("frequency_hz", 2.437e9))
    wavelength = 299_792_458.0 / frequency
    scale = float(cfg.get("radio", {}).get("fresnel_scale", 0.6))
    rows = []
    for tx_cfg in cfg["links"].values():
        tx = np.array([tx_cfg["x"], tx_cfg["y"]], dtype=float)
        direct = np.linalg.norm(tx - rx)
        d_tx = np.linalg.norm(points - tx, axis=1)
        d_rx = np.linalg.norm(points - rx, axis=1)
        excess = d_tx + d_rx - direct
        radius = np.sqrt(np.maximum(wavelength * direct * 0.5, 1e-9)) * scale
        width = max(radius, wavelength * 0.25)
        # Smooth Fresnel sensitivity around the first-zone path-difference boundary.
        row = np.exp(-0.5 * (excess / width) ** 2)
        row /= max(np.linalg.norm(row), 1e-12)
        rows.append(row)
    return np.asarray(rows, dtype=np.float64), (ny, nx)


def _soft_threshold(x: np.ndarray, threshold: float) -> np.ndarray:
    return np.maximum(x - threshold, 0.0)


def fista_nonnegative(A: np.ndarray, y: np.ndarray, iterations: int = 100,
                      sparsity: float = 0.08) -> np.ndarray:
    """Solve min_x 0.5||Ax-y||² + sparsity*||x||₁, x >= 0."""
    if A.ndim != 2 or y.ndim != 1 or A.shape[0] != y.size:
        raise ValueError("A/y dimensions do not match")
    if A.shape[0] == 0 or A.shape[1] == 0:
        raise ValueError("Empty tomography system")
    if not np.all(np.isfinite(A)) or not np.all(np.isfinite(y)):
        raise ValueError("Non-finite measurements")
    spectral = np.linalg.norm(A, ord=2) ** 2
    if spectral < 1e-12:
        return np.zeros(A.shape[1], dtype=np.float64)
    step = 1.0 / spectral
    x = np.zeros(A.shape[1], dtype=np.float64)
    z = x.copy()
    momentum = 1.0
    for _ in range(max(1, int(iterations))):
        grad = A.T @ (A @ z - y)
        nxt = _soft_threshold(z - step * grad, max(0.0, sparsity) * step)
        next_momentum = (1.0 + np.sqrt(1.0 + 4.0 * momentum**2)) / 2.0
        z = nxt + ((momentum - 1.0) / next_momentum) * (nxt - x)
        x, momentum = nxt, next_momentum
    peak = float(np.max(x))
    return x / peak if peak > 1e-12 else x


def reconstruct(cfg: dict[str, Any], measurements: dict[str, float]) -> np.ndarray:
    A, shape = fresnel_matrix(cfg)
    macs = list(cfg["links"].keys())
    y = np.asarray([measurements.get(mac, 0.0) for mac in macs], dtype=float)
    solver = cfg.get("solver", {})
    image = fista_nonnegative(A, y, solver.get("iterations", 100),
                              solver.get("sparsity", 0.08))
    return image.reshape(shape)
