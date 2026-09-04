*scogf* computes, for each incoming origin, an Origin Goodness of Fit (OGF): a
measure of how well the observed real-time ground-motion envelopes match envelope
templates predicted for the origin's location, depth and magnitude. The result is
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
#. scores each station by how well the observed envelope matches the predicted
   template in shape (a correlation coefficient) and in peak amplitude against
   the GMPE PGV and averages the per-station scores into the overall OGF.

Optionally an envelope magnitude ``Menv`` is derived as the magnitude whose
templates best fit the observations, see :confval:`envelopeMagnitude.enable`.

The predicted templates, GMPE PGV coefficients and per-station soil/amplification
bindings are read from :confval:`predictionArchivePath`.


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
best-fitting magnitude: the raw template, the observed envelope, and the SGF,
PGV, amplification and correlation-window values.

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
``--style compact|spec``
   ``compact`` (default) overlays the observed envelope and the predicted
   template in one axes per station (the template on a secondary axis, since
   only its shape matters); ``spec`` draws them in two stacked panels per
   station instead.
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
* **template** — the predicted envelope, in its own units on a secondary axis
  (dashed grey line). Only its shape is compared with the observed envelope; the
  amplitude side of the SGF is a separate check of the observed peak against the
  GMPE PGV and is not drawn;
* the **correlation window** used for that station's SGF, drawn as a shaded
  band, and the **P** and **S** travel times as dotted vertical lines.

With ``--style spec`` the observed envelope and the template are drawn in two
stacked panels per station instead of overlaid; the shaded band and the arrival
lines are repeated on each panel.

The header line of a station block reads ``NET.STA.LOC   Δ <km>   SGF <value>
ampFit <value>   corr <value>   maxObs <value>   maxPred <value>   StaAmp
<factor>   PGV <value>``, where ``corr`` is the shape (Pearson) correlation,
``ampFit`` the amplitude-fit term, ``maxObs`` / ``maxPred`` the observed and
GMPE-scaled predicted peaks in the window, and
``SGF = sqrt(corr * ampFit)``. The resolved template file path is printed dim
underneath it.

The correlation window normally starts at the P arrival time truncated to the
second, and ends at the earliest of ``ttS * postArrivalTimeShare``, the template
length and the end of the buffered data. When
:confval:`minimumCorrelationWindow` is set, a station whose window comes out
shorter than that is dropped from the OGF (skip reason *correlation window too
short*). ``--context`` sets how many seconds of observed envelope are shown
after the window; envelope before the window (for example between origin time
and P) is always shown.

The figure title carries the origin publicID, the best-fitting magnitude type
and value, the overall OGF and the contributing-station count (all of them, even
when ``--max-stations`` limits how many are drawn). A second line underneath
gives the origin's latitude, longitude, depth and the station search radius
(cutoff distance) that applied for the best-fitting magnitude value (see
:confval:`distancePerMagnitude`). The footer summarises the stations that were
associated but did not contribute, counted by reason (no template, outside the
cutoff distance for that magnitude, empty buffer, no PGV, correlation window
too short, non-finite fit), and notes when contributing stations were hidden by
``--max-stations``.


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
