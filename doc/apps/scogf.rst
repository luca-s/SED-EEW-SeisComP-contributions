*scogf* is a real-time implementation of the goodness-of-fit measure of
:ref:`Jozinović et al. (2024) <scogf-references>`. A source estimate
(hypocentre and magnitude) is judged by how well the ground motions it
*predicts* match those actually *observed* in real time, rather than by
origin-quality proxies such as pick count, azimuthal gap or RMS. The measure is
absolute (bounded between 0 and 100) and independent of the algorithm that
produced the origin, so OGF values can be compared across pipelines and against
a fixed alerting threshold.

*scogf* computes, for each incoming origin, an Origin Goodness of Fit (OGF): a
measure of how well the observed real-time ground-motion envelopes match the
envelopes predicted for the origin's location, depth and magnitude.   The result is
written to the origin as a comment (``eew.ogf.value``) together with the publicID
of the best-fitting magnitude (``eew.ogf.mag``), for :ref:`scevent` to use when
scoring or selecting the preferred origin.

For each origin *scogf*

#. associates nearby sensor locations within :confval:`maximumDistance`
   (optionally magnitude-dependent, see :confval:`distancePerMagnitude`),
#. computes P and S travel times with the configured travel-time interface
   (:confval:`tableType`, :confval:`table`),
#. buffers the combined horizontal velocity envelopes produced by
   :ref:`sceewenv`,
#. once a station's predicted P arrival is covered by its buffered data, scores
   it by combining a shape term (the correlation between the observed and
   predicted envelope) and an amplitude term (the observed peak against the
   predicted peak, i.e. the GMM PGV scaled by the station amplification), then
   averages the per-station scores into the overall OGF (see `Method`_).

Optionally an envelope magnitude ``Menv`` is derived as the magnitude whose
predicted envelopes best fit the observations, see
:confval:`envelopeMagnitude.enable`.

The predicted envelopes, GMM PGV coefficients and per-station soil/amplification
bindings are read from :confval:`predictionArchivePath`.

Predicted envelopes
-------------------

The predicted horizontal velocity envelopes follow the functional forms of
:ref:`Cua (2005) <scogf-references>`. Because those were calibrated on
southern-California data and were found to over-predict Swiss ground motions,
Jozinović et al. (2024) rescale them with the Swiss ground-motion model of
:ref:`Cauzzi et al. (2015) <scogf-references>`. *scogf* does not evaluate
these relations itself; it reads a pre-computed archive
(:confval:`predictionArchivePath`) holding

* the predicted envelope shape per soil class, magnitude and distance bin
  (``envelopes/<soil>/<mag>/<dist>/V_H.npy``), nearest-neighbour matched to the
  origin magnitude and the station **hypocentral** distance;
* the GMM peak ground velocity (PGV) per region, magnitude and (hypocentral)
  distance (``GMM.csv`` with the region polygons in ``GMMpolygon.bna``);
* the per-station soil class and site amplification factor
  (``station-config.csv``). Stations missing from that file fall back to
  :confval:`sensorLocations.defaultSoilClass` and an amplification of 1.

As in the paper, site response enters only through the soil class — typically
*rock* (EC8 ground types A and B) or *soil* — and the scalar amplification
factor. The stored envelope shape is normalised to unit peak and then scaled to
``PGV * amplification`` before the amplitude comparison; the shape correlation
is unaffected by this scaling.

The archive is indexed by hypocentral distance (the Cua 2005 and Cauzzi et al.
2015 relations are defined on it), so *scogf* looks up both the envelope and the
PGV at ``sqrt(epicentral^2 + depth^2)``. The station search radius
(:confval:`maximumDistance`, :confval:`distancePerMagnitude`) is epicentral.

Station goodness of fit (SGF)
-----------------------------

For each contributing station the fit combines an amplitude term and a shape
term, following equations 1–3 of Jozinović et al. (2024):

* **Amplitude fit** ``A = 1 - ((o - m) / (o + m))^2``, where ``o`` is the peak
  of the observed envelope in the correlation window and ``m`` the peak of the
  scaled predicted envelope there. ``A`` is 1 for a perfect match and decays
  towards 0 as the peaks diverge. It depends only on the ratio of the two
  peaks, which keeps it bounded and independent of earthquake size.
* **Shape correlation** ``C`` is the normalised zero-lag cross-correlation of
  the observed and predicted envelope samples over the window: both series are
  demeaned and normalised by their standard deviation, which makes ``C`` the
  Pearson correlation coefficient.

The station score is ``SGF = sqrt(A * C)``. The paper's per-station value is
``G = 100 * SGF``.

The correlation window is described under `Reading the plot`_. Following
Jozinović et al. (2024), every station shares one start time ``t0`` — the
predicted P arrival at the closest associated station minus
:confval:`preArrivalTimeWindow`, clamped to the origin time. The window ends,
per station, at the earliest of :confval:`postArrivalTimeShare` times that
station's S travel time, the predicted-envelope length and the end of the
buffered data.

Station selection
-----------------

Following Jozinović et al. (2024), a station contributes to the OGF only once
its **predicted P arrival** is covered by the ground-motion data buffered for
it — that is, once the correlation window reaches that station's ``ttP``. Early
in an event, or for a high-latency station, the window would otherwise hold
only pre-P noise. Stations are also limited to the search radius
(:confval:`maximumDistance`, :confval:`distancePerMagnitude`). Stations that are
associated but not yet contributing appear in the debug snapshot with skip
reason *predicted P not yet arrived*.

Origin goodness of fit (OGF)
----------------------------

The OGF is ``100`` times the unweighted mean of ``SGF`` over all stations that
contributed for a given magnitude; stations are not distance-weighted. *scogf*
evaluates every magnitude attached to the origin (and, when
:confval:`envelopeMagnitude.enable` is set, every value on the
:confval:`envelopeMagnitude` grid) and writes the **highest** OGF to
:confval:`commentID` and the publicID of the magnitude that produced it to
:confval:`commentMagID`.


Showing the OGF in scolv
========================

To display the OGF value on the Location tab information panel of :ref:`scolv`,
add to :file:`scolv.cfg`:

.. code-block:: properties

   display.origin.comment.id      = eew.ogf.value
   display.origin.comment.label   = OGF
   display.origin.comment.default = "-"

The comment can likewise be shown as a column in the Event and Events tabs, see
:ref:`scolv` (``eventedit.origin.customColumn.*``, ``eventlist.customColumn.*``).


Debugging with scogfplot
========================

Set :confval:`debug.dumpPath` to make *scogf* write, for every processed origin,
one JSON snapshot describing every station that entered the computation for the
best-fitting magnitude: the raw predicted envelope, the observed envelope, and
the SGF, PGV, amplification and correlation-window values.

Each origin produces one file ``<debug.dumpPath>/<originID>.json`` (the origin
publicID with every character outside ``[A-Za-z0-9._-]`` replaced by ``_``). The
file is overwritten on every update of that origin and left frozen once the
origin ages out of the cache, so it always reflects the last OGF computation.
:confval:`debug.keepDays` and :confval:`debug.maxFiles` bound the disk usage;
*scogf* prunes the directory itself, at most once every five minutes.

The *scogfplot* tool renders one such snapshot into an image.


scogfplot
---------

*scogfplot* is a plain command-line script (Python, requires
``python3-matplotlib``); it has no configuration file.

.. code-block:: sh

   scogfplot --dump-dir DIR [options] originID [eventID]

``--dump-dir`` is required and must point at *scogf*'s :confval:`debug.dumpPath`.
The ``eventID`` argument is accepted so the same command works from *scolv*, but
it is ignored.

``--dump-dir DIR``
   Directory holding the snapshots. Required.
``--sort distance|sgf``
   Station order: ``distance`` (nearest first, default) or ``sgf`` (station
   goodness of fit, highest first). Stations that did not contribute are always
   listed last.
``--max-stations N``
   Plot at most ``N`` contributing stations, taken in ``--sort`` order. ``0``
   (default) means no limit. A dense event can have more stations than fit
   comfortably in one figure; combine ``--max-stations`` with ``--sort sgf``
   to keep, say, the 15 best- or worst-fitting stations. The footer reports
   how many were hidden.
``--context SECONDS``
   Seconds of observed envelope shown after the end of the correlation window
   (default: 20).
``--out FILE``
   Write the PNG here instead of a temporary file.
``--no-show``
   Only write and print the PNG path; do not open a viewer. For reports or
   headless machines.
``--viewer CMD``
   Image viewer command (default: ``xdg-open``). Split on spaces; the PNG path
   is appended.

Exit codes: ``0`` success, ``2`` bad arguments, ``3`` no snapshot for that
origin, ``4`` snapshot could not be parsed, ``5`` rendering failed (for example
``python3-matplotlib`` is missing).


Reading the plot
----------------

Stations are stacked vertically in the order chosen with ``--sort``. Every
station block uses a common x-axis of seconds since the origin time and shows:

* **observed** — the buffered real-time envelope (solid dark line);
* **predicted** — the envelope predicted for this station, in its own units on a
  secondary axis (dashed grey line). Only its shape is compared with the observed
  envelope; the amplitude side of the SGF is a separate check of the observed
  peak against the scaled predicted peak (GMM PGV times station amplification)
  and is not drawn;
* the **correlation window** used for that station's SGF, drawn as a shaded
  band, and the **P** and **S** travel times as dotted vertical lines.

The header line of a station block reads ``NET.STA.LOC   Δ <km> (Rhyp <km>)
SGF <value>   AmpFit <value>   Corr <value>   MaxObs <value>   MaxPred <value>
StaAmp <factor>   PGV <value>``, where ``Δ`` is the epicentral distance, ``Rhyp``
the hypocentral distance that selected the predicted-envelope and PGV bins,
``Corr`` is the shape correlation ``C``, ``AmpFit`` the amplitude-fit term
``A``, ``MaxObs`` / ``MaxPred`` the observed and scaled predicted peaks in the
window, and ``SGF = sqrt(Corr * AmpFit)`` (the paper's per-station ``G`` is
``100 * SGF``; see `Method`_). The resolved predicted-envelope file path is
printed dim underneath it.

Every station's window starts at the same ``t0`` — the closest station's
predicted P arrival minus :confval:`preArrivalTimeWindow`, clamped to the origin
time and truncated to the second — so on a nearer station the shaded band opens
well before that station's own P. It ends at the earliest of
``ttS * postArrivalTimeShare``, the predicted envelope length and the end of the
buffered data. When :confval:`minimumCorrelationWindow` is set, a station whose
window comes out shorter than that is dropped from the OGF (skip reason
*correlation window too short*). ``--context`` sets how many seconds of observed
envelope are shown after the window; the envelope before ``t0`` is always shown.

Three header lines sit above the axes:

#. the origin publicID, the best-fitting magnitude type and value, and the
   origin's latitude, longitude and depth;
#. the overall OGF, the station search radius (cutoff distance) that applied for
   the best-fitting magnitude value (see :confval:`distancePerMagnitude`), the
   contributing-station count (out of the associated stations, even when
   ``--max-stations`` limits how many are drawn), the ground-motion region whose
   polygon contains the origin (``—`` if the origin is outside every region, in
   which case no PGV is available and stations are skipped) and the common
   correlation-window start ``t0``;
#. the stations that were associated but did not contribute, counted by reason
   (no soil class, no prediction, outside the cutoff distance for that
   magnitude, empty buffer, no PGV, no predicted P arrival, predicted P not yet
   arrived, correlation window too short, non-finite fit) — omitted when every
   associated station contributed.

The footer carries the generator name and notes when contributing stations were
hidden by ``--max-stations``.


Launching from scolv
--------------------

*scolv* runs the configured command as ``<command> <originID> <eventID>``. The
snapshot directory is not known to *scolv*, so ``--dump-dir`` must be part of the
command string. ``@ROOTDIR@`` and the other path variables are expanded in the
whole string. Both wiring options are on the Location tab.

**Command menu action** (recommended) — any number of entries under a **Run...**
button next to *Relocate*, with a process manager showing the output:

.. code-block:: properties

   olv.commandMenuAction.ogfPlot.enable      = true
   olv.commandMenuAction.ogfPlot.text        = "OGF envelope plot"
   olv.commandMenuAction.ogfPlot.command     = "scogfplot --dump-dir /path/scogf/debug"
   olv.commandMenuAction.ogfPlot.showProcess = true

**Custom button** — *scolv* also allows up to two fixed buttons on the Location
tab (:confval:`button0` / :confval:`button1` with :confval:`scripts.script0` /
:confval:`scripts.script1`), launched with the same ``<originID> <eventID>``
arguments:

.. code-block:: properties

   button0         = "OGF envelope plot"
   scripts.script0 = @ROOTDIR@/bin/scogfplot --dump-dir @ROOTDIR@/var/lib/scogf/debug

Use a custom button to have the plot one click away rather than behind the
**Run...** menu; the command menu action is otherwise the better choice
(unlimited entries, progress feedback).

In both cases the origin selected in *scolv* must be one that *scogf* has
processed. A not-yet-committed relocation has a temporary publicID with no
snapshot, and *scogfplot* then exits with code 3.


.. _scogf-references:

References
==========

* Jozinović, D., Clinton, J., Massin, F., Böse, M., and Cauzzi, C. (2024).
  Realtime Selection of Optimal Source Parameters Using Ground Motion
  Envelopes. *Seismica*, 3(1). doi:`10.26443/seismica.v3i1.1142
  <https://doi.org/10.26443/seismica.v3i1.1142>`_
* Cua, G. (2005). *Creating the Virtual Seismologist: developments in ground
  motion characterization and seismic early warning.* PhD thesis, California
  Institute of Technology.
* Cauzzi, C., Edwards, B., Fäh, D., Clinton, J., Wiemer, S., Kästli, P., Cua,
  G., and Giardini, D. (2015). New predictive equations and site amplification
  estimates for the next-generation Swiss ShakeMaps. *Geophysical Journal
  International*, 200(1), 421–438. doi:`10.1093/gji/ggu404
  <https://doi.org/10.1093/gji/ggu404>`_
