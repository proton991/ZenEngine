"""Write native-pixel, lossless playback of a production sweep/cut and its first 16 recovery frames."""
import argparse
from pathlib import Path

from PIL import Image

import gi_quick_gates as gates
from gi_stability import read_json


def make_clip(folders, visit, crop, fps, output):
    runs = [read_json(folder / 'report.json') for folder in folders]
    schedules = [[(row['frame'], row['angle'], row['recovery']) for row in run['moving']
                  if row['visit'] == visit and row['recovery'] <= 16] for run in runs]
    schedule = schedules[0]
    if not schedule or any(other != schedule for other in schedules):
        raise ValueError('Capture schedules differ or contain no frames')
    frames = [f for f, _, _ in schedule]
    if frames != list(range(frames[0], frames[-1] + 1)):
        raise ValueError('Playback requires every consecutive rendered frame')
    if not (0 < fps <= 120):
        raise ValueError('Playback rate must be in (0, 120] FPS')
    images = []
    for frame in frames:
        panels = []
        camera = None
        for folder in folders:
            path = gates.frame_path(folder / 'moving', frame)
            state = read_json(str(path) + '.json')
            if camera is not None and state['projectionViewBits'] != camera:
                raise ValueError(f'Camera mismatch at frame {frame}')
            camera = state['projectionViewBits']
            with Image.open(path) as source:
                x0, y0, x1, y1 = crop
                if not (0 <= x0 < x1 <= source.width and 0 <= y0 < y1 <= source.height):
                    raise ValueError(f'Crop outside capture at {path}')
                panels.append(source.crop(crop).convert('RGB'))
        w, h = panels[0].size
        image = Image.new('RGB', (len(panels) * w, h))
        for index, panel in enumerate(panels):
            image.paste(panel, (index * w, 0))
        images.append(image)
    ticks = [round(i * 1000 / fps) for i in range(len(images) + 1)]
    images[0].save(output, format='WEBP', save_all=True, append_images=images[1:], loop=0,
                   duration=[end - start for start, end in zip(ticks, ticks[1:])], lossless=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runs', type=Path, nargs='+', help='Completed replays in left-to-right order')
    parser.add_argument('--visit', choices=('out', 'return'), default='out')
    parser.add_argument('--crop', type=int, nargs=4, required=True, metavar=('X0', 'Y0', 'X1', 'Y1'))
    parser.add_argument('--fps', type=float, required=True, help='Measured native playback rate')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    make_clip(args.runs, args.visit, args.crop, args.fps, args.output)
