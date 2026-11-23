"""Build-only adapter for the locked DeepGHS dynamic ONNX, without changing the weights."""

import sys
from pathlib import Path

import onnx
import onnxruntime as ort


def convert(source, destination):
    if (onnx.__version__, ort.__version__) != ('1.20.1', '1.24.4'):
        raise RuntimeError('install the exact ONNX conversion dependencies from requirements-smart-crop.txt')
    model = onnx.load(source, load_external_data=False)
    for entries, name, sizes in ((model.graph.input, 'images', (1, 3, 640, 640)),
                                  (model.graph.output, 'output0', (1, 5, 8400))):
        if len(entries) != 1 or entries[0].name != name:
            raise ValueError('unexpected DeepGHS input/output contract')
        dimensions = entries[0].type.tensor_type.shape.dim
        if len(dimensions) != len(sizes):
            raise ValueError('unexpected DeepGHS tensor rank')
        for dimension, size in zip(dimensions, sizes):
            dimension.dim_value = size
    options = ort.SessionOptions()
    # Basic folding removes dynamic shape operations before OpenCV 4.12 sees them.
    # Do not use hardware-specific graph optimizations or change the checkpoint.
    options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_BASIC
    options.optimized_model_filepath = str(destination)
    ort.InferenceSession(model.SerializeToString(), options, providers=['CPUExecutionProvider'])


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: prepare_anime_face.py <verified-upstream.onnx> <fixed-shape.onnx>')
    convert(Path(sys.argv[1]), Path(sys.argv[2]))
