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


#define SEISCOMP_COMPONENT eew/scogf/prediction

#include <seiscomp/logging/log.h>
#include <seiscomp/core/strings.h>
#include <fstream>
#include <stdexcept>
#include <sstream>

#include "prediction.h"
#include "npy.h"


using namespace std;
using namespace Seiscomp::Core;
namespace fs = filesystem;


// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
namespace EEW::OGF {
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
Prediction::Prediction() {}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
Prediction::~Prediction() {}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void Prediction::setSource(const std::string &source) {
	fs::path p(source);

	_soilClasses.clear();
	_zoneNames.clear();
	_zones.clear();
	_bindings.clear();
	_gmm.clear();
	_envlp.clear();

	if ( !fs::exists(p) ) {
		throw runtime_error(source + ": path does not exist");
	}

	// scan envelopes directory structure
	_envelopePath = p / "envelopes";
	if ( !fs::exists(_envelopePath) || !fs::is_directory(_envelopePath) ) {
		throw runtime_error(source + "/envelope: path does not exist");
	}

	auto tryParseDouble = [](const std::string& str, double& value) -> bool {
		std::stringstream ss(str);
		return (ss >> value) && ss.eof();
	};

	// Iterate through Level 1: Soil Class (e.g., "R")
	for ( const auto &soilDir : fs::directory_iterator{p / "envelopes"} ) {
		if ( !soilDir.is_directory() ) continue;
		std::string soilClass = soilDir.path().filename().string();
		_soilClasses.push_back(soilClass);

		// Iterate through Level 2: Magnitude (e.g., "1", "1.1", "1.2")
		for ( const auto &magDir : fs::directory_iterator{soilDir.path()} ) {
			if ( !magDir.is_directory() ) continue;

			double magnitude = 0.0;
			if ( !tryParseDouble(magDir.path().filename().string(), magnitude) ) {
				SEISCOMP_DEBUG("Envelopes: skip invalid magnitude %s", magDir.path().string());
				continue;
			}

			// Iterate through Level 3: Distance (e.g., "2", "3", "4"...)
			for ( const auto &distDir : fs::directory_iterator{magDir.path()} ) {
				if ( !distDir.is_directory() ) continue;

				double distance = 0.0;
				if ( !tryParseDouble(distDir.path().filename().string(), distance) ) {
					SEISCOMP_DEBUG("Envelopes: skip invalid distance %s", distDir.path().string());
					continue;
				}

				// Check if the target file 'V_H.npy' exists inside this folder
				fs::path targetFile = distDir.path() / "V_H.npy";
				if ( fs::exists(targetFile) && fs::is_regular_file(targetFile) ) {

					// Store the full path of the file
					_envlp[soilClass][magnitude][distance] = fs::absolute(targetFile).string();
				}
			}
		}
	}

	auto cnt = _zones.readFile(p / "GMMpolygon.bna", nullptr);
	if ( cnt < 0 ) {
		throw runtime_error("GMMpolygon.bna: invalid zone file name");
	}

	if ( !cnt ) {
		throw runtime_error("GMMpolygon.bna: no zones read");
	}

	for ( const auto &zone : _zones.features() ) {
		_zoneNames.push_back(zone->name());
	}

	// Read and parse GMM data
	ifstream ifs;
	ifs.open(p / "GMM.csv");
	if ( !ifs ) {
		throw runtime_error("GMM.csv: file not found but required");
	}

	string line;
	vector<string> header;
	vector<double> values;
	size_t lineNumber{0};

	bool first = true;
	while ( getline(ifs, line) ) {
		++lineNumber;

		trim(line);
		if ( line.empty() ) {
			continue;
		}
		if ( line[0] == '#' ) {
			// Allow comments with leading hashes
			continue;
		}

		vector<string> cols;
		split(cols, line, ",", false);

		if ( first ) {
			first = false;

			if ( cols.size() != (_zoneNames.size() + 2) ) {
				throw runtime_error(stringify("GMM.csv:%d: expected %d columns, got %d",
				                              lineNumber, _zoneNames.size() + 2, cols.size()));
			}

			for ( auto &col : cols ) {
				header.push_back(trim(col));
			}

			if ( header[0] != "Magnitude" ) {
				throw runtime_error("GMM.csv: first header must be 'Magnitude'");
			}

			if ( header[1] != "Distance" ) {
				throw runtime_error("GMM.csv: second header must be 'Distance'");
			}

			for ( size_t i = 2; i < header.size(); ++i ) {
				if ( find(_zoneNames.begin(), _zoneNames.end(), header[i]) == _zoneNames.end() ) {
					throw runtime_error(
						stringify(
							"GMM.csv: zone name header at column %d does not match the available zones: '%s' not in ['%s']",
							2 + i, header[i], join(_zoneNames, "', '")
						)
					);
				}
			}

			values.resize(header.size());

			continue;
		}

		if ( cols.size() != header.size() ) {
			throw runtime_error(stringify("GMM.csv:%d: expected %d columns, got %d",
			                              lineNumber, header.size(), cols.size()));
		}

		for ( size_t i = 0; i < cols.size(); ++i ) {
			if ( !fromString(values[i], cols[i]) ) {
				throw runtime_error(stringify("GMM.csv:%d: invalid value at column %d: %s",
				                              lineNumber, i, cols[i]));
			}
		}

		for ( size_t i = 0; i < _zoneNames.size(); ++i ) {
			_gmm[_zoneNames[i]][values[0]][values[1]] = values[2 +i];
		}
	}

	// Done reading GMM.csv
	ifs.close();

	// Open station-config.csv
	ifs.open(p / "station-config.csv");
	if ( !ifs ) {
		throw runtime_error("station-config.csv: file not found but required");
	}

	header.clear();
	lineNumber = 0;
	first = true;

	while ( getline(ifs, line) ) {
		++lineNumber;

		trim(line);
		if ( line.empty() ) {
			continue;
		}
		if ( line[0] == '#' ) {
			// Allow comments with leading hashes
			continue;
		}

		vector<string> cols;
		split(cols, line, ",", false);

		if ( first ) {
			first = false;

			if ( cols.size() != 3 ) {
				throw runtime_error(stringify("station-config.csv:%d: expected 3 columns, got %d",
				                              lineNumber, cols.size()));
			}

			for ( auto &col : cols ) {
				header.push_back(trim(col));
			}

			if ( header[0] != "NSLC" ) {
				throw runtime_error("station-config.csv: first header must be 'NSLC'");
			}

			if ( header[1] != "Soil Class" ) {
				throw runtime_error("station-config.csv: second header must be 'Soil Class'");
			}

			if ( header[2] != "Amplification Factor" ) {
				throw runtime_error("station-config.csv: third header must be 'Amplification Factor'");
			}

			values.resize(header.size());

			continue;
		}

		if ( cols.size() != header.size() ) {
			throw runtime_error(stringify("station-config.csv:%d: expected %d columns, got %d",
			                              lineNumber, header.size(), cols.size()));
		}

		for ( size_t i = 2; i < cols.size(); ++i ) {
			if ( !fromString(values[i], cols[i]) ) {
				throw runtime_error(stringify("station-config.csv:%d: invalid value at column %d: %s",
				                              lineNumber, i, cols[i]));
			}
		}

		vector<string> toks;
		if ( split(toks, cols[0], ".", false) != 4 ) {
			throw runtime_error(stringify("station-config.csv:%d: expected 4 NSLC tokens, got %d",
			                              lineNumber, toks.size()));
		}

		cols[0] = toks[0] + "." + toks[1] + "." + toks[2];

		auto &binding = _bindings[cols[0]];
		binding.soilClass = cols[1];
		binding.amplification = values[2];
	}

	ifs.close();
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void Prediction::setDefaultSoilClass(const string &defaultSoilClass) {
	_defaultSoilClass = defaultSoilClass;
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
Seiscomp::Array *Prediction::trace(const std::string &soilClass, double mag, double dist) {

	auto it = _envlp.find(soilClass);
	if ( it == _envlp.end() ) {
		SEISCOMP_WARNING("No soil class (%s) envelopes found", soilClass);
		return nullptr;
	}

	const auto &magnitudes = it->second;
	auto mit = magnitudes.lower_bound(mag);
	if ( mit == magnitudes.end() ||
	    (mit == magnitudes.begin() && mit->first > mag) ) {
		// mag is smaller or greater than all keys
		SEISCOMP_DEBUG("No prediction (%s) available for magnitude %f", soilClass, mag);
		return nullptr;
	}
	else if ( mit != magnitudes.begin() ) {
		// find the closest mag between the greater and smaller neighbours
		auto prev = mit;
		--prev;
		if ( (mit->first - mag) > (mag - prev->first) ) {
			mit = prev;
		}
	}

	const auto &distances = mit->second;
	auto dit = distances.lower_bound(dist);
	if ( dit == distances.end()  ||
	    (dit == distances.begin() && dit->first > dist) ) { 
		// dist is smaller or greater than all keys
		SEISCOMP_DEBUG("No prediction (%s) available for distance %f (mag %f)",
		               soilClass, dist, mag);
		return nullptr;
	}
	else if ( dit != distances.begin() ) {
		// find the closest distance between the greater and smaller neighbours
		auto prev = dit;
		--prev;
		if ( (dit->first - dist) > (dist - prev->first) ) {
			dit = prev;
		}
	}

	//SEISCOMP_DEBUG("Trying to read prediction %s", dit->second);

	// TODO: Option to implement a cache in the future.
	return loadNpy(dit->second);
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
Seiscomp::Array *Prediction::get(const string &streamID, double mag, double dist) {

	//SEISCOMP_DEBUG("Load prediction for %s mag %f dist %f", streamID, mag, dist);

	auto it = _bindings.find(streamID);
	if ( it == _bindings.end() ) {
		if ( _defaultSoilClass.empty() ) {
			return nullptr;
		}

		return trace(_defaultSoilClass, mag, dist);
	}
	else {
		if ( it->second.soilClass.empty() && _defaultSoilClass.empty() ) {
			return nullptr;
		}

		return trace(it->second.soilClass, mag, dist);
	}
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
double Prediction::pgv(const Seiscomp::DataModel::Origin *org,
                       double mag, double dist) const {
	for ( const auto &f : _zones.features() ) {
		if ( f->contains({ org->latitude().value(), org->longitude().value() }) ) {

			auto it = _gmm.find(f->name());
			if ( it == _gmm.end() ) {
				SEISCOMP_WARNING("%s: no zone (%s) predictions found",
				                 org->publicID(), f->name());
				break;
			}

			const auto &magnitudes = it->second;
			auto mit = magnitudes.lower_bound(mag);
			if ( mit == magnitudes.end() ||
			    (mit == magnitudes.begin() && mit->first > mag) ) {
				// mag is smaller or greater than all keys
				SEISCOMP_DEBUG("No pgv(%s) available for magnitude %f", f->name(), mag);
				continue;
			}
			else if ( mit != magnitudes.begin() ) {
				// find the closest mag between the greater and smaller neighbours
				auto prev = mit;
				--prev;
				if ( (mit->first - mag) > (mag - prev->first) ) {
					mit = prev;
				}
			}

			const auto &distances = mit->second;
			auto dit = distances.lower_bound(dist);
			if ( dit == distances.end()  ||
			    (dit == distances.begin() && dit->first > dist) ) {
				// dist is smaller or greater than all keys
				SEISCOMP_DEBUG("No pgv(%s) available for distance %f (mag %f)",
				                f->name(), dist, mag);
				continue;
			}
			else if ( dit != distances.begin() ) {
				// find the closest distance between the greater and smaller neighbours
				auto prev = dit;
				--prev;
				if ( (dit->first - dist) > (dist - prev->first) ) {
					dit = prev;
				}
			}

			//SEISCOMP_DEBUG("%s: pgv: %s, %f (%f), %f (%f) = %f", org->publicID(),
			//               f->name(), mit->first, mag, dit->first, dist, dit->second);
			return dit->second;
		}
	}

	throw runtime_error("no pgv for origin");
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
double Prediction::amplification(const std::string &streamID) const {
	auto it = _bindings.find(streamID);
	if ( it == _bindings.end() ) {
		return 1.0;
	}

	return it->second.amplification;
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<





