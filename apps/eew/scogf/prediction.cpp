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


#include <seiscomp/core/strings.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sstream>

#include "prediction.h"


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

	if ( !fs::exists(p) ) {
		throw runtime_error(source + ": path does not exist");
	}

	if ( !fs::exists(p / "envelopes") ) {
		throw runtime_error(source + "/envelope: path does not exist");
	}

	for ( const auto &dirEntry : fs::directory_iterator{p / "envelopes"} ) {
		_soilClasses.push_back(dirEntry.path().filename());
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

		auto &binding = _bindings[cols[0]];
		binding.soilClass = cols[1];
		binding.amplification = values[2];
	}

	ifs.close();
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<





