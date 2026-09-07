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
 ******************************************************************************/


#define SEISCOMP_COMPONENT eew/scogf/debugdump

#include <seiscomp/logging/log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "debugdump.h"


using namespace std;
namespace fs = std::filesystem;


namespace EEW::OGF {
namespace {


string num(double v) {
	if ( !std::isfinite(v) ) {
		return "null";
	}
	char buf[32];
	snprintf(buf, sizeof(buf), "%.10g", v);
	return buf;
}


string jsonString(const string &s) {
	string o;
	o.reserve(s.size() + 2);
	o += '"';
	for ( unsigned char c : s ) {
		switch ( c ) {
			case '"':  o += "\\\""; break;
			case '\\': o += "\\\\"; break;
			case '\b': o += "\\b"; break;
			case '\f': o += "\\f"; break;
			case '\n': o += "\\n"; break;
			case '\r': o += "\\r"; break;
			case '\t': o += "\\t"; break;
			default:
				if ( c < 0x20 ) {
					char buf[8];
					snprintf(buf, sizeof(buf), "\\u%04x", c);
					o += buf;
				}
				else {
					o += static_cast<char>(c);
				}
		}
	}
	o += '"';
	return o;
}


string jsonSeries(double t0, const vector<double> &v) {
	string o = "{\"t0\":";
	o += num(t0);
	o += ",\"v\":[";
	for ( size_t i = 0; i < v.size(); ++i ) {
		if ( i ) {
			o += ',';
		}
		o += num(v[i]);
	}
	o += "]}";
	return o;
}


} // anonymous namespace




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
string slugify(const string &s) {
	string o = s;
	for ( char &c : o ) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		                (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
		if ( !ok ) {
			c = '_';
		}
	}
	if ( o.empty() ) {
		o = "origin";
	}
	return o;
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
string toJSON(const OriginSnapshot &snap) {
	ostringstream os;

	os << "{\n"
	   << "  \"schemaVersion\": 1,\n"
	   << "  \"generator\": \"scogf\",\n"
	   << "  \"origin\": {"
	   << "\"publicID\": " << jsonString(snap.publicID)
	   << ", \"time\": " << jsonString(snap.time)
	   << ", \"author\": " << jsonString(snap.author)
	   << ", \"latitude\": " << num(snap.latitude)
	   << ", \"longitude\": " << num(snap.longitude)
	   << ", \"depth\": " << num(snap.depth)
	   << "},\n"
	   << "  \"ogf\": " << num(snap.ogf) << ",\n"
	   << "  \"minimumStations\": " << snap.minimumStations << ",\n"
	   << "  \"cutoffDistanceKm\": " << num(snap.cutoffDistanceKm) << ",\n"
	   << "  \"bestMagnitude\": {"
	   << "\"publicID\": " << jsonString(snap.magID)
	   << ", \"type\": " << jsonString(snap.magType)
	   << ", \"value\": " << num(snap.magValue)
	   << "},\n"
	   << "  \"stations\": [";

	for ( size_t i = 0; i < snap.stations.size(); ++i ) {
		const auto &s = snap.stations[i];
		os << (i ? ",\n" : "\n") << "    {"
		   << "\"sid\": " << jsonString(s.sid)
		   << ", \"distanceKm\": " << num(s.distanceKm)
		   << ", \"used\": " << (s.used ? "true" : "false");

		if ( !s.skipReason.empty() ) {
			os << ", \"skipReason\": " << jsonString(s.skipReason);
		}

		if ( s.used ) {
			os << ", \"soilClass\": " << jsonString(s.soilClass)
			   << ", \"predictedPath\": " << jsonString(s.predictedPath)
			   << ", \"ttP\": " << num(s.ttP)
			   << ", \"ttS\": " << num(s.ttS)
			   << ", \"pgv\": " << num(s.pgv)
			   << ", \"amplification\": " << num(s.amplification)
			   << ", \"predMax\": " << num(s.predMax)
			   << ", \"scale\": " << num(s.scale)
			   << ", \"window\": {\"startSec\": " << s.windowStart
			   << ", \"endSec\": " << s.windowEnd << "}"
			   << ", \"maxObs\": " << num(s.maxObs)
			   << ", \"maxPred\": " << num(s.maxPred)
			   << ", \"amplitudeFit\": " << num(s.amplitudeFit)
			   << ", \"correlation\": " << num(s.correlation)
			   << ", \"sgf\": " << num(s.sgf)
			   << ", \"series\": {\"sampleRateHz\": 1.0"
			   << ", \"rawPredicted\": " << jsonSeries(s.rawPredictedT0, s.rawPredicted)
			   << ", \"observed\": " << jsonSeries(s.observedT0, s.observed)
			   << "}";
		}

		os << "}";
	}

	os << (snap.stations.empty() ? "" : "\n  ") << "]\n}\n";
	return os.str();
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
bool writeSnapshotFile(const string &dir, const OriginSnapshot &snap) {
	error_code ec;
	fs::create_directories(dir, ec);

	const fs::path target = fs::path(dir) / (slugify(snap.publicID) + ".json");
	const fs::path tmp = fs::path(dir) / (slugify(snap.publicID) + ".json.tmp");

	{
		ofstream ofs(tmp, ios::binary | ios::trunc);
		if ( !ofs ) {
			SEISCOMP_WARNING("scogf debug: cannot open %s for writing",
			                 tmp.string());
			return false;
		}
		ofs << toJSON(snap);
		ofs.flush();
		if ( !ofs ) {
			SEISCOMP_WARNING("scogf debug: write failed for %s", tmp.string());
			fs::remove(tmp, ec);
			return false;
		}
	}

	fs::rename(tmp, target, ec);
	if ( ec ) {
		SEISCOMP_WARNING("scogf debug: rename %s -> %s failed: %s",
		                 tmp.string(), target.string(), ec.message());
		fs::remove(tmp, ec);
		return false;
	}

	return true;
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void pruneSnapshots(const string &dir, int keepDays, int maxFiles) {
	namespace ch = std::chrono;
	error_code ec;

	const fs::path root(dir);
	if ( !fs::is_directory(root, ec) ) {
		return;
	}

	vector<pair<fs::file_time_type, fs::path>> files;

	for ( fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec) ) {
		if ( !it->is_regular_file(ec) ) {
			continue;
		}

		const string name = it->path().filename().string();
		const auto mtime = it->last_write_time(ec);
		if ( ec ) {
			ec.clear();
			continue;
		}

		// Sweep stale temp files (older than 1 h).
		if ( name.size() > 9 && name.compare(name.size() - 9, 9, ".json.tmp") == 0 ) {
			if ( fs::file_time_type::clock::now() - mtime > ch::hours(1) ) {
				fs::remove(it->path(), ec);
			}
			continue;
		}

		if ( name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0 ) {
			files.emplace_back(mtime, it->path());
		}
	}

	// Age cap.
	if ( keepDays > 0 ) {
		const auto cutoff = fs::file_time_type::clock::now() -
		                    ch::hours(24 * static_cast<long>(keepDays));
		vector<pair<fs::file_time_type, fs::path>> kept;
		kept.reserve(files.size());
		for ( auto &f : files ) {
			if ( f.first < cutoff ) {
				fs::remove(f.second, ec);
			}
			else {
				kept.push_back(std::move(f));
			}
		}
		files.swap(kept);
	}

	// Count cap: remove the oldest files above the ceiling.
	if ( maxFiles > 0 && files.size() > static_cast<size_t>(maxFiles) ) {
		std::sort(files.begin(), files.end(),
		          [](const auto &a, const auto &b) { return a.first < b.first; });
		const size_t excess = files.size() - static_cast<size_t>(maxFiles);
		for ( size_t i = 0; i < excess; ++i ) {
			fs::remove(files[i].second, ec);
		}
	}
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<


}
