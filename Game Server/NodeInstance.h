#pragma once

#include <iostream>
#include <cstring>
#include <string>

#include <libconfig.h++>

#include <Utilities/Common.h>
#include <Utilities/SQLDatabase.h>
#include <Utilities/RequestServer.h>

#include "BaseMessages.h"
#include "DBContext.h"

#include "Common.h"

namespace GameServer {
	template<typename T> class NodeInstance {
		private:
			Utilities::RequestServer* requestServer;

		public:
			libconfig::Config config;

			typedef BaseRequest<T>* (*HandlerCreator)(uint8 category, uint8 method, uint64 userId, uint16& errorCode);
			typedef T* (*ContextCreator)(Utilities::SQLDatabase::Connection::Parameters& parameters);

			exported NodeInstance(HandlerCreator handlerCreator, ContextCreator contextCreator, std::string configFileName) {
				this->config.readFile(configFileName.c_str());
				libconfig::Setting& settings = this->config.getRoot();

				this->workers = static_cast<uint32>(settings["workerThreads"]);
				this->tcpPort = string(settings["tcpServerPort"].c_str());
				this->wsPort = string(settings["webSocketServerPort"].c_str());
				this->handlerCreator = handlerCreator;
				this->dbConnections = new T*[this->workers];
				this->requestServer = nullptr;

				const libconfig::Setting& dbSettings = settings["Database"];
				Utilities::SQLDatabase::Connection::Parameters parameters = {dbSettings["host"].c_str(), dbSettings["port"].c_str(), dbSettings["dbname"].c_str(), dbSettings["role"].c_str(), dbSettings["password"].c_str()};
				for (uint8 i = 0; i < this->workers; i++)
					this->dbConnections[i] = contextCreator(parameters);
			}

			exported ~NodeInstance(){
				for (uint8 i = 0; i < this->workers; i++)
					delete this->dbConnections[i];

				delete[] this->dbConnections;

				if (this->requestServer)
					delete this->requestServer;
			}

			exported void run() {
				std::vector<string> ports;
				ports.push_back(this->tcpPort);
				ports.push_back(this->wsPort);

				std::vector<bool> flags;
				flags.push_back(false);
				flags.push_back(true);

				this->requestServer = new RequestServer(ports, this->workers, flags, IResultCode::RETRY_LATER, onRequest, this);

				int8 input;
				do {
					cin >> input;
				} while (input != 'c');
			}

			exported void sendNotification(ObjectId userId, uint64 connectionId, Utilities::DataStream& message) {
				this->requestServer->send(userId, connectionId, message);
			}


		private:
			uint32 workers;
			std::string tcpPort;
			std::string wsPort;
			HandlerCreator handlerCreator;
			T** dbConnections;

			static bool onRequest(uint8 workerNumber, Utilities::RequestServer::Client& client, uint8 requestCategory, uint8 requestMethod, Utilities::DataStream& parameters, Utilities::DataStream& response, void* state) {
				NodeInstance& node = *static_cast<NodeInstance*>(state);
				ResultCode resultCode = IResultCode::SUCCESS;
				T& context = *node.dbConnections[workerNumber];

				auto handler = node.handlerCreator(requestCategory, requestMethod, client.authenticatedId, resultCode);
				if (resultCode != IResultCode::SUCCESS) {
					response.write(resultCode);
					return true;
				}

				try {
					handler->deserialize(parameters);
				} catch (Utilities::DataStream::ReadPastEndException&) {
					response.write(resultCode);
					return true;
				}
	
				context.beginTransaction();
				resultCode = handler->process(client.authenticatedId, client.id, client.ipAddress, context);
				try {
					context.commitTransaction();
				} catch (const Utilities::SQLDatabase::Exception& e) {
					std::cout << e.what << std::endl;
					context.rollbackTransaction();
					resultCode = IResultCode::SERVER_ERROR;
				}

				response.write<ResultCode>(static_cast<ResultCode>(resultCode));
				if (resultCode == IResultCode::SUCCESS)
					handler->serialize(response);

				delete handler;

				return true;
			}
	};
}
