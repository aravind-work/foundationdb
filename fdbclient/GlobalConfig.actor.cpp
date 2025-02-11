/*
 * GlobalConfig.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbclient/DatabaseContext.h"
#include "fdbclient/GlobalConfig.actor.h"
#include "fdbclient/SpecialKeySpace.actor.h"
#include "fdbclient/SystemData.h"
#include "fdbclient/Tuple.h"
#include "flow/flow.h"
#include "flow/genericactors.actor.h"

#include "flow/actorcompiler.h" // This must be the last #include.

const KeyRef fdbClientInfoTxnSampleRate = LiteralStringRef("config/fdb_client_info/client_txn_sample_rate");
const KeyRef fdbClientInfoTxnSizeLimit = LiteralStringRef("config/fdb_client_info/client_txn_size_limit");

const KeyRef transactionTagSampleRate = LiteralStringRef("config/transaction_tag_sample_rate");
const KeyRef transactionTagSampleCost = LiteralStringRef("config/transaction_tag_sample_cost");

const KeyRef samplingFrequency = LiteralStringRef("visibility/sampling/frequency");
const KeyRef samplingWindow = LiteralStringRef("visibility/sampling/window");

GlobalConfig::GlobalConfig(DatabaseContext* cx) : cx(cx), lastUpdate(0) {}

void GlobalConfig::applyChanges(Transaction& tr,
                                const VectorRef<KeyValueRef>& insertions,
                                const VectorRef<KeyRangeRef>& clears) {
	VersionHistory vh{ 0 };
	for (const auto& kv : insertions) {
		vh.mutations.emplace_back_deep(vh.mutations.arena(), MutationRef(MutationRef::SetValue, kv.key, kv.value));
		tr.set(kv.key.withPrefix(globalConfigKeysPrefix), kv.value);
	}
	for (const auto& range : clears) {
		vh.mutations.emplace_back_deep(vh.mutations.arena(),
		                               MutationRef(MutationRef::ClearRange, range.begin, range.end));
		tr.clear(
		    KeyRangeRef(range.begin.withPrefix(globalConfigKeysPrefix), range.end.withPrefix(globalConfigKeysPrefix)));
	}

	// Record the mutations in this commit into the global configuration history.
	Key historyKey = addVersionStampAtEnd(globalConfigHistoryPrefix);
	ObjectWriter historyWriter(IncludeVersion());
	historyWriter.serialize(vh);
	tr.atomicOp(historyKey, historyWriter.toStringRef(), MutationRef::SetVersionstampedKey);

	// Write version key to trigger update in cluster controller.
	tr.atomicOp(globalConfigVersionKey,
	            LiteralStringRef("0123456789\x00\x00\x00\x00"), // versionstamp
	            MutationRef::SetVersionstampedValue);
}

Key GlobalConfig::prefixedKey(KeyRef key) {
	return key.withPrefix(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::GLOBALCONFIG).begin);
}

const Reference<ConfigValue> GlobalConfig::get(KeyRef name) {
	auto it = data.find(name);
	if (it == data.end()) {
		return Reference<ConfigValue>();
	}
	return it->second;
}

const std::map<KeyRef, Reference<ConfigValue>> GlobalConfig::get(KeyRangeRef range) {
	std::map<KeyRef, Reference<ConfigValue>> results;
	for (const auto& [key, value] : data) {
		if (range.contains(key)) {
			results[key] = value;
		}
	}
	return results;
}

Future<Void> GlobalConfig::onInitialized() {
	return initialized.getFuture();
}

Future<Void> GlobalConfig::onChange() {
	return configChanged.onTrigger();
}

void GlobalConfig::trigger(KeyRef key, std::function<void(std::optional<std::any>)> fn) {
	callbacks.emplace(key, std::move(fn));
}

void GlobalConfig::insert(KeyRef key, ValueRef value) {
	// TraceEvent(SevInfo, "GlobalConfigInsert").detail("Key", key).detail("Value", value);
	data.erase(key);

	Arena arena(key.expectedSize() + value.expectedSize());
	KeyRef stableKey = KeyRef(arena, key);
	try {
		std::any any;
		Tuple t = Tuple::unpack(value);
		if (t.getType(0) == Tuple::ElementType::UTF8) {
			any = StringRef(arena, t.getString(0).contents());
		} else if (t.getType(0) == Tuple::ElementType::INT) {
			any = t.getInt(0);
		} else if (t.getType(0) == Tuple::ElementType::BOOL) {
			any = t.getBool(0);
		} else if (t.getType(0) == Tuple::ElementType::FLOAT) {
			any = t.getFloat(0);
		} else if (t.getType(0) == Tuple::ElementType::DOUBLE) {
			any = t.getDouble(0);
		} else if (t.getType(0) == Tuple::ElementType::VERSIONSTAMP) {
			any = t.getVersionstamp(0);
		} else {
			ASSERT(false);
		}
		data[stableKey] = makeReference<ConfigValue>(std::move(arena), std::move(any));

		if (callbacks.find(stableKey) != callbacks.end()) {
			callbacks[stableKey](data[stableKey]->value);
		}
	} catch (Error& e) {
		TraceEvent(SevWarn, "GlobalConfigTupleParseError").detail("What", e.what());
	}
}

void GlobalConfig::erase(Key key) {
	erase(KeyRangeRef(key, keyAfter(key)));
}

void GlobalConfig::erase(KeyRangeRef range) {
	// TraceEvent(SevInfo, "GlobalConfigErase").detail("Range", range);
	auto it = data.begin();
	while (it != data.end()) {
		if (range.contains(it->first)) {
			if (callbacks.find(it->first) != callbacks.end()) {
				callbacks[it->first](std::nullopt);
			}
			it = data.erase(it);
		} else {
			++it;
		}
	}
}

// Updates local copy of global configuration by reading the entire key-range
// from storage (proxied through the GrvProxies).
ACTOR Future<Void> GlobalConfig::refresh(GlobalConfig* self, Version lastKnown) {
	// TraceEvent trace(SevInfo, "GlobalConfigRefresh");
	self->erase(KeyRangeRef(""_sr, "\xff"_sr));

	state Backoff backoff(CLIENT_KNOBS->GLOBAL_CONFIG_REFRESH_BACKOFF, CLIENT_KNOBS->GLOBAL_CONFIG_REFRESH_MAX_BACKOFF);
	loop {
		try {
			GlobalConfigRefreshReply reply =
			    wait(timeoutError(basicLoadBalance(self->cx->getGrvProxies(UseProvisionalProxies::False),
			                                       &GrvProxyInterface::refreshGlobalConfig,
			                                       GlobalConfigRefreshRequest{ lastKnown }),
			                      CLIENT_KNOBS->GLOBAL_CONFIG_REFRESH_TIMEOUT));
			for (const auto& kv : reply.result) {
				KeyRef systemKey = kv.key.removePrefix(globalConfigKeysPrefix);
				self->insert(systemKey, kv.value);
			}
			return Void();
		} catch (Error& e) {
			wait(backoff.onError());
		}
	}
}

// Applies updates to the local copy of the global configuration when this
// process receives an updated history.
ACTOR Future<Void> GlobalConfig::updater(GlobalConfig* self, const ClientDBInfo* dbInfo) {
	loop {
		try {
			if (self->initialized.canBeSet()) {
				wait(self->cx->onConnected());

				wait(self->refresh(self, -1));
				self->initialized.send(Void());
			}

			loop {
				try {
					wait(self->dbInfoChanged.onTrigger());

					auto& history = dbInfo->history;
					if (history.size() == 0) {
						continue;
					}

					if (self->lastUpdate < history[0].version) {
						// This process missed too many global configuration
						// history updates or the protocol version changed, so it
						// must re-read the entire configuration range.
						wait(self->refresh(self, history.back().version));
						if (dbInfo->history.size() > 0) {
							self->lastUpdate = dbInfo->history.back().version;
						}
					} else {
						// Apply history in order, from lowest version to highest
						// version. Mutation history should already be stored in
						// ascending version order.
						for (const auto& vh : history) {
							if (vh.version <= self->lastUpdate) {
								continue; // already applied this mutation
							}

							for (const auto& mutation : vh.mutations.contents()) {
								if (mutation.type == MutationRef::SetValue) {
									self->insert(mutation.param1, mutation.param2);
								} else if (mutation.type == MutationRef::ClearRange) {
									self->erase(KeyRangeRef(mutation.param1, mutation.param2));
								} else {
									ASSERT(false);
								}
							}

							ASSERT(vh.version > self->lastUpdate);
							self->lastUpdate = vh.version;
						}
					}

					self->configChanged.trigger();
				} catch (Error& e) {
					throw;
				}
			}
		} catch (Error& e) {
			// There shouldn't be any uncaught errors that fall to this point,
			// but in case there are, catch them and restart the updater.
			TraceEvent("GlobalConfigUpdaterError").error(e);
			wait(delay(1.0));
		}
	}
}
