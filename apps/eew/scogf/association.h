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


#ifndef SEDEEW_OGF_ASSOCIATION_H
#define SEDEEW_OGF_ASSOCIATION_H


#include <seiscomp/datamodel/origin.h>
#include <unordered_map>
#include <string>


namespace EEW::OGF {


struct Evaluation {
	double               gof{-1}; //!< Current goodness of fit
	std::string          bestMagnitude; //!< The magnitude which caused the highest fit
	bool                 dirty{false}; //!< Dirty flag, e.g. association has changed
	Seiscomp::Core::Time eol; //!< Maximum time to live => end of lifetime
};


struct Association {
	double               dist{-1};
	double               ttP{-1}; //!< P travel time w.r.t. origin time
	double               ttS{-1}; //!< S travel time w.r.t. origin time
	Seiscomp::Core::Time endTime; //!< Envelope end time for correlation computation
	double               correlation{-1}; //!< Current correlation with envelope templates
	OPT(double)          lastMag; //!< Magnitude of last successful computation
};


class AssociationTable {
	// ----------------------------------------------------------------------
	//  Public types
	// ----------------------------------------------------------------------
	public:
		using ObjectType = Seiscomp::DataModel::Origin*;
		using Key = std::pair<ObjectType, std::string>;
		using Value = Association;
		using AssociationContainer = std::map<Key, Value>;
		using EvaluationContainer = std::map<ObjectType, Evaluation>;
		using Key1Index = std::unordered_multimap<ObjectType, std::string>;
		using Key2Index = std::unordered_multimap<std::string, ObjectType>;


	// ----------------------------------------------------------------------
	//  Public interface
	// ----------------------------------------------------------------------
	public:
		//! Return the number of associations for a particular sensor ID.
		size_t count(const std::string &sid) const;
		//! Return the number of associations for a particular origin.
		size_t count(ObjectType) const;

		//! Registers an origin and returns its evaluation object.
		Evaluation *insert(ObjectType);

		Association *insert(ObjectType, const std::string &);
		size_t remove(ObjectType);

		//! Returns if an association exists
		bool isAssociated(ObjectType, const std::string &) const;

		const Association *assoc(ObjectType, const std::string &) const;
		Association *assoc(ObjectType, const std::string &);

		//! Returns the evaluation for a particular origin.
		Evaluation *get(ObjectType);

		//! Sets all associations for a particular sensor ID dirty.
		void setDirty(const std::string &sid);

		template <typename T>
		struct RangeWrapper {
			using iterator = typename T::iterator;
			iterator &begin() { return itp.first; }
			iterator &end() { return itp.second; }
			std::pair<iterator, iterator> itp;
		};

		template <typename T>
		struct ConstRangeWrapper {
			using const_iterator = typename T::const_iterator;
			const const_iterator &begin() const { return itp.first; }
			const const_iterator &end() const { return itp.second; }
			std::pair<const_iterator, const_iterator> itp;
		};

		RangeWrapper<EvaluationContainer> origins();
		ConstRangeWrapper<EvaluationContainer> origins() const;
		ConstRangeWrapper<AssociationContainer> assocs() const;
		ConstRangeWrapper<Key1Index> sensors(ObjectType origin) const;
		ConstRangeWrapper<Key2Index> origins(const std::string &sid) const;


	// ----------------------------------------------------------------------
	//  Private members
	// ----------------------------------------------------------------------
	private:
		EvaluationContainer  _eval;
		AssociationContainer _assoc;
		Key1Index            _key1Index;
		Key2Index            _key2Index;


	friend std::ostream &operator<<(std::ostream &, const AssociationTable &);
};


inline size_t AssociationTable::count(ObjectType origin) const {
	return _key1Index.count(origin);
}

inline size_t AssociationTable::count(const std::string &sid) const {
	return _key2Index.count(sid);
}

inline bool AssociationTable::isAssociated(ObjectType org,
                                           const std::string &sid) const {
	return _assoc.find({ org, sid }) != _assoc.end();
}

inline const Association *AssociationTable::assoc(ObjectType org, const std::string &sid) const {
	auto it = _assoc.find({ org, sid });
	if ( it != _assoc.end() ) {
		return &it->second;
	}
	return nullptr;
}

inline Association *AssociationTable::assoc(ObjectType org, const std::string &sid) {
	auto it = _assoc.find({ org, sid });
	if ( it != _assoc.end() ) {
		return &it->second;
	}
	return nullptr;
}

inline AssociationTable::RangeWrapper<AssociationTable::EvaluationContainer>
AssociationTable::origins() {
	return { { _eval.begin(), _eval.end() } };
}

inline AssociationTable::ConstRangeWrapper<AssociationTable::EvaluationContainer>
AssociationTable::origins() const {
	return { { _eval.begin(), _eval.end() } };
}

inline AssociationTable::ConstRangeWrapper<AssociationTable::AssociationContainer>
AssociationTable::assocs() const {
	return { { _assoc.begin(), _assoc.end() } };
}

inline AssociationTable::ConstRangeWrapper<AssociationTable::Key1Index>
AssociationTable::sensors(ObjectType origin) const {
	return { _key1Index.equal_range(origin) };
}

inline AssociationTable::ConstRangeWrapper<AssociationTable::Key2Index>
AssociationTable::origins(const std::string &sid) const {
	return { _key2Index.equal_range(sid) };
}


}


#endif
