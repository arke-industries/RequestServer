#include "CacheProvider.h"

#include <algorithm>
#include <cstring>
#include <chrono>

using namespace std;
using namespace util;
using namespace game_server;
using namespace game_server::objects;

cache_provider::cache_provider(coord start_x, coord start_y, dimension width, dimension height, dimension los_radius) {
	this->set_bounds(start_x, start_y, width, height, los_radius);
}

void cache_provider::set_bounds(coord start_x, coord start_y, dimension width, dimension height, dimension los_radius) {
	this->start_x = start_x;
	this->start_y = start_y;
	this->end_x = start_x + width;
	this->end_y = start_y + height;
	this->width = width;
	this->height = height;
	this->los_radius = los_radius;
}

cache_provider::~cache_provider() {
	for (auto i : this->id_idx)
		delete i.second;
}

void cache_provider::lock() {
	this->mtx.lock();
	this->lock_holder = this_thread::get_id();
}

void cache_provider::unlock() {
	this->lock_holder = thread::id();
	this->mtx.unlock();
}

void cache_provider::begin_update(coord x, coord y, dimension width, dimension height) {
	this->lock();
}

void cache_provider::end_update() {
	this->unlock();
}

updatable* cache_provider::get_next_updatable(word position) {
	if (this->lock_holder != this_thread::get_id())
		throw sql::synchronization_exception();

	return position < this->updatable_idx.size() ? this->updatable_idx[position] : nullptr;
}

void cache_provider::add_internal(base_obj* object) {
	this->id_idx[object->id] = object;

	this->owner_idx[object->owner].push_back(object);

	auto as_updatable = dynamic_cast<updatable*>(object);
	if (as_updatable)
		this->updatable_idx.push_back(as_updatable);
}

bool cache_provider::add_internal(map_obj* object) {
	for (coord x = object->x; x < object->x + object->width; x++)
		for (coord y = object->y; y < object->y + object->height; y++)
			if (this->get_loc(x, y) != nullptr)
				return false;

	for (coord x = object->x; x < object->x + object->width; x++)
		for (coord y = object->y; y < object->y + object->height; y++)
			this->get_loc(x, y) = object;

	return true;
}

void cache_provider::remove_internal(base_obj* object) {
	this->id_idx.erase(object->id);

	if (object->owner != 0) {
		auto& owner = this->owner_idx[object->owner];
		auto iter = find(owner.begin(), owner.end(), object);
		if (iter != owner.end())
			owner.erase(iter);
	}

	auto as_updatable = dynamic_cast<updatable*>(object);
	if (as_updatable) {
		auto iter = find(this->updatable_idx.begin(), this->updatable_idx.end(), as_updatable);
		if (iter != this->updatable_idx.end())
			this->updatable_idx.erase(iter);
	}
}

void cache_provider::remove_internal(map_obj* object) {
	for (coord x = object->x; x < object->x + object->width; x++)
		for (coord y = object->y; y < object->y + object->height; y++)
			this->get_loc(x, y) = nullptr;
}

map_obj*& cache_provider::get_loc(coord x, coord y) {
	return this->loc_idx[x][y];
}

bool cache_provider::is_root_object(map_obj* obj, coord x, coord y) {
	return obj->x == x && obj->y == y;
}

void cache_provider::clamp(coord& start_x, coord& start_y, coord& end_x, coord& end_y) {
	if (start_x < this->start_x) start_x = this->start_x;
	if (start_y < this->start_y) start_y = this->start_y;
	if (end_x >= this->end_x) end_x = this->end_x - 1;
	if (end_y >= this->end_y) end_y = this->end_y - 1;
}

unique_ptr<base_obj> cache_provider::get_by_id(obj_id search_id) {
	unique_lock<recursive_mutex> lck(this->mtx);

	if (this->id_idx.count(search_id) == 0)
		return nullptr;

	return unique_ptr<base_obj>(this->id_idx[search_id]->clone());
}

unique_ptr<map_obj> cache_provider::get_at_location(coord x, coord y) {
	unique_lock<recursive_mutex> lck(this->mtx);

	auto obj = this->get_loc(x, y);
	if (obj)
		return unique_ptr<map_obj>(obj->clone());
	else
		return unique_ptr<map_obj>();
}

unordered_map<obj_id, unique_ptr<map_obj>> cache_provider::get_in_area(coord x, coord y, dimension width, dimension height) {
	unordered_map<obj_id, unique_ptr<map_obj>> result;
	coord end_x = x + width;
	coord end_y = y + height;
	
	this->clamp(x, y, end_x, end_y);
				
	unique_lock<recursive_mutex> lck(this->mtx);
	for (; x < end_x; x++) {
		for (y = end_y - height; y < end_y; y++) {
			map_obj* current = this->get_loc(x, y);
			if (current && this->is_root_object(current, x, y))
				result.emplace(current->id, unique_ptr<map_obj>(current->clone()));
		}
	}

	return result;
}

unordered_map<obj_id, unique_ptr<base_obj>> cache_provider::get_by_owner(owner_id owner) {
	unordered_map<obj_id, unique_ptr<base_obj>> result;

	unique_lock<recursive_mutex> lck(this->mtx);
	if (this->owner_idx.count(owner) != 0)
		for (auto i : this->owner_idx[owner])
			result.emplace(i->id, unique_ptr<base_obj>(i->clone()));

	return result;
}

unordered_map<obj_id, unique_ptr<map_obj>> cache_provider::get_in_owner_los(owner_id owner) {
	unordered_map<obj_id, unique_ptr<map_obj>> result;

	unique_lock<recursive_mutex> lck(this->mtx);
	if (this->owner_idx.count(owner) == 0)
		return result;

	auto& owner_objects = this->owner_idx[owner];
	coord start_x, start_y, end_x, end_y, x, y;
	for (auto object : owner_objects) {
		auto current_object = dynamic_cast<map_obj*>(object);
		if (!current_object)
			continue;

		start_x = current_object->x - this->los_radius;
		start_y = current_object->y - this->los_radius;
		end_x = current_object->x + this->los_radius;
		end_y = current_object->y + this->los_radius;

		this->clamp(start_x, start_y, end_x, end_y);
		
		for (x = start_x; x < end_x; x++) {
			for (y = start_y; y < end_y; y++) {
				auto current_test_object = this->get_loc(x, y);
				if (current_test_object && this->is_root_object(current_test_object, x, y))
					result.emplace(current_test_object->id, unique_ptr<map_obj>(current_test_object->clone()));
			}
		}
	}

	return result;
}

unordered_map<obj_id, unique_ptr<map_obj>> cache_provider::get_in_owner_los(owner_id owner, coord x, coord y, dimension width, dimension height) {
	unordered_map<obj_id, unique_ptr<map_obj>> result;

	for (auto& current_object : this->get_in_owner_los(owner))
	if (current_object.second->x >= x && current_object.second->y >= y && current_object.second->x <= x + width && current_object.second->y <= y + height)
			result.insert(move(current_object));

	return result;
}

unordered_set<obj_id> cache_provider::get_users_with_los_at(coord x, coord y) {
	unordered_set<obj_id> result;
	coord end_x = x + this->los_radius;
	coord end_y = y + this->los_radius;
	coord start_x = x - this->los_radius;
	coord start_y = y - this->los_radius;

	this->clamp(start_x, start_y, end_x, end_y);

	unique_lock<recursive_mutex> lck(this->mtx);
	for (coord this_x = start_x; this_x < end_x; this_x++) {
		for (coord this_y = start_y; this_y < end_y; this_y++) {
			auto* current = this->get_loc(this_x, this_y);
			if (current && current->owner != 0)
				result.insert(current->owner);
		}
	}

	return result;
}

bool cache_provider::is_area_empty(coord x, coord y, dimension width, dimension height) {
	coord end_x = x + width;
	coord end_y = y + height;

	this->clamp(x, y, end_x, end_y);

	unique_lock<recursive_mutex> lck(this->mtx);
	for (; x < end_x; x++)
		for (y = end_y - height; y < end_y; y++)
			if (this->get_loc(x, y))
				return false;

	return true;
}

bool cache_provider::is_location_in_los(coord x, coord y, owner_id owner) {
	coord end_x = x + this->los_radius;
	coord end_y = y + this->los_radius;
	coord start_x = x - this->los_radius;
	coord start_y = y - this->los_radius;

	this->clamp(start_x, start_y, end_x, end_y);

	unique_lock<recursive_mutex> lck(this->mtx);
	for (x = start_x; x < end_x; x++) {
		for (y = start_y; y < end_y; y++) {
			auto current = this->get_loc(x, y);
			if (current && this->is_root_object(current, x, y) && current->owner == owner) {
				return true;
			}
		}
	}

	return false;
}

bool cache_provider::is_location_in_bounds(coord x, coord y, dimension width, dimension height) {
	return x >= this->start_x && y >= this->start_y && x + width <= this->end_x && y + height <= this->end_y;
}

bool cache_provider::is_user_present(obj_id user_id) {
	unique_lock<recursive_mutex> lck(this->mtx);
	return this->owner_idx.count(user_id) != 0;
}