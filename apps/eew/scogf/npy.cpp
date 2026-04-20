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


#include <seiscomp/core/endianess.h>
#include <seiscomp/core/strings.h>
#include <fstream>
#include <stdexcept>

#include "npy.h"


using namespace std;
using namespace Seiscomp;


namespace EEW::OGF {


Seiscomp::Array *loadNpy(const string &filename) {
	ifstream ifs(filename);
	if ( !ifs.is_open() ) {
		throw runtime_error(filename + " not found");
	}

	char buffer[16];
	if ( !ifs.read(buffer, 10) ) {
		throw runtime_error("failed to read npy magic");
	}

	if ( memcmp(buffer, "\x93NUMPY", 6) ) {
		throw runtime_error("wrong npy magic");
	}

	uint8_t vMajor, vMinor;
	vMajor = buffer[6];
	vMinor = buffer[7];

	if ( (vMajor != 1) || (vMinor != 0) ) {
		throw runtime_error("wrong npy version, only 1.0 is supported");
	}

	uint16_t headerSize = *reinterpret_cast<uint16_t*>(buffer + 8);
	headerSize = Core::Endianess::Converter::ToLittleEndian(headerSize);
	string header;
	header.resize(headerSize);
	if ( !ifs.read(header.data(), headerSize) ) {
		throw runtime_error("failed to read npy header");
	}

	headerSize += 10;
	headerSize = (headerSize + 63) / 64 * 64;

	string_view headerView = header;
	auto p0 = headerView.find("'fortran_order':");
	if ( p0 == string::npos ) {
		throw runtime_error("failed to find npy header keyword 'fortran_order'");
	}

	p0 += 16;
	auto p1 = headerView.find(',', p0);
	if ( p1 == string::npos ) {
		throw runtime_error("failed to parse npy header value 'fortran_order'");
	}

	auto fortranOrder = Core::trim(headerView.substr(p0, p1 - p0)) == "True" ? true : false;

	p0 = headerView.find("'shape':");
	if ( p0 == string::npos ) {
		throw runtime_error("failed to find npy header keyword 'shape'");
	}
	p0 += 8;
	p1 = headerView.find(')', p0);
	if ( p1 == string::npos ) {
		throw runtime_error("failed to parse npy header value 'shape'");
	}

	auto shape = Core::trim(headerView.substr(p0, p1 - p0));
	if ( shape.empty() ) {
		throw runtime_error("invalid npy header value 'shape'");
	}
	if ( shape[0] == '(' ) {
		shape = shape.substr(1);
	}

	if ( shape.empty() ) {
		throw runtime_error("invalid npy header value 'shape'");
	}

	if ( shape.back() == ',' ) {
		shape = shape.substr(0, shape.size() - 1);
	}

	vector<size_t> dimensions;
	if ( !Core::fromString(dimensions, shape, ',') ) {
		throw runtime_error("invalid npy header value 'shape'");
	}

	if ( dimensions.size() != 1 ) {
		throw runtime_error("invalid npy dimensions, only 1-dimensional arrays supported");
	}

	p0 = headerView.find("'descr':");
	if ( p0 == string::npos ) {
		throw runtime_error("failed to find npy header keyword 'descr'");
	}
	p0 += 8;
	p1 = headerView.find(',', p0);
	if ( p1 == string::npos ) {
		throw runtime_error("failed to parse npy header value 'descr'");
	}

	auto desc = Core::trim(headerView.substr(p0, p1 - p0));
	if ( desc.empty() ) {
		throw runtime_error("invalid npy header value 'descr'");
	}
	if ( desc.front() == '\'' ) {
		desc = desc.substr(1);
	}
	if ( desc.back() == '\'' ) {
		desc = desc.substr(0, desc.size() - 1);
	}
	if ( desc.size() < 3 ) {
		throw runtime_error("invalid npy header value 'descr'");
	}

	bool littleEndian = desc[0] == '<' || desc[0] == '|' ? true : false;

	Array *data;

	if ( desc[1] == 'f' ) {
		// Floating point
		if ( desc[2] == '4' ) {
			// float
			data = new FloatArray(dimensions[0]);
		}
		else if ( desc[2] == '8' ) {
			// double
			data = new DoubleArray(dimensions[0]);
		}
		else {
			throw runtime_error("invalid npy format floating point size value in 'descr' header");
		}
	}
	else if ( desc[1] == 'i' ) {
		if ( desc[2] == '4' ) {
			// int32
			data = new Int32Array(dimensions[0]);
		}
		else {
			throw runtime_error("invalid npy format integer size value in 'descr' header");
		}
	}
	else {
		throw runtime_error("invalid npy format type value in 'descr' header");
	}

	// Jump to data offset
	ifs.seekg(headerSize, ios::beg);

	auto blob = reinterpret_cast<char*>(const_cast<void*>(data->data()));
	if ( !ifs.read(blob, data->size() * data->elementSize()) ) {
		delete data;
		throw runtime_error("failed to read npy array data");
	}

	if ( littleEndian ) {
		if ( data->elementSize() == 4 ) {
			Core::Endianess::Swapper<int32_t, Core::Endianess::Current::BigEndian, 4>::Take(reinterpret_cast<int32_t*>(blob), data->size());
		}
		else if ( data->elementSize() == 8 ) {
			Core::Endianess::Swapper<int64_t, Core::Endianess::Current::BigEndian, 8>::Take(reinterpret_cast<int64_t*>(blob), data->size());
		}
	}
	else {
		if ( data->elementSize() == 4 ) {
			Core::Endianess::Swapper<int32_t, Core::Endianess::Current::LittleEndian, 4>::Take(reinterpret_cast<int32_t*>(blob), data->size());
		}
		else if ( data->elementSize() == 8 ) {
			Core::Endianess::Swapper<int64_t, Core::Endianess::Current::LittleEndian, 8>::Take(reinterpret_cast<int64_t*>(blob), data->size());
		}
	}

	return data;
}


}
