"""Reproduce the denoiser plan's drift, step and prediction check in linear RGB."""
import json
import numpy as np


def simulate(signal, visibility, predict, sigma=1.68 / np.sqrt(40), histories=4096):
    rng = np.random.default_rng(7)
    log_sigma = np.sqrt(np.log1p(sigma ** 2))
    truth = signal * visibility[:, None]
    history = np.broadcast_to(truth[0], (histories, 3)).copy()
    age = 256.
    errors, means = [], []
    for frame, current in enumerate(truth):
        previous = max(frame - 1, 0)
        # Evaluate both light states on each realization of the same angular sample.
        # Occluder changes lack a signed prediction and enter the step rule.
        factor = rng.lognormal(-.5 * log_sigma ** 2, log_sigma, (histories, 1))
        scale = max(current.max(), truth[previous].max(), 1e-8)
        change = np.max(np.abs(current - truth[previous])) / scale
        signed = (signal[frame] - signal[previous]) * visibility[frame] if predict else np.zeros(3)
        residual = max(.5 * change, np.max(np.abs(current - truth[previous] - signed)) / scale)
        if residual > 0:
            drift = np.clip(np.cbrt(sigma ** 2 / (4 * residual ** 2)), 4, 256)
            cap = min(drift, 1 + sigma ** 2 / residual ** 2)
        else:
            cap = 256
        age = min(age + 1, cap)
        history += factor * signed
        history += (factor * current - history) / age
        history = np.maximum(history, 0)
        means.append(history.mean(axis=0))
        errors.append(np.mean((history - current) ** 2))
    peak = np.max(truth)
    bias = np.asarray(means) - truth
    return dict(rms_over_peak=float(np.sqrt(np.mean(errors)) / peak),
                mean_energy_error=float(abs(bias.sum()) / truth.sum()),
                frame8_peak_bias=float(np.max(abs(bias[135])) / peak),
                frame32_peak_bias=float(np.max(abs(bias[159])) / peak))


def angular_prediction():
    """Measure L1 SH over a fixed window under a rotating, coloured environment."""
    indices = np.arange(65536, dtype=np.float64)
    z = (indices + .5) / len(indices)
    phi = indices * (np.pi * (3 - np.sqrt(5)))
    radius = np.sqrt(1 - z * z)
    directions = np.column_stack((radius * np.cos(phi), radius * np.sin(phi), z))
    window = (directions[:, 0] > -.15) & (np.abs(directions[:, 1]) < .6)
    basis = np.column_stack((np.full(len(indices), .2820947918), .4886025119 * directions))

    def sky(angle):
        axis = np.array([np.cos(angle), np.sin(angle), 1]) / np.sqrt(2)
        peak = np.maximum(directions @ axis, 0) ** 24
        return window[:, None] * (np.array([.06, .1, .2]) + peak[:, None] * np.array([3.0, .8, .4]))

    before = sky(0)
    scale = .5
    after = scale * sky(np.pi / 4)
    previous = 2 * np.pi * (before.T @ basis) / len(indices)
    target = 2 * np.pi * (after.T @ basis) / len(indices)
    residual = 2 * np.pi * (after - scale * before)[:, :, None] * basis[:, None, :]
    selected = np.random.default_rng(11).integers(len(indices), size=(4096, 40))
    predicted = scale * previous + residual[selected].mean(axis=1)
    scalar = previous * (target[:, :1] / previous[:, :1])
    scaled_target = 2 * np.pi * ((scale * sky(0)).T @ basis) / len(indices)
    return dict(pure_scale_max_sh_error=float(np.max(np.abs(scaled_target - scale * previous))),
                rotation_scalar_l1_relative_error=float(np.linalg.norm(scalar[:, 1:] - target[:, 1:]) /
                                                        np.linalg.norm(target[:, 1:])),
                same_ray_mean_sh_relative_error=float(np.linalg.norm(predicted.mean(axis=0) - target) /
                                                      np.linalg.norm(target)))


def sparse_observation_weight():
    """Exact expectation when one of sixteen directions sees a bright source."""
    sample = np.array([16., 0.])
    probability = np.array([1. / 16, 15. / 16])
    mean = probability @ sample
    mature_age = 256.
    # A previously observed neighbor has relative change 1/8. The center has no evidence.
    variance_ratio = (.266 / .125) ** 2
    neighbor_age = min(max(4., np.cbrt(.25 * variance_ratio)), 1. + variance_ratio, mature_age)
    hit_conditioned_age = np.array([neighbor_age, mature_age])
    # The rough filter used the same batch for change detection and radiance.
    # Conditional on either previous outcome, the new batch still has unit mean.
    rough_age = np.array([1. + .266 ** 2, mature_age])
    independent = mean + (sample[None, :] - mean) / rough_age[:, None]
    return dict(target=float(mean),
                current_hit_selects_neighbor=float(probability @ (mean + (sample - mean) / hit_conditioned_age)),
                previous_evidence_only=float(probability @ (mean + (sample - mean) / mature_age)),
                rough_current_batch=float(probability @ (mean + (sample - mean) / rough_age)),
                rough_previous_batch=float(probability @ independent @ probability))


def main():
    frames = np.arange(512)
    steady = np.ones((512, 3))
    on = np.ones(512)
    step = steady.copy()
    step[:128] = 0
    hue = steady.copy()
    hue[:128] *= [1, 0, 0]
    hue[128:] *= [0, .2126 / .7152, 0]
    motion = (.52 + .48 * np.cos(frames * 2 * np.pi / 64))[:, None] * steady
    cases = dict(turn_on=(step, on), equal_luma_hue=(hue, on),
                 shadow_snap=(steady, (frames < 128) | (frames >= 384)), symmetric_motion=(motion, on))
    results = {case: {str(pred): simulate(signal, visibility, pred) for pred in (False, True)}
               for case, (signal, visibility) in cases.items()}
    results['angular_prediction'] = angular_prediction()
    results['sparse_observation_weight'] = sparse_observation_weight()
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
