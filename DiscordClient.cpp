
#include "DiscordClient.h"
#include "Blech/Blech.h"

#include <mq/Plugin.h>
#include <spdlog/spdlog.h>

#include <string_view>
#include <sstream>

uint32_t __stdcall MQ2DataVariableLookup(char* VarName, char* Value, size_t ValueLen);

// Callback function for blech parser match
static void __stdcall BlechMatch(unsigned int ID, void* pData, PBLECHVALUE pValues);

using CommandCallback = void (DiscordClient::*)(const std::string&, const dpp::parameter_list_t&, const dpp::command_source&);

template <typename OutIter>
void StripMQChat(std::string_view in, OutIter out)
{
	size_t i = 0;

	while (i < in.size() && in[i])
	{
		if (in[i] == '\a')
		{
			i++;
			if (in[i] == '-')
			{
				i++;
			}
			else if (in[i] == '#')
			{
				i += 6;
			}
		}
		else if (in[i] == '\n')
		{
		}
		else
		{
			*out++ = in[i];
		}

		i++;
	}
}

// Escapes special discord characters
static std::string escape_markdown(const std::string& s)
{
	fmt::memory_buffer buf;
	fmt::appender itr(buf);

	for (char c : s)
	{
		if (c == '`' || c == '*' || c == '_' || c == '~' || c == '\\')
			*itr++ = '\\';

		*itr++ = c;
	}

	fmt::memory_buffer stripped;
	fmt::appender itr2(stripped);

	StripMQChat(std::string_view{ buf.data(), buf.size() }, itr2);

	return fmt::to_string(stripped);
}

std::string ParseMacroDataSync(std::string_view input)
{
	if (input.empty())
		return std::string();

	std::unique_lock lock(globalDataMutex);

	char buffer[MAX_STRING] = { 0 };
	strncpy_s(buffer, input.data(), input.size());

	ParseMacroData(buffer, MAX_STRING);
	return buffer;
}

//----------------------------------------------------------------------------

DiscordClient::DiscordClient(DiscordConfig config, std::vector<ChannelConfig>&& channels)
	: m_config(std::move(config))
	, m_bot(m_config.token, dpp::i_default_intents | dpp::i_message_content,
		0 /* shards */, 0 /* cluser_id */, 1 /* maxclusters */, true /* compressed */,
		{ dpp::cp_aggressive, dpp::cp_aggressive, dpp::cp_aggressive } /* policy */,
		1 /* request_threads */, 1 /* request_threads_raw */)
{
	for (ChannelConfig& config : channels)
	{
		m_channels.emplace(dpp::snowflake(config.id), std::move(config));
	}

	m_userIds.reserve(config.user_ids.size());
	for (const std::string& userId : config.user_ids)
	{
		m_userIds.push_back(dpp::snowflake(userId));
	}

	m_blech = std::make_unique<Blech>('#', '|', MQ2DataVariableLookup);

	// Add events to the parser and store the id/channel in the appropriate map
	for (const auto& [id, config] : m_channels)
	{
		for (const std::string& allow : config.allowed)
			m_blechAllowEvents[m_blech->AddEvent(allow.c_str(), BlechMatch, this)] = id;
		for (const std::string& block : config.blocked)
			m_blechBlockEvents[m_blech->AddEvent(block.c_str(), BlechMatch, this)] = id;
		for (const std::string& notify : config.notify)
			m_blechNotifyEvents[m_blech->AddEvent(notify.c_str(), BlechMatch, this)] = id;
	}

	m_bot.on_log([this](const dpp::log_t& event) {
		switch (event.severity) {
		case dpp::ll_trace:
			SPDLOG_TRACE("{}", event.message);
			break;
		case dpp::ll_debug:
			SPDLOG_DEBUG("{}", event.message);
			break;
		case dpp::ll_info:
			SPDLOG_INFO("{}", event.message);
			break;
		case dpp::ll_warning:
			SPDLOG_WARN("{}", event.message);
			break;
		case dpp::ll_error:
			SPDLOG_ERROR("{}", event.message);
			break;
		case dpp::ll_critical:
		default:
			SPDLOG_CRITICAL("{}", event.message);
			break;
		}
	});

	m_bot.on_ready([this](const dpp::ready_t& event) { on_ready(event); });
	m_bot.on_message_create([this](const dpp::message_create_t& event) { on_message_create(event); });
	m_bot.on_interaction_create([this](const dpp::interaction_create_t& event) { on_interaction_create(event); });

	m_startupThread = std::thread([this]() { m_bot.start(true); });
}

DiscordClient::~DiscordClient()
{
	if (m_startupThread.joinable())
	m_startupThread.join();
}

void DiscordClient::PushCallback(QueuedEvent&& cb)
{
	std::unique_lock lock(m_mutex);
	m_queuedCallbacks.push_back(std::move(cb));
}

void DiscordClient::ProcessEvents()
{
	std::unique_lock lock(m_mutex);

	if (!m_queuedCallbacks.empty())
	{
		std::vector<QueuedEvent> tempEvents;
		std::swap(tempEvents, m_queuedCallbacks);

		lock.unlock();

		for (const auto& callback : tempEvents)
		{
			callback();
		}

		lock.lock();
	}

	if (!m_queuedMessages.empty())
	{
		// grab all queued messages, put them into a map of channel -> combined message
		std::map<dpp::snowflake, fmt::memory_buffer> combinedMessages;

		while (!m_queuedMessages.empty())
		{
			const auto& [channelId, message] = m_queuedMessages.front();
			auto& buffer = combinedMessages[channelId];

			if (buffer.size() > 1800)
				continue;

			fmt::format_to(fmt::appender(buffer), "{}\n", message);
			m_queuedMessages.pop();
		}

		for (const auto& [channelId, messageBuffer] : combinedMessages)
		{
			std::string formattedMessage = fmt::to_string(messageBuffer);

			SPDLOG_DEBUG("Sending message to channel {}: {}", channelId, formattedMessage);
			m_bot.message_create(dpp::message(channelId, formattedMessage));
		}
	}
}

void DiscordClient::AddCommandPermissions(dpp::slashcommand& command)
{
	command.disable_default_permissions();

	for (dpp::snowflake id : m_userIds)
		command.add_permission(dpp::command_permission(id, dpp::cpt_user, true));

	for (dpp::snowflake id : m_roleIds)
		command.add_permission(dpp::command_permission(id, dpp::cpt_role, true));
}

void DiscordClient::on_ready(const dpp::ready_t& event)
{
	m_ready.store(true);
	SPDLOG_DEBUG("Bot is ready");

	std::vector<dpp::snowflake> channels;
	for (const auto& [id, channel] : m_channels)
	{
		if (channel.send_connected)
		{
			if (std::find(std::begin(channels), std::end(channels), id) == std::end(channels))
			{
				channels.push_back(id);
				m_bot.message_create(dpp::message(id, "Connected"));
			}
		}
	}

	// /status
	dpp::slashcommand status;
	status.set_name("status")
		.set_description("Get current discord plugin status")
		.set_application_id(m_bot.me.id);
	AddCommandPermissions(status);
	m_bot.global_command_create(status);

	// /echo
	dpp::slashcommand echo;
	echo.set_name("echo")
		.set_description("Echo a string from the game client")
		.set_application_id(m_bot.me.id)
		.add_option(
			dpp::command_option(dpp::co_string, "text", "The text string to echo from the command", true)
		);
	AddCommandPermissions(echo);
	m_bot.global_command_create(echo);

	// /cmd
	dpp::slashcommand cmd;
	cmd.set_name("cmd")
		.set_description("Run a command from the game client")
		.set_application_id(m_bot.me.id)
		.add_option(
			dpp::command_option(dpp::co_string, "command", "The slash command to execute in the game client", true)
		);
	AddCommandPermissions(cmd);
	m_bot.global_command_create(cmd);
}

void DiscordClient::on_interaction_create(const dpp::interaction_create_t& event)
{
	if (event.command.type == dpp::it_application_command)
	{
		// The command validates the user permission. This validates the channel.
		auto iter = m_channels.find(event.command.channel_id);
		if (iter == m_channels.end())
		{

		}

		auto& channel = iter->second;
		const dpp::command_interaction& cmd_data = std::get<dpp::command_interaction>(event.command.data);

		if (cmd_data.name == "status")
		{
			fmt::memory_buffer buffer;
			fmt::format_to(fmt::appender(buffer), "{}Status: Conected", ParseMacroDataSync(channel.prefix));

			event.reply(dpp::ir_channel_message_with_source, fmt::to_string(buffer));
			return;
		}

		if (cmd_data.name == "echo")
		{
			const std::string& command = std::get<std::string>(event.get_parameter("text"));

			fmt::memory_buffer buffer;
			fmt::format_to(fmt::appender(buffer), "{}{}", ParseMacroDataSync(channel.prefix), ParseMacroDataSync(command));

			event.reply(dpp::ir_channel_message_with_source, fmt::to_string(buffer));
			return;
		}

		if (cmd_data.name == "cmd")
		{
			const std::string& argument = std::get<std::string>(event.get_parameter("command"));

			if (!channel.allow_commands)
			{
				fmt::memory_buffer buffer;
				fmt::format_to(fmt::appender(buffer), "{}Commands are not allowed on this channel",
					ParseMacroDataSync(channel.prefix));

				event.reply(fmt::to_string(buffer));
				SPDLOG_WARN("Command received on channel with commands disabled: {}", event.command.channel_id);
				return;
			}

			DiscordExecuteCommand(event.command.channel_id, argument);
			return;
		}

		return;
	}
}

void DiscordClient::on_message_create(const dpp::message_create_t& event)
{
	// Don't respond to our own messages
	if (event.msg.author.id == m_bot.me.id)
	{
		return;
	}

	// handle only bot commands here
	if (event.msg.content.length() <= 1
		|| (event.msg.content[0] != '!' && event.msg.content[0] != '/'))
	{
		return;
	}

	dpp::snowflake id = event.msg.channel_id;
	dpp::snowflake msg_id = event.msg.id;

	auto iter = m_channels.find(id);
	if (iter == m_channels.end())
	{
		// For better or worse, this happens a fair bit due to this class only knowing about channels the current character is a part of
		// Will always happen when a command is issued to another character
		return;
	}

	auto& channel = iter->second;

	if (std::find(std::begin(m_userIds), std::end(m_userIds), event.msg.author.id) == std::end(m_userIds))
	{
		fmt::memory_buffer buffer;
		fmt::format_to(fmt::appender(buffer), "{}You are not authorized to issue commands on this channel",
			ParseMacroDataSync(channel.prefix));

		event.reply(fmt::to_string(buffer));
		SPDLOG_WARN("Command received on channel {} from unauthorized user {}", id, event.msg.author.username);

		return;
	}

	if (event.msg.content == "!status")
	{
		fmt::memory_buffer buffer;
		fmt::format_to(fmt::appender(buffer), "{}Status: Conected", ParseMacroDataSync(channel.prefix));

		event.reply(fmt::to_string(buffer));
		return;
	}

	if (starts_with(event.msg.content, "!echo "))
	{
		std::string_view command = std::string_view{ event.msg.content }.substr(6);

		fmt::memory_buffer buffer;
		fmt::format_to(fmt::appender(buffer), "{}{}", ParseMacroDataSync(channel.prefix), ParseMacroDataSync(command));

		event.reply(fmt::to_string(buffer));
		return;
	}

	// Ingame commands
	if (starts_with(event.msg.content, "/"))
	{
		if (!channel.allow_commands)
		{
			fmt::memory_buffer buffer;
			fmt::format_to(fmt::appender(buffer), "{}Commands are not allowed on this channel",
				ParseMacroDataSync(channel.prefix));

			event.reply(fmt::to_string(buffer));
			SPDLOG_WARN("Command received on channel with commands disabled: {}", channel.id);
			return;
		}

		DiscordExecuteCommand(id, event.msg.content);
		return;
	}
}

void DiscordClient::DiscordExecuteCommand(dpp::snowflake id, const std::string& command)
{
	auto iter = m_channels.find(id);
	if (iter == m_channels.end())
		return;

	auto& channel = iter->second;

	if (!channel.allow_commands)
		return;

	if (channel.show_command_response > 0)
	{
		std::unique_lock lock(m_responseTimesMutex);

		m_responseExpiryTimes[id] = std::chrono::steady_clock::now()
			+ std::chrono::milliseconds(channel.show_command_response);
	}

	{
		std::unique_lock lock(globalDataMutex);

		//m_executeCommand(unescape_json(message.content));
		EzCommand(command.c_str());
	}
}

void DiscordClient::EnqueueIfMatch(const std::string& message)
{
	// Clear results & set every channel to no match initially
	m_filterMatches.clear();

	for (const auto& [id, _] : m_channels)
		m_filterMatches[id] = FilterMatch::None;

	// Feed the message through the parser. Colour codes are removed beforehand.
	char buffer[2048] = { 0 };
	StripMQChat(message.c_str(), buffer);
	m_blech->Feed(buffer);

	// Send to any channels that matched
	for (const auto& [id, filter] : m_filterMatches)
	{
		auto& channel = m_channels[id];

		if (channel.show_command_response > 0)
		{
			std::unique_lock lock(m_responseTimesMutex);

			if (m_responseExpiryTimes.find(id) != m_responseExpiryTimes.end()
				&& m_responseExpiryTimes[id] > std::chrono::steady_clock::now())
			{
				Enqueue(id, ParseMacroDataSync(channel.prefix) + escape_markdown(message));
				continue;
			}
		}

		if (filter == FilterMatch::Allow)
		{
			Enqueue(id, ParseMacroDataSync(channel.prefix) + escape_markdown(message));
			continue;
		}
		
		if (filter == FilterMatch::Notify)
		{
			Enqueue(id, ParseMacroDataSync(channel.prefix) + escape_markdown(message) + " @everyone");
			continue;
		}
	}
}

void DiscordClient::EnqueueAll(std::string_view message)
{
	std::unique_lock lock(m_mutex);

	for (const auto& [id, channel] : m_channels)
	{
		fmt::memory_buffer buffer;
		fmt::format_to(fmt::appender(buffer), "{}{}", ParseMacroDataSync(channel.prefix), message);

		m_queuedMessages.emplace(id, fmt::to_string(buffer));
	}
}

void DiscordClient::Enqueue(dpp::snowflake channelId, std::string_view message)
{
	std::unique_lock lock(m_mutex);

	m_queuedMessages.emplace(channelId, std::string(message));
}

// Callback function for blech parser match
void __stdcall BlechMatch(unsigned int ID, void* pData, PBLECHVALUE pValues)
{
	auto pClient = static_cast<DiscordClient*>(pData);
	pClient->HandleBlechEvent(ID);
}

void DiscordClient::HandleBlechEvent(uint32_t ID)
{
	// If it matched a block filter, mark it as blocked regardless of what it was before
	auto blockEvent = m_blechBlockEvents.find(ID);
	if (blockEvent != m_blechBlockEvents.end())
	{
		m_filterMatches[blockEvent->second] = FilterMatch::Block;
		return;
	}

	// If it matches a notify, mark it to notify unless it's already blocked
	auto notifyEvent = m_blechNotifyEvents.find(ID);
	if (notifyEvent != m_blechNotifyEvents.end() && m_filterMatches[notifyEvent->second] != FilterMatch::Block)
	{
		m_filterMatches[notifyEvent->second] = FilterMatch::Notify;
		return;
	}

	// Otherwise, it's matched an allow event, so set to allow unless it's already block or notify
	dpp::snowflake channelId = m_blechAllowEvents[ID];
	if (m_filterMatches[channelId] == FilterMatch::None)
		m_filterMatches[channelId] = FilterMatch::Allow;
}
