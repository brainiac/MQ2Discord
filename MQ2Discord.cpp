
#include "DiscordClient.h"
#include "Config.h"

#include <fstream>
#include <regex>
#include <yaml-cpp/yaml.h>
#include <filesystem>

#include <mq/Plugin.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/msvc_sink.h>

PreSetup("MQ2Discord");
PLUGIN_VERSION(2.0);

#define PLUGIN_MSG_LOG(x) x "[Discord]\ax "

void DiscordCmd(PSPAWNINFO pChar, PCHAR szLine);
void Reload();

static bool s_ourMessage = false;
static bool s_showDebug = true;
static std::unique_ptr<DiscordClient> s_client;

std::queue<std::string> messages;
std::mutex messagesMutex;
DWORD mainThreadId;

std::recursive_mutex globalDataMutex;

template<typename M>
class unique_unlock
{
public:
	unique_unlock(M& m) noexcept(false) : m_(m) {
		m_.unlock();
	}
	~unique_unlock() {
		m_.lock();
	}
private:
	M& m_;
};

class WriteChatSink : public spdlog::sinks::base_sink<spdlog::details::null_mutex>
{
protected:
	void sink_it_(const spdlog::details::log_msg& msg) override
	{
		using namespace spdlog;

		fmt::memory_buffer formatted;
		switch (msg.level)
		{
		case level::critical:
		case level::err:
			fmt::format_to(fmt::appender(formatted), PLUGIN_MSG_LOG("\ar") "{}", msg.payload);
			break;

		case level::trace:
		case level::debug:
			fmt::format_to(fmt::appender(formatted), PLUGIN_MSG_LOG("\a#7f7f7f") "{}", msg.payload);
			break;

		case level::warn:
			fmt::format_to(fmt::appender(formatted), PLUGIN_MSG_LOG("\ay") "{}", msg.payload);
			break;

		case level::info:
		default:
			fmt::format_to(fmt::appender(formatted), PLUGIN_MSG_LOG("\ag") "{}", msg.payload);
			break;
		}

		s_ourMessage = true;
		WriteChatf("%.*s", formatted.size(), formatted.data());
		s_ourMessage = false;
	}

	void flush_() override {}
};

void InitializeLogging()
{
	// set up default logger
	auto logger = spdlog::create<WriteChatSink>("MQ2Discord");
	logger->set_level(s_showDebug ? spdlog::level::trace : spdlog::level::info);

#if defined(_DEBUG)
	logger->sinks().push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
#endif
	spdlog::set_default_logger(logger);
	spdlog::set_pattern("%L %Y-%m-%d %T.%f [%n] %v (%@)");
	spdlog::flush_on(spdlog::level::trace);
}

void ShutdownLogging()
{
	spdlog::shutdown();
}

uint32_t __stdcall MQ2DataVariableLookup(char* VarName, char* Value, size_t ValueLen)
{
	strcpy_s(Value, ValueLen, VarName);

	if (!pLocalPlayer)
		return (uint32_t)strlen(Value);

	return (uint32_t)strlen(ParseMacroParameter(pLocalPlayer, Value, ValueLen));
}

void OutputMessage(const char* prepend, const char* format, va_list args)
{
	char output[MAX_STRING];
	strcpy_s(output, prepend);
	vsprintf_s(&output[strlen(output)], sizeof(output) - strlen(output) - 1, format, args);

	// If we're on the main thread write direct, otherwise queue it
	if (GetCurrentThreadId() == mainThreadId)
	{
		s_ourMessage = true;
		WriteChatf(output);
		s_ourMessage = false;
	}
	else
	{
		std::lock_guard<std::mutex>	lock(messagesMutex);
		messages.emplace(output);
	}
}

void OutputDebug(const char * format, ...)
{
	if (s_showDebug)
	{
		va_list args;
		va_start(args, format);
		OutputMessage("\ag[MQ2Discord] \amDebug: \aw ", format, args);
		va_end(args);
	}
}

void OutputError(const char * format, ...)
{
	va_list args;
	va_start(args, format);
	OutputMessage("\ag[MQ2Discord] \arError: \aw ", format, args);
	va_end(args);
}

void OutputWarning(const char * format, ...)
{
	va_list args;
	va_start(args, format);
	OutputMessage("\ag[MQ2Discord] \ayWarning: \aw ", format, args);
	va_end(args);
}

void OutputNormal(const char * format, ...)
{
	va_list args;
	va_start(args, format);
	OutputMessage("\ag[MQ2Discord] \aw", format, args);
	va_end(args);
}

void ProcessMessage(std::string Message)
{
	if (s_client && !s_ourMessage && GetGameState() == GAMESTATE_INGAME)
	{
		char myMessage[MAX_STRING] = { 0 };
		strcpy_s(myMessage, Message.c_str());
		// Should be okay to modify the message since it's a copy.
		StripTextLinks(myMessage);
		// Resize the string to match the first null terminator.
		//Message.erase(std::find(Message.begin(), Message.end(), '\0'), Message.end());
		s_client->EnqueueIfMatch(myMessage);
	}
}

std::string ParseMacroDataString(std::string_view input)
{
	if (input.empty())
		return std::string();

	char buffer[MAX_STRING] = { 0 };
	strncpy_s(buffer, input.data(), input.size());

	ParseMacroData(buffer, MAX_STRING);
	return buffer;
}

void SetDefaults(DiscordConfig& config)
{
	config.token = "YourTokenHere";
	config.user_ids.emplace_back("YourUserIdHere");

	// A channel for just this character that relays all echoes
	ChannelConfig charChannel;
	charChannel.name = "CharChannel (This field is ignored)";
	charChannel.id = "YourCharacterChannelHere";
	charChannel.prefix = "";
	charChannel.allow_commands = true;
	charChannel.show_command_response = 2000;
	config.characters[ParseMacroDataString("${EverQuest.Server}_${Me.Name}")].push_back(charChannel);

	// A channel for a group of characters that relays deaths
	ChannelConfig groupChannel;
	groupChannel.name = "Deaths";
	groupChannel.id = "YourGroupChannelHere";
	groupChannel.prefix = "[${Me.Name}]";
	groupChannel.allowed.emplace_back("You have been slain by");
	groupChannel.allow_commands = false;
	groupChannel.show_command_response = 0;
	GroupConfig group;
	group.name = "YourGroup";
	group.characters.emplace_back(ParseMacroDataString("${EverQuest.Server}_${Me.Name}"));
	group.characters.emplace_back("server_OtherCharInGroup");
	group.characters.emplace_back("server_OneMore");
	group.channels.emplace_back(groupChannel);
	config.groups.emplace_back(group);

	// A channel for all characters that relays tells.
	ChannelConfig tellsChannel;
	tellsChannel.name = "TellsChannel";
	tellsChannel.id = "YourChannelForTellsHere";
	tellsChannel.prefix = "[${EverQuest.Server}_${Me.Name}]";
	tellsChannel.allowed.emplace_back("tells you");
	tellsChannel.blocked.emplace_back("|${Me.Pet.DisplayName}| tells you#*#");
	tellsChannel.allow_commands = false;
	tellsChannel.show_command_response = 0;
	config.all.emplace_back(tellsChannel);
}

void WriteConfig(const DiscordConfig& config, const std::string& filename)
{
	YAML::Node node;
	node = config;
	std::ofstream fout(filename);
	fout << node;
}

DiscordConfig GetConfig()
{
	// Load existing config if it exists
	const std::filesystem::path configFile = std::filesystem::path(gPathConfig) / "MQ2Discord.yaml";

	std::error_code ec_exists;
	if (std::filesystem::exists(configFile, ec_exists))
		return YAML::LoadFile(configFile.string()).as<DiscordConfig>();

	// If old .json configs exist, convert them
	std::map<std::string, YAML::Node> jsonConfigs;
	for (const auto& file : std::filesystem::directory_iterator(gPathConfig))
	{
		const auto filename = file.path().filename().string();
		const std::regex re("MQ2Discord_(.*)\\.json");

		std::smatch matches;
		if (std::regex_match(filename, matches, re))
			jsonConfigs[matches[1]] = YAML::LoadFile(file.path().string());
	}

	if (!jsonConfigs.empty())
	{
		auto imported = false;
		DiscordConfig config;
		for (const auto & kvp : jsonConfigs)
		{
			// Skip if there's no channel id
			if (kvp.second["channel"].as<std::string>().empty())
				continue;

			imported = true;

			// Create a channel and add it to characters
			ChannelConfig channel;
			channel.name = kvp.first;
			channel.id = kvp.second["channel"].as<std::string>();
			channel.allow_commands = true;
			channel.show_command_response = 2000;
			for (const auto & filter : kvp.second["allow"].as<std::vector<std::string>>())
				channel.allowed.push_back(filter);
			for (const auto & filter : kvp.second["block"].as<std::vector<std::string>>())
				channel.blocked.push_back(filter);

			config.characters[kvp.first].push_back(channel);

			// Set the token & add the user id to the allowed list
			config.token = kvp.second["token"].as<std::string>();
			if (std::find(config.user_ids.begin(), config.user_ids.end(), kvp.second["user"].as<std::string>()) != config.user_ids.end())
				config.user_ids.push_back(kvp.second["user"].as<std::string>());

			OutputNormal("Imported config for %s", kvp.first.c_str());
		}

		if (imported)
		{
			WriteConfig(config, configFile.string());
			return config;
		}
	}

	// Otherwise, create a default config
	DiscordConfig config;
	SetDefaults(config);
	WriteConfig(config, configFile.string());
	OutputNormal("Created a default configuration. Edit this, then do \ag/discord reload");

	return config;
}

void Reload()
{
	if (s_client)
		s_client.reset();

	DiscordConfig config;
	try
	{
		config = GetConfig();
	}
	catch (std::exception& e)
	{
		OutputError("Failed to load config, %s", e.what());
		return;
	}

	for (const auto& warning : config.warnings())
		OutputWarning(warning.c_str());

	auto errors = config.errors();
	for (const auto& error : errors)
		OutputError(error.c_str());

	if (!errors.empty())
	{
		OutputNormal("Config not loaded due to errors, please fix them and \ag/discord reload");
		return;
	}

	//const std::string server = EQADDR_SERVERNAME;
	//const std::string server_character = server + "_" + GetCharInfo()->Name;
	//const std::string classShortName = pEverQuest->GetClassThreeLetterCode(((PSPAWNINFO)pCharSpawn)->mActorClient.Class);

	const std::string server = ParseMacroDataString("${EverQuest.Server}");
	const std::string server_character = server + "_" + ParseMacroDataString("${Me.Name}");
	const std::string classShortName = ParseMacroDataString("${Me.Class.ShortName}");

	std::vector<ChannelConfig> channels;

	// Character's own channels
	if (config.characters.find(server_character) != config.characters.end())
		channels.insert(channels.end(), config.characters[server_character].begin(), config.characters[server_character].end());

	// Server channels
	if (config.servers.find(GetServerShortName()) != config.servers.end())
		channels.insert(channels.end(), config.servers[GetServerShortName()].begin(), config.servers[GetServerShortName()].end());

	// Class channels
	if (config.classes.find(classShortName) != config.classes.end())
		channels.insert(channels.end(), config.classes[classShortName].begin(), config.classes[classShortName].end());

	// Groups
	for (const auto& group : config.groups)
		if (std::find(group.characters.begin(), group.characters.end(), server_character) != group.characters.end())
			channels.insert(channels.end(), group.channels.begin(), group.channels.end());

	// Global
	channels.insert(channels.end(), config.all.begin(), config.all.end());

	if (channels.empty())
	{
		OutputWarning("No channels configured for this character");
		return;
	}

	for (auto &channel : channels)
	{
		if (!channel.prefix.empty())
		{
			// Make prefixes end with a space if they don't already
			if (channel.prefix.back() != ' ')
				channel.prefix.append(" ");
			channel.prefix = ParseMacroDataString(channel.prefix);
		}

		// FIXME:  A lot of allocations happening here.
		// Add #*# at the start/end of any filters that don't have it already
		for (auto& filter : channel.allowed)
			if (filter.substr(0, 3) != "#*#" && filter.substr(filter.length() - 3, 3) != "#*#")
				filter = "#*#" + filter + "#*#";
		for (auto& filter : channel.blocked)
			if (filter.substr(0, 3) != "#*#" && filter.substr(filter.length() - 3, 3) != "#*#")
				filter = "#*#" + filter + "#*#";
		for (auto& filter : channel.notify)
			if (filter.substr(0, 3) != "#*#" && filter.substr(filter.length() - 3, 3) != "#*#")
				filter = "#*#" + filter + "#*#";
	}

	s_client = std::make_unique<DiscordClient>(
		std::move(config),
		std::move(channels)
	);
}

void DiscordCmd(PSPAWNINFO pChar, PCHAR szLine)
{
	char buffer[MAX_STRING] = { 0 };

	GetArg(buffer, szLine, 1);

	if (!_stricmp(buffer, "reload"))
	{
		Reload();
	}
	else if (!_stricmp(buffer, "stop"))
	{
		//client->Stop();
	}
	else if (!_stricmp(buffer, "debug"))
	{
		s_showDebug = !s_showDebug;
		spdlog::set_level(s_showDebug ? spdlog::level::trace : spdlog::level::info);

		OutputNormal("Debug is now: %s\ax", s_showDebug ? "\agON" : "\arOFF");
	}
	else if (!_stricmp(buffer, "process"))
	{
		std::string strLine = szLine;
		const std::string strBuffer = buffer;
		if (strLine.length() > strBuffer.length())
		{
			strLine = strLine.substr((strBuffer).length() + 1);
			OutputDebug("Processing: %s", strLine.c_str());
			ProcessMessage(std::move(strLine));
		}
		else
		{
			OutputError("Process is used to process a specific line, you must enter something after the word 'process' ex: /discord process This is a test");
		}
	}
	else
	{
		OutputWarning("Invalid command.  Valid commands are process, reload, and for debugging: debug, stop.");
	}
}

PLUGIN_API void InitializePlugin()
{
	InitializeLogging();

	mainThreadId = GetCurrentThreadId();
	AddCommand("/discord", DiscordCmd);

	globalDataMutex.lock();
}

PLUGIN_API void ShutdownPlugin()
{
	globalDataMutex.unlock();

	s_client.reset();
	RemoveCommand("/discord");

	ShutdownLogging();
}

PLUGIN_API void OnPulse()
{
	// While we are processing events in the discord client, unlock the mutex
	// so that it can run calls against the main thread.
	unique_unlock unlock(globalDataMutex);

	// Output any queued messages
	while (true)
	{
		std::lock_guard<std::mutex> lock(messagesMutex);

		if (messages.empty())
			break;

		s_ourMessage = true;
		WriteChatf(messages.front().c_str());
		s_ourMessage = false;
		messages.pop();
	}

	if (s_client)
	{
		s_client->ProcessEvents();
	}
}

PLUGIN_API void OnWriteChatColor(const char* Line, int Color, int Filter)
{
	ProcessMessage(Line);
}

PLUGIN_API bool OnIncomingChat(const char* Line, DWORD Color)
{
	ProcessMessage(Line);
	return false;
}

PLUGIN_API void SetGameState(int GameState)
{
	if (GameState == GAMESTATE_INGAME)
	{
		Reload();
	}
	else
	{
		if (s_client)
		{
			s_client->EnqueueAll("Disconnecting, no longer in game");
			s_client.reset();
		}
	}
}
