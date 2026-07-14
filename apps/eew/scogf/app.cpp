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

#include <cmath>
#include <filesystem>
#include <limits>
#include <set>

#include "app.h"


using namespace std;
using namespace Seiscomp;
using namespace Seiscomp::Core;
using namespace Seiscomp::DataModel;


#define DUMP_DATA 0


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
	size_t dirtyCount = 0, updatedCount = 0;
	for ( auto it = origins.begin(); it != origins.end(); ++it) {
		auto *org = it->first;
		auto &eval = it->second;

		if ( !eval.dirty ) {
			// Nothing to do
			dirtyCount++;
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

	if ( updatedCount > 0 || dirtyCount > 0 || toDelete.size() > 0 ) {
		SEISCOMP_DEBUG("Associations: %zu updated, %zu removed, %zu unchanged",
		               updatedCount, toDelete.size(), dirtyCount);
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

	auto ttimes = _ttt->compute(org->latitude().value(), org->longitude().value(),
	                           depth,
	                           buffer.lat, buffer.lon, buffer.elev);
	if ( !ttimes ) {
		return nullptr;
	}

	auto assoc = _associationTable.insert(org, sid);
	assoc->dist = Math::Geo::deg2km(dist);

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
	SEISCOMP_DEBUG("%s: add associations", org->publicID());

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
	eval->eol += Core::TimeSpan(maxTravelTime * _settings.postArrivalTimeShare);

	SEISCOMP_DEBUG("%s: eol = %s", org->publicID(), eval->eol.iso());
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

			if ( (rec->channelCode()[1] != 'H') || (rec->channelCode()[2] != 'X') ) {
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
	eval.bestMagnitude = {};
	eval.gof = -1;

	if ( _settings.envelopeMagnitude.enable ) {
		OPT(double) envMagGOF;
		double envMagValue;
		int stationCount, envMagStationCount;

		for ( double m = _settings.envelopeMagnitude.minimum;
		      m < _settings.envelopeMagnitude.maximum;
		      m += _settings.envelopeMagnitude.spacing ) {
			auto gof = compute(org, m, &stationCount);
			if ( stationCount >= _settings.minimumStations &&
			     (!envMagGOF || (*envMagGOF < gof)) ) {
				envMagGOF = gof;
				envMagValue = m;
				envMagStationCount = stationCount;
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
		auto gof = compute(org, mag, &stationCount);
		if ( stationCount >= _settings.minimumStations && gof > eval.gof ) {
			eval.gof = gof;
			eval.bestMagnitude = mag->publicID();
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
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
double App::compute(Origin *org, const Magnitude *mag, int *stationCount) {
	SEISCOMP_DEBUG("Compute %s %s %s", org->publicID(), mag->publicID(), mag->type());
	return compute(org, mag->magnitude().value(), stationCount);
}
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<




// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
double App::compute(Origin *org, double mag, int *stationCount) {
	vector<double> gofs;

	// Do not check the eval.dirty flag as this has been done already
	for ( const auto &[org, sid] : _associationTable.sensors(org) ) {
		auto it = _envelopeBuffers.find(sid);
		if ( it == _envelopeBuffers.end() ) {
			continue;
		}

		auto buffer = it->second.get();
		if ( !buffer || buffer->empty() ) {
			// No envelopes
			continue;
		}

		auto assoc = _associationTable.assoc(org, sid);
		if ( !assoc ) {
			// No association
			continue;
		}

		if ( !assoc->lastMag
		  || !_prediction.equal(*assoc->lastMag, mag) ) {
			// A dirty association requires a recomputation
			ArrayPtr array;
			try {
				array = _prediction.get(sid, mag, assoc->dist);
				if ( !array ) {
					// No predictions
					continue;
				}
			}
			catch ( exception &e ) {
				// No predictions
				SEISCOMP_WARNING("No predictions for %s: %s", sid, e.what());
				continue;
			}

			double scale = 1.0;
			double pgv = 1.0;
			try {
				pgv = _prediction.pgv(org, mag, assoc->dist);
				scale = pgv;
			}
			catch ( exception &e ) {
				SEISCOMP_WARNING("No pgv, assume 1: %s", e.what());
			}

			DoubleArrayPtr pred = DoubleArray::Cast(array);
			if ( !pred ) {
				pred = static_cast<DoubleArray*>(array->copy(Array::DOUBLE));
			}

			auto predMax = pred->max();
			scale /= predMax;

			double amplification = _prediction.amplification(sid);
			scale *= amplification;

			// Desired time window (a)
			double startTimeA = assoc->ttP - _settings.preArrivalTimeWindow;
			double endTimeA = assoc->ttS * _settings.postArrivalTimeShare;

			// Time window of available template (b)
			double startTimeB = 0;
			double endTimeB = pred->size();

			// The time interval (the samples) covered by envelope values in the buffer (c)
			double startTimeC = static_cast<double>(buffer->front().timestamp - org->time().value());
			double endTimeC = static_cast<double>(buffer->back().timestamp - org->time().value()) + 1.0;

			double startTime = max(startTimeA, max(startTimeB, startTimeC));
			double endTime = min(endTimeA, min(endTimeB, endTimeC));

			int idx0 = static_cast<int>(startTime);
			int idx1 = static_cast<int>(endTime);

			if ( idx0 >= idx1 ) {
				SEISCOMP_DEBUG("Empty correlation time window: %d:%d", idx0, idx1);
				continue;
			}

			int count = idx1 - idx0;
			double *dataPred = pred->typedData() + idx0;

			// Move buffer to start index
			auto bit = buffer->begin();
			int idx0Obs = (org->time().value() + TimeSpan(startTime) - bit->timestamp).seconds();
			for ( int i = 0; i < idx0Obs; ++i ) {
				++bit;
			}

			auto bitSave = bit;

			double maxObs, maxPred;

			#if DUMP_DATA
			ofstream ofs;
			ofs.open(sid + ".csv");
			#endif

			for ( int i = 0; i < count; ++i, ++bit ) {
				auto obs = bit->value;
				auto pred = dataPred[i] * scale;

				#if DUMP_DATA
				ofs << obs << "\t" << pred << "\n";
				#endif

				if ( !i ) {
					maxObs = obs;
					maxPred = pred;
				}
				else {
					if ( maxObs < obs ) {
						maxObs = obs;
					}
					if  ( maxPred < pred ) {
						maxPred = pred;
					}
				}
			}

			#if DUMP_DATA
			ofs.close();
			#endif

			bit = bitSave;

			double numericScale = 1.0 / maxPred;
			double sumX{0}, sumY{0}, sumX2{0}, sumY2{0}, sumXY{0};

			for ( int i = 0; i < count; ++i, ++bit ) {
				auto obs = bit->value * numericScale;
				auto pred = dataPred[i] * scale * numericScale;

				sumX += obs;
				sumY += pred;
				sumX2 += obs * obs;
				sumY2 += pred * pred;
				sumXY += obs * pred;
			}

			double amplitudeFit = 1.0 - pow((maxObs - maxPred) / (maxObs + maxPred), 2.0);
			// Pearson correlation coefficient
			// Ref: https://en.wikipedia.org/wiki/Pearson_correlation_coefficient
			double corr = max(0.0, (count * sumXY - sumX * sumY) / sqrt(count * sumX2 - sumX * sumX) / sqrt(count * sumY2 - sumY * sumY));
			assoc->correlation = sqrt(corr * amplitudeFit);
			assoc->lastMag = mag;

			/*
			cerr << toString(sumX) << " " << toString(sumX2) << " " << toString(sumY)
			     << " " << toString(sumY2) << " " << toString(sumXY) << endl;
			*/

			/*
			SEISCOMP_DEBUG("%s: [%d(%d):%d#%d] dist=%f, mag=%f, gMaxPred=%f, "
			               "scale=pgv(%f)*amplification(%f)/max(%f)=%f, maxObs=%f, maxPred=%f, "
			               "ampFit=%f, corr=%f, SGF=%f",
			               sid, idx0, idx0Obs, idx1, count, assoc->dist, mag,
			               predMax, pgv, amplification, predMax, scale,
			               maxObs, maxPred, amplitudeFit, corr, assoc->correlation);
			*/
		}
		else {
			// SEISCOMP_DEBUG("%s: reuse SGF=%f", sid, assoc->correlation);
		}

		gofs.push_back(assoc->correlation);
	}

	if ( stationCount ) {
		*stationCount = static_cast<int>(gofs.size());
	}

	return Math::Statistics::mean(gofs) * 100;
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

			if ( (rec->channelCode()[1] != 'H') || (rec->channelCode()[2] != 'X') ) {
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
