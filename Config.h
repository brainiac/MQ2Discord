#pragma once

#include <string>
#include <vector>
#include <map>
#include <yaml-cpp/node/node.h>
#include <regex>

struct ChannelConfig
{
	ChannelConfig() : allow_commands(false), send_connected(true), show_command_response(2000) { }

	std::string name;
	std::string id;
	std::vector<std::string> allowed;
	std::vector<std::string> blocked;
	std::vector<std::string> notify;
	std::string prefix;
	bool send_connected;
	bool allow_commands;
	uint32_t show_command_response;
};

struct GroupConfig
{
	std::string name;
	std::vector<std::string> characters;
	std::vector<ChannelConfig> channels;
};

struct DiscordConfig
{
	std::string token;
	std::vector<std::string> user_ids;

	std::vector<ChannelConfig> all;
	std::map<std::string, std::vector<ChannelConfig>> characters;
	std::map<std::string, std::vector<ChannelConfig>> servers;
	std::map<std::string, std::vector<ChannelConfig>> classes;
	std::vector<GroupConfig> groups;

	std::vector<std::string> warnings() const
	{
		std::vector<std::string> results;
		std::regex idRegex("^\\d+$");

		for (const auto& user : user_ids)
			if (!std::regex_match(user, idRegex))
				results.push_back("User id \ay" + user + "\aw looks wrong");

		return results;
	}

	std::vector<std::string> errors() const
	{
		std::vector<std::string> results;
		std::regex idRegex("^\\d+$");

		for (const auto& channel : all)
			if (!std::regex_match(channel.id, idRegex))
				results.push_back("Channel id \ay" + channel.id + "\aw in \ayall\aw looks wrong");

		for (const auto& character : characters)
			for (const auto& channel : character.second)
				if (!std::regex_match(channel.id, idRegex))
					results.push_back("Channel id \ay" + channel.id + "\aw in character \ay" + character.first + "\aw looks wrong");

		for (const auto& server : servers)
			for (const auto& channel : server.second)
				if (!std::regex_match(channel.id, idRegex))
					results.push_back("Channel id \ay" + channel.id + "\aw in server \ay" + server.first + "\aw looks wrong");

		for (const auto& cls : classes)
			for (const auto& channel : cls.second)
				if (!std::regex_match(channel.id, idRegex))
					results.push_back("Channel id \ay" + channel.id + "\aw in class \ay" + cls.first + "\aw looks wrong");

		for (const auto& group : groups)
			for (const auto& channel : group.channels)
				if (!std::regex_match(channel.id, idRegex))
					results.push_back("Channel id \ay" + channel.id + "\aw in group \ay" + group.name + "\aw looks wrong");

		const std::regex tokenRegex(R"([\w\-_]{24}\.[\w\-_]{6}\.[\w\-_]{27,})");
		if (!std::regex_match(token, tokenRegex))
			results.push_back("Invalid token");

		return results;
	}
};

namespace YAML {
	template<>
	struct convert<DiscordConfig> {
		static Node encode(const DiscordConfig& rhs) {
			Node node;
			node["token"] = rhs.token;
			node["user_ids"] = rhs.user_ids;
			node["characters"] = rhs.characters;
			node["servers"] = rhs.servers;
			node["classes"] = rhs.classes;
			node["groups"] = rhs.groups;
			node["all"] = rhs.all;
			return node;
		}

		static bool decode(const Node& node, DiscordConfig& rhs) {
			rhs.token = node["token"].as<std::string>();
			rhs.user_ids = node["user_ids"].as<std::vector<std::string>>();
			if (node["characters"])
				rhs.characters = node["characters"].as<std::map<std::string, std::vector<ChannelConfig>>>();
			if (node["servers"])
				rhs.servers = node["servers"].as<std::map<std::string, std::vector<ChannelConfig>>>();
			if (node["classes"])
				rhs.servers = node["classes"].as<std::map<std::string, std::vector<ChannelConfig>>>();
			if (node["groups"])
				rhs.groups = node["groups"].as<std::vector<GroupConfig>>();
			if (node["all"])
				rhs.all = node["all"].as<std::vector<ChannelConfig>>();
			return true;
		}
	};

	template<>
	struct convert<ChannelConfig> {
		static Node encode(const ChannelConfig& rhs) {
			Node node;
			node["name"] = rhs.name;
			node["id"] = rhs.id;
			node["allowed"] = rhs.allowed;
			node["blocked"] = rhs.blocked;
			node["notify"] = rhs.notify;
			node["prefix"] = rhs.prefix;
			node["send_connected"] = rhs.send_connected;
			node["allow_commands"] = rhs.allow_commands;
			node["show_command_response"] = rhs.show_command_response;
			return node;
		}

		static bool decode(const Node& node, ChannelConfig& rhs) {
			rhs.id = node["id"].as<std::string>();
			if (node["name"])
				rhs.name = node["name"].as<std::string>();
			if (node["allowed"])
				rhs.allowed = node["allowed"].as<std::vector<std::string>>();
			if (node["blocked"])
				rhs.blocked = node["blocked"].as<std::vector<std::string>>();
			if (node["notify"])
				rhs.notify = node["notify"].as<std::vector<std::string>>();
			if (node["prefix"])
				rhs.prefix = node["prefix"].as<std::string>();
			if (node["send_connected"])
				rhs.send_connected = node["send_connected"].as<bool>();
			if (node["allow_commands"])
				rhs.allow_commands = node["allow_commands"].as<bool>();
			if (node["show_command_response"])
				rhs.show_command_response = node["show_command_response"].as<uint32_t>();
			return true;
		}
	};

	template<>
	struct convert<GroupConfig> {
		static Node encode(const GroupConfig& rhs) {
			Node node;
			node["name"] = rhs.name;
			node["characters"] = rhs.characters;
			node["channels"] = rhs.channels;
			return node;
		}

		static bool decode(const Node& node, GroupConfig& rhs) {
			if (node["name"])
				rhs.name = node["name"].as<std::string>();
			if (node["characters"])
				rhs.characters = node["characters"].as<std::vector<std::string>>();
			if (node["channels"])
				rhs.channels = node["channels"].as<std::vector<ChannelConfig>>();
			return true;
		}
	};
}