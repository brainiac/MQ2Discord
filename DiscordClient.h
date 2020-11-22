#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <utility>
#include <vector>
#include <mutex>
#include <queue>
#include <sleepy_discord\websocketpp_websocket.h>
#include "Config.h"
#include "Blech/Blech.h"

unsigned int __stdcall MQ2DataVariableLookup(char * VarName, char * Value, size_t ValueLen);

namespace MQ2Discord
{
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
			void(*writeError)(const char * format, ...),
			void(*writeWarning)(const char * format, ...),
			void(*writeNormal)(const char * format, ...))
			: _token(std::move(token)), _userIds(std::move(userIds)), _channels(std::move(channels)), _parseMacroData(std::move(parseMacroData)),
			_executeCommand(std::move(executeCommand)), _writeError(writeError), _writeWarning(writeWarning), _writeNormal(writeNormal), _stop(false),
			_blech('#', '|', MQ2DataVariableLookup)
		{
			// Add events to the parser and store the id/channel in the appropriate map
			for (auto channelIt = _channels.begin(); channelIt != _channels.end(); ++channelIt)
			{
				for (auto allow : channelIt->allowed)
					_blechAllowEvents[_blech.AddEvent(allow.c_str(), blechMatch, this)] = &(*channelIt);
				for (auto block : channelIt->blocked)
					_blechBlockEvents[_blech.AddEvent(block.c_str(), blechMatch, this)] = &(*channelIt);
				for (auto notify : channelIt->notify)
					_blechNotifyEvents[_blech.AddEvent(notify.c_str(), blechMatch, this)] = &(*channelIt);
			}

			// Create background thread, this starts it too
			_thread = std::thread{ &DiscordClient::threadStart, this };

			for (const auto &channel : _channels)
				if (channel.send_connected)
					enqueue(channel.id, "Connected");
		}

		~DiscordClient()
		{
			// Signal thread to stop, then wait for it to do so
			_stop = true;
			_thread.join();
		}

		/// Queue a message to be sent on all channels
		void enqueueAll(std::string message)
		{
			std::lock_guard<std::mutex> lock(_messagesMutex);
			for (auto channel : _channels)
				_messages.emplace(channel.id, _parseMacroData(channel.prefix) + message);
		}

		/// Queue a message to be sent on any channel with matching filters
		void enqueueIfMatch(std::string message)
		{
			// Clear results & set every channel to no match initially
			_filterMatches.clear();
			for (const auto& _channel : _channels)
				_filterMatches[&_channel] = FilterMatch::None;

			// Feed the message through the parser. Colour codes are removed beforehand.
			char buffer[2048] = { 0 };
			strcpy_s(buffer, std::regex_replace(message, std::regex("\a\\-?."), "").c_str());
			_blech.Feed(buffer);

			// Send to any channels that matched
			for (auto kvp : _filterMatches)
				if ((kvp.first->show_command_response > 0
						&& _responseExpiryTimes.find(kvp.first) != _responseExpiryTimes.end() 
						&& _responseExpiryTimes[kvp.first] > std::chrono::system_clock::now())
					|| kvp.second == FilterMatch::Allow)
				{
					enqueue(kvp.first->id, _parseMacroData(kvp.first->prefix) + escape_discord(message));
				}
				else if (kvp.second == FilterMatch::Notify)
				{
					enqueue(kvp.first->id, _parseMacroData(kvp.first->prefix) + escape_discord(message) + " @everyone");
				}
		}

	private:
		/// Discord API token
		const std::string _token;

		/// List of user ids allowed to issue commands
		const std::vector<std::string> _userIds;;

		/// List of configured channels to send/receive messages to/from
		const std::vector<ChannelConfig> _channels;

		/// Function to parse a string containing MQ2 variables
		const std::function<std::string(std::string input)> _parseMacroData;

		/// Function to execute an ingame command. Must be threadsafe as it won't be invoked from the main thread
		const std::function<void(std::string command)> _executeCommand;

		/// Function to write an error message to ingame chat. Must be threadsafe
		void(*const _writeError)(const char * format, ...);

		/// Function to write an warning message to ingame chat. Must be threadsafe
		void(*const _writeWarning)(const char * format, ...);

		/// Function to write an regular message to ingame chat. Must be threadsafe
		void(*const _writeNormal)(const char * format, ...);

		/// Background thread to handle discord communications. Will run for the lifetime of this class
		std::thread _thread;

		/// Stop signal	for the background thread
		std::atomic<bool> _stop;

		/// Queue of messages to send to discord. Tuple of channelId, messageText
		std::queue<std::tuple<std::string, std::string>> _messages;

		/// Sync mutex for access to _messages
		std::mutex _messagesMutex;

		/// Parser to match chat text to channels based on allow/block filters
		Blech _blech;

		/// Mapping from Blech event ID to channel for all allow events
		std::map<unsigned int, const ChannelConfig *> _blechAllowEvents;

		/// Mapping from Blech event ID to channel for all block events
		std::map<unsigned int, const ChannelConfig *> _blechBlockEvents;

		/// Mapping from Blech event ID to channel for all notify events
		std::map<unsigned int, const ChannelConfig *> _blechNotifyEvents;

		/// Channel -> FilterMatch. Cleared before parsing, and populated by the blech match callback.
		std::map<const ChannelConfig *, FilterMatch> _filterMatches;

		/// Channel -> Time to stop sending everything in response to a command
		std::map<const ChannelConfig *, std::chrono::time_point<std::chrono::system_clock>> _responseExpiryTimes;

		/// Queue a message to be sent on a specific channel
		void enqueue(const std::string& channelId, const std::string& message)
		{
			std::lock_guard<std::mutex> lock(_messagesMutex);
			_messages.emplace(channelId, message);
		}

		/// Callback function for blech parser match
		static void __stdcall blechMatch(unsigned int ID, void * pData, PBLECHVALUE pValues)
		{
			auto pClient = reinterpret_cast<DiscordClient *>(pData);

			// If it matched a block filter, mark it as blocked regardless of what it was before
			auto blockEvent = pClient->_blechBlockEvents.find(ID);
			if (blockEvent != pClient->_blechBlockEvents.end())
			{
				pClient->_filterMatches[blockEvent->second] = FilterMatch::Block;
				return;
			}

			// If it matches a notify, mark it to notify unless it's already blocked
			auto notifyEvent = pClient->_blechNotifyEvents.find(ID);
			if (notifyEvent != pClient->_blechNotifyEvents.end() && pClient->_filterMatches[notifyEvent->second] != FilterMatch::Block)
			{
				pClient->_filterMatches[notifyEvent->second] = FilterMatch::Notify;
				return;
			}

			// Otherwise, it's matched an allow event, so set to allow unless it's already block or notify
			auto pChannel = pClient->_blechAllowEvents[ID];
			if (pClient->_filterMatches[pChannel] == FilterMatch::None)
				pClient->_filterMatches[pChannel] = FilterMatch::Allow;
		}

		// Real simple client, all it does is invoke a callback when a message is received
		class CallbackDiscordClient : public SleepyDiscord::DiscordClient {
		public:
			using SleepyDiscord::DiscordClient::DiscordClient;
			CallbackDiscordClient(const std::string& token, std::function<void(SleepyDiscord::Message &)> callback)
				: SleepyDiscord::DiscordClient(token, 0),
				_callback(std::move(callback))
			{
			}

			void onMessage(SleepyDiscord::Message message) override
			{
				if (_callback)
					_callback(message);
			}
		private:
			std::function<void(SleepyDiscord::Message &)> _callback;
		};

		void onMessageReceived(SleepyDiscord::Message& message)
		{
			// Did it come from a channel we recognize?
			auto channel = std::find_if(_channels.begin(), _channels.end(), [&](auto channel) { return message.channelID == channel.id; });
			if (channel == _channels.end())
			{
				// For better or worse, this happens a fair bit due to this class only knowing about channels the current character is a part of
				// Will always happen when a command is issued to another character
				//_writeWarning("Message received on unknown channel: %s", message.channelID.string().c_str());
				return;
			}

			// Basic commands
			if (message.content == "!status")
			{
				enqueue(channel->id, _parseMacroData(channel->prefix) + "Status: Connected");
				return;
			}
			if (message.startsWith("!echo "))
			{
				if (std::find(_userIds.begin(), _userIds.end(), static_cast<std::string>(message.author.ID)) != _userIds.end())
					enqueue(channel->id, _parseMacroData(channel->prefix + message.content.substr(6, message.content.size() - 6)));
				else
				{
					enqueue(channel->id, _parseMacroData(channel->prefix) + "You are not authorized to issue commands on this channel");
					_writeWarning("Command received on channel %s from unauthorized user %s", channel->id.c_str(), ((std::string)message.author.ID).c_str());
				}
				return;
			}

			// Ingame commands
			if (message.startsWith("/"))
			{
				if (!channel->allow_commands)
				{
					enqueue(channel->id, _parseMacroData(channel->prefix) + "Commands are not allowed on this channel");
					_writeWarning("Command received on channel with commands disabled: %s", channel->id.c_str());
					return;
				}
				if (std::find(_userIds.begin(), _userIds.end(), static_cast<std::string>(message.author.ID)) != _userIds.end())
				{
					_executeCommand(unescape_json(message.content));
					if (channel->show_command_response > 0)
						_responseExpiryTimes[&*channel] = std::chrono::system_clock::now() + std::chrono::milliseconds(channel->show_command_response);
				}
				else
				{
					enqueue(channel->id, _parseMacroData(channel->prefix) + "You are not authorized to issue commands on this channel");
					_writeWarning("Command received on channel %s from unauthorized user %s", channel->id.c_str(), ((std::string)message.author.ID).c_str());
				}
			}

		}

		static std::string errorString(SleepyDiscord::ErrorCode errorCode)
		{
			switch (errorCode)
			{
			case SleepyDiscord::ERROR_ZERO: return "ERROR_ZERO";
			case SleepyDiscord::SWITCHING_PROTOCOLS: return "SWITCHING_PROTOCOLS";
			case SleepyDiscord::OK: return "OK";
			case SleepyDiscord::CREATED: return "CREATED";
			case SleepyDiscord::NO_CONTENT: return "NO_CONTENT";
			case SleepyDiscord::NOT_MODIFIED: return "NOT_MODIFIED";
			case SleepyDiscord::BAD_REQUEST: return "BAD_REQUEST";
			case SleepyDiscord::UNAUTHORIZED: return "UNAUTHORIZED";
			case SleepyDiscord::FORBIDDEN: return "FORBIDDEN";
			case SleepyDiscord::NOT_FOUND: return "NOT_FOUND";
			case SleepyDiscord::METHOD_NOT_ALLOWED: return "METHOD_NOT_ALLOWED";
			case SleepyDiscord::TOO_MANY_REQUESTS: return "TOO_MANY_REQUESTS";
			case SleepyDiscord::GATEWAY_UNAVAILABLE: return "GATEWAY_UNAVAILABLE";
			case SleepyDiscord::UNKNOWN_ERROR: return "UNKNOWN_ERROR";
			case SleepyDiscord::UNKNOWN_OPCODE: return "UNKNOWN_OPCODE";
			case SleepyDiscord::DECODE_ERROR: return "DECODE_ERROR";
			case SleepyDiscord::NOT_AUTHENTICATED: return "NOT_AUTHENTICATED";
			case SleepyDiscord::AUTHENTICATION_FAILED: return "AUTHENTICATION_FAILED";
			case SleepyDiscord::ALREADY_AUTHENTICATED: return "ALREADY_AUTHENTICATED";
			case SleepyDiscord::SESSION_NO_LONGER_VALID: return "SESSION_NO_LONGER_VALID";
			case SleepyDiscord::INVALID_SEQ: return "INVALID_SEQ";
			case SleepyDiscord::RATE_LIMITED: return "RATE_LIMITED";
			case SleepyDiscord::SESSION_TIMEOUT: return "SESSION_TIMEOUT";
			case SleepyDiscord::INVALID_SHARD: return "INVALID_SHARD";
			case SleepyDiscord::SHARDING_REQUIRED: return "SHARDING_REQUIRED";
			case SleepyDiscord::UNKNOWN_PROTOCOL: return "UNKNOWN_PROTOCOL";
			case SleepyDiscord::INVALID_INTENTS: return "INVALID_INTENTS";
			case SleepyDiscord::DISALLOWED_INTENTS: return "DISALLOWED_INTENTS";
			case SleepyDiscord::VOICE_SERVER_CRASHED: return "VOICE_SERVER_CRASHED";
			case SleepyDiscord::UNKNOWN_ENCRYPTION_MODE: return "UNKNOWN_ENCRYPTION_MODE";
			case SleepyDiscord::CONNECT_FAILED: return "CONNECT_FAILED";
			case SleepyDiscord::EVENT_UNKNOWN: return "EVENT_UNKNOWN";
			case SleepyDiscord::GATEWAY_FAILED: return "GATEWAY_FAILED";
			case SleepyDiscord::GENERAL_ERROR: return "GENERAL_ERROR";
			case SleepyDiscord::LAZY_ERROR: return "LAZY_ERROR";
			case SleepyDiscord::ERROR_NOTE: return "ERROR_NOTE";
			default: return "UNKNOWN";
			}
		}

		static std::string errorDesc(SleepyDiscord::ErrorCode errorCode)
		{
			switch (errorCode)
			{
			case SleepyDiscord::ERROR_ZERO: return "Zero error?!";
			case SleepyDiscord::SWITCHING_PROTOCOLS: return "The server has acknowledged a request to switch protocols";
			case SleepyDiscord::OK: return "The request completed successfully";
			case SleepyDiscord::CREATED: return "The entity was created successfully";
			case SleepyDiscord::NO_CONTENT: return "The request completed successfully but returned no content";
			case SleepyDiscord::NOT_MODIFIED: return "The entity was not modified(no action was taken)";
			case SleepyDiscord::BAD_REQUEST: return "The request was improperly formatted, or the server couldn't understand it";
			case SleepyDiscord::UNAUTHORIZED: return "The Authorization header was missing or invalid";
			case SleepyDiscord::FORBIDDEN: return "The Authorization token you passed did not have permission to the resource";
			case SleepyDiscord::NOT_FOUND: return "The resource at the location specified doesn't exist";
			case SleepyDiscord::METHOD_NOT_ALLOWED: return "The HTTP method used is not valid for the location specified";
			case SleepyDiscord::TOO_MANY_REQUESTS: return "You've made too many requests, see Rate Limiting";
			case SleepyDiscord::GATEWAY_UNAVAILABLE: return "There was not a gateway available to process your request.Wait a bit and retry";
			case SleepyDiscord::UNKNOWN_ERROR: return "We're not sure what went wrong. Try reconnecting?";
			case SleepyDiscord::UNKNOWN_OPCODE: return "You sent an invalid Gateway OP Code.Don't do that!";
			case SleepyDiscord::DECODE_ERROR: return "You sent an invalid payload to us.Don't do that!";
			case SleepyDiscord::NOT_AUTHENTICATED: return "You sent us a payload prior to identifying.";
			case SleepyDiscord::AUTHENTICATION_FAILED: return "The account token sent with your identify payload is incorrect.";
			case SleepyDiscord::ALREADY_AUTHENTICATED: return "You sent more than one identify payload.Don't do that!";
			case SleepyDiscord::SESSION_NO_LONGER_VALID: return "Your session is no longer valid.";
			case SleepyDiscord::INVALID_SEQ: return "The sequence sent when resuming the session was invalid.Reconnect and start a new session.";
			case SleepyDiscord::RATE_LIMITED: return "Woah nelly!You're sending payloads to us too quickly. Slow it down!";
			case SleepyDiscord::SESSION_TIMEOUT: return "Your session timed out.Reconnect and start a new one.";
			case SleepyDiscord::INVALID_SHARD: return "You sent us an invalid shard when identifying.";
			case SleepyDiscord::SHARDING_REQUIRED: return "The session would have handled too many guilds - you are required to shard your connection in order to connect.";
			case SleepyDiscord::UNKNOWN_PROTOCOL: return "We didn't recognize the protocol you sent.";
			case SleepyDiscord::INVALID_INTENTS: return "Oh no! You sent an invalid intent for a gateway (Whatever that means).";
			case SleepyDiscord::DISALLOWED_INTENTS: return "Oh no! You sent a disallowed intent for a gateway (Whatever that means).";
			case SleepyDiscord::VOICE_SERVER_CRASHED: return "The server crashed. Our bad! Try resuming.";
			case SleepyDiscord::UNKNOWN_ENCRYPTION_MODE: return "We didn't recognize your encryption.";
			case SleepyDiscord::CONNECT_FAILED: return "Failed to connect to the Discord api after 4 trys";
			case SleepyDiscord::EVENT_UNKNOWN: return "Unexpected or unknown event occurred";
			case SleepyDiscord::GATEWAY_FAILED: return "Could not get the gateway";
			case SleepyDiscord::GENERAL_ERROR: return "Used when you are too lazy to use a error code";
			case SleepyDiscord::LAZY_ERROR: return "Used when you are too lazy to use a error code and message";
			case SleepyDiscord::ERROR_NOTE: return "Comes after an error to give more detail about an error in the message";
			default: return "Unrecognized error";
			}
		}

		static std::string escape_json(const std::string &s) {
			std::ostringstream o;
			for (auto c = s.cbegin(); c != s.cend(); c++) {
				if (*c == '"' || *c == '\\' || ('\x00' <= *c && *c <= '\x1f')) {
					o << "\\u"
						<< std::hex << std::setw(4) << std::setfill('0') << (int)*c;
				}
				else {
					o << *c;
				}
			}
			return o.str();
		}

		static std::string unescape_json(const std::string &s) {
			std::ostringstream o;
			for (auto c = s.cbegin(); c != s.cend(); ++c) {
				if (*c == '\\')
				{
					++c;
					switch (*c)
					{
					case 't': { o << "\t"; break; }
					case 'b': { o << "\b"; break; }
					case 'f': { o << "\f"; break; }
					case 'n': { o << "\n"; break; }
					case 'r': { o << "\r"; break; }
					case '\\': { o << "\\"; break; }
					case '/': { o << "/";  break; }
					case '"': { o << "\""; break; }
					default:
						;
					}
				}
				else {
					o << *c;
				}
			}
			return o.str();
		}

		// Escapes special discord characters
		static std::string escape_discord(const std::string &s) {
			std::ostringstream o;
			for (auto c : s)
			{
				if (c == '`' || c == '*' || c == '_' || c == '~')
					o << "\\";
				o << c;
			}
			// Clean up colour codes. First try to put anything between a colour code and a cancel in backticks e.g. \awStuff\ax -> `Stuff`
			auto result = std::regex_replace(o.str(), std::regex("\a([^\\-x]|\\-[^x])([^\a]+)\ax"), "`$2`");
			// Then remove any codes left over
			result = std::regex_replace(result, std::regex("\a."), "");
			// Then make sure there's no consecutive backticks as it makes things look funny
			result = std::regex_replace(result, std::regex("``"), "` `");
			return result;
		}

		void threadStart()
		{
			try
			{
				CallbackDiscordClient client(_token, std::bind(&DiscordClient::onMessageReceived, this, std::placeholders::_1));

				_writeNormal("Connected");

				int count = 0;
				while (!_stop)
				{
					// Every minute, send typing, to keep connection alive. Crude timer based on 1s sleep below
					try
					{
						if (++count % 60 == 0 && !client.isRateLimited())
							for (const auto& channel : _channels)
								client.sendTyping(channel.id);
					}
					// This is not so critical that it should shut things down if it doesn't work
					catch (...)	{ }

					// grab all queued messages, put them into a map of channel -> combined message
					std::map<std::string, std::string> combinedMessages;
					while (true)
					{
						std::tuple<std::string, std::string> message;
						{
							std::lock_guard<std::mutex> lock(_messagesMutex);
							if (_messages.empty())
								break;
							message = _messages.front();
							_messages.pop();
						}
						//combinedMessages[std::get<0>(message)] += escape_json(std::get<1>(message) + "\n");
						combinedMessages[std::get<0>(message)] += std::get<1>(message) + '\n';

						// If the message is too long, send what we currently have and grab the rest the next go through
						if (combinedMessages[std::get<0>(message)].length() > 1800)
							break;
					}

					for (const auto& kvp : combinedMessages)
					{
						while (true)
						{
							try
							{
								client.sendMessage(kvp.first, kvp.second);
								break;
							}
							catch (SleepyDiscord::ErrorCode& e)
							{
								// If we're rate limited, back off a bit and try again shortly. Otherwise, bail out
								if (e == SleepyDiscord::TOO_MANY_REQUESTS || e == SleepyDiscord::RATE_LIMITED)
									std::this_thread::sleep_for(std::chrono::milliseconds(200));
								else
								{
									_writeError("\ar%s\aw - %s", errorString(e).c_str(), errorDesc(e).c_str());
									break;
								}
							}
						}
					}

					client.poll();
					std::this_thread::sleep_for(std::chrono::milliseconds(1000));
				}
				_writeNormal("Disconnecting...");
			}
			catch (std::exception& e)
			{
				_writeError("Exception in thread, %s", e.what());
			}
			catch (...)
			{
				auto e = std::current_exception();
				_writeError("Unknown error in thread");
			}
		}
	};
}