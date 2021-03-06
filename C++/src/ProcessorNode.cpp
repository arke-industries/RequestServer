#include "ProcessorNode.h"

using namespace std;
using namespace util;
using namespace util::net;
using namespace game_server;

processor_node::processor_node(word workers, endpoint ep, endpoint broker_ep, obj_id area_id) : processor_node(workers, vector<endpoint> { ep }, broker_ep, area_id) {

}

processor_node::processor_node(word workers, vector<endpoint> eps, endpoint broker_ep, obj_id area_id) : server(eps, workers, result_codes::retry_later) {
	this->workers = workers;
	this->area_id = area_id;
	this->broker_ep = broker_ep;

	this->server.on_disconnect += std::bind(&processor_node::on_disconnect, this, std::placeholders::_1);
	this->server.on_request += std::bind(&processor_node::on_request, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5, std::placeholders::_6);
}

processor_node::~processor_node() {

}

void processor_node::start() {
	this->server.start();

	if (this->area_id != 0) {
		this->broker = this->server.adopt(tcp_connection(broker_ep));
		this->broker->state = reinterpret_cast<void*>(this->area_id);
		this->authenticated_clients[this->area_id].push_back(this->broker);
		this->send_to_broker(this->area_id, this->create_message(0x00, 0x00));
	}
}

void processor_node::add_client(obj_id id, shared_ptr<tcp_connection> conn) {
	if (id == 0)
		return;

	std::unique_lock<std::mutex> lck(this->clients_lock);

	conn->state = reinterpret_cast<void*>(id);
	this->authenticated_clients[id].push_back(conn);
}

void processor_node::del_client(obj_id id, shared_ptr<tcp_connection> conn) {
	if (id == 0)
		return;

	if (id == this->area_id)
		throw broker_node_down_exception();

	std::unique_lock<std::mutex> lck(this->clients_lock);

	auto i = this->authenticated_clients.find(id);
	if (i != this->authenticated_clients.end()) {
		auto& list = i->second;
		auto j = find(list.begin(), list.end(), conn);

		list.erase(j);

		if (list.size() == 0)
			this->authenticated_clients.erase(id);
	}
}

void processor_node::send(obj_id receipient_id, data_stream notification) {
	std::unique_lock<std::mutex> lck(this->clients_lock);

	auto iter = this->authenticated_clients.find(receipient_id);
	if (iter != this->authenticated_clients.end())
		for (auto i : iter->second)
			this->server.enqueue_outgoing(request_server::message(i, notification));
}

void processor_node::send_to_broker(obj_id target_id, data_stream message) {
	message.write(target_id);
	this->server.enqueue_outgoing(request_server::message(this->broker, std::move(message)));
}

data_stream processor_node::create_message(uint8 category, uint8 type) {
	data_stream notification;
	request_server::message::write_header(notification, 0x00, category, type);
	return notification;
}

void processor_node::on_connect(shared_ptr<tcp_connection> client) {

}

void processor_node::on_disconnect(shared_ptr<tcp_connection> client) {
	this->del_client(reinterpret_cast<obj_id>(client->state), client);
}

request_server::request_result processor_node::on_request(shared_ptr<tcp_connection> client, word worker_num, uint8 category, uint8 method, data_stream& parameters, data_stream& response) {
	result_code result = result_codes::success;
	obj_id authenticated_id = reinterpret_cast<obj_id>(client->state);
	obj_id start_id = authenticated_id;
	uint16 type = (category << 8) | method;

	if ((authenticated_id != 0 && this->authenticated_handlers.count(type) == 0) || (authenticated_id == 0 && this->unauthenticated_handlers.count(type) == 0)) {
		response.write(result_codes::invalid_request_type);
		return request_server::request_result::success;
	}

	auto& handler = *reinterpret_cast<base_handler*>(authenticated_id != 0 ? this->authenticated_handlers[type][worker_num].get() : this->unauthenticated_handlers[type][worker_num].get());

	try {
		handler.deserialize(parameters);
	}
	catch (data_stream::read_past_end_exception&) {
		response.write(result_codes::invalid_parameters);
		return request_server::request_result::success;
	}

	try {
		result = handler.process(authenticated_id);
	}
	catch (sql::synchronization_exception&) {
		return request_server::request_result::retry_later;
	}

	response.write(result);

	if (result == result_codes::success)
		handler.serialize(response);
	else if (result == result_codes::no_response)
		return request_server::request_result::no_response;

	if (authenticated_id != start_id) {
		if (authenticated_id != 0) {
			this->add_client(authenticated_id, client);
		}
		else {
			this->del_client(start_id, client);
		}
	}

	return request_server::request_result::success;
}