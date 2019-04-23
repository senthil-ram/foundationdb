#define BOOST_SYSTEM_NO_LIB
#define BOOST_DATE_TIME_NO_LIB
#define BOOST_REGEX_NO_LIB
#include <boost/process.hpp>
#include "fdbserver/FDBExecHelper.actor.h"
#include "flow/Trace.h"
#include "flow/flow.h"
#if defined(CMAKE_BUILD) || !defined(WIN32)
#include "versions.h"
#endif
#include "flow/actorcompiler.h"  // This must be the last #include.

ExecCmdValueString::ExecCmdValueString(StringRef pCmdValueString) {
	cmdValueString = pCmdValueString;
	parseCmdValue();
}

void ExecCmdValueString::setCmdValueString(StringRef pCmdValueString) {
	// reset everything
	binaryPath = StringRef();
	keyValueMap.clear();

	// set the new cmdValueString
	cmdValueString = pCmdValueString;

	// parse it out
	parseCmdValue();
}

StringRef ExecCmdValueString::getCmdValueString() {
	return cmdValueString.toString();
}

StringRef ExecCmdValueString::getBinaryPath() {
	return binaryPath;
}

VectorRef<StringRef> ExecCmdValueString::getBinaryArgs() {
	return binaryArgs;
}

StringRef ExecCmdValueString::getBinaryArgValue(StringRef key) {
	StringRef res;
	if (keyValueMap.find(key) != keyValueMap.end()) {
		res = keyValueMap[key];
	}
	return res;
}

void ExecCmdValueString::parseCmdValue() {
	StringRef param = this->cmdValueString;
	// get the binary path
	this->binaryPath = param.eat(LiteralStringRef(":"));

	// no arguments provided
	if (param == StringRef()) {
		return;
	}

	// extract the arguments
	while (param != StringRef()) {
		StringRef token = param.eat(LiteralStringRef(","));
		this->binaryArgs.push_back(this->binaryArgs.arena(), token);

		StringRef key = token.eat(LiteralStringRef("="));
		keyValueMap.insert(std::make_pair(key, token));
	}
	return;
}

void ExecCmdValueString::dbgPrint() {
	auto te = TraceEvent("ExecCmdValueString");

	te.detail("CmdValueString", cmdValueString.toString());
	te.detail("BinaryPath", binaryPath.toString());

	int i = 0;
	for (auto elem : binaryArgs) {
		te.detail(format("Arg", ++i).c_str(), elem.toString());
	}
	return;
}

ACTOR Future<int> spawnProcess(std::string binPath, std::vector<std::string> paramList, double maxWaitTime, bool isSync)
{
	state std::string argsString;
	for (auto const& elem : paramList) {
		argsString += elem + ",";
	}
	TraceEvent("SpawnProcess").detail("Cmd", binPath).detail("Args", argsString);

	state int err = 0;
	state double runTime = 0;
	state boost::process::child c(binPath, boost::process::args(paramList),
								  boost::process::std_err > boost::process::null);
	if (!isSync) {
		while (c.running() && runTime <= maxWaitTime) {
			wait(delay(0.1));
			runTime += 0.1;
		}
		if (c.running()) {
			c.terminate();
			err = -1;
		} else {
			err = c.exit_code();
		}
		if (!c.wait_for(std::chrono::seconds(1))) {
			TraceEvent(SevWarnAlways, "SpawnProcessFailedToExit")
				.detail("Cmd", binPath)
				.detail("Args", argsString);
		}
	} else {
		state std::error_code errCode;
		bool succ = c.wait_for(std::chrono::seconds(3), errCode);
		err = errCode.value();
		if (!succ) {
			err = -1;
			c.terminate();
			if (!c.wait_for(std::chrono::seconds(1))) {
				TraceEvent(SevWarnAlways, "SpawnProcessFailedToExit")
					.detail("Cmd", binPath)
					.detail("Args", argsString);
			}
		}
	}
	TraceEvent("SpawnProcess")
		.detail("Cmd", binPath)
		.detail("Error", err);
	return err;
}

ACTOR Future<int> execHelper(ExecCmdValueString* execArg, std::string folder, std::string role) {
	state StringRef uidStr = execArg->getBinaryArgValue(LiteralStringRef("uid"));
	state int err = 0;
	state Future<int> cmdErr;
	if (!g_network->isSimulated()) {
		// get bin path
		auto snapBin = execArg->getBinaryPath();
		auto dataFolder = "path=" + folder;
		std::vector<std::string> paramList;
		paramList.push_back(snapBin.toString());
		// get user passed arguments
		auto listArgs = execArg->getBinaryArgs();
		for (auto elem : listArgs) {
			paramList.push_back(elem.toString());
		}
		// get additional arguments
		paramList.push_back(dataFolder);
		const char* version = FDB_VT_VERSION;
		std::string versionString = "version=";
		versionString += version;
		paramList.push_back(versionString);
		paramList.push_back(role);
		cmdErr = spawnProcess(snapBin.toString(), paramList, 3.0, false /*isSync*/);
		wait(success(cmdErr));
		err = cmdErr.get();
	} else {
		// copy the files
		state std::string folderFrom = "./" + folder + "/.";
		state std::string folderTo = "./" + folder + "-snap-" + uidStr.toString();
		std::vector<std::string> paramList;
		std::string mkdirBin = "/bin/mkdir";
		paramList.push_back(folderTo);
		cmdErr = spawnProcess(mkdirBin, paramList, 3.0, false /*isSync*/);
		wait(success(cmdErr));
		err = cmdErr.get();
		if (err == 0) {
			std::vector<std::string> paramList;
			std::string cpBin = "/bin/cp";
			paramList.push_back("-a");
			paramList.push_back(folderFrom);
			paramList.push_back(folderTo);
			cmdErr = spawnProcess(cpBin, paramList, 3.0, true /*isSync*/);
			wait(success(cmdErr));
			err = cmdErr.get();
		}
	}
	return err;
}

std::map<NetworkAddress, std::set<UID>> execOpsInProgress;

bool isExecOpInProgress(UID execUID) {
	NetworkAddress addr = g_network->getLocalAddress();
	return (execOpsInProgress[addr].find(execUID) != execOpsInProgress[addr].end());
}

void setExecOpInProgress(UID execUID) {
	NetworkAddress addr = g_network->getLocalAddress();
	ASSERT(execOpsInProgress[addr].find(execUID) == execOpsInProgress[addr].end());
	execOpsInProgress[addr].insert(execUID);
	return;
}

void clearExecOpInProgress(UID execUID) {
	NetworkAddress addr = g_network->getLocalAddress();
	ASSERT(execOpsInProgress[addr].find(execUID) != execOpsInProgress[addr].end());
	execOpsInProgress[addr].erase(execUID);
	return;
}

std::map<NetworkAddress, std::set<UID>> tLogsAlive;

void registerTLog(UID uid) {
	NetworkAddress addr = g_network->getLocalAddress();
	tLogsAlive[addr].insert(uid);
}
void unregisterTLog(UID uid) {
	NetworkAddress addr = g_network->getLocalAddress();
	if (tLogsAlive[addr].find(uid) != tLogsAlive[addr].end()) {
		tLogsAlive[addr].erase(uid);
	}
}
bool isTLogInSameNode() {
	NetworkAddress addr = g_network->getLocalAddress();
	return tLogsAlive[addr].size() >= 1;
}
