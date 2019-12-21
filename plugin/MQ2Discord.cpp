#include "DiscordClient.h"
#include "Config.h"
#include <fstream>
#include <regex>
#include <yaml-cpp\yaml.h>
#include <filesystem>

// Sleepy discord library is compiled with default struct align, MQ2 needs 4 byte
// Even with this pragma push/pop, having headers for both included in the same cpp causes stack corruption :(
// So this file contains MQ2 stuff, other file contains discord stuff
#pragma pack(push, 4)
#include <mq/Plugin.h>
#pragma pack(pop)

PreSetup("MQ2Discord");
PLUGIN_VERSION(1.0);

// MQ2Main isn't nice enough to export this
unsigned int __stdcall MQ2DataVariableLookup(char * VarName, char * Value, size_t ValueLen)
{
	strcpy_s(Value, ValueLen, VarName);
	if (!pLocalPlayer)
		return strlen(Value);
	return strlen(ParseMacroParameter((PSPAWNINFO)pLocalPlayer, Value, ValueLen));
}

VOID DiscordCmd(PSPAWNINFO pChar, PCHAR szLine);
void Reload();

std::unique_ptr<MQ2Discord::DiscordClient> client;
bool disabled = false;
std::queue<std::string> commands;
std::mutex commandsMutex;
std::queue<std::string> messages;
std::mutex messagesMutex;
DWORD mainThreadId;

PLUGIN_API VOID InitializePlugin(VOID)
{
	mainThreadId = GetCurrentThreadId();
	AddCommand("/discord", DiscordCmd, 0, 0, 1);
}

PLUGIN_API VOID ShutdownPlugin(VOID)
{
	if (client)
		client.reset();
	RemoveCommand("/discord");
}

PLUGIN_API VOID OnPulse(VOID)
{
	// Execute any queued commands
	while (true)
	{
		char command[MAX_STRING] = {0};
		{
			std::lock_guard<std::mutex> lock(commandsMutex);

			if (commands.empty())
				break;

			strcpy_s(command,commands.front().c_str());
			commands.pop();
		}
		DoCommand((PSPAWNINFO)pCharSpawn, command);
	}

	// Output any queued messages
	while (true)
	{
		char message[MAX_STRING] = { 0 };
		{
			std::lock_guard<std::mutex> lock(messagesMutex);

			if (messages.empty())
				break;

			strcpy_s(message, messages.front().c_str());
			messages.pop();
		}
		disabled = true;
		WriteChatf(message);
		disabled = false;
	}

}

PLUGIN_API DWORD OnWriteChatColor(PCHAR Line, DWORD Color, DWORD Filter)
{
	if (client && !disabled)
		client->enqueueIfMatch(Line);
    return 0;
}

PLUGIN_API DWORD OnIncomingChat(PCHAR Line, DWORD Color)
{
	if (client && !disabled)
		client->enqueueIfMatch(Line);
    return 0;
}

PLUGIN_API VOID SetGameState(DWORD GameState)
{
	if (GameState == GAMESTATE_INGAME)
		Reload();
	else
	{
		if (client)
		{
			client->enqueueAll("Disconnecting, no longer in game");
			client.reset();
		}
	}
}

std::string ParseMacroDataString(const std::string& input)
{
	char buffer[MAX_STRING] = { 0 };
	strcpy_s(buffer, input.c_str());
	ParseMacroData(buffer, MAX_STRING);
	return buffer;
}

void OnCommand(std::string command)
{
	std::lock_guard<std::mutex> _lock(commandsMutex);
	commands.emplace(command);
}

void OutputMessage(const char * prepend, const char * format, va_list args)
{
	char output[MAX_STRING];
	strcpy_s(output, prepend);
	vsprintf_s(&output[strlen(output)], sizeof(output) - strlen(output) - 1, format, args);

	// If we're on the main thread write direct, otherwise queue it
	if (GetCurrentThreadId() == mainThreadId)
	{
		disabled = true;
		WriteChatf(output);
		disabled = false;
	}
	else
	{
		std::lock_guard<std::mutex>	lock(messagesMutex);
		messages.emplace(output);
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
	OutputMessage("\ag[MQ2Discord] \aw ", format, args);
	va_end(args);
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
	charChannel.allowed.emplace_back("[MQ2]#*#");
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
	const auto configFile = std::string(gszINIPath) + "\\MQ2Discord.yaml";
	if (std::filesystem::exists(configFile))
		return YAML::LoadFile(configFile).as<DiscordConfig>();

	// If old .json configs exist, convert them
	std::map<std::string, YAML::Node> jsonConfigs;
	for (const auto & file : std::filesystem::directory_iterator(gszINIPath))
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
			if (kvp.second["channel"].as<std::string>() == "")
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
			WriteConfig(config, configFile);
			return config;
		}
	}

	// Otherwise, create a default config
	DiscordConfig config;
	SetDefaults(config);
	WriteConfig(config, configFile);
	OutputNormal("Created a default configuration. Edit this, then do \ag/discord reload");
	return config;
}

void Reload()
{
	if (client)
		client.reset();

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
	if (config.servers.find(EQADDR_SERVERNAME) != config.servers.end())
		channels.insert(channels.end(), config.servers[EQADDR_SERVERNAME].begin(), config.servers[EQADDR_SERVERNAME].end());

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
		// Make prefixes end with a space if they don't already
		if (*channel.prefix.end() != ' ')
			channel.prefix.push_back(' ');
		channel.prefix = ParseMacroDataString(channel.prefix);

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

	client = std::make_unique<MQ2Discord::DiscordClient>(config.token, config.user_ids, channels, OnCommand, ParseMacroDataString, OutputError, OutputWarning, OutputNormal);
}

VOID DiscordCmd(PSPAWNINFO pChar, PCHAR szLine)
{
	char buffer[MAX_STRING] = { 0 };

	GetArg(buffer, szLine, 1);

	if (!_stricmp(buffer, "reload"))
	{
		Reload();
	}
}