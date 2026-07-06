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
#include <seiscomp/io/recordstream.h>
#include <seiscomp/seismology/ttt.h>
#include <seiscomp/utils/stringfirewall.h>

#include <condition_variable>
#include <memory>
#include <mutex>

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
		bool validateParameters() override;

		bool init() override;
		bool run() override;
		void exit(int exitCode) override;

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
				void append(const Envelope &env) {
					// TODO: Check overlaps and out-of-order
					push_back(env);
					++_appends;
				}

				size_t appended() const { return _appends; }

			public:
				double lat;
				double lon;
				double elev;
				bool   dirty;

			private:
				size_t _appends{0};
		};

		bool playback();
		Association *addAssociation(Seiscomp::DataModel::Origin *org,
		                            const std::string &sid, const EnvelopeBuffer &buffer);
		void addAssociations(const std::string &sid, const EnvelopeBuffer &buffer);
		void addAssociations(Seiscomp::DataModel::Origin *org);
		void process(Seiscomp::DataModel::Origin *org, Seiscomp::IO::RecordStream *rs);
		void process(Seiscomp::DataModel::Origin *org, Evaluation &eval);
		double compute(Seiscomp::DataModel::Origin *org, const Seiscomp::DataModel::Magnitude *mag);
		double compute(Seiscomp::DataModel::Origin *org, double mag, int *stationCount = nullptr);


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
				& cfg(cacheSize, "cacheSize")
				& cfg(maximumDistance, "maximumDistance")
				& cfg(minimumStations, "minimumStations")
				& cfg(envelopeMagnitude, "envelopeMagnitude")
				& cfg(updateInterval, "updateInverval")
				& cfg(preArrivalTimeWindow, "preArrivalTimeWindow")
				& cfg(postArrivalTimeShare, "postArrivalTimeShare")
				& cfg(predictionArchivePath, "predictionArchivePath")
				& cfg(commentID, "commentID")
				& cfg(commentMagID, "commentMagID")
				& cfg(envelopes, "envelopes")
				& cfg(sensorLocations, "sensorLocations")
				& cli(recordStreamURL, "Input", "record-url,I",
					"The RecordStream source URL. Format: [service://]location[#type]. "
					"'service' is the name of the RecordStream driver. If 'service' is "
					"not given, 'file://' is being used and the name of a miniSEED file "
					"can be given.\n"
					"This parameter is only required when reading pre-calculated envelope "
					"values in offline processing, see --ep and --origin-id."
				)
				& cliSwitch(test, "Messaging", "test", "Test mode, no messages are sent")
				& cli(epFile, "Input", "ep",
					"Name of input XML file (SCML) with all origin for offline "
					"processing.  Use '-' to read from stdin. The database connection "
					"is not received from messaging and must be provided. Results will "
					"be written as XML to stdout. Please note that envelope data must "
					"be provided with -I."
				)
				& cli(originID, "Input", "origin-id,O",
					"OriginID to be processed. ")
				& cliSwitch(formatted, "Output", "formatted,f",
					"Use formatted XML output. Otherwise XML is unformatted.")
				& cliSwitch(dump, "Mode", "dump",
					"Dump results as XML rather than sending messages."
				)
				& cliSwitch(playback, "Playback", "playback",
					"Enables playback of envelopes and event parameters. This option "
					"required -I and --ep to be set."
				)
				& cliSwitch(shiftTimes, "Playback", "real-time",
					"Enables shifting object times into real-time. This has only an "
					"effect in combination with --playback."
				)
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
			}                        sensorLocations;

			struct {
				void accept(SettingsLinker &linker) {
					linker
					& cfg(enable, "enable")
					& cfg(type, "type")
					& cfg(minimum, "minimum")
					& cfg(maximum, "maximum")
					& cfg(spacing, "spacing")
					;
				}

				bool                     enable{false};
				std::string              type{"Menv"};
				double                   minimum{3.5};
				double                   maximum{5.5};
				double                   spacing{0.5};
			}                        envelopeMagnitude;

			Seiscomp::Core::TimeSpan cacheSize{1800, 0};
			size_t                   updateInterval{1};
			double                   maximumDistance{5};
			size_t                   minimumStations{0};
			double                   preArrivalTimeWindow{0};
			double                   postArrivalTimeShare{1.5};
			std::string              predictionArchivePath{"@DATADIR@/scogf"};
			std::string              commentID{"eew.ogf.value"};
			std::string              commentMagID{"eew.ogf.mag"};
			std::string              recordStreamURL;
			bool                     test{false};
			std::string              epFile;
			bool                     formatted{false};
			std::string              originID;
			bool                     dump{false};
			bool                     playback{false};
			bool                     shiftTimes{false};
		} _settings;

		Cache                        _cache;
		EnvelopeBuffers              _envelopeBuffers;
		AssociationTable             _associationTable;
		Firewall                     _slocFirewall;
		Prediction                   _prediction;
		Seiscomp::TravelTimeTable    _ttt;

		std::mutex                   _mutexAlert;
		std::condition_variable      _signalAlert;
};


}


#endif
