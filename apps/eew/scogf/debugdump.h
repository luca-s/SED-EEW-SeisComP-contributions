/******************************************************************************
 *     Copyright (C) by ETHZ/SED                                              *
 *                                                                            *
 *   This program is free software: you can redistribute it and/or modify     *
 *   it under the terms of the GNU Affero General Public License as published *
 *   by the Free Software Foundation, either version 3 of the License, or     *
 *   (at your option) any later version.                                      *
 *                                                                            *
 *   This program is distributed in the hope that it will be useful,          *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of           *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            *
 *   GNU Affero General Public License for more details.                      *
 *                                                                            *
 *   -----------------------------------------------------------------------  *
 *                                                                            *
 *   Per-origin debug snapshot of the OGF computation. One JSON file per       *
 *   origin, overwritten in place, consumed by the 'scogf-plot' tool.          *
 *                                                                            *
 ******************************************************************************/


#ifndef SEDEEW_OGF_DEBUGDUMP_H
#define SEDEEW_OGF_DEBUGDUMP_H


#include <string>
#include <vector>


namespace EEW::OGF {


/**
 * @brief Per-station detail of one OGF computation, captured for the magnitude
 *        that produced the best overall fit.
 *
 * All times are seconds relative to the origin time. Series are sampled at 1 Hz
 * (the envelope interval assumed throughout scogf). The scaled template shown in
 * the plot is rawTemplate[i] * scale, at time rawTemplateT0 + i.
 */
struct StationEval {
	std::string sid;                   //!< NET.STA.LOC
	double      distanceKm{-1};
	bool        used{false};           //!< contributed to the OGF mean
	std::string skipReason;            //!< populated when !used

	std::string soilClass;             //!< resolved envelope soil class
	std::string templatePath;          //!< resolved V_H.npy template

	double      ttP{-1};
	double      ttS{-1};
	double      pgv{0};
	double      amplification{1};

	double      predMax{0};            //!< max of the raw prediction template
	double      scale{0};              //!< pgv / predMax * amplification

	int         windowStart{0};        //!< correlation window start [s]
	int         windowEnd{0};          //!< correlation window end [s]

	double      maxObs{0};             //!< max of the observed envelope
	double      maxPred{0};            //!< max of prediction in corr win, scaled
	double      amplitudeFit{0};
	double      correlation{0};        //!< Pearson coefficient, clamped >= 0
	double      sgf{0};                //!< station goodness of fit

	double              rawTemplateT0{0};
	std::vector<double> rawTemplate;

	double              observedT0{0};
	std::vector<double> observed;
};


/**
 * @brief Everything needed to reproduce the OGF plot for one origin.
 */
struct OriginSnapshot {
	std::string publicID;
	std::string time;                  //!< ISO origin time
	std::string author;
	double      latitude{0};
	double      longitude{0};
	double      depth{0};

	double      ogf{-1};               //!< final overall goodness of fit
	size_t      minimumStations{0};
	double      cutoffDistanceKm{-1};  //!< station search radius for magValue

	std::string magID;                 //!< publicID of the best magnitude
	std::string magType;
	double      magValue{0};

	std::vector<StationEval> stations;
};


/**
 * @brief Replaces every character outside [A-Za-z0-9._-] with '_' so an origin
 *        publicID can be used as a file name.
 */
std::string slugify(const std::string &s);

/**
 * @brief Serializes a snapshot to a JSON document.
 */
std::string toJSON(const OriginSnapshot &snap);

/**
 * @brief Writes toJSON(snap) to <dir>/<slug(publicID)>.json atomically
 *        (temp file + rename). Returns false on any I/O error.
 */
bool writeSnapshotFile(const std::string &dir, const OriginSnapshot &snap);

/**
 * @brief Deletes snapshot files older than keepDays and, beyond that, the
 *        oldest files above maxFiles. A value <= 0 disables the respective cap.
 *        Also removes stale *.tmp leftovers. Cheap enough to call after a write;
 *        the caller is expected to rate-limit it.
 */
void pruneSnapshots(const std::string &dir, int keepDays, int maxFiles);


}


#endif
