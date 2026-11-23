"""Verify a normalized PNG against an independently annotated full-height wide fixture."""

import sys

from PIL import Image, ImageChops


def verify(source_path, center_path, smart_path, face):
    with Image.open(source_path) as image:
        source = image.convert('RGB')
    with Image.open(center_path) as image:
        center = image.convert('RGB')
    with Image.open(smart_path) as image:
        smart = image.convert('RGB')
    width, height = 180, 320
    left, top, right, bottom = face  # Half-open pixel coordinates; never taken from a detector.
    if not (0 <= left < right <= source.width and 0 <= top < bottom <= height
            and right - left <= width < source.width and source.height == height
            and center.size == smart.size == (width, height)):
        raise ValueError('invalid annotated fixture or normalized dimensions')
    middle = (source.width - width) // 2

    def matches(candidate, offset):
        crop = source.crop((offset, 0, offset + width, height))
        return ImageChops.difference(candidate, crop).getbbox() is None

    if not matches(center, middle) or middle <= left < right <= middle + width:
        raise ValueError('fixture must reproduce center crop and expose a clipped main face')
    # First match a scanline in the face, then require the entire output to be an exact crop.
    # This keeps the test independent of inference, positioning metrics and PNG encoder bytes.
    row = (top + bottom) // 2
    anchor = smart.crop((0, row, width, row + 1)).tobytes()
    scanline = source.crop((0, row, source.width, row + 1)).tobytes()
    low, high = max(0, right - width), min(left, source.width - width)
    for offset in range(low, high + 1):
        if scanline.startswith(anchor, offset * 3) and matches(smart, offset):
            return
    raise ValueError(f'smart crop did not fully preserve the annotated face {face}')


if __name__ == '__main__':
    verify(*sys.argv[1:4], tuple(map(int, sys.argv[4:])))
