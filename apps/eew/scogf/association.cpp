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


#define SEISCOMP_COMPONENT sedeew/ogf/assocation
#include <seiscomp/logging/log.h>
#include "association.h"


// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
namespace EEW::OGF {
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
std::ostream &operator<<(std::ostream &os, const Association &assoc) {
	os << assoc.ttP << ", " << assoc.ttS << ", " << assoc.correlation;
	if ( assoc.dirty ) {
		os << " (D)";
	}
	return os;
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
std::ostream &operator<<(std::ostream &os, const AssociationTable &table) {
	for ( auto it : table._assoc ) {
		os << "| " << it.first.first << " | " << it.first.second << " | " << it.second << '\n';
	}
	return os;
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
Evaluation *AssociationTable::insert(ObjectType org) {
	auto it = _eval.find(org);
	if ( it == _eval.end() ) {
		auto *eval = &_eval[org];
		return eval;
	}
	else {
		return &it->second;
	}
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
Association *AssociationTable::insert(
	const std::string &sid, ObjectType org
) {
	auto it = _assoc.find({ org, sid });
	if ( it != _assoc.end() ) {
		return &it->second;
	}

	insert(org);

	_key1Index.insert({ org, sid });
	_key2Index.insert({ sid, org });
	return &_assoc[{ org, sid }];
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
size_t AssociationTable::remove(ObjectType org) {
	size_t cnt = 0;
	for ( auto it = _assoc.begin(); it != _assoc.end(); ) {
		if ( it->first.first == org ) {
			it = _assoc.erase(it);
			++cnt;
		}
		else {
			++it;
		}
	}

	auto it = _eval.find(org);
	if ( it != _eval.end() ) {
		_eval.erase(it);
	}

	// Update the index tables mapping sid:org and org:sid.
	auto itp1 = _key1Index.equal_range(org);
	for ( auto it1 = itp1.first; it1 != itp1.second; ++it1 ) {
		auto itp2 = _key2Index.equal_range(it1->second);
		for ( auto it2 = itp2.first; it2 != itp2.second; ++it2 ) {
			if ( it2->second == org ) {
				_key2Index.erase(it2);
				break;
			}
		}
	}
	_key1Index.erase(itp1.first, itp1.second);

	return cnt;
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void AssociationTable::setDirty(const std::string &sid) {
	for ( auto it : origins(sid) ) {
		auto eit = _eval.find(it.second);
		if ( eit == _eval.end() ) {
			SEISCOMP_WARNING("This should never happen: eval "
			                 "not available after index1 lookup");
		}
		else {
			eit->second.dirty = true;
		}

		auto ait = _assoc.find({ it.second, sid });
		if ( ait == _assoc.end() ) {
			SEISCOMP_WARNING("This should never happen: table primary key "
			                 "not available after index1 lookup");
		}
		else {
			ait->second.dirty = true;
		}
	}
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
Evaluation *AssociationTable::get(ObjectType org) {
	auto it = _eval.find(org);
	if ( it == _eval.end() ) {
		return nullptr;
	}

	return &it->second;
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
