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
		 * @brief Returns the available gmpe zones.
		 * @return A list of zone names.
		 */
		const std::vector<std::string> &zones() const;

		/**
		 * @brief Returns the available soil classes of the envelope templates.
		 * @return A list of soil class names.
		 */
		const std::vector<std::string> &soilClasses() const;


	// ----------------------------------------------------------------------
	//  Private members
	// ----------------------------------------------------------------------
	private:
		std::vector<std::string> _zones;
		std::vector<std::string> _soilClasses;
};


inline const std::vector<std::string> &Prediction::zones() const {
	return _zones;
}

inline const std::vector<std::string> &Prediction::soilClasses() const {
	return _soilClasses;
}


}


#endif
