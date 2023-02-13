
#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <utility>
#include <vector>
#include <mutex>
#include <queue>

#pragma warning(push)
#pragma warning(disable: 4267)
#define NONEXISTENT_OPUS
#include <sleepy_discord\sleepy_discord.h>
#pragma warning(pop)

#include "Config.h"
#include "Blech/Blech.h"

unsigned int __stdcall MQ2DataVariableLookup(char * VarName, char * Value, size_t ValueLen);

namespace MQ2Discord {

enum class FilterMatch
{
	None,
	Allow,
	Block,
	Notify
};

class DiscordClient
{
public:
	DiscordClient(std::string token,
		std::vector<std::string> userIds,
		std::vector<ChannelConfig> channels,
		std::function<void(std::string command)> executeCommand,
		std::function<std::string(std::string input)> parseMacroData,
		void(*writeError)(const char* format, ...),
		void(*writeWarning)(const char* format, ...),
		void(*writeNormal)(const char* format, ...),
		void(*writeDebug)(const char* format, ...));

	~DiscordClient();

	/// Queue a message to be sent on all channels
	void enqueueAll(std::string message);

	void Stop();

	bool IsStopped() const { return _stopped; }

	// Queue a message to be sent on any channel with matching filters
	void enqueueIfMatch(std::string message);

private:
	// Queue a message to be sent on a specific channel
	void enqueue(const std::string& channelId, const std::string& message);

	// Callback function for blech parser match
	static void __stdcall blechMatch(unsigned int ID, void* pData, PBLECHVALUE pValues);

	void onMessageReceived(SleepyDiscord::Message& message);

	void threadStart();

private:
	// Discord API token
	const std::string _token;

	// List of user ids allowed to issue commands
	const std::vector<std::string> _userIds;

	// List of configured channels to send/receive messages to/from
	const std::vector<ChannelConfig> _channels;

	// Function to parse a string containing MQ2 variables
	const std::function<std::string(std::string input)> _parseMacroData;

	// Function to execute an ingame command. Must be threadsafe as it won't be invoked from the main thread
	const std::function<void(std::string command)> _executeCommand;

	// Function to write an error message to ingame chat. Must be threadsafe
	void(*const _writeError)(const char * format, ...);

	// Function to write an warning message to ingame chat. Must be threadsafe
	void(*const _writeWarning)(const char * format, ...);

	// Function to write an regular message to ingame chat. Must be threadsafe
	void(*const _writeNormal)(const char * format, ...);

	// Function to write an debug message to ingame chat. Must be threadsafe
	void(*const _writeDebug)(const char * format, ...);

	// Background thread to handle discord communications. Will run for the lifetime of this class
	std::thread _thread;

	// Stop signal	for the background thread
	std::atomic<bool> _stop;

	// Determine if the thread is actually stopped.
	std::atomic<bool> _stopped;

	// Queue of messages to send to discord. Tuple of channelId, messageText
	std::queue<std::tuple<std::string, std::string>> _messages;

	// Sync mutex for access to _messages
	std::mutex _messagesMutex;

	// Parser to match chat text to channels based on allow/block filters
	Blech _blech;

	// Mapping from Blech event ID to channel for all allow events
	std::map<unsigned int, const ChannelConfig *> _blechAllowEvents;

	// Mapping from Blech event ID to channel for all block events
	std::map<unsigned int, const ChannelConfig *> _blechBlockEvents;

	// Mapping from Blech event ID to channel for all notify events
	std::map<unsigned int, const ChannelConfig *> _blechNotifyEvents;

	// Channel -> FilterMatch. Cleared before parsing, and populated by the blech match callback.
	std::map<const ChannelConfig *, FilterMatch> _filterMatches;

	// Channel -> Time to stop sending everything in response to a command
	std::map<const ChannelConfig *, std::chrono::time_point<std::chrono::system_clock>> _responseExpiryTimes;
};

} // namespace MQ2Discord
