#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Convert the ST model-zoo YuNet QDQ-INT8 ONNX from BGR to RGB input.

YuNet is trained with BGR input (OpenCV convention, see the zoo config
`color_mode: bgr`), while the NE302 PIPE2 camera path feeds RGB888. Instead
of swapping channels at runtime on the device, we swap them inside the
model: the first convolution is the only consumer of the network input,
and conv is linear over its input channels, so

    W_orig * x_bgr == W_orig[:, ::-1, :, :] * x_rgb        (channel swap
                                                           is an involution)

The first-conv weight is stored as an int8 initializer dequantized with a
per-OUTPUT-channel scale (axis 0), so reordering the input-channel axis
(axis 1) is exact - no re-quantization, no extra ops, no runtime cost.
The input QuantizeLinear is per-tensor (scalar scale / zero point), hence
unaffected by the channel order.

The script asserts every precondition above before touching anything and
refuses to convert otherwise.

Usage:
    python Script/yunet_bgr_to_rgb.py \
        --input  Model/weights/yunetn_320_qdq_int8_fd_ui.onnx \
        --output Model/weights/yunetn_320_qdq_int8_rgb_fd_ui.onnx

Verify afterwards with --self-test (needs onnxruntime): for random uint8
images, original(swap(x)) must equal converted(x).
"""

import argparse

import numpy as np
import onnx
from onnx import numpy_helper


def find_single(lst, pred, what):
    hits = [x for x in lst if pred(x)]
    if len(hits) != 1:
        raise RuntimeError("expected exactly one %s, found %d" % (what, len(hits)))
    return hits[0]


def convert(src, dst):
    model = onnx.load(src)
    graph = model.graph
    inits = {i.name: i for i in graph.initializer}

    # 1. graph input -> QuantizeLinear (the only consumer)
    gin = graph.input[0].name
    ql = find_single(graph.node, lambda n: gin in n.input,
                     "QuantizeLinear consuming the graph input")
    if ql.op_type != "QuantizeLinear":
        raise RuntimeError("graph input is consumed by %s, not QuantizeLinear" % ql.op_type)

    # 2. input quantization must be per-tensor (scalar scale / zero point)
    for tname in (ql.input[1], ql.input[2]):
        if tname not in inits or list(inits[tname].dims) != []:
            raise RuntimeError("input scale/zero_point %s is not a per-tensor "
                               "scalar initializer" % tname)

    # 3. QuantizeLinear -> DequantizeLinear -> a single Conv
    dq = find_single(graph.node, lambda n: n.input and n.input[0] == ql.output[0],
                     "DequantizeLinear after the input QuantizeLinear")
    conv = find_single(graph.node, lambda n: dq.output[0] in n.input,
                       "consumer of the dequantized input")
    if conv.op_type != "Conv":
        raise RuntimeError("dequantized input feeds a %s, not a Conv" % conv.op_type)

    # 4. conv weight must come from an int8 initializer through one DQ
    wdq = find_single(graph.node,
                      lambda n: n.op_type == "DequantizeLinear"
                      and n.output and n.output[0] == conv.input[1],
                      "DequantizeLinear producing the conv weight")
    wq_name = wdq.input[0]
    wq = inits[wq_name]
    w = numpy_helper.to_array(wq)
    if w.ndim != 4 or w.shape[1] != 3:
        raise RuntimeError("conv weight %s has shape %s, expected [O, 3, kH, kW]"
                           % (wq_name, w.shape))
    # 5. weight scale must be per-output-channel (axis 0) or per-tensor
    ws = numpy_helper.to_array(inits[wdq.input[1]])
    if ws.size not in (1, w.shape[0]):
        raise RuntimeError("conv weight scale shape %s is neither per-tensor "
                           "nor per-output-channel" % (ws.shape,))

    # swap input-channel axis of the int8 weight (exact for quantized values)
    w_rgb = w[:, ::-1, :, :].copy()
    new_init = numpy_helper.from_array(w_rgb, name=wq_name)
    for i, t in enumerate(graph.initializer):
        if t.name == wq_name:
            graph.initializer[i].CopyFrom(new_init)
            break

    note = ("converted to RGB input order by reversing the first-conv "
            "input-channel axis (%s, shape %s)" % (wq_name, list(w.shape)))
    if model.metadata_props:
        for p in model.metadata_props:
            if p.key == "color_mode":
                p.value = "rgb"
                break
    else:
        meta = model.metadata_props.add()
        meta.key, meta.value = "color_mode", "rgb"
    meta = model.metadata_props.add()
    meta.key, meta.value = "bgr_to_rgb", note

    onnx.checker.check_model(model)
    onnx.save(model, dst)
    print("[OK] %s\n     -> %s\n     %s" % (src, dst, note))


def self_test(src, dst, rounds=5):
    import onnxruntime as ort
    o = ort.InferenceSession(src, providers=["CPUExecutionProvider"])
    c = ort.InferenceSession(dst, providers=["CPUExecutionProvider"])
    name = o.get_inputs()[0].name
    rng = np.random.default_rng(42)
    worst = 0.0
    for _ in range(rounds):
        x_rgb_u8 = rng.integers(0, 256, size=(1, 3, 320, 320), dtype=np.uint8)
        x_rgb = x_rgb_u8.astype(np.float32)
        x_bgr = x_rgb[:, ::-1].copy()
        ref = o.run(None, {name: x_bgr})
        got = c.run(None, {name: x_rgb})
        for r, g in zip(ref, got):
            worst = max(worst, float(np.abs(r.astype(np.float64) - g.astype(np.float64)).max()))
    print("[OK] self-test: %d random images, max |original(swap(x)) - rgb(x)| = %.3g"
          % (rounds, worst))
    if worst > 1e-3:
        raise SystemExit("outputs differ more than expected")


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    p.add_argument("--input", required=True)
    p.add_argument("--output", required=True)
    p.add_argument("--self-test", action="store_true",
                   help="after converting, numerically verify equivalence "
                        "with onnxruntime")
    a = p.parse_args()
    convert(a.input, a.output)
    if a.self_test:
        self_test(a.input, a.output)


if __name__ == "__main__":
    main()
