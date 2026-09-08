#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Copyright (C) by ETHZ/SED

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published
by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

--------------------------------------------------------------------------------

scogfplot renders one per-origin OGF debug snapshot written by scogf into a
single image and opens it.

Dependencies: python3-matplotlib.
"""

import argparse
import collections
import gzip
import json
import os
import re
import subprocess
import sys
import tempfile


EXIT_OK = 0
EXIT_ARGS = 2
EXIT_NO_SNAPSHOT = 3
EXIT_PARSE = 4
EXIT_RENDER = 5


def eprint(*args):
    print(*args, file=sys.stderr)


def slugify(public_id):
    s = re.sub(r"[^A-Za-z0-9._-]", "_", public_id)
    return s or "origin"


def load_snapshot(dump_dir, origin_id):
    slug = slugify(origin_id)
    candidates = [
        os.path.join(dump_dir, slug + ".json"),
        os.path.join(dump_dir, slug + ".json.gz"),
    ]
    path = next((p for p in candidates if os.path.isfile(p)), None)
    if path is None:
        eprint(
            "scogfplot: no OGF debug snapshot for %s in %s\n"
            "           Check --dump-dir and that scogf has processed this origin."
            % (origin_id, dump_dir)
        )
        sys.exit(EXIT_NO_SNAPSHOT)

    opener = gzip.open if path.endswith(".gz") else open
    try:
        with opener(path, "rt") as fp:
            return path, json.load(fp)
    except Exception as exc:
        eprint("scogfplot: cannot parse %s: %s" % (path, exc))
        sys.exit(EXIT_PARSE)


def sort_stations(stations, mode):
    """Contributing stations first, then skipped ones (always by distance).
    mode 'distance': nearest first. mode 'sgf': highest station GoF first."""
    if mode == "sgf":
        def key(s):
            if s.get("used"):
                return (0, -s.get("sgf", 0.0))
            return (1, s.get("distanceKm", 1e9))
    else:
        def key(s):
            return (0 if s.get("used") else 1, s.get("distanceKm", 1e9))
    return sorted(stations, key=key)


def annotate(ax, s):
    def g(key):
        return s.get(key, float("nan"))

    hypo = s.get("hypoDistanceKm")
    dist_str = "Δ %.0f km" % g("distanceKm")
    if hypo is not None and hypo >= 0:
        dist_str += " (Rhyp %.0f)" % hypo
    header = (
        "%s   %s   SGF %.2f   AmpFit %.2f   Corr %.2f   "
        "MaxObs %.3g   MaxPred %.3g   StaAmp %.2f   PGV %.3g"
    ) % (
        s.get("sid", "?"),
        dist_str,
        g("sgf"),
        g("amplitudeFit"),
        g("correlation"),
        g("maxObs"),
        g("maxPred"),
        g("amplification"),
        s.get("pgv", 0.0),
    )
    predicted = s.get("predictedPath")
    ax.set_title(header, loc="left", fontsize=8, fontfamily="monospace",
                 pad=13 if predicted else 6)
    if predicted:
        ax.text(0.0, 1.0, predicted, transform=ax.transAxes, ha="left", va="bottom",
                fontsize=6.5, color="#8b95a1", fontfamily="monospace")


def series_xy(block):
    v = block.get("v", [])
    t0 = block.get("t0", 0.0)
    return [t0 + i for i in range(len(v))], v


def plot_station(ax, s, context, legend=False):
    pred = s.get("series", {}).get("rawPredicted", {"t0": 0, "v": []})
    obs = s.get("series", {}).get("observed", {"t0": 0, "v": []})

    xp, vp = series_xy(pred)
    xo, vo = series_xy(obs)

    h_obs, = ax.plot(xo, vo, color="#1b2530", lw=1.3, label="observed", zorder=3)

    # The predicted envelope shares only its shape with the observed one (the SGF
    # correlation is scale invariant), so it keeps its own units on a secondary
    # axis. The dashed grey line is identified by the legend; the grey right-hand
    # ticks give its scale.
    twin = ax.twinx()
    h_pred, = twin.plot(xp, vp, color="#9aa7ad", lw=1.1, ls="--", label="predicted", zorder=1)
    twin.tick_params(axis="y", labelsize=7, colors="#9aa7ad")

    if legend:
        ax.legend(
            handles=[h_obs, h_pred],
            loc="upper right",
            fontsize=7,
            framealpha=0.85,
        )

    w0, w1 = s.get("window", {}).get("startSec", 0), s.get("window", {}).get("endSec", 0)
    ax.axvspan(w0, w1, color="#0e6b6a", alpha=0.08, zorder=0)
    for tt, name in ((s.get("ttP"), "P"), (s.get("ttS"), "S")):
        if tt is not None and tt >= 0:
            ax.axvline(tt, color="#b25a12", lw=0.9, ls=":")
            ax.annotate(
                name,
                (tt, 1.0),
                xycoords=("data", "axes fraction"),
                xytext=(2, -9),
                textcoords="offset points",
                fontsize=8,
                color="#b25a12",
            )

    left = min([0.0] + xo) if xo else 0.0
    ax.set_xlim(left, w1 + context)
    ax.tick_params(labelsize=8)
    ax.margins(y=0.15)
    annotate(ax, s)


def build_figure(snap, context, sort, max_stations):
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    stations = sort_stations(snap.get("stations", []), sort)
    used_all = [s for s in stations if s.get("used")]
    skipped = [s for s in stations if not s.get("used")]

    if not used_all:
        eprint("scogfplot: snapshot has no contributing stations")
        sys.exit(EXIT_RENDER)

    used = used_all[:max_stations] if max_stations > 0 else used_all
    hidden = len(used_all) - len(used)
    n = len(used)

    fig_h = 2.0 + 1.5 * n
    fig, axgrid = plt.subplots(n, 1, figsize=(10, fig_h), squeeze=False, sharex=False)
    axlist = [a[0] for a in axgrid]

    for i, s in enumerate(used):
        plot_station(axlist[i], s, context, legend=(i == 0))

    axlist[-1].set_xlabel("seconds since origin time", fontsize=9)

    org = snap.get("origin", {})
    title = "%s    %s %.2f    OGF %.1f    %d/%d stations used" % (
        org.get("publicID", "?"),
        snap.get("bestMagnitude", {}).get("type", "?"),
        snap.get("bestMagnitude", {}).get("value", float("nan")),
        snap.get("ogf", float("nan")),
        len(used_all),
        len(stations),
    )
    loc_line = "lat %.3f°   lon %.3f°   depth %.1f km   station cutoff distance %.0f km" % (
        org.get("latitude", float("nan")),
        org.get("longitude", float("nan")),
        org.get("depth", float("nan")),
        snap.get("cutoffDistanceKm", float("nan")),
    )
    # Title and location line are two short lines above the axes; space them by
    # a fixed number of inches (converted to a figure fraction) so the gap
    # stays legible regardless of the station count / figure height.
    title_y = 1 - 0.25 / fig_h
    loc_y = 1 - 0.55 / fig_h
    top_rect = 1 - 0.85 / fig_h
    fig.suptitle(title, fontsize=11, fontfamily="monospace", y=title_y)
    fig.text(0.5, loc_y, loc_line, fontsize=8.5, fontfamily="monospace",
             color="#5b6675", ha="center")

    parts = ["generator %s" % snap.get("generator", "scogf")]
    if hidden:
        parts.append("showing %d of %d contributing stations (--max-stations)"
                     % (n, len(used_all)))
    if skipped:
        # Summarise by reason rather than one entry per station: a dense event
        # can have hundreds of associated-but-unused stations.
        counts = collections.Counter(s.get("skipReason", "?") for s in skipped)
        by_reason = ", ".join(
            "%s x%d" % (reason, c) for reason, c in counts.most_common()
        )
        parts.append("%d skipped: %s" % (len(skipped), by_reason))
    fig.text(0.01, 0.005, "    ".join(parts), fontsize=7, color="#5b6675")

    fig.tight_layout(rect=(0, 0.02, 1, top_rect))
    return fig


def open_viewer(path, viewer):
    cmd = viewer or "xdg-open"
    print("Starting viewer %s %s" % (cmd, path))
    try:
        subprocess.Popen(
            cmd.split() + [path],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
    except Exception as exc:
        eprint("scogfplot: could not launch viewer '%s': %s" % (cmd, exc))


def main():
    parser = argparse.ArgumentParser(
        prog="scogfplot",
        description="Render one scogf OGF debug snapshot for an origin.",
    )
    parser.add_argument("originID", help="origin publicID (argv[1] from scolv)")
    parser.add_argument("eventID", nargs="?", help="event publicID (ignored, accepted for scolv)")
    parser.add_argument("--dump-dir", required=True,
                        help="directory holding the snapshots (scogf 'debug.dumpPath')")
    parser.add_argument("--sort", choices=("distance", "sgf"), default="distance",
                        help="station order: distance (nearest first, default) or "
                             "sgf (station goodness of fit, highest first)")
    parser.add_argument("--max-stations", type=int, default=0, metavar="N",
                        help="plot at most N contributing stations, taken in --sort "
                             "order; 0 (default) means no limit")
    parser.add_argument("--context", type=float, default=20.0,
                        help="seconds of observed envelope shown after the window (default: 20)")
    parser.add_argument("--out", help="write the PNG here instead of a temporary file")
    parser.add_argument("--no-show", action="store_true", help="do not launch an image viewer")
    parser.add_argument("--viewer", help="image viewer command (default: xdg-open)")
    args = parser.parse_args()

    dump_dir = os.path.expanduser(args.dump_dir)
    path, snap = load_snapshot(dump_dir, args.originID)

    try:
        fig = build_figure(snap, args.context, args.sort, args.max_stations)
    except SystemExit:
        raise
    except ImportError as exc:
        eprint("scogfplot: matplotlib is required (%s). Install python3-matplotlib." % exc)
        sys.exit(EXIT_RENDER)
    except Exception as exc:
        eprint("scogfplot: rendering failed: %s" % exc)
        sys.exit(EXIT_RENDER)

    out = args.out
    if not out:
        fd, out = tempfile.mkstemp(prefix="scogf_%s_" % slugify(args.originID), suffix=".png")
        os.close(fd)

    try:
        fig.savefig(out, dpi=110)
    except Exception as exc:
        eprint("scogfplot: could not save %s: %s" % (out, exc))
        sys.exit(EXIT_RENDER)

    print("%s -> %s" % (os.path.basename(path), out))

    if not args.no_show:
        open_viewer(out, args.viewer)

    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
