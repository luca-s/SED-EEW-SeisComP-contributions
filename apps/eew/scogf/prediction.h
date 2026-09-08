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
 *   SeisComP EEW OGF module that computes the Goodness of Fit to envelope    *
 *   templates and populates origin comments to be evaluated by scevent       *
 *   later.                                                                   *
 *                                                                            *
 *   -----------------------------------------------------------------------  *
 *                                                                            *
 *   Author: Jan Becker, gempa GmbH <jabe@gempa.de>                           *
 *                                                                            *
 ******************************************************************************/


#ifndef SEDEEW_OGF_PREDICTION_H
#define SEDEEW_OGF_PREDICTION_H


#include <seiscomp/core/typedarray.h>
#include <seiscomp/geo/featureset.h>

#include <filesystem>
#include <map>
#include <vector>


namespace EEW::OGF {


/**
 * @brief The Prediction class returns predicted PGV values (from the Swiss
 *        ground-motion model, per region) and predicted envelopes.
 */
class Prediction {
	// ----------------------------------------------------------------------
	//  X'truction
	// ----------------------------------------------------------------------
	public:
		//! C'tor
		Prediction();
		//! D'tor
		~Prediction();


	// ----------------------------------------------------------------------
	//  Public interface
	// ----------------------------------------------------------------------
	public:
		/**
		 * @brief Sets the source of the archive and reads its metadata.
		 * This methods throws exceptions in case of an error.
		 * @param source Path to the archive: the envelopes/ directory, the
		 *               per-region PGV table GMM.csv with its region polygons
		 *               GMMpolygon.bna, and the per-station station-config.csv.
		 */
		void setSource(const std::string &source);

		/**
		 * @brief Sets the default soil class.
		 * If a trace for a streamID should be returned which is not part of the
		 * bindings or has an empty soil class, this default will be used
		 * instead.
		 * @param defaultSoilClass Soil class name.
		 */
		void setDefaultSoilClass(const std::string &defaultSoilClass);

		/**
		 * @brief Returns the available ground-motion region names.
		 * @return A list of region names.
		 */
		const std::vector<std::string> &regions() const;

		/**
		 * @brief Returns the available soil classes of the predicted envelopes.
		 * @return A list of soil class names.
		 */
		const std::vector<std::string> &soilClasses() const;

		/**
		 * @brief Returns whether two magnitude values are equal in this context.
		 * @param mag1 Magnitude value one.
		 * @param mag2 Magnitude value two.
		 * @return Equality flag.
		 */
		constexpr bool equal(double mag1, double mag2) const;

		/**
		 * @brief Returns the predicted trace.
		 * @param soilClass The soil class.
		 * @param mag The magnitude.
		 * @param dist The distance in kilometers.
		 * @return The data array of the trace.
		 */
		Seiscomp::Array *trace(const std::string &soilClass, double mag, double dist);

		/**
		 * @brief Resolves the soil class for a streamID: the binding's soil
		 * class, or the default soil class if there is no binding or the binding
		 * has an empty soil class. May be empty.
		 */
		std::string resolvedSoilClass(const std::string &streamID) const;

		/**
		 * @brief Returns the path of the predicted-envelope file that trace()
		 * would load for the given parameters, without loading it. Empty if none
		 * matches.
		 */
		std::string tracePath(const std::string &soilClass, double mag, double dist) const;

		/**
		 * @brief Returns the name of the ground-motion region whose polygon
		 * contains the given coordinate, or an empty string if none does. Pass
		 * the result to pgv().
		 */
		std::string regionName(double lat, double lon) const;

		/**
		 * @brief Returns the predicted PGV for a region (see regionName()) at the
		 * nearest magnitude and distance bin.
		 * This method throws an exception if the region is unknown or has no bin
		 * covering the given magnitude and distance.
		 * @param region The region name.
		 * @param mag The magnitude.
		 * @param dist The hypocentral distance in kilometers.
		 * @return The PGV value.
		 */
		double pgv(const std::string &region, double mag, double dist) const;

		/**
		 * @brief Returns the site amplification factor bound to a sensor
		 * location in the archive's station-config.csv, or 1.0 if the stream is
		 * not listed. The predicted envelope for a station is scaled by the
		 * GMM PGV times this factor.
		 * @param streamID The NET.STA.LOC stream ID.
		 */
		double amplification(const std::string &streamID) const;


	// ----------------------------------------------------------------------
	//  Private methods
	// ----------------------------------------------------------------------
	private:
		/**
		 * @brief Nearest-neighbour lookup of a predicted-envelope file in the
		 * envelope archive. Shared by trace() and tracePath(). Returns nullptr if
		 * the soil class is unknown or no magnitude/distance bin matches.
		 */
		const std::string *lookupTraceFile(const std::string &soilClass,
		                                   double mag, double dist) const;


	// ----------------------------------------------------------------------
	//  Private members
	// ----------------------------------------------------------------------
	private:
		using PgvDistanceMap = std::map<double, double>;
		using PgvMagnitudeMap = std::map<double, PgvDistanceMap>;
		using RegionPgvTable = std::map<std::string, PgvMagnitudeMap>;
		using EnvDistanceMap = std::map<double, std::string>;
		using EnvMagnitudeMap = std::map<double, EnvDistanceMap>;
		using Envelope = std::map<std::string, EnvMagnitudeMap>;

		struct ChannelBinding {
			std::string soilClass;
			double      amplification;
		};
		using ChannelBindings = std::map<std::string, ChannelBinding>;

		std::filesystem::path        _envelopePath;
		std::string                  _defaultSoilClass;
		std::vector<std::string>     _regionNames;
		std::vector<std::string>     _soilClasses;
		Seiscomp::Geo::GeoFeatureSet _regions;
		RegionPgvTable               _regionPgv;
		Envelope                     _envlp;
		ChannelBindings              _bindings;
};


inline const std::vector<std::string> &Prediction::regions() const {
	return _regionNames;
}

inline const std::vector<std::string> &Prediction::soilClasses() const {
	return _soilClasses;
}

inline constexpr bool Prediction::equal(double mag1, double mag2) const {
	return static_cast<int>(mag1 * 10) == static_cast<int>(mag2 * 10);
}

}


#endif
