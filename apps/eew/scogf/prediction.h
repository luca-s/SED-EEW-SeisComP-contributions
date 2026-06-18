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
#include <seiscomp/datamodel/origin.h>
#include <seiscomp/geo/featureset.h>

#include <filesystem>
#include <map>
#include <vector>


namespace EEW::OGF {


/**
 * @brief The Prediction class returns predicted GMPE PGV values and envelope templates.
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
		 * @param source The path to the archive containing the three folders:
		 *               amplifications, envelope and gmpe
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
		 * @brief Returns the available gmpe zones.
		 * @return A list of zone names.
		 */
		const std::vector<std::string> &zones() const;

		/**
		 * @brief Returns the available soil classes of the envelope templates.
		 * @return A list of soil class names.
		 */
		const std::vector<std::string> &soilClasses() const;

		/**
		 * @brief Returns the predicted trace.
		 * @param soilClass The soil class.
		 * @param mag The magnitude.
		 * @param dist The distance in kilometers.
		 * @return The data array of the trace.
		 */
		Seiscomp::Array *trace(const std::string &soilClass, double mag, double dist);

		/**
		 * @brief Returns the predicted trace for a streamID.
		 * This method resolves the channel bindings to get the corresponding
		 * soil class and calls trace().
		 * @param streamID The NSLC streamID.
		 * @param mag The magnitude.
		 * @param dist The distance in kilometers.
		 * @return The data array of the trace.
		 */
		Seiscomp::Array *get(const std::string &streamID, double mag, double dist);

		/**
		 * @brief Returns the predicted PGV.
		 * This method throws an exception if no pgv can be looked up.
		 * @param org The origin.
		 * @param mag The magnitude.
		 * @param dist The distance in kilometers.
		 * @return The PGV value.
		 */
		double pgv(const Seiscomp::DataModel::Origin *org, double mag, double dist) const;

		double amplification(const std::string &streamID) const;


	// ----------------------------------------------------------------------
	//  Private members
	// ----------------------------------------------------------------------
	private:
		using DistanceMap = std::map<double, double>;
		using MagnitudeMap = std::map<double, DistanceMap>;
		using GMM = std::map<std::string, MagnitudeMap>;

		struct ChannelBinding {
			std::string soilClass;
			double      amplification;
		};
		using ChannelBindings = std::map<std::string, ChannelBinding>;

		std::filesystem::path        _envelopePath;
		std::string                  _defaultSoilClass;
		std::vector<std::string>     _zoneNames;
		std::vector<std::string>     _soilClasses;
		Seiscomp::Geo::GeoFeatureSet _zones;
		GMM                          _gmm;
		ChannelBindings              _bindings;
};


inline const std::vector<std::string> &Prediction::zones() const {
	return _zoneNames;
}

inline const std::vector<std::string> &Prediction::soilClasses() const {
	return _soilClasses;
}


}


#endif
