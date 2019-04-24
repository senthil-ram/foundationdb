#include <boost/lexical_cast.hpp>
#include "fdbclient/ManagementAPI.actor.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/ReadYourWrites.h"
#include "fdbrpc/ContinuousSample.h"
#include "fdbmonitor/SimpleIni.h"
#include "fdbserver/ClusterRecruitmentInterface.h"
#include "fdbserver/Status.h"
#include "fdbserver/TesterInterface.actor.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "fdbserver/workloads/BulkSetup.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "flow/actorcompiler.h"

void getVersionAndnumTags(TraceEventFields md, Version& version, int& numTags) {
	version = -1;
	numTags = -1;

	version = boost::lexical_cast<int64_t>(md.getValue("Version"));
	numTags = boost::lexical_cast<int>(md.getValue("NumTags"));
}

void getTagAndDurableVersion(TraceEventFields md, Version version, Tag& tag, Version& durableVersion) {
	Version verifyVersion;
	durableVersion = -1;

	verifyVersion = boost::lexical_cast<int64_t>(md.getValue("Version"));
	std::string tagString = md.getValue("Tag");
	int colon = tagString.find_first_of(':');
	std::string localityString = tagString.substr(0, colon);
	std::string idString = tagString.substr(colon + 1);
	tag.locality = boost::lexical_cast<int>(localityString);
	tag.id = boost::lexical_cast<int>(idString);

	durableVersion = boost::lexical_cast<int64_t>(md.getValue("DurableVersion"));
}

void getMinAndMaxTLogVersions(TraceEventFields md, Version version, Tag tag, Version& minTLogVersion,
                              Version& maxTLogVersion) {
	Version verifyVersion;
	Tag verifyTag;
	minTLogVersion = maxTLogVersion = -1;

	verifyVersion = boost::lexical_cast<int64_t>(md.getValue("Version"));
	std::string tagString = md.getValue("Tag");
	int colon = tagString.find_first_of(':');
	std::string localityString = tagString.substr(0, colon);
	std::string idString = tagString.substr(colon + 1);
	verifyTag.locality = boost::lexical_cast<int>(localityString);
	verifyTag.id = boost::lexical_cast<int>(idString);
	if (tag != verifyTag) {
		return;
	}
	minTLogVersion = boost::lexical_cast<int64_t>(md.getValue("PoppedTagVersion"));
	maxTLogVersion = boost::lexical_cast<int64_t>(md.getValue("QueueCommittedVersion"));
}

void filterEmptyMessages(std::vector<Future<TraceEventFields>>& messages) {
	messages.erase(std::remove_if(messages.begin(), messages.end(),
								  [](Future<TraceEventFields>const & msgFuture)
								  {
									  return !msgFuture.isReady() || msgFuture.get().size() == 0;
								  }
					   ), messages.end());
	return;
}

void printMessages(std::vector<Future<TraceEventFields>>& messages) {
	for (int i = 0; i < messages.size(); i++) {
		TraceEvent("SnapTestMessages").detail("I", i).detail("Value", messages[i].get().toString());
	}
	return;
}

struct SnapTestWorkload : TestWorkload {
public: // variables
	int numSnaps; // num of snapshots to be taken
	              // FIXME: currently validation works on numSnap = 1
	double maxSnapDelay; // max delay before which a snapshot will be taken
	bool snapCheck; // check for the successful snap create
	int testID; // test id
	UID snapUID; // UID used for snap name
	std::string restartInfoLocation; // file location to store the snap restore info
	int maxRetryCntToRetrieveMessage; // number of retires to do trackLatest

public: // ctor & dtor
	SnapTestWorkload(WorkloadContext const& wcx)
	  : TestWorkload(wcx), numSnaps(0), maxSnapDelay(0.0), snapCheck(false), testID(0), snapUID() {
		TraceEvent("SnapTestWorkload Constructor");
		std::string workloadName = "SnapTest";
		maxRetryCntToRetrieveMessage = 10;

		numSnaps = getOption(options, LiteralStringRef("numSnaps"), 0);
		maxSnapDelay = getOption(options, LiteralStringRef("maxSnapDelay"), 25.0);
		snapCheck = getOption(options, LiteralStringRef("snapCheck"), false);
		testID = getOption(options, LiteralStringRef("testID"), 0);
		restartInfoLocation =
		    getOption(options, LiteralStringRef("restartInfoLocation"), LiteralStringRef("simfdb/restartInfo.ini"))
		        .toString();
	}

public: // workload functions
	std::string description() override { return "SnapTest"; }
	Future<Void> setup(Database const& cx) override {
		TraceEvent("SnapTestWorkloadSetup");
		return Void();
	}
	Future<Void> start(Database const& cx) override {
		TraceEvent("SnapTestWorkloadStart");
		if (clientId == 0) {
			return _start(cx, this);
		}
		return Void();
	}

	Future<bool> check(Database const& cx) override {
		TraceEvent("SnapTestWorkloadCheck").detail("ClientID", clientId).detail("SnapCheck", this->snapCheck);
		if (!this->snapCheck || clientId != 0) {
			TraceEvent("SnapTestCheckSucc");
			return true;
		}
		switch (this->testID) {
		case 0:
		case 1:
		case 2:
		case 3: {
			// snap restore will verify that the snap was a success
			return true;
		}
		case 4: {
			// QuietDatabase will test this
			return true;
		}
		case 6: {
			// std::string token = "SnapFailIgnorePopNotSet";
			// return verifyTLogTrackLatest(cx, this, token);
			// break;
			return true;
		}
		case 7: {
			// std::string token = "SnapFailedDisableTLogUidMismatch";
			// return verifyTLogTrackLatest(cx, this, token);
			// break;
			return true;
		}
		default: { break; }
		}
		TraceEvent(SevError, "InvalidPathCheckOptions");
		return false;
	}

	void getMetrics(vector<PerfMetric>& m) override { TraceEvent("SnapTestWorkloadGetMetrics"); }

	ACTOR Future<Void> _create_keys(Database cx, std::string prefix, bool even = true) {
		state Transaction tr(cx);
		state vector<int64_t> keys;

		for (int i = 0; i < 1000; i++) {
			keys.push_back(g_random->randomInt64(0, INT64_MAX - 2));
		}

		state int retry = 0;
		tr.reset();
		loop {
			try {
				for (auto id : keys) {
					if (even) {
						if (id % 2 != 0) {
							id++;
						}
					} else {
						if (id % 2 == 0) {
							id++;
						}
					}
					std::string Key1 = prefix + std::to_string(id);
					Key key1Ref(Key1);
					std::string Val1 = std::to_string(id);
					Value val1Ref(Val1);
					tr.set(key1Ref, val1Ref, false);
				}
				wait(tr.commit());
				break;
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}
		return Void();
	}

	ACTOR Future<Void> _start(Database cx, SnapTestWorkload* self) {
		state Transaction tr(cx);

		if (self->testID == 0) {
			// create even keys before the snapshot
			wait(self->_create_keys(cx, "snapKey"));
		} else if (self->testID == 1) {
			// create a snapshot
			state double toDelay = fmod(g_random->randomUInt32(), self->maxSnapDelay);
			TraceEvent("ToDelay").detail("Value", toDelay);
			ASSERT(toDelay < self->maxSnapDelay);
			wait(delay(toDelay));

			state int retry = 0;
			loop {
				self->snapUID = g_random->randomUniqueID();
				try {
					StringRef snapCmdRef = LiteralStringRef("/bin/snap_create.sh");
					Future<Void> status = snapCreate(cx, snapCmdRef, self->snapUID);
					wait(status);
					break;
				} catch (Error& e) {
					++retry;
					TraceEvent(retry > 3 ? SevWarn : SevInfo, "SnapCreateCommandFailed").detail("Error", e.what());
					if (retry > 3) {
						throw operation_failed();
					}
				}
			}
			CSimpleIni ini;
			ini.SetUnicode();
			ini.LoadFile(self->restartInfoLocation.c_str());
			std::string uidStr = self->snapUID.toString();
			ini.SetValue("RESTORE", "RestoreSnapUID", uidStr.c_str());
			ini.SaveFile(self->restartInfoLocation.c_str());
			// write the snapUID to a file
			TraceEvent("Snapshot create succeeded");
		} else if (self->testID == 2) {
			// create odd keys after the snapshot
			wait(self->_create_keys(cx, "snapKey", false /*even*/));
		} else if (self->testID == 3) {
			state KeySelector begin = firstGreaterOrEqual(normalKeys.begin);
			state KeySelector end = firstGreaterOrEqual(normalKeys.end);
			state int cnt = 0;
			// read the entire normalKeys range and look at keys prefixed
			// with snapKeys 1) validate that all key ids are even ie -
			// created before snap 2) values are same as the key id 3) # of
			// keys adds up to the total keys created before snap
			tr.reset();
			loop {
				try {
					Standalone<RangeResultRef> kvRange = wait(tr.getRange(begin, end, CLIENT_KNOBS->TOO_MANY));
					if (!kvRange.more && kvRange.size() == 0) {
						TraceEvent("SnapTestNoMoreEntries");
						break;
					}

					for (int i = 0; i < kvRange.size(); i++) {
						if (kvRange[i].key.startsWith(LiteralStringRef("snapKey"))) {
							std::string tmp1 = kvRange[i].key.substr(7).toString();
							int64_t id = strtol(tmp1.c_str(), nullptr, 0);
							if (id % 2 != 0) {
								throw operation_failed();
							}
							++cnt;
							std::string tmp2 = kvRange[i].value.toString();
							int64_t value = strtol(tmp2.c_str(), nullptr, 0);
							if (id != value) {
								throw operation_failed();
							}
						}
					}
					begin = firstGreaterThan(kvRange.end()[-1].key);
				} catch (Error& e) {
					wait(tr.onError(e));
				}
			}
			TraceEvent("SnapTestVerifyCntValue").detail("Value", cnt);
			if (cnt != 1000) {
				throw operation_failed();
			}
		} else if (self->testID == 4) {
			// description: if disable of a TLog pop was not followed by a
			// corresponding enable, then TLog will automatically enable the
			// popping of TLogs. this test case validates that we auto
			// enable the popping of TLogs
			disableConnectionFailuresAfter(0.0, "TestRun");
			tr.reset();
			loop {
				// disable pop of the TLog
				try {
					StringRef payLoadRef = LiteralStringRef("empty-binary:uid=a36b2ca0e8dab0452ac3e12b6b926f4b");
					tr.execute(execDisableTLogPop, payLoadRef);
					wait(tr.commit());
					break;
				} catch (Error& e) {
					wait(tr.onError(e));
				}
			}
		} else if (self->testID == 6) {
			// snapshot create without disabling pop of the TLog
			tr.reset();
			state Standalone<StringRef> uidStr = LiteralStringRef("d78b08d47f341158e9a54d4baaf4a4dd");
			loop {
				try {
					Standalone<StringRef> snapPayload = LiteralStringRef("/bin/"
					                                         "snap_create.sh:uid=").withSuffix(uidStr);
					tr.execute(execSnap, snapPayload);
					wait(tr.commit());
					break;
				} catch (Error& e) {
					TraceEvent("SnapCreate").detail("SnapCreateErrorSnapTLogStorage", e.what());
					wait(tr.onError(e));
				}
			}
			self->snapUID = UID::fromString(uidStr.toString());
			wait(delay(30.0));
			tr.reset();
			// read the key SnapFailedTLog.$UID
			loop {
				try {
					Standalone<StringRef> keyStr = LiteralStringRef("SnapFailedTLog.").withSuffix(uidStr);
					TraceEvent("TestKeyStr").detail("Value", keyStr);
					Optional<Value> val = wait(tr.get(keyStr));
					ASSERT(val.present());
					break;
				} catch (Error &e) {
					wait(tr.onError(e));
				}
			}
		} else if (self->testID == 7) {
			// disable popping of TLog and snapshot create with mis-matching
			tr.reset();
			loop {
				// disable pop of the TLog
				try {
					StringRef payLoadRef = LiteralStringRef("empty-binary:uid=f49d27ddf7a28b6549d930743e0ebdbe");
					tr.execute(execDisableTLogPop, payLoadRef);
					wait(tr.commit());
					break;
				} catch (Error& e) {
					wait(tr.onError(e));
				}
			}
			tr.reset();
			uidStr = LiteralStringRef("ba61e9612a561d60bd83ad83e1b63568");
			loop {
				// snap create with different UID
				try {
					Standalone<StringRef> snapPayload = LiteralStringRef("/bin/snap_create.sh:uid=").withSuffix(uidStr);
					tr.execute(execSnap, snapPayload);
					wait(tr.commit());
					break;
				} catch (Error& e) {
					TraceEvent("SnapCreate").detail("SnapCreateErrorSnapTLogStorage", e.what());
					wait(tr.onError(e));
				}
			}
			self->snapUID = UID::fromString(uidStr.toString());
			tr.reset();
			// read the key SnapFailedTLog.$UID
			loop {
				try {
					Standalone<StringRef> keyStr = LiteralStringRef("SnapFailedTLog.").withSuffix(uidStr);
					TraceEvent("TestKeyStr").detail("Value", keyStr);
					Optional<Value> val = wait(tr.get(keyStr));
					ASSERT(val.present());
					break;
				} catch (Error &e) {
					wait(tr.onError(e));
				}
			}
		} else if (self->testID == 8) {
			// create a snapshot with a non whitelisted binary path and operation
			// should fail
			state bool testedFailure = false;
			retry = 0;
			loop {
				self->snapUID = g_random->randomUniqueID();
				try {
					StringRef snapCmdRef = LiteralStringRef("/bin/snap_create1.sh");
					Future<Void> status = snapCreate(cx, snapCmdRef, self->snapUID);
					wait(status);
					break;
				} catch (Error& e) {
					++retry;
					if (retry >= 5) {
						break;
					}
					if (e.code() == error_code_transaction_not_permitted) {
						testedFailure = true;
						break;
					}
				}
			}
			ASSERT(testedFailure == true);
		}
		wait(delay(0.0));
		return Void();
	}

	ACTOR Future<bool> verifyTLogTrackLatest(Database cx, SnapTestWorkload* self, std::string event) {
		TraceEvent("VerifyTLogTrackLatest");
		state StringRef eventTokenRef(event);
		state vector<WorkerInterface> tLogWorkers;
		state std::vector<Future<TraceEventFields>> tLogMessages;
		state std::vector<WorkerDetails> workers = wait(getWorkers(self->dbInfo));
		state std::map<NetworkAddress, WorkerInterface> address_workers;

		for (auto const& worker : workers) {
			address_workers[worker.interf.address()] = worker.interf;
		}
		vector<TLogInterface> tLogServers = self->dbInfo->get().logSystemConfig.allLocalLogs();

		for (auto s : tLogServers) {
			auto it = address_workers.find(s.address());
			if (it != address_workers.end()) {
				tLogWorkers.push_back(it->second);
				TraceEvent("TLogWorker")
				    .detail("Address", s.address())
				    .detail("Id", s.id())
				    .detail("Locality", s.locality.toString());
			}
		}

		state int entryi = 0;
		state int foundTagServers = 0;
		for (; entryi < tLogWorkers.size(); entryi++) {
			tLogMessages.push_back(
			    timeoutError(tLogWorkers[entryi].eventLogRequest.getReply(EventLogRequest(eventTokenRef)), 3.0));

			try {
				TraceEvent(SevDebug, "WaitingForTlogMessages");
				wait(waitForAll(tLogMessages));
			} catch (Error& e) {
				TraceEvent(SevError, "UnableToRetrieveTLogMessages")
					.detail("Token", eventTokenRef.toString())
					.detail("Reason", "FailedToGetTLogMessages")
					.detail("Code", e.what());
				return false;
			}
			printMessages(tLogMessages);
			filterEmptyMessages(tLogMessages);
			if (tLogMessages.size() < 1) {
				TraceEvent("VerifyTLogTrackLatestMessageNotFound")
				    .detail("Address", tLogWorkers[entryi].address())
				    .detail("Token", eventTokenRef.toString());
			} else {
				++foundTagServers;
			}
			tLogMessages.clear();
		}
		// FIXME: logSystemConfig.allLocalLogs returns remote tlogServers also in few cases and hence the test fails.
		// Verify that foundTagServers matches the number of TLogServers in the local region
		if (foundTagServers < 1) {
			TraceEvent(SevError, "VerifyTLogTrackLatestMessageNotReachAllTLogservers")
				.detail("Token", eventTokenRef.toString())
				.detail("FoundaTagServers", foundTagServers);
			return false;
		}
		TraceEvent("VerifyTLogTrackLatestDone");
		return true;
	}
};

WorkloadFactory<SnapTestWorkload> SnapTestWorkloadFactory("SnapTest");
