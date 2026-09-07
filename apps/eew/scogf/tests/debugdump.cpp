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
 *   Author: Jan Becker, gempa GmbH <jabe@gempa.de>                           *
 *                                                                            *
 ******************************************************************************/


#define SEISCOMP_TEST_MODULE SeisComP
#define SEISCOMP_COMPONENT eew/ogf/debugdump


#include <seiscomp/unittest/unittests.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "../debugdump.h"


namespace bu = boost::unit_test;
namespace fs = std::filesystem;

using namespace EEW::OGF;


namespace {

fs::path scratchDir() {
	auto p = fs::temp_directory_path() / "scogf_debugdump_test";
	std::error_code ec;
	fs::remove_all(p, ec);
	fs::create_directories(p, ec);
	return p;
}

OriginSnapshot sampleSnapshot() {
	OriginSnapshot snap;
	snap.publicID = "Origin/20260902110401.123456.42";
	snap.time = "2026-09-02T11:03:58.4Z";
	snap.author = "scautoloc@proc";
	snap.latitude = 46.21;
	snap.longitude = 7.34;
	snap.depth = 8.0;
	snap.ogf = 71.3;
	snap.minimumStations = 1;
	snap.cutoffDistanceKm = 55.6;
	snap.magID = "Magnitude/20260902110405.1.7";
	snap.magType = "MVS";
	snap.magValue = 4.18;

	StationEval a;
	a.sid = "CH.SENIN.";
	a.distanceKm = 12.4;
	a.used = true;
	a.soilClass = "R";
	a.predictedPath = "/data/scogf/envelopes/R/4.2/12/V_H.npy";
	a.ttP = 3.1;
	a.ttS = 5.4;
	a.pgv = 0.0042;
	a.amplification = 1.2;
	a.predMax = 7.1e-4;
	a.scale = 0.71;
	a.windowStart = 3;
	a.windowEnd = 8;
	a.maxObs = 0.0039;
	a.maxPred = 0.0044;
	a.amplitudeFit = 0.987;
	a.correlation = 0.71;
	a.sgf = 0.83;
	a.rawPredictedT0 = 0.0;
	a.rawPredicted = { 0.0, 1.0e-4, 5.0e-4, 7.1e-4, 3.0e-4, 1.0e-4 };
	a.observedT0 = -2.0;
	a.observed = { 0.0, 0.0, 1.0e-4, 6.0e-4, 3.9e-3, 2.0e-3, 5.0e-4 };
	snap.stations.push_back(a);

	StationEval b;
	b.sid = "CH.MMK.";
	b.distanceKm = 41.0;
	b.used = false;
	b.skipReason = "no prediction";
	snap.stations.push_back(b);

	return snap;
}

}


//<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
BOOST_AUTO_TEST_SUITE(sedeew_ogf_debugdump)
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>


//<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
BOOST_AUTO_TEST_CASE(slug) {
	BOOST_CHECK_EQUAL(slugify("Origin/2026.42:xy"), "Origin_2026.42_xy");
	BOOST_CHECK_EQUAL(slugify("smi:ch.ethz.sed/origin/NLL.foo"),
	                  "smi_ch.ethz.sed_origin_NLL.foo");
	BOOST_CHECK_EQUAL(slugify("plain-123_ok.json"), "plain-123_ok.json");
	BOOST_CHECK_EQUAL(slugify(""), "origin");
}
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>


//<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
BOOST_AUTO_TEST_CASE(json_content) {
	auto snap = sampleSnapshot();
	auto js = toJSON(snap);

	// Balanced braces/brackets - a cheap well-formedness screen.
	int braces = 0, brackets = 0;
	bool inStr = false, esc = false;
	for ( char c : js ) {
		if ( inStr ) {
			if ( esc ) esc = false;
			else if ( c == '\\' ) esc = true;
			else if ( c == '"' ) inStr = false;
			continue;
		}
		if ( c == '"' ) inStr = true;
		else if ( c == '{' ) ++braces;
		else if ( c == '}' ) --braces;
		else if ( c == '[' ) ++brackets;
		else if ( c == ']' ) --brackets;
	}
	BOOST_CHECK_EQUAL(braces, 0);
	BOOST_CHECK_EQUAL(brackets, 0);
	BOOST_CHECK(!inStr);

	BOOST_CHECK(js.find("\"schemaVersion\": 1") != std::string::npos);
	BOOST_CHECK(js.find("\"ogf\": 71.3") != std::string::npos);
	BOOST_CHECK(js.find("\"cutoffDistanceKm\": 55.6") != std::string::npos);
	BOOST_CHECK(js.find("\"type\": \"MVS\"") != std::string::npos);
	BOOST_CHECK(js.find("\"sid\": \"CH.SENIN.\"") != std::string::npos);
	BOOST_CHECK(js.find("\"skipReason\": \"no prediction\"") != std::string::npos);
	BOOST_CHECK(js.find("\"sgf\": 0.83") != std::string::npos);
	BOOST_CHECK(js.find("\"predictedPath\": ") != std::string::npos);
	BOOST_CHECK(js.find("\"rawPredicted\": ") != std::string::npos);
	// The skipped station carries no series.
	auto mmk = js.find("CH.MMK.");
	BOOST_REQUIRE(mmk != std::string::npos);
	BOOST_CHECK(js.find("series", mmk) == std::string::npos);
}
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>


//<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
BOOST_AUTO_TEST_CASE(write_atomic) {
	auto dir = scratchDir();
	auto snap = sampleSnapshot();

	BOOST_REQUIRE(writeSnapshotFile(dir.string(), snap));

	auto target = dir / "Origin_20260902110401.123456.42.json";
	BOOST_CHECK(fs::exists(target));
	BOOST_CHECK(fs::file_size(target) > 0);

	// No temp leftover.
	size_t tmpCount = 0;
	for ( const auto &e : fs::directory_iterator(dir) ) {
		if ( e.path().extension() == ".tmp" ) ++tmpCount;
	}
	BOOST_CHECK_EQUAL(tmpCount, 0u);

	// Overwrite in place keeps a single file.
	snap.ogf = 42.0;
	BOOST_REQUIRE(writeSnapshotFile(dir.string(), snap));
	size_t jsonCount = 0;
	for ( const auto &e : fs::directory_iterator(dir) ) {
		if ( e.path().extension() == ".json" ) ++jsonCount;
	}
	BOOST_CHECK_EQUAL(jsonCount, 1u);

	std::ifstream ifs(target);
	std::stringstream ss;
	ss << ifs.rdbuf();
	BOOST_CHECK(ss.str().find("\"ogf\": 42") != std::string::npos);

	fs::remove_all(dir);
}
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>


//<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
BOOST_AUTO_TEST_CASE(prune) {
	auto dir = scratchDir();

	auto touchFile = [&](const std::string &name, int ageDays) {
		auto p = dir / name;
		std::ofstream(p) << "{}";
		if ( ageDays > 0 ) {
			fs::last_write_time(
				p, fs::file_time_type::clock::now() - std::chrono::hours(24 * ageDays));
		}
	};

	for ( int i = 0; i < 6; ++i ) {
		touchFile("recent_" + std::to_string(i) + ".json", i);       // 0..5 days old
	}
	touchFile("old_a.json", 30);
	touchFile("old_b.json", 40);
	touchFile("stale.json.tmp", 2);

	// Age cap only: 14 days, no count cap.
	pruneSnapshots(dir.string(), 14, 0);

	BOOST_CHECK(!fs::exists(dir / "old_a.json"));
	BOOST_CHECK(!fs::exists(dir / "old_b.json"));
	BOOST_CHECK(!fs::exists(dir / "stale.json.tmp"));
	size_t remaining = 0;
	for ( const auto &e : fs::directory_iterator(dir) ) {
		if ( e.path().extension() == ".json" ) ++remaining;
	}
	BOOST_CHECK_EQUAL(remaining, 6u);

	// Count cap: keep the 3 newest.
	pruneSnapshots(dir.string(), 0, 3);
	remaining = 0;
	for ( const auto &e : fs::directory_iterator(dir) ) {
		if ( e.path().extension() == ".json" ) ++remaining;
	}
	BOOST_CHECK_EQUAL(remaining, 3u);
	BOOST_CHECK(fs::exists(dir / "recent_0.json"));   // newest
	BOOST_CHECK(!fs::exists(dir / "recent_5.json"));  // oldest of the batch

	fs::remove_all(dir);
}
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>


//<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
BOOST_AUTO_TEST_SUITE_END()
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
