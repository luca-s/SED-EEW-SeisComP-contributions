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


#define SEISCOMP_COMPONENT eew/scogf

#include <seiscomp/logging/log.h>
#include <seiscomp/core/datamessage.h>
#include <seiscomp/core/strings.h>
#include <seiscomp/client/inventory.h>
#include <seiscomp/datamodel/utils.h>
#include <seiscomp/datamodel/vs/envelope.h>
#include <seiscomp/datamodel/vs/envelopechannel.h>
#include <seiscomp/datamodel/vs/envelopevalue.h>
#include <seiscomp/math/geo.h>
#include <seiscomp/utils/misc.h>

#include <filesystem>

#include "app.h"


using namespace std;
using namespace Seiscomp;
using namespace Seiscomp::DataModel;


namespace EEW::OGF {
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
namespace {


template<typename T, typename... Args>
std::string join(const std::string &link, T head, Args... args) {
	return Core::toString(head) + (... + (link + Core::toString(args)));
}


}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
App::App(int argc, char **argv) : Application(argc, argv) {
	// Subscribe to envelopes
	addMessagingSubscription("AMPLITUDE");
	addMessagingSubscription("LOCATION");
	addMessagingSubscription("MAGNITUDE");
	bindSettings(&_settings);
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
bool App::init() {
	if ( !Application::init() ) {
		return false;
	}

	_settings.predictionArchivePath = Environment::Instance()->absolutePath(_settings.predictionArchivePath);
	try {
		_prediction.setSource(_settings.predictionArchivePath);
	}
	catch ( exception &e ) {
		SEISCOMP_ERROR("Predictions: %s", e.what());
		return false;
	}

	if ( !_settings.zoneFile.empty() ) {
		if ( _zones.readFile(_settings.zoneFile, nullptr) < 0 ) {
			SEISCOMP_ERROR("%s: failed to read zones", _settings.zoneFile);
			return false;
		}
	}

	SEISCOMP_DEBUG("Available envelope soil classes: %s", Core::join(_prediction.soilClasses(), ", "));
	SEISCOMP_DEBUG("Available gmpe zones: %s", Core::join(_prediction.zones(), ", "));
	SEISCOMP_DEBUG("Configured zones: %d", _zones.features().size());

	_cache.setDatabaseArchive(query());
	_cache.setTimeSpan(_settings.cacheSize);
	_cache.setPopCallback([this](PublicObject *obj) {
		if ( Origin::Cast(obj) ) {
			// Remove all station -> origin associations
			_associationTable.remove(static_cast<Origin*>(obj));
		}
	});
	_streamFirewall.allow = Firewall::StringSet(_settings.stations.include.begin(), _settings.stations.include.end());
	_streamFirewall.deny = Firewall::StringSet(_settings.stations.exclude.begin(), _settings.stations.exclude.end());
	enableTimer(_settings.updateInterval);

	return true;
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void App::handleTimeout() {
	Core::Time now = Core::Time::UTC();

	auto origins = _associationTable.origins();

	// Iteration must be done this way because the origins map is subject
	// to modifications when an origin is removed from the cache.
	for ( auto it = origins.begin(); it != origins.end(); ) {
		auto *org = it->first;
		auto &eval = it->second;
		++it;

		if ( eval.eol <= now ) {
			_cache.remove(org);
			continue;
		}

		if ( !eval.dirty) {
			// Nothing to do
			continue;
		}

		process(org, eval);
		eval.dirty = false;
	}
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void App::handleMessage(Seiscomp::Core::Message *msg) {
	// This causes callbacks (addObject, updateObject) to be called
	// when messages arrive
	Application::handleMessage(msg);

	auto dm = Core::DataMessage::Cast(msg);
	if ( !dm ) {
		return;
	}

	for ( auto obj : *dm ) {
		auto vsenv = VS::Envelope::Cast(obj);
		if ( !vsenv ) {
			continue;
		}

		for ( size_t ci = 0; ci < vsenv->envelopeChannelCount(); ++ci ) {
			auto chan = vsenv->envelopeChannel(ci);
			if ( chan->name() != "H" ) {
				// Only take combined horizontals into account
				continue;
			}

			auto sid = join(".", chan->waveformID().networkCode(), chan->waveformID().stationCode(), chan->waveformID().locationCode());
			if ( _streamFirewall.isDenied(sid) ) {
				continue;
			}

			for ( size_t vi = 0; vi < chan->envelopeValueCount(); ++vi ) {
				auto value = chan->envelopeValue(vi);
				if ( value->type() != "vel" ) {
					// Velocity channels are required
					continue;
				 }

				EnvelopeBuffer *buffer;

				auto it = _envelopeBuffers.find(sid);
				if ( it == _envelopeBuffers.end() ) {
					auto inv = Client::Inventory::Instance();
					auto sloc = inv->getSensorLocation(chan->waveformID().networkCode(),
					                                   chan->waveformID().stationCode(),
					                                   chan->waveformID().locationCode(),
					                                   vsenv->timestamp());
					if ( !sloc ) {
						SEISCOMP_WARNING("%s: no inventory information", sid);
						break;
					}

					try {
						auto loc = DataModel::getLocation(sloc);
						double elev = 0;
						try {
							elev = sloc->elevation();
						}
						catch ( ... ) {
							try {
								elev = sloc->station()->elevation();
							}
							catch ( ... ) {}
						}

						buffer = new EnvelopeBuffer(_settings.envelopes.bufferSize);
						buffer->lat = loc.lat;
						buffer->lon = loc.lon;
						buffer->elev = elev;

						addAssociations(sid, *buffer);

						_envelopeBuffers[sid].reset(buffer);
					}
					catch( exception &e ) {
						SEISCOMP_WARNING("%s: %s", sid, e.what());
						break;
					}
				}
				else {
					buffer = it->second.get();
				}

				buffer->push_back({
					vsenv->timestamp(),
					value->value()
				});
				buffer->dirty = true;
				break;
			}
		}
	}
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void App::addObject(const std::string &, Seiscomp::DataModel::Object *obj) {
	auto org = DataModel::Origin::Cast(obj);
	if ( org ) {
		addAssociations(org);
	}
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void App::removeObject(const std::string &, Seiscomp::DataModel::Object *obj) {
	auto org = DataModel::Origin::Cast(obj);
	if ( org ) {
		_cache.remove(org);
	}
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void App::updateObject(const std::string &, Seiscomp::DataModel::Object *obj) {
	//
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
Association *App::addAssociation(Seiscomp::DataModel::Origin *org,
                                 const std::string &sid, const EnvelopeBuffer &buffer) {
	double dist;
	Math::Geo::delazi(org->latitude().value(), org->longitude().value(),
	                  buffer.lat, buffer.lon, &dist);
	if ( dist >= _settings.maximumDistance ) {
		return nullptr;
	}

	double depth;
	try {
		depth = org->depth().value();
	}
	catch ( ... ) {
		return nullptr;
	}

	auto assoc = _associationTable.insert(sid, org);

	auto ttimes = _ttt.compute(org->latitude().value(), org->longitude().value(),
	                           depth,
	                           buffer.lat, buffer.lon, buffer.elev);
	if ( !ttimes ) {
		return nullptr;
	}

	// Travel times are assumed to be sorted by time
	for ( const auto &tt : *ttimes ) {
		auto ph = Util::getShortPhaseName(tt.phase);
		if ( (assoc->ttP < 0) && (ph == 'P') ) {
			assoc->ttP = tt.time;
		}

		if ( (assoc->ttS < 0) && (ph == 'S') ) {
			assoc->ttS = tt.time;
		}
	}

	auto duration = Core::TimeSpan(max(assoc->ttP, assoc->ttS) * _settings.postArrivalTimeShare * 0.01);
	assoc->endTime = org->time().value() + duration;

	delete ttimes;
	return assoc;
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void App::addAssociations(const std::string &sid, const EnvelopeBuffer &buffer) {
	for ( auto &[org, eval] : _associationTable.origins() ) {
		if ( addAssociation(org, sid, buffer) ) {
			eval.dirty = true;
		}
	}
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void App::addAssociations(Seiscomp::DataModel::Origin *org) {
	auto *eval = _associationTable.insert(org);
	eval->eol = org->time().value() + _settings.envelopes.maxDelay;

	double maxTravelTime = 0;

	for ( auto &[sid, buffer] : _envelopeBuffers ) {
		auto assoc = addAssociation(org, sid, *buffer);
		if ( !assoc ) {
			continue;
		}
		maxTravelTime = max(maxTravelTime, max(assoc->ttP, assoc->ttS));
	}

	// TODO: Compute maximum travel time for all stations / sensorlocation
	//       which are not yet registered (via envelope buffers).

	// Update the end-of-lifetime timestamp according to the maximum
	// expected traveltime scaled by postArrivalTimeShare.
	eval->eol += Core::TimeSpan(maxTravelTime * _settings.postArrivalTimeShare * 0.01);
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void App::process(Seiscomp::DataModel::Origin *org, Evaluation &eval) {
	// TODO: Do computations
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
