// Copyright (c) 2021- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "Common/Data/Encoding/Base64.h"
#include "Common/Swap.h"
#include "Core/HLE/sceRtc.h"
#include "Core/Core.h"
#include "Core/CoreParameter.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Display.h"
#include "Core/Replay.h"
#include "Core/SaveState.h"
#include "Core/System.h"
#include "Core/Debugger/WebSocket/ReplaySubscriber.h"

DebuggerSubscriber *WebSocketReplayInit(DebuggerEventHandlerMap &map) {
	// No need to bind or alloc state, these are all global.
	map["replay.begin"] = &WebSocketReplayBegin;
	map["replay.abort"] = &WebSocketReplayAbort;
	map["replay.flush"] = &WebSocketReplayFlush;
	map["replay.execute"] = &WebSocketReplayExecute;
	map["replay.status"] = &WebSocketReplayStatus;
	map["replay.time.get"] = &WebSocketReplayTimeGet;
	map["replay.time.set"] = &WebSocketReplayTimeSet;
	map["state.capture"] = &WebSocketStateCapture;
	map["state.restore"] = &WebSocketStateRestore;
	map["state.list"] = &WebSocketStateList;
	map["state.drop"] = &WebSocketStateDrop;
	map["state.clear"] = &WebSocketStateClear;

	return nullptr;
}

// Begin or resume recording of replay data (replay.begin)
//
// If a replay was previously being played back, this will keep any executed replay data up to
// this point for the next flush.  To discard, break the CPU, abort, and then begin.
//
// No parameters.
//
// Response (same event name) with no extra data.
void WebSocketReplayBegin(DebuggerRequest &req) {
	// Replay state is consumed by the CPU thread as it runs, so mutate it over there rather than
	// from this WebSocket handler thread - see Core_RunOnCPUThread() in Core.h. Same for the rest
	// of the handlers below.
	Core_RunOnCPUThread([&] {
		ReplayBeginSave();
	});
	req.Respond();
}

// Abort any replay execution or recording (replay.abort)
//
// This stops executing any replay and discards any in progress recording.
//
// No parameters.
//
// Response (same event name) with no extra data.
void WebSocketReplayAbort(DebuggerRequest &req) {
	Core_RunOnCPUThread([&] {
		ReplayAbort();
	});
	req.Respond();
}

// Flush current recording data (replay.flush)
//
// Flushes event data and returns it.  Note when combining, you must decode first.
//
// No parameters.
//
// Response (same event name):
//  - version: unsigned integer, version number of data.
//  - base64: base64 encode of binary data.
void WebSocketReplayFlush(DebuggerRequest &req) {
	bool running = false;
	std::vector<uint8_t> data;
	Core_RunOnCPUThread([&] {
		running = PSP_GetBootState() == BootState::Complete;
		if (running)
			ReplayFlushBlob(&data);
	});
	if (!running)
		return req.Fail("Game not running");

	JsonWriter &json = req.Respond();
	json.writeInt("version", ReplayVersion());
	json.writeString("base64", Base64Encode(data.data(), data.size()));
}

// Begin executing a replay (replay.execute)
//
// Parameters:
//  - version: unsigned integer, same version from replay.flush.
//  - base64: base64 encoded replay data.
//
// Response (same event name) with no extra data.
void WebSocketReplayExecute(DebuggerRequest &req) {
	uint32_t version = -1;
	if (!req.ParamU32("version", &version))
		return;
	std::string encoded;
	if (!req.ParamString("base64", &encoded))
		return;

	std::vector<uint8_t> data = Base64Decode(encoded.data(), encoded.size());
	bool running = false;
	bool ok = false;
	Core_RunOnCPUThread([&] {
		running = PSP_GetBootState() == BootState::Complete;
		if (running)
			ok = ReplayExecuteBlob(version, data);
	});
	if (!running)
		return req.Fail("Game not running");
	if (!ok)
		return req.Fail("Invalid replay data or version");

	req.Respond();
}

// Get replay status (replay.status)
//
// No parameters.
//
// Response (same event name):
//  - executing: boolean if a replay is being executed.
//  - saving: boolean if a replay is being recorded.
void WebSocketReplayStatus(DebuggerRequest &req) {
	bool executing = false, saving = false;
	Core_RunOnCPUThread([&] {
		executing = ReplayIsExecuting();
		saving = ReplayIsSaving();
	});

	JsonWriter &json = req.Respond();
	json.writeBool("executing", executing);
	json.writeBool("saving", saving);
}

// Get the base RTC (real time clock) time for replay data (replay.time.get)
//
// The base time is constant during a game session, and represents the "power on" time of the
// emulated PSP.
//
// No parameters.
//
// Response (same event name):
//  - value: unsigned integer, may have more than 32 integer bits.
void WebSocketReplayTimeGet(DebuggerRequest &req) {
	bool running = false;
	uint32_t baseTime = 0;
	Core_RunOnCPUThread([&] {
		running = PSP_GetBootState() == BootState::Complete;
		if (running)
			baseTime = RtcBaseTime();
	});
	if (!running)
		return req.Fail("Game not running");

	JsonWriter &json = req.Respond();
	json.writeUint("value", baseTime);
}

// Overwrite the base RTC time (replay.time.set)
//
// Parameters:
//  - value: unsigned integer.
//
// Response (same event name) with no extra data.
void WebSocketReplayTimeSet(DebuggerRequest &req) {
	uint32_t value;
	if (!req.ParamU32("value", &value, false)) {
		return;
	}

	bool running = false;
	Core_RunOnCPUThread([&] {
		running = PSP_GetBootState() == BootState::Complete;
		if (running)
			RtcSetBaseTime((int32_t)value);
	});
	if (!running)
		return req.Fail("Game not running");

	req.Respond();
}


namespace {

constexpr size_t MAX_DEBUGGER_SNAPSHOTS = 32;
constexpr uint64_t MAX_DEBUGGER_SNAPSHOT_BYTES = 1024ULL * 1024ULL * 1024ULL;

struct DebuggerSnapshot {
	std::vector<u8> data;
	std::string gamePath;
	u64 emulatedUs = 0;
	int vcount = 0;
};

std::map<std::string, DebuggerSnapshot> g_debuggerSnapshots;
uint64_t g_debuggerSnapshotBytes = 0;
uint64_t g_nextDebuggerSnapshotId = 1;

std::string NextDebuggerSnapshotId() {
	std::string id;
	do {
		id = "s" + std::to_string(g_nextDebuggerSnapshotId++);
	} while (g_debuggerSnapshots.find(id) != g_debuggerSnapshots.end());
	return id;
}

void WriteDebuggerSnapshot(JsonWriter &json, const std::string &id, const DebuggerSnapshot &snapshot) {
	json.writeString("id", id);
	json.writeFloat("bytes", (double)snapshot.data.size());
	json.writeString("gamePath", snapshot.gamePath);
	json.writeFloat("us", (double)snapshot.emulatedUs);
	json.writeInt("vcount", snapshot.vcount);
}

bool CheckSnapshotGame(DebuggerRequest &req, const DebuggerSnapshot &snapshot) {
	const std::string currentPath = PSP_CoreParameter().fileToStart.ToString();
	if (currentPath != snapshot.gamePath) {
		req.Fail("Snapshot belongs to a different game path");
		return false;
	}
	return true;
}

}  // namespace

// Capture an in-memory emulator checkpoint (state.capture)
//
// The CPU must already be stopped. The serialized state remains inside this PPSSPP process; only
// the small id/metadata response crosses the WebSocket, which makes repeated branch experiments
// cheap. Snapshots survive debugger reconnects but not process exit.
//
// Parameters:
//  - id: optional string. If omitted, PPSSPP generates "s1", "s2", ...
//  - replace: optional boolean, false by default. Required to overwrite an existing id.
//
// Response (same event name):
//  - id, bytes, gamePath, us, vcount: metadata for the captured state.
//  - totalBytes: total memory currently used by debugger snapshots.
//
// Storage is bounded to 32 snapshots and 1 GiB per process.
void WebSocketStateCapture(DebuggerRequest &req) {
	std::string requestedId;
	if (!req.ParamString("id", &requestedId, DebuggerParamType::OPTIONAL))
		return;
	bool replace = false;
	if (!req.ParamBool("replace", &replace, DebuggerParamType::OPTIONAL))
		return;

	Core_RunOnCPUThread([&] {
		if (PSP_GetBootState() != BootState::Complete)
			return req.Fail("Game not running");
		if (coreState != CORE_STEPPING_CPU)
			return req.Fail("CPU must be in CPU stepping mode (cpu.stepping first)");

		std::string id = requestedId.empty() ? NextDebuggerSnapshotId() : requestedId;
		auto existing = g_debuggerSnapshots.find(id);
		if (existing != g_debuggerSnapshots.end() && !replace)
			return req.Fail("Snapshot id already exists (pass replace=true to overwrite it)");
		if (existing == g_debuggerSnapshots.end() && g_debuggerSnapshots.size() >= MAX_DEBUGGER_SNAPSHOTS)
			return req.Fail("Debugger snapshot limit reached (drop or clear snapshots first)");

		std::vector<u8> data;
		const CChunkFileReader::Error result = SaveState::SaveToRam(data);
		if (result != CChunkFileReader::ERROR_NONE)
			return req.Fail("Failed to serialize emulator state");

		const uint64_t oldBytes = existing != g_debuggerSnapshots.end() ? existing->second.data.size() : 0;
		const uint64_t projectedBytes = g_debuggerSnapshotBytes - oldBytes + data.size();
		if (projectedBytes > MAX_DEBUGGER_SNAPSHOT_BYTES)
			return req.Fail("Debugger snapshot memory limit reached (drop or clear snapshots first)");

		DebuggerSnapshot snapshot;
		snapshot.data = std::move(data);
		snapshot.gamePath = PSP_CoreParameter().fileToStart.ToString();
		snapshot.emulatedUs = CoreTiming::GetGlobalTimeUs();
		snapshot.vcount = __DisplayGetVCount();

		g_debuggerSnapshotBytes = projectedBytes;
		g_debuggerSnapshots[id] = std::move(snapshot);

		JsonWriter &json = req.Respond();
		WriteDebuggerSnapshot(json, id, g_debuggerSnapshots[id]);
		json.writeFloat("totalBytes", (double)g_debuggerSnapshotBytes);
	});
}

// Restore an in-memory emulator checkpoint (state.restore)
//
// Parameters:
//  - id: required snapshot id from state.capture.
//
// CPU must already be stopped. The current boot path must match the one captured in the snapshot.
// Response contains the restored snapshot metadata. Debugger-only state such as replay recording,
// breakpoints and the process-level diagnostic VBlank count are intentionally not part of the
// savestate and are therefore not rewound.
void WebSocketStateRestore(DebuggerRequest &req) {
	std::string id;
	if (!req.ParamString("id", &id))
		return;

	Core_RunOnCPUThread([&] {
		if (PSP_GetBootState() != BootState::Complete)
			return req.Fail("Game not running");
		if (coreState != CORE_STEPPING_CPU)
			return req.Fail("CPU must be in CPU stepping mode (cpu.stepping first)");

		auto it = g_debuggerSnapshots.find(id);
		if (it == g_debuggerSnapshots.end())
			return req.Fail("Snapshot id not found");
		if (!CheckSnapshotGame(req, it->second))
			return;

		std::string error;
		const CChunkFileReader::Error result = SaveState::LoadFromRam(it->second.data, &error);
		if (result != CChunkFileReader::ERROR_NONE) {
			if (error.empty())
				error = "unknown savestate error";
			return req.Fail("Failed to restore emulator state: " + error);
		}

		Core_ResetException();

		JsonWriter &json = req.Respond();
		WriteDebuggerSnapshot(json, id, it->second);
		json.writeFloat("currentUs", (double)CoreTiming::GetGlobalTimeUs());
		json.writeInt("currentVcount", __DisplayGetVCount());
	});
}

// List in-memory debugger checkpoints (state.list)
//
// No parameters.
//
// Response:
//  - states: array of snapshot metadata.
//  - totalBytes: total memory used by all snapshots.
void WebSocketStateList(DebuggerRequest &req) {
	Core_RunOnCPUThread([&] {
		JsonWriter &json = req.Respond();
		json.pushArray("states");
		for (const auto &entry : g_debuggerSnapshots) {
			json.pushDict();
			WriteDebuggerSnapshot(json, entry.first, entry.second);
			json.pop();
		}
		json.pop();
		json.writeFloat("totalBytes", (double)g_debuggerSnapshotBytes);
	});
}

// Delete one in-memory debugger checkpoint (state.drop)
//
// Parameters:
//  - id: required snapshot id.
//
// Response has no extra data.
void WebSocketStateDrop(DebuggerRequest &req) {
	std::string id;
	if (!req.ParamString("id", &id))
		return;

	Core_RunOnCPUThread([&] {
		auto it = g_debuggerSnapshots.find(id);
		if (it == g_debuggerSnapshots.end())
			return req.Fail("Snapshot id not found");

		g_debuggerSnapshotBytes -= it->second.data.size();
		g_debuggerSnapshots.erase(it);
		req.Respond();
	});
}

// Delete all in-memory debugger checkpoints (state.clear)
//
// No parameters.
//
// Response:
//  - dropped: number of snapshots removed.
//  - bytes: bytes released.
void WebSocketStateClear(DebuggerRequest &req) {
	Core_RunOnCPUThread([&] {
		const size_t dropped = g_debuggerSnapshots.size();
		const uint64_t bytes = g_debuggerSnapshotBytes;
		g_debuggerSnapshots.clear();
		g_debuggerSnapshotBytes = 0;

		JsonWriter &json = req.Respond();
		json.writeUint("dropped", (uint32_t)dropped);
		json.writeFloat("bytes", (double)bytes);
	});
}
