"""Builds the `persistkv` Python module: the engine's sources plus python/persistkv_py.cpp.

    pip install .                 # or, for development:
    python setup.py build_ext --inplace
"""
from glob import glob

from pybind11.setup_helpers import Pybind11Extension, build_ext
from setuptools import setup

ext = Pybind11Extension(
    "persistkv",
    sorted(glob("src/*.cpp")) + ["python/persistkv_py.cpp"],
    include_dirs=["include"],
    cxx_std=14,
    define_macros=[("_CRT_SECURE_NO_WARNINGS", None)],
)

setup(
    name="persistkv",
    version="0.1.0",
    description="Crash-safe, thread-safe key-value storage engine (B+Tree + WAL) with Python bindings",
    ext_modules=[ext],
    cmdclass={"build_ext": build_ext},
    python_requires=">=3.8",
    zip_safe=False,
)
