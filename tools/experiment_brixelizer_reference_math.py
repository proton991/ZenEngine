"""Numerical field/slot experiment; no engine code and no GPU-performance claims."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import platform
import sys
import time

import numpy as np

REPO = Path(__file__).resolve().parents[1]
ROOT = REPO / "build/compute-gi/brixelizer-experiment"
PIN = "c6efa6bf7f2027b3ec94f28578bb5965eabb9e55"
SPACINGS = (0.001, 0.0025, 0.005, 0.01, 0.02, 0.04, 0.07)
SLOTS = (0.0, 0.01, 0.02, 0.04)
PHASES = (0.0, 0.25, 0.5, 0.75)
ANGLES = (0, 30, 60)
TOLERANCES = (0.125, 0.25, 0.5, 0.75, 1.0)
BIASES = (0.0, 0.25, 0.5, 1.0)
THICKNESS = 0.01
HIT, MISS, UNAVAILABLE = 1, 2, 3


def box_triangles(lower, upper):
    lo, hi = np.asarray(lower, dtype=float), np.asarray(upper, dtype=float)
    vertices = np.array([[x, y, z] for z in (lo[2], hi[2])
                         for y in (lo[1], hi[1]) for x in (lo[0], hi[0])])
    faces = ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1),
             (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3))
    return np.array([[vertices[a], vertices[b], vertices[c]]
                     for face in faces for a, b, c in
                     ((face[0], face[1], face[2]), (face[0], face[2], face[3]))])


def fixture(slot):
    if slot == 0:
        parts = [box_triangles((-.3, -.5, -.005), (.3, .5, .005))]
    else:
        parts = [box_triangles((-.3, -.5, -.005), (-slot/2, .5, .005)),
                 box_triangles((slot/2, -.5, -.005), (.3, .5, .005)),
                 box_triangles((-slot/2, -.5, -.005), (slot/2, -.3, .005)),
                 box_triangles((-slot/2, .3, -.005), (slot/2, .5, .005))]
    return np.concatenate(parts)


def triangle_distances(points, triangles):
    """Exact closest point to nondegenerate triangles, evaluated in float64."""
    best = np.full(len(points), np.inf)
    for a, b, c in triangles:
        ab, ac = b-a, c-a
        normal = np.cross(ab, ac)
        n2 = np.dot(normal, normal)
        ap = points-a
        signed = ap @ normal
        projected = ap - signed[:, None] / n2 * normal
        d00, d01, d11 = np.dot(ab, ab), np.dot(ab, ac), np.dot(ac, ac)
        d20, d21 = projected @ ab, projected @ ac
        denom = d00*d11-d01*d01
        u = (d11*d20-d01*d21)/denom
        v = (d00*d21-d01*d20)/denom
        inside = (u >= 0) & (v >= 0) & (u+v <= 1)
        edge = np.full(len(points), np.inf)
        for p0, p1 in ((a, b), (b, c), (c, a)):
            vector = p1-p0
            fraction = np.clip((points-p0) @ vector / np.dot(vector, vector), 0, 1)
            delta = points-p0-fraction[:, None]*vector
            edge = np.minimum(edge, np.einsum("ij,ij->i", delta, delta))
        best = np.minimum(best, np.where(inside, signed*signed/n2, edge))
    return np.sqrt(np.maximum(best, 0))


def exact_hits(origins, directions, lengths, triangles):
    """Independent Moller-Trumbore segment oracle; ignore origin contact."""
    distance = np.full(len(origins), np.inf)
    for a, b, c in triangles:
        e1, e2 = b-a, c-a
        p = np.cross(directions, e2)
        det = p @ e1
        valid = np.abs(det) > 1e-12
        inv = np.divide(1.0, det, out=np.zeros_like(det), where=valid)
        s = origins-a
        u = np.einsum("ij,ij->i", s, p)*inv
        q = np.cross(s, e1)
        v = np.einsum("ij,ij->i", directions, q)*inv
        t = q @ e2 * inv
        valid &= (u >= -1e-10) & (v >= -1e-10) & (u+v <= 1+1e-10)
        valid &= (t > 1e-6) & (t < lengths)
        distance = np.minimum(distance, np.where(valid, t, np.inf))
    return np.isfinite(distance)


class Field:
    def __init__(self, triangles, spacing, phase_x, phase_z):
        self.spacing = spacing
        self.brick = 7*spacing
        # Rays stay in y=0. Two exact sample planes suffice to evaluate the same
        # trilinear interpolation as a 3D field along that plane.
        self.origin = np.array((-.42+phase_x*spacing, -.5*spacing, -.21-spacing+phase_z*spacing))
        self.nx = int(np.ceil(.84/self.brick))*7+1
        self.nz = int(np.ceil((.42+spacing)/self.brick))*7+1
        x = self.origin[0]+np.arange(self.nx)*spacing
        z = self.origin[2]+np.arange(self.nz)*spacing
        # Both y planes are equivalent by the fixture's mirror symmetry.
        xx, zz = np.meshgrid(x, z, indexing="ij")
        yy = np.full_like(xx, self.origin[1])
        p = np.column_stack((xx.ravel(), yy.ravel(), zz.ravel()))
        exact = triangle_distances(p, triangles).reshape(self.nx, self.nz)
        # The reference's valid seed conversion sqrt((d/h)^2/16)*4/7 = d/B.
        # Fill every sample with exact geometry distance instead of simulating
        # small-triangle centroids, sparse seed emission or Eikonal approximation.
        q = np.rint(np.clip(exact/self.brick, 0, 1)*255).astype(np.uint8)
        self.quantized = q
        self.plane = q.astype(np.float64)/255

    def sample(self, p):
        u = (p[:, (0, 2)]-self.origin[(0, 2),])/self.spacing
        i = np.floor(u).astype(int)
        fraction = u-i
        assert np.all(i >= 0) and np.all(i+1 < (self.nx, self.nz)), "outside sampled domain"
        x, z = i[:, 0], i[:, 1]
        fx, fz = fraction[:, 0], fraction[:, 1]
        return ((1-fx)*(1-fz)*self.plane[x, z] + fx*(1-fz)*self.plane[x+1, z]
                + (1-fx)*fz*self.plane[x, z+1] + fx*fz*self.plane[x+1, z+1])

    def trace(self, origins, directions, lengths, tolerance):
        # tolerance*h is the effective reference hit threshold, 2*EPS*B.
        eps = tolerance/14
        t = np.full(len(origins), 1e-6)
        samples = np.zeros(len(origins), dtype=int)
        local = np.zeros(len(origins), dtype=int)
        result = np.zeros(len(origins), dtype=int)
        for _ in range(512):
            ids = np.flatnonzero(result == 0)
            if not len(ids):
                break
            done = t[ids] >= lengths[ids]
            result[ids[done]] = MISS
            ids = ids[~done]
            if not len(ids):
                continue
            p = origins[ids]+t[ids, None]*directions[ids]
            d = self.sample(p)-eps
            samples[ids] += 1
            local[ids] += 1
            hit = d < eps
            result[ids[hit]] = HIT
            advancing = ids[~hit]
            if not len(advancing):
                continue
            p = p[~hit]
            direction = directions[advancing]
            cell = np.floor((p-self.origin)/self.brick).astype(int)
            exit_face = self.origin+(cell+(direction > 0))*self.brick
            dt = np.divide(exit_face-p, direction, out=np.full_like(p, np.inf),
                           where=np.abs(direction) > 1e-14)
            exit_dt = np.min(dt, axis=1)
            step = d[~hit]*self.brick
            exits = step > exit_dt
            t[advancing] += np.where(exits, exit_dt+self.brick/1024, step)
            # The reference advances past a cell using B/1024. This experiment
            # makes eight-sample exhaustion explicit rather than a false miss.
            local[advancing[exits]] = 0
            result[advancing[(~exits) & (local[advancing] >= 8)]] = UNAVAILABLE
        result[result == 0] = UNAVAILABLE
        return result, samples


def transmission_rays(slot, angle):
    # Probe aperture densely, plus equal weight opaque controls away from edges.
    if slot:
        centers = np.linspace(-.49*slot, .49*slot, 49)
    else:
        centers = np.linspace(-.02, .02, 49)
    controls = np.r_[np.linspace(-.18, -.05, 25), np.linspace(.05, .18, 25)]
    targets = np.r_[centers, controls]
    theta = np.deg2rad(angle)
    direction = np.array((np.sin(theta), 0, np.cos(theta)))
    origins = np.column_stack((targets-.06*np.tan(theta), np.zeros(len(targets)),
                               np.full(len(targets), -.06)))
    return origins, np.tile(direction, (len(targets), 1)), np.full(len(targets), .12/np.cos(theta))


def self_rays(spacing, bias, angle):
    theta = np.deg2rad(angle)
    x = np.r_[np.linspace(-.18, -.06, 17), np.linspace(.06, .18, 17)]
    origins = np.column_stack((x, np.zeros(len(x)), np.full(len(x), -.005-bias*spacing)))
    direction = np.array((np.sin(theta), 0, -np.cos(theta)))
    return origins, np.tile(direction, (len(x), 1)), np.full(len(x), .10/np.cos(theta))


def counts(actual, expected, core=None):
    if core is None:
        core = np.zeros(len(actual), dtype=bool)
    return dict(rays=len(actual), exact_open=int((~expected).sum()), exact_blocked=int(expected.sum()),
                correct_open=int(((actual == MISS) & ~expected).sum()),
                false_blocked=int(((actual == HIT) & ~expected).sum()),
                wall_leaks=int(((actual == MISS) & expected).sum()),
                unavailable_open=int(((actual == UNAVAILABLE) & ~expected).sum()),
                unavailable_blocked=int(((actual == UNAVAILABLE) & expected).sum()),
                exact_core_open=int((core & ~expected).sum()),
                correct_core_open=int((core & ~expected & (actual == MISS)).sum()))


def aggregate(rows, keys):
    groups = {}
    names = ("rays", "exact_open", "exact_blocked", "correct_open", "false_blocked",
             "wall_leaks", "unavailable_open", "unavailable_blocked", "exact_core_open", "correct_core_open")
    for row in rows:
        key = tuple(row[k] for k in keys)
        group = groups.setdefault(key, dict(zip(keys, key)) | {name: 0 for name in names})
        for name in names:
            group[name] += row[name]
    for row in groups.values():
        for output, numerator, denominator in (
                ("open_survival", "correct_open", "exact_open"),
                ("core_open_survival", "correct_core_open", "exact_core_open"),
                ("false_block_rate", "false_blocked", "exact_open"),
                ("wall_leak_rate", "wall_leaks", "exact_blocked"),
                ("unavailable_rate", None, "rays")):
            value = row[numerator] if numerator else row["unavailable_open"]+row["unavailable_blocked"]
            row[output] = value/row[denominator] if row[denominator] else None
    return list(groups.values())


def validate_math():
    box = box_triangles((-1, -1, -.005), (1, 1, .005))
    points = np.array(((0, 0, -.025), (0, 0, .005), (0, 0, 0), (1.1, 0, .005)))
    assert np.allclose(triangle_distances(points, box), (.02, 0, .005, .1), atol=1e-12)
    origins = np.array(((0, 0, -.06), (1.2, 0, -.06), (0, 0, -.005)))
    dirs = np.array(((0, 0, 1), (0, 0, 1), (0, 0, -1)))
    assert exact_hits(origins, dirs, np.full(3, .12), box).tolist() == [True, False, False]
    # Independent analytic box distance tests triangle faces, edges, corners,
    # and interior distance to the nearest surface.
    random = np.random.default_rng(15).uniform((-1.2, -1.2, -.2), (1.2, 1.2, .2), (1000, 3))
    delta = np.abs(random)-np.array((1, 1, .005))
    analytic = np.linalg.norm(np.maximum(delta, 0), axis=1)+np.minimum(np.max(delta, axis=1), 0)
    assert np.allclose(triangle_distances(random, box), np.abs(analytic), atol=1e-12)
    for slot in SLOTS:
        sample = random.copy()
        sample[:, 1] = -.015
        mirror = sample.copy()
        mirror[:, 1] *= -1
        assert np.allclose(triangle_distances(sample, fixture(slot)),
                           triangle_distances(mirror, fixture(slot)), atol=1e-12)
    for h in SPACINGS:
        d = np.linspace(0, 3.99*h, 1000)
        compressed = np.sqrt((d/h)**2/16)*4/7
        assert np.allclose(compressed, d/(7*h), atol=1e-15)


def plot_results(summary, self_summary):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(1, 3, figsize=(13, 4), constrained_layout=True)
    for spacing in SPACINGS:
        selected = [r for r in summary if r["spacing_cm"] == round(spacing*100, 5) and r["slot_cm"] == 2]
        selected.sort(key=lambda r: r["hit_threshold_samples"])
        for ax, metric, title in zip(axes, ("open_survival", "wall_leak_rate", "unavailable_rate"),
                                    ("2 cm slot: exact open rays retained", "Exact blocked rays leaking", "Unavailable rays")):
            ax.plot([r["hit_threshold_samples"] for r in selected],
                    [100*r[metric] for r in selected], marker="o", label=f"{spacing*100:g} cm spacing")
            ax.set(title=title, xlabel="Effective hit threshold / sample spacing", ylabel="Percent", ylim=(-2, 102))
            ax.grid(alpha=.25)
    axes[0].legend(fontsize=8)
    fig.suptitle("Exact-triangle R8 field: 16 grid alignments, 0/30/60° rays; eight samples per brick")
    fig.savefig(ROOT / "slot-tradeoff.png", dpi=180)
    plt.close(fig)
    fig, ax = plt.subplots(figsize=(6.4, 4.4), constrained_layout=True)
    for spacing in SPACINGS:
        selected = [r for r in self_summary if r["spacing_cm"] == round(spacing*100, 5) and r["hit_threshold_samples"] == .5]
        selected.sort(key=lambda r: r["bias_samples"])
        ax.plot([r["bias_samples"] for r in selected], [100*r["false_block_rate"] for r in selected],
                marker="o", label=f"{spacing*100:g} cm spacing")
    ax.set(title="Near-surface outward rays falsely occluded", xlabel="Outward normal bias / sample spacing",
           ylabel="Percent", ylim=(-2, 102))
    ax.grid(alpha=.25)
    ax.legend()
    fig.savefig(ROOT / "surface-bias.png", dpi=180)
    plt.close(fig)


def main():
    global ROOT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=ROOT)
    parser.add_argument("--plot-only", action="store_true", help="Plot existing results using matplotlib")
    parser.add_argument("--plot-libs", type=Path, help="Optional local matplotlib dependency directory")
    args = parser.parse_args()
    ROOT = args.output_dir.resolve()
    ROOT.mkdir(parents=True, exist_ok=True)
    if args.plot_only:
        if args.plot_libs:
            sys.path.insert(0, str(args.plot_libs.resolve()))
        payload = json.loads((ROOT / "results.json").read_text())
        plot_results(payload["transmission_summary"], payload["surface_summary"])
        return
    started = time.perf_counter()
    validate_math()
    transmission, surface = [], []
    for spacing in SPACINGS:
        for slot in SLOTS:
            triangles = fixture(slot)
            references = {angle: (*transmission_rays(slot, angle),) for angle in ANGLES}
            refs = {angle: exact_hits(*rays, triangles) for angle, rays in references.items()}
            cores = {}
            for angle, (origins, _, _) in references.items():
                centers = origins[:, 0]+.06*np.tan(np.deg2rad(angle))
                clearance = slot/2-np.abs(centers)-THICKNESS/2*np.tan(np.deg2rad(angle))
                cores[angle] = (slot > 0) & (clearance >= slot*.2)
            for phase_x in PHASES:
                for phase_z in PHASES:
                    field = Field(triangles, spacing, phase_x, phase_z)
                    for tolerance in TOLERANCES:
                        base = dict(spacing_cm=round(spacing*100, 5), slot_cm=round(slot*100, 5),
                                    phase_x=phase_x, phase_z=phase_z,
                                    hit_threshold_samples=tolerance,
                                    epsilon_normalized=tolerance/14,
                                    upstream_sdf_solve_epsilon=tolerance*4/7)
                        for angle in ANGLES:
                            actual, samples = field.trace(*references[angle], tolerance)
                            transmission.append(base | dict(angle_degrees=angle) | counts(actual, refs[angle], cores[angle]) |
                                                dict(mean_distance_samples=float(samples.mean()),
                                                     maximum_distance_samples=int(samples.max())))
                            if slot == 0:
                                for bias in BIASES:
                                    rays = self_rays(spacing, bias, angle)
                                    exact = exact_hits(*rays, triangles)
                                    assert not exact.any(), "outward rays must be unobstructed"
                                    actual, samples = field.trace(*rays, tolerance)
                                    surface.append(base | dict(angle_degrees=angle, bias_samples=bias) |
                                                   counts(actual, exact))
            print(f"spacing {spacing*100:g} cm; slot {slot*100:g} cm complete", flush=True)
            (ROOT / "checkpoint.json").write_text(json.dumps(dict(transmission=transmission, surface=surface)))
    grouped = aggregate(transmission, ("spacing_cm", "slot_cm", "hit_threshold_samples"))
    self_grouped = aggregate(surface, ("spacing_cm", "hit_threshold_samples", "bias_samples"))
    elapsed = time.perf_counter()-started
    metadata = dict(pinned_commit=PIN, python=platform.python_version(), numpy=np.__version__,
                    script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    fixture_wall_thickness_cm=1, grid_alignments_per_case=16,
                    normalized_sample_spacing="B/7", atlas_format="R8_UNORM",
                    quantization_model="nearest integer, ties to even; not measured imageStore rounding",
                    distance_model="exact nearest mesh triangle at every sample; no occupancy transform",
                    interpolation="trilinear, evaluated at y=0 using two adjacent y sample planes",
                    omitted="sparse emission, centroid approximation, Eikonal, AABB hierarchy, alpha, GPU precision/performance",
                    status_model="hit, covered miss, unavailable after 8 local samples; full-cell conservative bounds",
                    cell_exit_model="clip steps at exact cell boundary then add B/1024; excludes upstream 0.001 normalized overshoot margin",
                    seconds=elapsed, transmission_records=len(transmission), self_records=len(surface),
                    transmission_rays=sum(r["rays"] for r in transmission), self_rays=sum(r["rays"] for r in surface))
    payload = dict(metadata=metadata, transmission_summary=grouped, surface_summary=self_grouped,
                   transmission=transmission, surface=surface)
    (ROOT / "results.json").write_text(json.dumps(payload, indent=2)+"\n")
    for name, rows in (("transmission", transmission), ("surface", surface)):
        with (ROOT / f"{name}.csv").open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=rows[0].keys())
            writer.writeheader()
            writer.writerows(rows)
    print(json.dumps(metadata, indent=2), flush=True)


if __name__ == "__main__":
    main()
