
#pragma once

#include "Config.h"

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <utility>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <queue>

#pragma warning(push)
#pragma warning(disable: 4251)
#include <dpp/dpp.h>
#pragma warning(pop)

class Blech;

std::string ParseMacroDataString(std::string_view input);

extern std::recursive_mutex globalDataMutex;

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
	DiscordClient(DiscordConfig config, std::vector<ChannelConfig>&& channels);
	~DiscordClient();

	// Queue a message to be sent on all channels
	void EnqueueAll(std::string_view message);

	// Queue a message to be sent on any channel with matching filters
	void EnqueueIfMatch(const std::string& message);

	void HandleBlechEvent(uint32_t id);

	void ProcessEvents();

private:
	// Queue a message to be sent on a specific channel
	void Enqueue(dpp::snowflake channelId, std::string_view message);

	using QueuedEvent = std::function<void()>;
	void PushCallback(QueuedEvent&& cb);


	void AddCommandPermissions(dpp::slashcommand& command);
	bool ValidateCommand(const dpp::command_source& src, bool checkUser = true);

private:
	// Callbacks that occur on the discord thread
	void on_message_create(const dpp::message_create_t& event);
	void on_interaction_create(const dpp::interaction_create_t& event);
	void on_ready(const dpp::ready_t& event);

	void DiscordExecuteCommand(dpp::snowflake id, const std::string& command);

private:
	DiscordConfig m_config;

	// List of user ids allowed to issue commands
	std::vector<dpp::snowflake> m_userIds;

	// List of role ids allowed to issue commands;
	std::vector<dpp::snowflake> m_roleIds;

	// List of configured channels to send/receive messages to/from
	std::map<dpp::snowflake, ChannelConfig> m_channels;

	// Queue of messages to send to discord. Tuple of channelId, messageText
	std::queue<std::pair<dpp::snowflake, std::string>> m_queuedMessages;

	// Parser to match chat text to channels based on allow/block filters
	std::unique_ptr<Blech> m_blech;

	std::atomic_bool m_ready{ false };

	// Mapping from Blech event ID to channel id for all allow events
	std::map<uint32_t, dpp::snowflake> m_blechAllowEvents;

	// Mapping from Blech event ID to channel id for all block events
	std::map<uint32_t, dpp::snowflake> m_blechBlockEvents;

	// Mapping from Blech event ID to channel id for all notify events
	std::map<uint32_t, dpp::snowflake> m_blechNotifyEvents;

	// Channel -> FilterMatch. Cleared before parsing, and populated by the blech match callback.
	std::map<dpp::snowflake, FilterMatch> m_filterMatches;

	// Channel -> Time to stop sending everything in response to a command
	std::map<dpp::snowflake, std::chrono::steady_clock::time_point> m_responseExpiryTimes;
	std::mutex m_responseTimesMutex;

	dpp::cluster m_bot;
	std::thread m_startupThread;

	std::vector<QueuedEvent> m_queuedCallbacks;
	std::mutex m_mutex;
};
