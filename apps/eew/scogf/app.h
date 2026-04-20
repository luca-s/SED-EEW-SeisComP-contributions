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


#ifndef SEDEEW_OGF_APP_H
#define SEDEEW_OGF_APP_H


#include <seiscomp/client/application.h>
#include <seiscomp/core/timewindow.h>
#include <seiscomp/datamodel/origin.h>
#include <seiscomp/datamodel/publicobjectcache.h>
#include <seiscomp/geo/featureset.h>
#include <seiscomp/seismology/ttt.h>
#include <seiscomp/utils/stringfirewall.h>
#include <memory>

#include "association.h"
#include "circular.h"
#include "prediction.h"


namespace EEW::OGF {


class App : public Seiscomp::Client::Application {
	// ----------------------------------------------------------------------
	//  X'truction
	// ----------------------------------------------------------------------
	public:
		//! C'tor
		App(int argc, char **argv);


	// ----------------------------------------------------------------------
	//  Application interface
	// ----------------------------------------------------------------------
	public:
		bool init() override;

		void handleTimeout() override;
		void handleMessage(Seiscomp::Core::Message *msg) override;

		void addObject(const std::string &parentID, Seiscomp::DataModel::Object*) override;
		void removeObject(const std::string &parentID, Seiscomp::DataModel::Object*) override;
		void updateObject(const std::string &parentID, Seiscomp::DataModel::Object*) override;


	// ----------------------------------------------------------------------
	//  Private types and methods
	// ----------------------------------------------------------------------
	private:
		struct Envelope {
			Seiscomp::Core::Time timestamp;
			double               value;
		};

		class EnvelopeBuffer : public circular_buffer<Envelope> {
			public:
				using circular_buffer<Envelope>::circular_buffer;

			public:
				double lat;
				double lon;
				double elev;
				bool   dirty;
		};

		Association *addAssociation(Seiscomp::DataModel::Origin *org,
		                            const std::string &sid, const EnvelopeBuffer &buffer);
		void addAssociations(const std::string &sid, const EnvelopeBuffer &buffer);
		void addAssociations(Seiscomp::DataModel::Origin *org);
		void process(Seiscomp::DataModel::Origin *org, Evaluation &eval);


	// ----------------------------------------------------------------------
	//  Private members
	// ----------------------------------------------------------------------
	private:
		using EnvelopeBuffers = std::unordered_map<std::string, std::unique_ptr<EnvelopeBuffer>>;
		using Cache = Seiscomp::DataModel::PublicObjectTimeSpanBuffer;
		using Firewall = Seiscomp::Util::WildcardStringFirewall;

		struct Settings : AbstractSettings {
			void accept(SettingsLinker &linker) override {
				linker
				& cfg(cacheSize, "ogf.cacheSize")
				& cfg(maximumDistance, "ogf.maximumDistance")
				& cfg(updateInterval, "ogf.updateInverval")
				& cfg(preArrivalTimeWindow, "ogf.preArrivalTimeWindow")
				& cfg(postArrivalTimeShare, "ogf.postArrivalTimeShare")
				& cfg(predictionArchivePath, "ogf.predictionArchivePath")
				& cfg(zoneFile, "ogf.zoneFile")
				& cfg(envelopes, "envelopes")
				& cfg(stations, "stations")
				;
			}

			struct {
				void accept(SettingsLinker &linker) {
					linker
					& cfg(bufferSize, "bufferSize")
					& cfg(maxDelay, "maxDelay")
					;
				}

				size_t                   bufferSize{1800};
				Seiscomp::Core::TimeSpan maxDelay{20,0};
			}                        envelopes;

			struct {
				void accept(SettingsLinker &linker) {
					linker
					& cfg(include, "include")
					& cfg(exclude, "exclude")
					& cfg(defaultSoilClass, "defaultSoilClass")
					;
				}

				std::vector<std::string> include;
				std::vector<std::string> exclude;
				std::string              defaultSoilClass;
			}                        stations;

			Seiscomp::Core::TimeSpan cacheSize{1800, 0};
			size_t                   updateInterval{1};
			double                   maximumDistance{5};
			double                   preArrivalTimeWindow{0};
			double                   postArrivalTimeShare{150};
			std::string              predictionArchivePath{"@DATADIR@/scogf"};
			std::string              zoneFile;
		} _settings;

		Cache                        _cache;
		EnvelopeBuffers              _envelopeBuffers;
		AssociationTable             _associationTable;
		Firewall                     _streamFirewall;
		Prediction                   _prediction;
		Seiscomp::Geo::GeoFeatureSet _zones;
		Seiscomp::TravelTimeTable    _ttt;
};


}


#endif
