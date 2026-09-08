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
#include <seiscomp/datamodel/eventparameters_package.h>
#include <seiscomp/datamodel/utils.h>
#include <seiscomp/datamodel/vs/envelope.h>
#include <seiscomp/datamodel/vs/envelopechannel.h>
#include <seiscomp/datamodel/vs/envelopevalue.h>
#include <seiscomp/datamodel/vs/vs.h>
#include <seiscomp/io/archive/xmlarchive.h>
#include <seiscomp/math/geo.h>
#include <seiscomp/math/mean.h>
#include <seiscomp/utils/misc.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <set>
#include <utility>

#include "app.h"


using namespace std;
using namespace Seiscomp;
using namespace Seiscomp::Core;
using namespace Seiscomp::DataModel;


namespace EEW::OGF {
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
namespace {


template<typename T, typename... Args>
string join(const string &link, T head, Args... args) {
	return toString(head) + (... + (link + toString(args)));
}

template <typename T>
void retouch(T &o, const Time &timestamp) {
	try {
		o.creationInfo();
	}
	catch ( ... ) {
		o.setCreationInfo(CreationInfo());
	}

	o.creationInfo().setCreationTime(timestamp);
	o.creationInfo().setModificationTime(None);
}

inline ostream &operator<<(ostream &os, const WaveformStreamID &wid) {
	os << wid.networkCode() << "." << wid.stationCode() << "."
	   << wid.locationCode() << "." << wid.channelCode();
	return os;
}


class PlaybackVisitor : public Visitor {
	public:
		using Storage = vector<pair<NotifierMessagePtr, Time>>;

	public:
		PlaybackVisitor(Storage &storage,
		                TimeSpan originDelay = {5, 0},
		                TimeSpan amplitudeDelay = {10, 0})
		: _storage(storage)
		, _originDelay(originDelay)
		, _amplitudeDelay(amplitudeDelay) {}

	public:
		bool visit(PublicObject *po) override {
			auto org = Origin::Cast(po);
			if ( org ) {
				try {
					auto ts = org->time().value() + _originDelay;
					NotifierMessagePtr nmsg = new NotifierMessage;
					nmsg->attach(new Notifier(org->parent()->publicID(), OP_ADD, org->clone()));
					_storage.push_back({ nmsg, ts });
				}
				catch ( exception &e ) {
					SEISCOMP_ERROR("%s: %s", org->publicID(), e.what());
					return true;
				}
				return true;
			}

			auto mag = Magnitude::Cast(po);
			if ( mag ) {
				_magnitudes.push_back(mag);
				return false;
			}

			auto stamag = StationMagnitude::Cast(po);
			if ( stamag ) {
				if ( stamag->amplitudeID().empty() ) {
					SEISCOMP_WARNING("%s: no amplitude referenced", stamag->publicID());
					return false;
				}

				auto amp = Amplitude::Find(stamag->amplitudeID());

				if ( !amp ) {
					SEISCOMP_WARNING("%s: referenced amplitude %s not found: ignoring",
					                 stamag->publicID(), stamag->amplitudeID());
					return false;
				}

				try {
					auto ts = amp->timeWindow().reference() + _amplitudeDelay;
					// NotifierMessagePtr nmsg = new NotifierMessage;
					// nmsg->attach(new Notifier(stamag->parent()->className(), OP_ADD, stamag->clone()));
					// _storage.push_back({ nmsg, ts });
					_stationMagnitudes[stamag->publicID()] = ts;
				}
				catch ( exception &e ) {
					SEISCOMP_WARNING("%s: %s", stamag->publicID(), e.what());
				}

				return false;
			}

			return false;
		}

		void visit(Object *o) override {
			return;
		}

		void finished() override {
			cerr << "Computing magnitude updates" << endl;
			for ( auto mag : _magnitudes ) {
				vector<pair<StationMagnitude*, Time>> stamags;
				for ( size_t i = 0; i < mag->stationMagnitudeContributionCount(); ++i ) {
					auto contrib = mag->stationMagnitudeContribution(i);
					auto it = _stationMagnitudes.find(contrib->stationMagnitudeID());
					if ( it == _stationMagnitudes.end() ) {
						continue;
					}
					auto smag = StationMagnitude::Find(contrib->stationMagnitudeID());
					if ( smag ) {
						stamags.push_back({ smag, it->second });
					}
				}
				if ( stamags.empty() ) {
					SEISCOMP_WARNING("%s: invalid magnitude: ignoring", mag->publicID());
					continue;
				}

				sort(stamags.begin(), stamags.end(), [](const auto &s1, const auto &s2) {
					return s1.second < s2.second;
				});

				cerr << "* " << mag->publicID() << " " << mag->type() << " " << stamags.size() << endl;

				double sum = 0.0;
				int count = 0;

				for ( auto &item : stamags ) {
					sum += item.first->magnitude().value();
					++count;

					auto newMag = static_cast<Magnitude*>(mag->clone());
					newMag->setMagnitude(RealQuantity(sum / count));
					newMag->setStationCount(count);
					newMag->setMethodID("mean");

					NotifierMessagePtr nmsg = new NotifierMessage;
					nmsg->attach(new Notifier(mag->parent()->publicID(), count < 2 ? OP_ADD : OP_UPDATE, newMag));
					_storage.push_back({ nmsg, item.second });

					cerr << "  * " << item.second << " " << newMag->stationCount()
					     << " " << newMag->magnitude().value()
					     << endl;
				}
			}

			_stationMagnitudes = {};
			_magnitudes = {};
		}

	private:
		Storage                                    &_storage;
		TimeSpan                                    _originDelay;
		TimeSpan                                    _amplitudeDelay;
		unordered_map<string, Time>                 _stationMagnitudes;
		vector<MagnitudePtr>                        _magnitudes;
};


}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
App::App(int argc, char **argv) : Application(argc, argv) {
	// Subscribe to envelopes
	setPrimaryMessagingGroup("LOCATION");
	addMessagingSubscription("AMPLITUDE");
	addMessagingSubscription("LOCATION");
	addMessagingSubscription("MAGNITUDE");
	setLoadStationsEnabled(true);
	bindSettings(&_settings);
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
bool App::validateParameters() {
	if ( !Client::Application::validateParameters() ) {
		return false;
	}

	if ( !_settings.epFile.empty() || !_settings.originID.empty() ) {
		if ( _settings.recordStreamURL.empty() ) {
			cerr << "Error: --record-url is required in combination with --ep or --origin-id" << endl;
			return false;
		}

		if ( (!_settings.playback && !_settings.epFile.empty()) || _settings.test ) {
			setMessagingEnabled(false);
		}

		if ( !isInventoryDatabaseEnabled() ) {
			setDatabaseEnabled(false, false);
		}
	}

	if ( _settings.commentID.empty() ) {
		cerr << "Error: commentID must not be empty" << endl;
		return false;
	}

	if ( _settings.playback ) {
		setLoadStationsEnabled(false);

		if ( _settings.recordStreamURL.empty() && _settings.epFile.empty() ) {
			cerr << "Error: --playback requires either -I or --ep or both" << endl;
			return false;
		}
	}

	_ttt = TravelTimeTableInterface::Create(_settings.tttType.c_str());
	if ( !_ttt ) {
		SEISCOMP_ERROR("Failed to create TravelTimeTableInterface '%s'",
		               _settings.tttType.c_str());
		return false;
	}

	if ( !_ttt->setModel(_settings.tttTable) ) {
		SEISCOMP_ERROR("Failed to set table %s for TravelTimeTableInterface '%s'",
		               _settings.tttTable.c_str(),
		               _settings.tttType.c_str());
		return false;
	}

	return true;
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

	_prediction.setDefaultSoilClass(_settings.sensorLocations.defaultSoilClass);

	SEISCOMP_DEBUG("Available envelope soil classes: %s", Core::join(_prediction.soilClasses(), ", "));
	SEISCOMP_DEBUG("Available gmpe zones: %s", Core::join(_prediction.zones(), ", "));

	if ( !_settings.debug.dumpPath.empty() ) {
		_settings.debug.dumpPath = Environment::Instance()->absolutePath(_settings.debug.dumpPath);
		try {
			filesystem::create_directories(_settings.debug.dumpPath);
		}
		catch ( exception &e ) {
			SEISCOMP_ERROR("debug.dumpPath: %s", e.what());
			return false;
		}
		SEISCOMP_INFO("OGF debug snapshots enabled: %s", _settings.debug.dumpPath);
	}

	_cache.setDatabaseArchive(query());
	_cache.setTimeSpan(_settings.cacheSize);
	_cache.setPopCallback([this](PublicObject *obj) {
		if ( Origin::Cast(obj) ) {
			// Remove all station -> origin associations
			_associationTable.remove(static_cast<Origin*>(obj));
		}
	});

	_slocFirewall.allow = Firewall::StringSet(
		_settings.sensorLocations.include.begin(), _settings.sensorLocations.include.end()
	);
	_slocFirewall.deny = Firewall::StringSet(
		_settings.sensorLocations.exclude.begin(), _settings.sensorLocations.exclude.end()
	);

	enableTimer(_settings.updateInterval);

	return true;
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
bool App::run() {
	if ( _settings.playback ) {
		return playback();
	}
	else if ( !_settings.epFile.empty() ) {
		Notifier::Disable();

		SEISCOMP_DEBUG("Reading envelopes from %s", _settings.recordStreamURL);

		IO::RecordStreamPtr rs = IO::RecordStream::Open(_settings.recordStreamURL.data());
		if ( !rs ) {
			SEISCOMP_ERROR("%s: failed to open recordstream", _settings.recordStreamURL);
			return false;
		}

		SEISCOMP_DEBUG("Reading event parameters from %s", _settings.epFile);
		IO::XMLArchive ar;
		if ( !ar.open(_settings.epFile.data()) ) {
			SEISCOMP_ERROR("%s: failed to open XML file", _settings.epFile);
			return false;
		}

		EventParametersPtr ep;
		ar >> ep;
		ar.close();

		if ( !ep ) {
			SEISCOMP_ERROR("%s: no event parameters found", _settings.epFile);
			return false;
		}

		for ( size_t i = 0; i < ep->originCount(); ++i ) {
			SEISCOMP_DEBUG("Processing origin %s", ep->origin(i)->publicID());
			addAssociations(ep->origin(i));
			process(ep->origin(i), rs.get());
		}

		ar.create("-");
		ar.setFormattedOutput(_settings.formatted);
		ar << ep;
		ar.close();

		return true;
	}
	else if ( !_settings.originID.empty() ) {
		IO::RecordStreamPtr rs = IO::RecordStream::Open(_settings.recordStreamURL.data());
		if ( !rs ) {
			SEISCOMP_ERROR("%s: failed to open recordstream", _settings.recordStreamURL);
			return false;
		}

		OriginPtr org = static_cast<Origin*>(
			query()->getObject(Origin::TypeInfo(), _settings.originID)
		);

		if ( !org ) {
			SEISCOMP_ERROR("%s: origin not found", _settings.originID);
			return false;
		}

		addAssociations(org.get());

		Notifier::Enable();

		process(org.get(), rs.get());

		Notifier::Disable();

		NotifierMessagePtr nmsg = Notifier::GetMessage();
		if ( nmsg ) {
			if ( _settings.test ) {
				cerr << "Got " << nmsg->size() << " notifiers" << endl;
			}
			else {
				connection()->send(nmsg.get());
			}
		}

		return true;
	}

	Notifier::Enable();
	return Client::Application::run();
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void App::exit(int exitCode) {
	{
		// Interupt running threads
		lock_guard lock(_mutexAlert);
		_signalAlert.notify_all();
	}

	Client::Application::exit(exitCode);
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void App::handleTimeout() {
	Core::Time now = Core::Time::UTC();

	auto origins = _associationTable.origins();

	vector<Origin*> toDelete;
	size_t unchangedCount = 0, updatedCount = 0;
	for ( auto it = origins.begin(); it != origins.end(); ++it) {
		auto *org = it->first;
		auto &eval = it->second;

		if ( !eval.dirty ) {
			// Nothing to do
			unchangedCount++;
			continue;
		}

		updatedCount++;
		process(org, eval);

		if ( eval.eol <= now ) {
			toDelete.push_back(org);
		}
	}

	for ( auto org : toDelete ) {
		_cache.remove(org);
	}

	if ( updatedCount > 0 || unchangedCount > 0 || toDelete.size() > 0 ) {
		SEISCOMP_DEBUG("Associations: %zu updated, %zu removed, %zu unchanged",
		               updatedCount, toDelete.size(), unchangedCount);
	}

	NotifierMessagePtr nmsg = Notifier::GetMessage();
	if ( nmsg ) {
		SEISCOMP_DEBUG("timeout at %s resulted in %d notifiers",
		               now.iso(), nmsg->size());
		if ( !_settings.test ) {
			connection()->send(nmsg.get());
		}
	}
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void App::handleMessage(Message *msg) {
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
			if ( _slocFirewall.isDenied(sid) ) {
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
					_associationTable.setDirty(sid);
				}

				buffer->append({
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
void App::addObject(const std::string &parentID, Object *obj) {
	auto org = Origin::Cast(obj);
	if ( org ) {
		auto tmp = _cache.get<Origin>(org->publicID());
		if ( !tmp ) {
			_cache.feed(org);
		}
		else {
			org = tmp.get();
		}

		addAssociations(org);
	}

	auto mag = Magnitude::Cast(obj);
	if ( mag ) {
		auto org = _cache.get<Origin>(parentID);
		if ( org ) {
			auto eval = _associationTable.get(org.get());
			if ( eval ) {
				SEISCOMP_DEBUG("%s: set dirty because of new %s magnitude",
				               org->publicID(), mag->type());
				eval->dirty = true;
			}
		}
	}
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void App::removeObject(const std::string &, Object *obj) {
	auto org = Origin::Cast(obj);
	if ( org ) {
		_cache.remove(org);
	}
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void App::updateObject(const std::string &parentID, Object *obj) {
	// Just forward it to addObject and handle the update in the same way.
	addObject(parentID, obj);
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
Association *App::addAssociation(Origin *org,
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
	if ( !_settings.tttAllowNegativeDepths && depth < 0) {
		depth = 0;
	}

	TravelTimeList* ttimes;
	try {
		ttimes = _ttt->compute(org->latitude().value(), org->longitude().value(),
		                       depth, buffer.lat, buffer.lon, buffer.elev);
	}
	catch( exception &e ) {
		SEISCOMP_DEBUG("%s", e.what());
		return nullptr;
	}
	if ( !ttimes ) {
		return nullptr;
	}

	auto assoc = _associationTable.insert(org, sid);
	assoc->dist = Math::Geo::deg2km(dist);

	assoc->hypoDist = std::hypot(assoc->dist, depth);

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

	auto duration = Core::TimeSpan(max(assoc->ttP, assoc->ttS) * _settings.postArrivalTimeShare);
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
void App::addAssociations(Origin *org) {
	SEISCOMP_DEBUG("%s (mags %d): add associations",
	               org->publicID(), org->magnitudeCount());

	auto *eval = _associationTable.insert(org);
	eval->eol = org->time().value() + _settings.envelopes.maxDelay;
	eval->dirty = true; // if this is an update eval->dirty may be false

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
	eval->eol += Core::TimeSpan(maxTravelTime * _settings.postArrivalTimeShare);

	SEISCOMP_DEBUG("%s: eol = %s num associations %zu", org->publicID(),
	               eval->eol.iso(), _associationTable.count(org));
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void App::process(Origin *org, IO::RecordStream *rs) {
	if ( rs ) {
		_envelopeBuffers.clear();

		while ( RecordPtr rec = rs->next() ) {
			if ( rec->channelCode().empty() || (rec->channelCode().size() != 3) ) {
				continue;
			}

			if ( rec->channelCode()[2] != 'X' ) { // Combined horizontals (sceewenv)
				continue;
			}

			auto sid = join(".", rec->networkCode(), rec->stationCode(), rec->locationCode());
			if ( _slocFirewall.isDenied(sid) ) {
				SEISCOMP_WARNING("%s: location is denied due to configuration", sid);
				continue;
			}

			EnvelopeBuffer *buffer;

			auto it = _envelopeBuffers.find(sid);
			if ( it == _envelopeBuffers.end() ) {
				auto inv = Client::Inventory::Instance();
				auto sloc = inv->getSensorLocation(rec->networkCode(),
				                                   rec->stationCode(),
				                                   rec->locationCode(),
				                                   rec->startTime());
				if ( !sloc ) {
					SEISCOMP_WARNING("%s: no inventory information for epoch at %s",
					                 sid, rec->startTime().iso());
					continue;
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

			DoubleArrayPtr tmp;
			const DoubleArray *data = DoubleArray::ConstCast(rec->data());
			if ( !data ) {
				tmp = static_cast<DoubleArray*>(rec->data()->copy(Array::DOUBLE));
				data = tmp.get();
			}

			auto timestamp = rec->startTime();
			auto dt = Core::TimeSpan(1.0 / rec->samplingFrequency());

			for ( int i = 0; i < data->size(); ++i ) {
				buffer->append({ timestamp, data->get(i) });
				timestamp += dt;
			}

			buffer->dirty = true;
		}

		size_t newLimit = 0;
		for ( const auto &[sid, buffer] : _envelopeBuffers ) {
			SEISCOMP_DEBUG("%s: registered %d/%d envelopes",
			               sid, buffer->size(), buffer->appended());
			if ( (buffer->appended() > buffer->size()) && (buffer->appended() > newLimit) ) {
				newLimit = buffer->appended();
			}
		}

		if ( newLimit > 0 ) {
			SEISCOMP_WARNING("envelopes.bufferSize = %d is not large enough to hold all read "
			                 "envelopes, consider increasing it to at least %d",
			                 _settings.envelopes.bufferSize, newLimit);
		}
	}

	Evaluation eval;
	process(org, eval);
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void App::process(Origin *org, Evaluation &eval) {
	SEISCOMP_DEBUG("Process %s num mags %d num associations %zu", org->publicID(),
	                org->magnitudeCount(), _associationTable.count(org));
	eval.bestMagnitude = {};
	eval.gof = -1;

	if ( !eval.zone ) {
		string z;
		try {
			z = _prediction.zoneName(org->latitude().value(),
			                         org->longitude().value());
		}
		catch ( ... ) {}
		if ( z.empty() ) {
			SEISCOMP_WARNING("%s: origin is outside every GMM zone; no predicted "
			                 "PGV, OGF not computed", org->publicID());
		}
		eval.zone = std::move(z);
	}

	if ( eval.zone->empty() ) {
		eval.dirty = false;
		return;
	}

	const string &zone = *eval.zone;

	// Debug snapshot: keep the per-station detail from the compute() call that
	// wins the overall GOF (not the last one).
	const bool wantSnapshot = !_settings.debug.dumpPath.empty();
	vector<StationEval> snapDetail, candDetail;
	string snapMagID, snapMagType;
	double snapMagValue = 0;
	bool snapIsEnvMag = false;

	if ( _settings.envelopeMagnitude.enable ) {
		OPT(double) envMagGOF;
		double envMagValue;
		int stationCount, envMagStationCount;

		for ( double m = _settings.envelopeMagnitude.minimum;
		      m < _settings.envelopeMagnitude.maximum;
		      m += _settings.envelopeMagnitude.spacing ) {
			auto gof = compute(org, m, zone, &stationCount,
			                   wantSnapshot ? &candDetail : nullptr);
			if ( isfinite(gof) && stationCount >= _settings.minimumStations &&
			     (!envMagGOF || (*envMagGOF < gof)) ) {
				envMagGOF = gof;
				envMagValue = m;
				envMagStationCount = stationCount;
				if ( wantSnapshot ) {
					snapDetail = std::move(candDetail);
					snapMagValue = m;
					snapMagType = _settings.envelopeMagnitude.type;
					snapMagID.clear();
					snapIsEnvMag = true;
				}
			}
		}

		if ( envMagGOF ) {
			Magnitude *envMag = nullptr;

			for ( size_t i = 0; i < org->magnitudeCount(); ++i ) {
				auto mag = org->magnitude(i);
				if ( _settings.envelopeMagnitude.type == mag->type() ) {
					envMag = mag;
					break;
				}
			}

			if ( !envMag ) {
				envMag = Magnitude::Create();
				envMag->setType(_settings.envelopeMagnitude.type);
				envMag->setStationCount(envMagStationCount);
				envMag->setCreationInfo(CreationInfo());
				envMag->creationInfo().setAgencyID(agencyID());
				envMag->creationInfo().setAuthor(author());
				org->add(envMag);
			}
			else {
				envMag->update();
			}

			envMag->setMagnitude(RealQuantity(envMagValue));
			touch(envMag);

			auto cmt = envMag->comment(_settings.commentID);

			if ( !cmt ) {
				cmt = new Comment;
				cmt->setId(_settings.commentID);
				envMag->add(cmt);
				touch(envMag);
				envMag->update();
			}

			cmt->setText(toString(*envMagGOF));
			cmt->update();

			eval.gof = *envMagGOF;
			eval.bestMagnitude = envMag->publicID();

			if ( wantSnapshot && snapIsEnvMag ) {
				snapMagID = envMag->publicID();
			}

			SEISCOMP_DEBUG("%s/%s: M=%f, GOF=%f (stations %d)", org->publicID(),
			               envMag->type(), envMag->magnitude().value(), *envMagGOF,
			               envMag->stationCount());
		}
	}

	for ( size_t i = 0; i < org->magnitudeCount(); ++i ) {
		auto mag = org->magnitude(i);
		if ( _settings.envelopeMagnitude.enable
		  && (mag->type() == _settings.envelopeMagnitude.type) ) {
			continue;
		}

		int stationCount;
		auto gof = compute(org, mag, zone, &stationCount,
		                   wantSnapshot ? &candDetail : nullptr);
		if ( isfinite(gof) && stationCount >= _settings.minimumStations &&
		     gof >= eval.gof ) {
			eval.gof = gof;
			eval.bestMagnitude = mag->publicID();
			if ( wantSnapshot ) {
				snapDetail = std::move(candDetail);
				snapMagID = mag->publicID();
				snapMagType = mag->type();
				snapMagValue = mag->magnitude().value();
				snapIsEnvMag = false;
			}
		}

		SEISCOMP_DEBUG("%s/%s: M=%f, GOF=%f (stations %d)", org->publicID(),
		               mag->type(), mag->magnitude().value(), gof, stationCount);
	}

	SEISCOMP_DEBUG("%s: GOF=%f, best mag=%s", org->publicID(), eval.gof, eval.bestMagnitude);
	eval.dirty = false;

	auto cmt = org->comment(_settings.commentID);

	if ( eval.gof >= 0 ) {
		if ( !cmt ) {
			cmt = new Comment;
			cmt->setId(_settings.commentID);
			org->add(cmt);
			touch(org);
			org->update();
			SEISCOMP_DEBUG("%s: add comment %s", org->publicID(), _settings.commentID);
		}
		else {
			SEISCOMP_DEBUG("%s: update comment %s", org->publicID(), _settings.commentID);
		}

		cmt->setText(toString(eval.gof));
		cmt->update();
	}
	else if ( cmt ) {
		SEISCOMP_DEBUG("%s: remove comment %s", org->publicID(), _settings.commentID);
		org->remove(cmt);
		touch(org);
		org->update();
	}

	cmt = org->comment(_settings.commentMagID);

	if ( !eval.bestMagnitude.empty() ) {
		if ( !cmt ) {
			cmt = new Comment;
			cmt->setId(_settings.commentMagID);
			org->add(cmt);
			touch(org);
			org->update();
			SEISCOMP_DEBUG("%s: add comment %s", org->publicID(), _settings.commentMagID);
		}
		else {
			SEISCOMP_DEBUG("%s: update comment %s", org->publicID(), _settings.commentMagID);
		}

		cmt->setText(eval.bestMagnitude);
		cmt->update();
	}
	else if ( cmt ) {
		SEISCOMP_DEBUG("%s: remove comment %s", org->publicID(), _settings.commentMagID);
		org->remove(cmt);
		touch(org);
		org->update();
	}

	if ( wantSnapshot && eval.gof >= 0
	  && any_of(snapDetail.begin(), snapDetail.end(),
	            [](const StationEval &s) { return s.used; }) ) {
		writeDebugSnapshot(org, eval, zone, snapMagID, snapMagType, snapMagValue, snapDetail);
	}
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
void App::writeDebugSnapshot(Origin *org, const Evaluation &eval,
                             const string &zone, const string &magID,
                             const string &magType, double magValue,
                             const vector<StationEval> &detail) {
	OriginSnapshot snap;
	snap.publicID = org->publicID();
	try { snap.time = org->time().value().iso(); } catch ( ... ) {}
	try { snap.author = org->creationInfo().author(); } catch ( ... ) {}
	try { snap.latitude = org->latitude().value(); } catch ( ... ) {}
	try { snap.longitude = org->longitude().value(); } catch ( ... ) {}
	try { snap.depth = org->depth().value(); } catch ( ... ) {}
	snap.zone = zone;

	snap.ogf = eval.gof;
	snap.minimumStations = _settings.minimumStations;
	snap.cutoffDistanceKm = cutoffDistanceKm(magValue);
	snap.t0Sec = commonWindowStartSec(org);
	snap.magID = magID;
	snap.magType = magType;
	snap.magValue = magValue;
	snap.stations = detail;

	// Contributing stations first, both groups sorted by distance.
	sort(snap.stations.begin(), snap.stations.end(),
	     [](const StationEval &a, const StationEval &b) {
		if ( a.used != b.used ) {
			return a.used;
		}
		return a.distanceKm < b.distanceKm;
	});

	if ( !writeSnapshotFile(_settings.debug.dumpPath, snap) ) {
		return;
	}

	SEISCOMP_DEBUG("%s: wrote OGF debug snapshot (%zu stations)",
	               org->publicID(), snap.stations.size());

	// Rate-limited directory cleanup.
	Core::Time now = Core::Time::UTC();
	if ( !_lastDebugPrune || (now - *_lastDebugPrune) >= Core::TimeSpan(300, 0) ) {
		_lastDebugPrune = now;
		pruneSnapshots(_settings.debug.dumpPath,
		               _settings.debug.keepDays, _settings.debug.maxFiles);
	}
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
double App::cutoffDistanceKm(double mag) const {
	// Optional magnitude dependent station search radius. If not configured,
	// maximumDistance is used regardless of magnitude.
	double cutoffDist = _settings.maximumDistance;
	if ( _settings.distancePerMagnitude ) {
		cutoffDist = min(*_settings.distancePerMagnitude * mag, _settings.maximumDistance);
	}
	return Math::Geo::deg2km(cutoffDist);
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
double App::commonWindowStartSec(Origin *org) const {
	// Closest station = smallest predicted P travel time over all associations.
	double closestTtP = -1;
	for ( const auto &[assocOrg, assocSid] : _associationTable.sensors(org) ) {
		const auto *assoc = _associationTable.assoc(assocOrg, assocSid);
		if ( assoc && (assoc->ttP >= 0)
		  && ((closestTtP < 0) || (assoc->ttP < closestTtP)) ) {
			closestTtP = assoc->ttP;
		}
	}

	if ( closestTtP < 0 ) {
		return 0.0;
	}

	return max(0.0, closestTtP - _settings.preArrivalTimeWindow);
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
double App::compute(Origin *org, const Magnitude *mag, const string &zone,
                    int *stationCount, vector<StationEval> *detail) {
	SEISCOMP_DEBUG("Compute %s %s %s", org->publicID(), mag->publicID(), mag->type());
	return compute(org, mag->magnitude().value(), zone, stationCount, detail);
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
double App::compute(Origin *org, double mag, const string &zone, int *stationCount,
                    vector<StationEval> *detail) {
	vector<double> gofs;

	if ( detail ) {
		detail->clear();
	}

	// Records a station that did not contribute to the OGF, for the debug
	// snapshot only. No-op when detail collection is off.
	auto skip = [detail](const Association *assoc, const string &sid, const char *reason) {
		if ( !detail ) {
			return;
		}
		StationEval se;
		se.sid = sid;
		se.distanceKm = assoc->dist;
		se.hypoDistanceKm = assoc->hypoDist;
		se.skipReason = reason;
		detail->push_back(std::move(se));
		SEISCOMP_DEBUG("Skip %s at %.1fkm: %s", sid, assoc->dist, reason);
	};

	const double cutoffDistKm = cutoffDistanceKm(mag);

	// Common correlation-window start time for every station
	const double windowStartSec = commonWindowStartSec(org);

	// Do not check the eval.dirty flag as this has been done already
	for ( const auto &[org, sid] : _associationTable.sensors(org) ) {

		auto *assoc = _associationTable.assoc(org, sid);
		if ( !assoc ) {
			SEISCOMP_WARNING("No associationTable for %s: %s", org->publicID(), sid);
			continue;
		}

		const double distKm = assoc->dist;

		if ( distKm > cutoffDistKm ) {
			// station farther then cutoff distance
			continue;
		}

		auto it = _envelopeBuffers.find(sid);
		if ( it == _envelopeBuffers.end() ) {
			skip(assoc, sid, "no envelope buffer");
			continue;
		}

		auto buffer = it->second.get();
		if ( !buffer || buffer->empty() ) {
			// No envelopes
			skip(assoc, sid, "no envelopes buffered");
			continue;
		}

		if ( detail
		  || !assoc->lastMag
		  || !_prediction.equal(*assoc->lastMag, mag) ) {
			// A dirty association requires a recomputation. The detail pass
			// always recomputes so the captured arrays are self-consistent for
			// this magnitude.

			assoc->correlation = -1;
			assoc->lastMag = Seiscomp::Core::None;

			string soilClass = _prediction.resolvedSoilClass(sid);
			if ( soilClass.empty() ) {
				SEISCOMP_WARNING("No soil class for station %s", sid);
				skip(assoc, sid, "no soil class for station");
				continue;
			}

			ArrayPtr array;
			try {
				array = _prediction.trace(soilClass, mag, assoc->hypoDist);
				if ( !array ) {
					// No predictions
					skip(assoc, sid, "no prediction");
					continue;
				}
			}
			catch ( exception &e ) {
				// No predictions
				SEISCOMP_WARNING("No predictions for %s: %s", sid, e.what());
				skip(assoc, sid, "no prediction");
				continue;
			}

			double pgv = 1.0;
			try {
				pgv = _prediction.pgv(zone, mag, assoc->hypoDist);
			}
			catch ( exception &e ) {
				SEISCOMP_WARNING("No pgv: %s", e.what());
				skip(assoc, sid, "no predicted PGV");
				continue;
			}

			DoubleArrayPtr pred = DoubleArray::Cast(array);
			if ( !pred ) {
				pred = static_cast<DoubleArray*>(array->copy(Array::DOUBLE));
			}

			auto predMax = pred->max();
			double amplification = _prediction.amplification(sid);

			// Physical scale of the predicted envelope: normalise it to unit peak,
			// bring it to the GMPE PGV for this magnitude and distance, then apply
			// the site amplification. Only the amplitude-fit term below uses this;
			// the shape correlation is invariant to it.
			double scale = pgv / predMax * amplification;

			// Correlation window [idx0, idx1) in whole seconds after the origin
			// time: the intersection of a, b, c

			// Desired time window (a): a common start time shared by every station
			double startTimeA = windowStartSec;
			double endTimeA = assoc->ttS * _settings.postArrivalTimeShare;

			// Time window of available predicted envelope (b)
			double startTimeB = 0;
			double endTimeB = pred->size();

			// The samples the envelope buffer covers (c)
			double startTimeC = static_cast<double>(buffer->front().timestamp - org->time().value());
			double endTimeC = static_cast<double>(buffer->back().timestamp - org->time().value()) + 1.0;

			double startTime = max(startTimeA, max(startTimeB, startTimeC));
			double endTime = min(endTimeA, min(endTimeB, endTimeC));

			int idx0 = static_cast<int>(startTime);
			int idx1 = static_cast<int>(endTime);

			if ( idx0 >= idx1 ) {
				skip(assoc, sid, "empty correlation time window");
				continue;
			}

			// A station contributes only once the data buffered for it covers the
			// second of its predicted P arrival
			if ( assoc->ttP < 0 ) {
				skip(assoc, sid, "no predicted P arrival");
				continue;
			}
			if ( idx1 <= static_cast<int>(assoc->ttP) ) {
				skip(assoc, sid, "predicted P not yet arrived");
				continue;
			}

			// Too short a window
			if ( endTime - startTime < _settings.minimumCorrelationWindow ) {
				skip(assoc, sid, "correlation window too short");
				continue;
			}

			int count = idx1 - idx0;
			const double *dataPred = pred->typedData() + idx0;

			// Position the buffer iterator at the first sample not before the
			// window start
			const Time winStart = org->time().value() + TimeSpan(startTime);
			auto bit = buffer->begin();
			while ( bit != buffer->end() && bit->timestamp < winStart ) {
				++bit;
			}

			if ( bit == buffer->end() ) {
				// A gap swallowed the whole window: no sample within [idx0, idx1)
				// even though the buffer's overall time span covers it.
				skip(assoc, sid, "empty correlation time window");
				continue;
			}

			// Peak amplitudes over the window: observed vs. GMPE-scaled predicted.
			// A gap inside the window can leave fewer observed samples than
			// count; nObs is what is actually available and is what the
			// correlation below is computed over.
			double maxObs = 0.0, maxPredWinRaw = 0.0;
			int nObs = 0;
			{
				auto it = bit;
				for ( ; nObs < count && it != buffer->end(); ++nObs, ++it ) {
					maxObs = max(maxObs, it->value);
					maxPredWinRaw = max(maxPredWinRaw, dataPred[nObs]);
				}
			}

			if ( nObs == 0 ) {
				skip(assoc, sid, "empty correlation time window");
				continue;
			}

			double maxPredWinScaled = maxPredWinRaw * scale;

			// Pearson correlation of the two shapes. Each series is normalised by
			// its own window peak: the coefficient does not depend on that (nor on
			// the GMPE scaling), the normalisation only keeps the sums well
			// conditioned.
			double normObs = maxObs > 0.0 ? 1.0 / maxObs : 1.0;
			double normPred = maxPredWinRaw > 0.0 ? 1.0 / maxPredWinRaw : 1.0;

			double sumX{0}, sumY{0}, sumX2{0}, sumY2{0}, sumXY{0};
			for ( int i = 0; i < nObs; ++i, ++bit ) {
				auto obs = bit->value * normObs;
				auto pred = dataPred[i] * normPred;

				sumX += obs;
				sumY += pred;
				sumX2 += obs * obs;
				sumY2 += pred * pred;
				sumXY += obs * pred;
			}

			// Amplitude fit: 1 when the observed peak matches the predicted peak,
			// falling off as they diverge.
			double ampRatio = (maxObs - maxPredWinScaled) / (maxObs + maxPredWinScaled);
			double amplitudeFit = 1.0 - ampRatio * ampRatio;

			// Pearson correlation coefficient
			// Ref: https://en.wikipedia.org/wiki/Pearson_correlation_coefficient
			double corr = max(0.0, (nObs * sumXY - sumX * sumY)
			                       / sqrt(nObs * sumX2 - sumX * sumX)
			                       / sqrt(nObs * sumY2 - sumY * sumY));

			double sgf = sqrt(corr * amplitudeFit); // Station Goodness of Fit

			if ( !isfinite(sgf) ) {
				if ( maxObs == 0.0 && maxPredWinScaled == 0.0 ) {
					// Nothing observed and nothing expected: trivially a perfect fit.
					sgf = 1.0;
				}
				else {
					SEISCOMP_DEBUG("%s: non-finite SGF [%d:%d #%d] dist=%.1f mag=%.2f "
					               "pgv=%g amp=%.2f scale=%g maxObs=%g maxPredWinScaled=%g "
					               "ampFit=%g corr=%g",
					               sid, idx0, idx1, nObs, assoc->dist, mag, pgv,
					               amplification, scale, maxObs, maxPredWinScaled, amplitudeFit, corr);
					skip(assoc, sid, "non-finite station GoF");
					continue;
				}
			}

			assoc->correlation = sgf;
			assoc->lastMag = mag;

			if ( detail ) {
				StationEval se;
				se.sid = sid;
				se.distanceKm = assoc->dist;
				se.hypoDistanceKm = assoc->hypoDist;
				se.used = true;
				se.soilClass = soilClass;
				se.predictedPath = _prediction.tracePath(soilClass, mag, assoc->hypoDist);
				se.ttP = assoc->ttP;
				se.ttS = assoc->ttS;
				se.pgv = pgv;
				se.amplification = amplification;
				se.predMax = predMax;
				se.scale = scale;
				se.windowStart = idx0;
				se.windowEnd = idx1;
				se.maxObs = maxObs;
				se.maxPred = maxPredWinScaled;
				se.amplitudeFit = amplitudeFit;
				se.correlation = corr;
				se.sgf = sgf;

				// Full raw predicted envelope: sample j is j seconds after the origin time.
				se.rawPredictedT0 = 0.0;
				se.rawPredicted.assign(pred->typedData(),
				                       pred->typedData() + pred->size());

				// Observed envelope over a context window around [idx0, idx1],
				// clamped to what the buffer actually covers.
				const double ctxStart = max(startTimeC, static_cast<double>(idx0) - 15.0);
				const double ctxEnd = min(endTimeC, static_cast<double>(idx1) + 45.0);
				const Time ctxStartAbs = org->time().value() + TimeSpan(ctxStart);
				auto cit = buffer->begin();
				while ( cit != buffer->end() && cit->timestamp < ctxStartAbs ) {
					++cit;
				}
				if ( cit != buffer->end() ) {
					se.observedT0 = static_cast<double>(cit->timestamp - org->time().value());
					for ( ; cit != buffer->end(); ++cit ) {
						double t = static_cast<double>(cit->timestamp - org->time().value());
						if ( t > ctxEnd ) {
							break;
						}
						se.observed.push_back(cit->value);
					}
				}

				detail->push_back(std::move(se));
			}
		}
		else {
			// SEISCOMP_DEBUG("%s: reuse SGF=%f", sid, assoc->correlation);
		}

		gofs.push_back(assoc->correlation);
	}

	if ( stationCount ) {
		*stationCount = static_cast<int>(gofs.size());
	}

	return Math::Statistics::mean(gofs) * 100; // Overall Goodness of Fit
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
bool App::playback() {
	Notifier::Disable();

	vector<VS::EnvelopePtr> envelopes;
	PlaybackVisitor::Storage notifiers;

	// Read envelopes
	if ( !_settings.recordStreamURL.empty() ) {
		IO::RecordStreamPtr rs = IO::RecordStream::Open(_settings.recordStreamURL.data());
		if ( !rs ) {
			SEISCOMP_ERROR("%s: failed to open recordstream", _settings.recordStreamURL);
			return false;
		}

		while ( RecordPtr rec = rs->next() ) {
			if ( rec->channelCode().empty() || (rec->channelCode().size() != 3) ) {
				continue;
			}

			if ( rec->channelCode()[2] != 'X' ) { // Combined horizontals (sceewenv)
				continue;
			}

			DoubleArrayPtr tmp;
			const DoubleArray *data = DoubleArray::ConstCast(rec->data());
			if ( !data ) {
				tmp = static_cast<DoubleArray*>(rec->data()->copy(Array::DOUBLE));
				data = tmp.get();
			}

			auto timestamp = rec->startTime();
			auto dt = Core::TimeSpan(1.0 / rec->samplingFrequency());

			for ( int i = 0; i < data->size(); ++i ) {
				VS::EnvelopePtr env = VS::Envelope::Create();
				env->setNetwork(rec->networkCode());
				env->setStation(rec->stationCode());
				env->setTimestamp(timestamp);
				auto *chan = VS::EnvelopeChannel::Create();
				chan->setName("H");
				chan->setWaveformID(WaveformStreamID(
					rec->networkCode(), rec->stationCode(),
					rec->locationCode(), rec->channelCode(),
					{}
				));
				auto *value = new VS::EnvelopeValue((*data)[i], "vel", None);
				chan->add(value);
				env->add(chan);
				envelopes.push_back(env);
				timestamp += dt;
			}
		}

		sort(envelopes.begin(), envelopes.end(), [](const auto &env1, const auto &env2) {
			return env1->timestamp() < env2->timestamp();
		});
	}

	// Read event parameters
	if ( !_settings.epFile.empty() ) {
		SEISCOMP_DEBUG("reading %s", _settings.epFile);

		IO::XMLArchive ar;
		if ( !ar.open(_settings.epFile.data()) ) {
			SEISCOMP_ERROR("%s: failed to open XML file", _settings.epFile);
			return false;
		}

		EventParametersPtr ep;
		ar >> ep;
		ar.close();

		if ( !ep ) {
			SEISCOMP_WARNING("%s: no event parameters found", _settings.epFile);
		}

		PlaybackVisitor visitor(notifiers);
		ep->accept(&visitor);

		sort(notifiers.begin(), notifiers.end(), [](const auto &n1, const auto &n2) {
			return n1.second < n2.second;
		});

		if ( !notifiers.empty() ) {
			set<string> magTypes;
			auto oldSize = notifiers.size();
			auto it = notifiers.begin();
			auto last_it = it++;
			for ( ; it != notifiers.end(); ) {
				auto stamag = StationMagnitude::Cast((*it->first->begin())->object());
				if ( stamag ) {
					magTypes.insert(stamag->type());
				}

				if ( last_it->second == it->second ) {
					last_it->first->attach(*it->first->begin());
					it = notifiers.erase(it);
				}
				else {
					last_it = it;
					++it;
				}
			}

			if ( notifiers.size() < oldSize ) {
				SEISCOMP_INFO("Notifiers compressed by %d%%",
				              100 - notifiers.size() * 100 / oldSize);
			}

			SEISCOMP_INFO("Magnitude types: %s", Core::join(magTypes, ", "));
		}
	}

	OPT(Time) earliestTime, latestTime;

	if ( !envelopes.empty() ) {
		earliestTime = envelopes.front()->timestamp();
		latestTime = envelopes.back()->timestamp();
		SEISCOMP_DEBUG("Envelopes start at %s", envelopes.front()->timestamp().iso());
	}

	if ( !notifiers.empty() ) {
		earliestTime = earliestTime ? min(*earliestTime, notifiers.front().second) : notifiers.front().second;
		latestTime = latestTime ? max(*latestTime, notifiers.back().second) : notifiers.back().second;
		SEISCOMP_DEBUG("Notifiers start at %s", notifiers.front().second.iso());
	}

	if ( !earliestTime ) {
		SEISCOMP_ERROR("Nothing to do");
		return false;
	}

	SEISCOMP_INFO("Playback duration: %s", (*latestTime - *earliestTime).toString());

	auto timeOffset = Time::UTC() - *earliestTime;

	thread envThread, epThread;

	if ( !envelopes.empty() ) {
		envThread = thread([&]() {
			for ( auto &env : envelopes ) {
				unique_lock lock(_mutexAlert);

				auto delay = (env->timestamp() + timeOffset - Time::UTC()).repr();
				if ( delay.count() > 0 ) {
					_signalAlert.wait_for(lock, delay, [this]() { return isExitRequested(); });
				}

				if ( isExitRequested() ) {
					break;
				}

				if ( _settings.shiftTimes ) {
					env->setTimestamp(env->timestamp() + timeOffset);
				}

				Core::DataMessagePtr msg = new Core::DataMessage;
				msg->attach(env.get());

				cout << env->timestamp().iso() << " "
				     << env->envelopeChannel(0)->waveformID() << " "
				     << env->envelopeChannel(0)->envelopeValue(0)->value()
				     << endl;

				if ( !_settings.test ) {
					connection()->send("AMPLITUDE", msg.get());
				}
			}
		});
	}

	if ( !notifiers.empty() ) {
		epThread = thread([&]() {
			for ( auto &nitem : notifiers ) {
				unique_lock lock(_mutexAlert);

				auto delay = (nitem.second + timeOffset - Time::UTC()).repr();
				if ( delay.count() > 0 ) {
					_signalAlert.wait_for(lock, delay, [this]() { return isExitRequested(); });
				}

				if ( isExitRequested() ) {
					break;
				}

				if ( _settings.shiftTimes ) {
					nitem.second += timeOffset;
				}

				cerr << nitem.second.iso() << " "
				     << nitem.first->size() << ":";
				unordered_map<string, size_t> counts;
				for ( auto n : *nitem.first ) {
					++counts[n->object()->className()];
					if ( _settings.shiftTimes ) {
						if ( Origin::Cast(n->object()) ) {
							retouch(*static_cast<Origin*>(n->object()), nitem.second);
							static_cast<Origin*>(n->object())->setTime(
								static_cast<Origin*>(n->object())->time().value() + timeOffset
							);
						}
						else if ( Magnitude::Cast(n->object()) ) {
							retouch(*static_cast<Magnitude*>(n->object()), nitem.second);
						}
					}
				}
				for ( auto &[classname, count] : counts ) {
					cerr << " " << count << " x " << classname;
				}
				cerr << endl;

				if ( !_settings.test ) {
					connection()->send(nitem.first.get());
				}
			}
		});
	}

	if ( envThread.joinable() ) {
		envThread.join();
	}

	if ( epThread.joinable() ) {
		epThread.join();
	}

	return true;
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
