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
#define SEISCOMP_COMPONENT eew/ogf/table


#include <seiscomp/unittest/unittests.h>
#include <seiscomp/logging/log.h>
#include <iostream>

#include "../association.h"


namespace sc = Seiscomp::Core;
namespace dm = Seiscomp::DataModel;
namespace bu = boost::unit_test;


using namespace EEW::OGF;


//<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
BOOST_AUTO_TEST_SUITE(sedeew_ogf_table)
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>




//<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
BOOST_AUTO_TEST_CASE(assoc) {
	AssociationTable table;
	dm::OriginPtr org1 = dm::Origin::Create();
	dm::OriginPtr org2 = dm::Origin::Create();
	dm::OriginPtr org3 = dm::Origin::Create();

	Association *assoc;

	assoc = table.insert("AB.STA1", org1.get());
	assoc->ttP = 1.1;
	assoc->ttS = 1.2;
	assoc->correlation = 1.1;
	BOOST_CHECK_EQUAL(table.count("AB.STA1"), 1);

	assoc = table.insert("AB.STA2", org1.get());
	assoc->ttP = 2.1;
	assoc->ttS = 2.2;
	assoc->correlation = 2.1;
	BOOST_CHECK_EQUAL(table.count("AB.STA2"), 1);

	assoc = table.insert("AB.STA3", org1.get());
	assoc->ttP = 3.1;
	assoc->ttS = 3.2;
	assoc->correlation = 3.1;
	BOOST_CHECK_EQUAL(table.count("AB.STA3"), 1);
	BOOST_CHECK_EQUAL(table.count(org1.get()), 3);

	assoc = table.insert("AB.STA1", org2.get());
	assoc->ttP = 1.1;
	assoc->ttS = 1.2;
	assoc->correlation = 1.2;
	BOOST_CHECK_EQUAL(table.count("AB.STA1"), 2);

	assoc = table.insert("AB.STA3", org2.get());
	assoc->ttP = 3.1;
	assoc->ttS = 3.2;
	assoc->correlation = 3.2;
	BOOST_CHECK_EQUAL(table.count("AB.STA3"), 2);

	assoc = table.insert("AB.STA2", org3.get());
	assoc->ttP = 2.1;
	assoc->ttS = 2.2;
	assoc->correlation = 2.3;
	BOOST_CHECK_EQUAL(table.count("AB.STA2"), 2);

	assoc = table.insert("AB.STA3", org3.get());
	assoc->ttP = 3.1;
	assoc->ttS = 3.2;
	assoc->correlation = 3.3;
	BOOST_CHECK_EQUAL(table.count("AB.STA3"), 3);

	table.setDirty("AB.STA2");

	size_t cnt = 0;
	for ( auto &[sid, org] : table.origins("AB.STA2") ) {
		BOOST_CHECK_EQUAL(sid, "AB.STA2");
		++cnt;
	}
	BOOST_CHECK_EQUAL(cnt, 2);

	cnt = 0;
	for ( auto &[org, sid] : table.sensors(org3.get()) ) {
		++cnt;
	}
	BOOST_CHECK_EQUAL(cnt, 2);

	table.setDirty("AB.STA2");

	for ( auto &[key, assoc] : table.assocs() ) {
		auto eval = table.get(key.first);
		BOOST_CHECK(eval);

		if ( key.second == "AB.STA2" ) {
			BOOST_CHECK(assoc.dirty);
			BOOST_CHECK(eval->dirty);
		}
		else {
			BOOST_CHECK(!assoc.dirty);
			if ( !table.isAssociated(key.first, "AB.STA2") ) {
				BOOST_CHECK(!eval->dirty);
			}
			else {
				BOOST_CHECK(eval->dirty);
			}
		}
	}

	std::cerr << table;
}
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>




//<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
BOOST_AUTO_TEST_SUITE_END()
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
